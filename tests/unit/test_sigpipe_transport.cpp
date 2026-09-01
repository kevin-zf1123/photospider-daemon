#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ipc/frame.hpp"
#include "ipc/unix_socket.hpp"
#include "support/test_support.hpp"

namespace {

/** @brief Product stream role exercised inside the supervised child. */
enum class ChildStreamRole : std::uint8_t {
  /** @brief Child uses the client descriptor prepared before connect. */
  ConnectedClient = 1U,
  /** @brief Child uses the server descriptor prepared immediately after accept.
   */
  AcceptedServer = 2U,
};

/**
 * @brief Returns one process-unique uncreated Unix socket path.
 * @return Bounded path under `/tmp`.
 * @throws std::bad_alloc If path construction fails.
 * @note A monotonic sequence keeps repeated test invocations independent.
 */
std::string socket_path() {
  static std::atomic<std::uint32_t> sequence{0U};
  return "/tmp/psd-sigpipe-" + std::to_string(::getpid()) + "-" +
         std::to_string(sequence.fetch_add(1U)) + ".sock";
}

/**
 * @brief Requires a post-close frame write to fail without terminating.
 * @param descriptor Connected local descriptor whose peer is already closed.
 * @return True when a bounded write attempt returns typed transport failure.
 * @throws std::bad_alloc If frame/status diagnostic allocation fails.
 * @note Multiple attempts tolerate one locally buffered send before EOF is
 * observed; no process-global signal disposition is changed.
 */
bool write_fails_after_peer_close(int descriptor) {
  const std::vector<std::uint8_t> payload{1U};
  for (int attempt = 0; attempt < 4; ++attempt) {
    const ps::Status status =
        ps::ipc::internal::write_frame(descriptor, payload);
    if (!status.ok()) {
      return status.code == ps::ErrorCode::Internal;
    }
  }
  return false;
}

/**
 * @brief Writes one synchronization byte with bounded EINTR handling.
 * @param descriptor Connected control-socket descriptor.
 * @param value Exact byte to send.
 * @return True only when exactly one byte is written.
 * @throws Nothing.
 * @note The control peer remains open during this operation; product SIGPIPE
 * behavior is exercised only by `write_frame()` on the separate stream.
 */
bool write_control_byte(int descriptor, std::uint8_t value) noexcept {
  ssize_t count = -1;
  do {
    count = ::write(descriptor, &value, sizeof(value));
  } while (count < 0 && errno == EINTR);
  return count == static_cast<ssize_t>(sizeof(value));
}

/**
 * @brief Reads one synchronization byte with bounded EINTR handling.
 * @param descriptor Connected control-socket descriptor.
 * @param expected Exact expected byte.
 * @return True only when exactly the expected byte is read.
 * @throws Nothing.
 */
bool read_control_byte(int descriptor, std::uint8_t expected) noexcept {
  std::uint8_t value = 0U;
  ssize_t count = -1;
  do {
    count = ::read(descriptor, &value, sizeof(value));
  } while (count < 0 && errno == EINTR);
  return count == static_cast<ssize_t>(sizeof(value)) && value == expected;
}

/**
 * @brief Parses one exact nonnegative descriptor argument.
 * @param text Null-terminated decimal argument.
 * @param descriptor Non-null parsed destination.
 * @return True only when the complete argument is a nonnegative `int`.
 * @throws Nothing.
 */
bool parse_descriptor(const char* text, int* descriptor) noexcept {
  if (text == nullptr || descriptor == nullptr) {
    return false;
  }
  const std::string_view input(text);
  int parsed = -1;
  const auto result =
      std::from_chars(input.data(), input.data() + input.size(), parsed);
  if (result.ec != std::errc() || result.ptr != input.data() + input.size() ||
      parsed < 0) {
    return false;
  }
  *descriptor = parsed;
  return true;
}

/**
 * @brief Reports whether one live descriptor has close-on-exec enabled.
 * @param descriptor Product descriptor to inspect.
 * @return True only when `F_GETFD` includes `FD_CLOEXEC`.
 * @throws Nothing.
 */
bool descriptor_is_cloexec(int descriptor) noexcept {
  const int flags = ::fcntl(descriptor, F_GETFD);
  return flags >= 0 && (flags & FD_CLOEXEC) != 0;
}

/**
 * @brief Creates one explicitly inheritable test-owned listener duplicate.
 * @param descriptor Product listener descriptor to duplicate.
 * @return Owned duplicate with `FD_CLOEXEC` cleared, or empty on failure.
 * @throws Nothing.
 * @note The product listener flags are never modified; only this duplicate is
 * inherited by the SIGPIPE self-exec child that must accept a test peer.
 */
ps::ipc::internal::UniqueDescriptor make_inheritable_duplicate(
    int descriptor) noexcept {
  ps::ipc::internal::UniqueDescriptor duplicate(::dup(descriptor));
  if (!duplicate.valid()) {
    return {};
  }
  const int flags = ::fcntl(duplicate.get(), F_GETFD);
  if (flags < 0 ||
      ::fcntl(duplicate.get(), F_SETFD, flags & ~FD_CLOEXEC) != 0) {
    return {};
  }
  return duplicate;
}

/**
 * @brief Confirms a descriptor number is closed after self-exec.
 * @param descriptor Pre-exec product descriptor number.
 * @return True only for `F_GETFD` failure with `EBADF`.
 * @throws Nothing.
 */
bool descriptor_is_closed_after_exec(int descriptor) noexcept {
  errno = 0;
  return ::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF;
}

/**
 * @brief Opens the selected product-prepared stream inside the child.
 * @param role Client-connect or server-accept lifecycle role.
 * @param path Exact listener pathname.
 * @param listener_descriptor Inherited listener descriptor.
 * @return Connected product stream or typed setup failure.
 * @throws std::bad_alloc If transport diagnostics allocate.
 * @note The inherited listener is closed before return in both roles.
 */
ps::Result<ps::ipc::internal::UniqueDescriptor> open_child_product_stream(
    ChildStreamRole role, const std::string& path, int listener_descriptor) {
  if (role == ChildStreamRole::ConnectedClient) {
    ::close(listener_descriptor);
    return ps::ipc::internal::connect_unix_socket(path);
  }
  auto accepted = ps::ipc::internal::accept_same_user(listener_descriptor);
  ::close(listener_descriptor);
  return accepted;
}

/**
 * @brief Runs the self-executed child under default SIGPIPE disposition.
 * @param role Product stream lifecycle role to exercise.
 * @param path Exact listener pathname.
 * @param listener_descriptor Inherited listener descriptor.
 * @param control_descriptor Child side of the synchronization socket.
 * @param parent_control_descriptor Inherited parent side to close immediately.
 * @return Zero only after typed post-close write failure while still alive.
 * @throws std::bad_alloc If product fixture or diagnostics allocate.
 * @note `SIGPIPE` is restored to `SIG_DFL` before connect/accept. Exit code is
 * observed by the parent; an unsuppressed signal instead produces
 * `WIFSIGNALED` and can never masquerade as success.
 */
int run_child(ChildStreamRole role, const std::string& path,
              int listener_descriptor, int control_descriptor,
              int parent_control_descriptor) {
  ::close(parent_control_descriptor);
  ps::ipc::internal::UniqueDescriptor control(control_descriptor);
  if (::signal(SIGPIPE, SIG_DFL) == SIG_ERR) {
    return 10;
  }
  auto stream_result =
      open_child_product_stream(role, path, listener_descriptor);
  if (!stream_result.ok()) {
    return 11;
  }
  ps::ipc::internal::UniqueDescriptor stream = stream_result.take_value();
  if (!write_control_byte(control.get(), 'R') ||
      !read_control_byte(control.get(), 'G')) {
    return 12;
  }
  return write_fails_after_peer_close(stream.get()) ? 0 : 13;
}

/**
 * @brief RAII supervision for one fork/exec child process.
 * @note Destruction terminates and reaps only an unreaped test child; normal
 * success is consumed by `wait_for_normal_exit()`.
 */
class ChildProcess final {
 public:
  /**
   * @brief Takes supervision responsibility for one child pid.
   * @param process Positive child pid, or a negative empty value.
   * @throws Nothing.
   */
  explicit ChildProcess(pid_t process) noexcept : process_(process) {}

  /**
   * @brief Terminates and reaps an outstanding test child.
   * @throws Nothing.
   * @note A child already consumed by `wait_for_normal_exit()` is untouched.
   */
  ~ChildProcess() noexcept {
    if (process_ > 0) {
      static_cast<void>(::kill(process_, SIGKILL));
      reap(nullptr);
    }
  }

  /**
   * @brief Forbids duplicate process supervision.
   * @param other Source supervisor that cannot be copied.
   * @throws Nothing; the operation is deleted.
   */
  ChildProcess(const ChildProcess& other) = delete;
  /**
   * @brief Forbids duplicate process-supervision assignment.
   * @param other Source supervisor that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  ChildProcess& operator=(const ChildProcess& other) = delete;

  /**
   * @brief Waits for a normal zero child exit.
   * @return True only for `WIFEXITED` with status zero.
   * @throws Nothing.
   * @note A default-disposition SIGPIPE is observed as `WIFSIGNALED` and
   * returns false.
   */
  bool wait_for_normal_exit() noexcept {
    int status = 0;
    if (!reap(&status)) {
      return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }

 private:
  /**
   * @brief Reaps the supervised pid with EINTR retry.
   * @param status Optional wait-status destination.
   * @return True when the exact child was reaped.
   * @throws Nothing.
   */
  bool reap(int* status) noexcept {
    int ignored = 0;
    int* destination = status != nullptr ? status : &ignored;
    pid_t observed = -1;
    do {
      observed = ::waitpid(process_, destination, 0);
    } while (observed < 0 && errno == EINTR);
    if (observed == process_) {
      process_ = -1;
      return true;
    }
    return false;
  }

  /** @brief Positive unreaped child pid, or -1 after settlement. */
  pid_t process_ = -1;
};

/**
 * @brief Exercises one product stream role in a supervised self-exec child.
 * @param executable Exact current test executable path.
 * @param role Client-connect or server-accept lifecycle role.
 * @return True for typed child failure plus normal zero exit.
 * @throws std::bad_alloc If fixture, argument, or transport allocation fails.
 * @note Parent closes the product peer before sending the `G` control byte, so
 * the child never races the intended broken-stream write.
 */
bool supervised_peer_close_write(const std::string& executable,
                                 ChildStreamRole role) {
  using ps::ipc::internal::accept_same_user;
  using ps::ipc::internal::BoundUnixListener;
  using ps::ipc::internal::connect_unix_socket;
  using ps::ipc::internal::create_unix_listener;
  using ps::ipc::internal::UniqueDescriptor;

  const std::string path = socket_path();
  auto listener_result = create_unix_listener(path, 4);
  if (!listener_result.ok()) {
    return false;
  }
  BoundUnixListener listener = listener_result.take_value();

  int controls[2]{-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, controls) != 0) {
    return false;
  }
  UniqueDescriptor parent_control(controls[0]);
  UniqueDescriptor child_control(controls[1]);
  UniqueDescriptor inherited_listener =
      make_inheritable_duplicate(listener.descriptor.get());
  if (!inherited_listener.valid()) {
    return false;
  }
  const std::string listener_argument =
      std::to_string(inherited_listener.get());
  const std::string child_control_argument =
      std::to_string(child_control.get());
  const std::string parent_control_argument =
      std::to_string(parent_control.get());
  const char* mode = role == ChildStreamRole::ConnectedClient
                         ? "--child-client"
                         : "--child-accepted";
  char* child_arguments[] = {const_cast<char*>(executable.c_str()),
                             const_cast<char*>(mode),
                             const_cast<char*>(path.c_str()),
                             const_cast<char*>(listener_argument.c_str()),
                             const_cast<char*>(child_control_argument.c_str()),
                             const_cast<char*>(parent_control_argument.c_str()),
                             nullptr};

  const pid_t process = ::fork();
  if (process < 0) {
    return false;
  }
  if (process == 0) {
    ::execv(executable.c_str(), child_arguments);
    ::_exit(127);
  }
  ChildProcess child(process);
  inherited_listener.reset();
  child_control.reset();

  auto peer_result = role == ChildStreamRole::ConnectedClient
                         ? accept_same_user(listener.descriptor.get())
                         : connect_unix_socket(path);
  if (!peer_result.ok()) {
    return false;
  }
  UniqueDescriptor peer = peer_result.take_value();
  if (!read_control_byte(parent_control.get(), 'R')) {
    return false;
  }
  peer.reset();
  if (!write_control_byte(parent_control.get(), 'G')) {
    return false;
  }
  return child.wait_for_normal_exit();
}

/**
 * @brief Verifies all product-owned socket descriptors are close-on-exec.
 * @param executable Exact current test executable path.
 * @return True when four live flags are set and none survive fork plus exec.
 * @throws std::bad_alloc If fixture or argument allocation fails.
 * @throws std::system_error If process setup fails.
 * @note The four roles are listener, fixed parent directory, connected client,
 * and accepted server stream. The child only observes inherited numbers.
 */
bool cloexec_descriptor_regression(const std::string& executable) {
  using ps::ipc::internal::accept_same_user;
  using ps::ipc::internal::BoundUnixListener;
  using ps::ipc::internal::connect_unix_socket;
  using ps::ipc::internal::create_unix_listener;
  using ps::ipc::internal::UniqueDescriptor;

  const std::string path = socket_path();
  auto listener_result = create_unix_listener(path, 4);
  if (!listener_result.ok()) {
    return false;
  }
  BoundUnixListener listener = listener_result.take_value();
  auto client_result = connect_unix_socket(path);
  if (!client_result.ok()) {
    return false;
  }
  auto accepted_result = accept_same_user(listener.descriptor.get());
  if (!accepted_result.ok()) {
    return false;
  }
  UniqueDescriptor client = client_result.take_value();
  UniqueDescriptor accepted = accepted_result.take_value();
  const int descriptors[] = {listener.descriptor.get(),
                             listener.socket_node.parent_descriptor(),
                             client.get(), accepted.get()};
  for (int descriptor : descriptors) {
    if (!descriptor_is_cloexec(descriptor)) {
      return false;
    }
  }

  const std::string listener_argument = std::to_string(descriptors[0]);
  const std::string parent_argument = std::to_string(descriptors[1]);
  const std::string client_argument = std::to_string(descriptors[2]);
  const std::string accepted_argument = std::to_string(descriptors[3]);
  char* child_arguments[] = {const_cast<char*>(executable.c_str()),
                             const_cast<char*>("--child-cloexec"),
                             const_cast<char*>(listener_argument.c_str()),
                             const_cast<char*>(parent_argument.c_str()),
                             const_cast<char*>(client_argument.c_str()),
                             const_cast<char*>(accepted_argument.c_str()),
                             nullptr};
  const pid_t process = ::fork();
  if (process < 0) {
    return false;
  }
  if (process == 0) {
    ::execv(executable.c_str(), child_arguments);
    ::_exit(127);
  }
  ChildProcess child(process);
  return child.wait_for_normal_exit();
}

/**
 * @brief Verifies Darwin SIGPIPE setup failure closes transferred ownership.
 * @return True for typed failure plus closed fd, or on non-Darwin platforms.
 * @throws std::bad_alloc If failure diagnostic allocation fails.
 * @note A pipe descriptor deterministically makes Darwin socket `setsockopt`
 * fail without injecting or replacing a production system call.
 */
bool sigpipe_configuration_failure_regression() {
#if defined(__APPLE__)
  int descriptors[2]{-1, -1};
  if (::pipe(descriptors) != 0) {
    return false;
  }
  const int transferred = descriptors[1];
  auto prepared = ps::ipc::internal::prepare_stream_for_test(transferred);
  errno = 0;
  const bool closed = ::fcntl(transferred, F_GETFD) == -1 && errno == EBADF;
  ::close(descriptors[0]);
  return !prepared.ok() && prepared.status().code == ps::ErrorCode::Internal &&
         closed;
#else
  return true;
#endif
}

}  // namespace

/**
 * @brief Exercises descriptor-scoped SIGPIPE suppression and rollback.
 * @param argc Argument count for parent or self-executed child mode.
 * @param argv Argument vector carrying child role/path/inherited descriptors.
 * @return Zero when peer-close writes fail safely and setup rollback is exact.
 * @throws std::bad_alloc If fixture or transport diagnostic allocation fails.
 * @note Behavioral failures return nonzero through `PS_IPC_CHECK`.
 */
int main(int argc, char* argv[]) {
  if (argc == 6 && std::string_view(argv[1]) == "--child-cloexec") {
    for (int index = 2; index < argc; ++index) {
      int descriptor = -1;
      if (!parse_descriptor(argv[index], &descriptor) ||
          !descriptor_is_closed_after_exec(descriptor)) {
        return 30 + index;
      }
    }
    return 0;
  }
  if (argc == 6 && (std::string_view(argv[1]) == "--child-client" ||
                    std::string_view(argv[1]) == "--child-accepted")) {
    int listener_descriptor = -1;
    int control_descriptor = -1;
    int parent_control_descriptor = -1;
    if (!parse_descriptor(argv[3], &listener_descriptor) ||
        !parse_descriptor(argv[4], &control_descriptor) ||
        !parse_descriptor(argv[5], &parent_control_descriptor)) {
      return 20;
    }
    const ChildStreamRole role = std::string_view(argv[1]) == "--child-client"
                                     ? ChildStreamRole::ConnectedClient
                                     : ChildStreamRole::AcceptedServer;
    return run_child(role, argv[2], listener_descriptor, control_descriptor,
                     parent_control_descriptor);
  }
  if (argc != 1) {
    return 2;
  }
  const std::string executable(argv[0]);
  PS_IPC_CHECK(supervised_peer_close_write(executable,
                                           ChildStreamRole::ConnectedClient));
  PS_IPC_CHECK(
      supervised_peer_close_write(executable, ChildStreamRole::AcceptedServer));
  PS_IPC_CHECK(sigpipe_configuration_failure_regression());
  PS_IPC_CHECK(cloexec_descriptor_regression(executable));
  return 0;
}
