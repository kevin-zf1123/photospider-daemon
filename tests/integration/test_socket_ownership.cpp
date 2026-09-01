#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "ipc/unix_socket.hpp"
#include "server/server.hpp"
#include "support/test_support.hpp"

namespace internal = ps::ipc::internal;

namespace {

/** @brief Stable filesystem identity captured without following symlinks. */
struct NodeIdentity final {
  /** @brief Filesystem device number. */
  dev_t device = 0;
  /** @brief Filesystem inode number. */
  ino_t inode = 0;
  /** @brief File type and permission bits. */
  mode_t mode = 0;
};

/**
 * @brief Returns one process-unique uncreated Unix socket path.
 * @return Bounded path under `/tmp`.
 * @throws std::bad_alloc If path construction fails.
 * @note A monotonic sequence separates all ownership cases.
 */
std::string socket_path() {
  static std::atomic<std::uint32_t> sequence{0U};
  return "/tmp/psd-owner-" + std::to_string(::getpid()) + "-" +
         std::to_string(sequence.fetch_add(1U)) + ".sock";
}

/**
 * @brief Captures one current pathname identity without following symlinks.
 * @param path Exact path to inspect.
 * @return Captured identity or a default zero identity on failure.
 * @throws Nothing.
 * @note Callers separately check the expected nonzero inode and type.
 */
NodeIdentity node_identity(const std::string& path) noexcept {
  struct stat state{};
  if (::lstat(path.c_str(), &state) != 0) {
    return {};
  }
  return NodeIdentity{state.st_dev, state.st_ino, state.st_mode};
}

/**
 * @brief Compares exact device/inode generation and file type.
 * @param left First captured identity.
 * @param right Second captured identity.
 * @return True only for one unchanged directory entry generation.
 * @throws Nothing.
 */
bool same_identity(const NodeIdentity& left,
                   const NodeIdentity& right) noexcept {
  return left.inode != 0 && left.device == right.device &&
         left.inode == right.inode &&
         (left.mode & S_IFMT) == (right.mode & S_IFMT);
}

/**
 * @brief Reports whether one path is absent with exact `ENOENT` classification.
 * @param path Exact pathname.
 * @return True only when `lstat` reports `ENOENT`.
 * @throws Nothing.
 */
bool path_absent(const std::string& path) noexcept {
  struct stat state{};
  errno = 0;
  return ::lstat(path.c_str(), &state) != 0 && errno == ENOENT;
}

/**
 * @brief Binds one raw Unix socket and leaves its filesystem node present.
 * @param path Exact unoccupied path.
 * @param listen Whether the descriptor should become a live listener.
 * @return Owned descriptor, or empty ownership on setup failure.
 * @throws std::bad_alloc If path storage allocation fails.
 * @note Closing the returned descriptor intentionally leaves a stale node.
 */
internal::UniqueDescriptor bind_raw_socket(const std::string& path,
                                           bool listen) {
  sockaddr_un address{};
  if (path.empty() || path.size() >= sizeof(address.sun_path)) {
    return {};
  }
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
  internal::UniqueDescriptor descriptor(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (!descriptor.valid() ||
      ::bind(descriptor.get(), reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0 ||
      (listen && ::listen(descriptor.get(), 4) != 0)) {
    return {};
  }
  return descriptor;
}

/**
 * @brief Builds the common bounded server configuration for one path.
 * @param path Exact local listener path.
 * @return Valid fixed configuration.
 * @throws std::bad_alloc If path copying fails.
 */
internal::ServerConfig server_config(const std::string& path) {
  return internal::ServerConfig{
      path, internal::ServiceConfig{1U, 4U, 2U, false}, 4, 4U, {}, {}};
}

/**
 * @brief Attempts construction and reports clean existing-path rejection.
 * @param path Existing path that must remain untouched.
 * @return True when construction throws before acquiring the path.
 * @throws std::bad_alloc If configuration copying fails.
 */
bool server_rejects_path(const std::string& path) {
  try {
    internal::Server unexpected(server_config(path));
  } catch (const std::exception&) {
    return true;
  }
  return false;
}

/**
 * @brief Proves a live listener cannot be replaced by a second instance.
 * @return True when second construction fails and the first remains reachable.
 * @throws std::bad_alloc If fixture allocation fails.
 * @note No accept thread is required for a bounded backlog connect.
 */
bool live_listener_rejection() {
  const std::string path = socket_path();
  internal::Server first(server_config(path));
  const NodeIdentity before = node_identity(path);
  const bool rejected = server_rejects_path(path);
  auto connected = internal::connect_unix_socket(path);
  const NodeIdentity after = node_identity(path);
  return rejected && connected.ok() && same_identity(before, after);
}

/**
 * @brief Proves a stale socket node is rejected and preserved.
 * @return True when no daemon reclaims or removes the stale generation.
 * @throws std::bad_alloc If fixture allocation fails.
 */
bool stale_socket_rejection() {
  const std::string path = socket_path();
  internal::UniqueDescriptor stale = bind_raw_socket(path, false);
  if (!stale.valid()) {
    return false;
  }
  stale.reset();
  const NodeIdentity before = node_identity(path);
  const bool rejected = server_rejects_path(path);
  const NodeIdentity after = node_identity(path);
  const bool preserved =
      rejected && S_ISSOCK(after.mode) && same_identity(before, after);
  ::unlink(path.c_str());
  return preserved;
}

/**
 * @brief Proves a regular file is rejected and preserved.
 * @return True when server construction leaves the same file generation.
 * @throws std::bad_alloc If fixture allocation fails.
 */
bool regular_file_rejection() {
  const std::string path = socket_path();
  internal::UniqueDescriptor file(
      ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR));
  if (!file.valid()) {
    return false;
  }
  file.reset();
  const NodeIdentity before = node_identity(path);
  const bool rejected = server_rejects_path(path);
  const NodeIdentity after = node_identity(path);
  const bool preserved =
      rejected && S_ISREG(after.mode) && same_identity(before, after);
  ::unlink(path.c_str());
  return preserved;
}

/**
 * @brief Proves old-instance cleanup preserves a replacement socket inode.
 * @return True when the replacement remains after the old server is destroyed.
 * @throws std::bad_alloc If fixture allocation fails.
 */
bool replacement_generation_is_preserved() {
  const std::string path = socket_path();
  internal::UniqueDescriptor replacement;
  NodeIdentity replacement_identity;
  {
    internal::Server first(server_config(path));
    if (::unlink(path.c_str()) != 0) {
      return false;
    }
    replacement = bind_raw_socket(path, true);
    if (!replacement.valid()) {
      return false;
    }
    replacement_identity = node_identity(path);
  }
  const NodeIdentity after = node_identity(path);
  const bool preserved =
      S_ISSOCK(after.mode) && same_identity(replacement_identity, after);
  replacement.reset();
  ::unlink(path.c_str());
  return preserved;
}

/**
 * @brief Proves concurrent first bind has one operating-system winner.
 * @return True when exactly one server owns the previously absent path.
 * @throws std::bad_alloc If async/server state allocation fails.
 * @throws std::system_error If test threads cannot start.
 */
bool concurrent_bind_has_one_winner() {
  const std::string path = socket_path();
  std::atomic<std::uint32_t> ready{0U};
  std::atomic<bool> start{false};
  auto attempt = [&]() -> std::unique_ptr<internal::Server> {
    ready.fetch_add(1U, std::memory_order_release);
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    try {
      return std::make_unique<internal::Server>(server_config(path));
    } catch (const std::exception&) {
      return nullptr;
    }
  };
  auto first = std::async(std::launch::async, attempt);
  auto second = std::async(std::launch::async, attempt);
  while (ready.load(std::memory_order_acquire) != 2U) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  auto first_server = first.get();
  auto second_server = second.get();
  const std::size_t winners =
      static_cast<std::size_t>(first_server != nullptr) +
      static_cast<std::size_t>(second_server != nullptr);
  auto connected = internal::connect_unix_socket(path);
  first_server.reset();
  second_server.reset();
  return winners == 1U && connected.ok() && path_absent(path);
}

/**
 * @brief Proves an identity-matched clean shutdown permits a fresh bind.
 * @return True when two sequential instances each remove only their own node.
 * @throws std::bad_alloc If fixture allocation fails.
 */
bool clean_restart_is_available() {
  const std::string path = socket_path();
  {
    internal::Server first(server_config(path));
  }
  if (!path_absent(path)) {
    return false;
  }
  {
    internal::Server second(server_config(path));
  }
  return path_absent(path);
}

/**
 * @brief Proves pre-arm allocation failure leaves no bound socket residue.
 * @return True when the exception propagates, the path stays absent, and an
 * immediate clean restart can bind and remove the same exact path.
 * @throws std::bad_alloc If fixture or restart allocation fails outside the
 * injected boundary.
 * @note Residue is removed only after all failure observations are captured so
 * the regression distinguishes safe rollback from test-owned cleanup.
 */
bool prearm_allocation_failure_is_clean() {
  const std::string path = socket_path();
  bool exception_propagated = false;
  try {
    auto unexpected =
        internal::create_unix_listener_with_prearm_allocation_failure_for_test(
            path, 4);
    static_cast<void>(unexpected);
  } catch (const std::bad_alloc&) {
    exception_propagated = true;
  }

  const bool absent_after_failure = path_absent(path);
  auto rebound = internal::create_unix_listener(path, 4);
  const bool clean_restart = rebound.ok();
  if (rebound.ok()) {
    auto listener = rebound.take_value();
    listener.descriptor.reset();
    listener.socket_node.remove();
  }
  if (!path_absent(path)) {
    ::unlink(path.c_str());
  }
  return exception_propagated && absent_after_failure && clean_restart;
}

/** @brief State captured while a bound socket pathname is replaced by a file.
 */
struct ReplacementModeProbe final {
  /** @brief Exact regular-file mode established through its open descriptor. */
  mode_t expected_mode = S_IRUSR | S_IWUSR | S_IRGRP;
  /** @brief Identity captured after the replacement is installed. */
  NodeIdentity identity;
  /** @brief True only after replacement creation and mode assignment succeed.
   */
  bool installed = false;
};

/**
 * @brief Replaces one armed socket pathname with a fixed-mode regular file.
 * @param path Exact armed listener pathname.
 * @param context Non-null `ReplacementModeProbe` destination.
 * @throws Nothing.
 * @note Descriptor-bound `fchmod` gives the fixture an exact starting mode;
 * the product under test must not later mutate it through the pathname.
 */
void install_fixed_mode_replacement(const std::string& path,
                                    void* context) noexcept {
  auto* probe = static_cast<ReplacementModeProbe*>(context);
  if (probe == nullptr || ::unlink(path.c_str()) != 0) {
    return;
  }
  internal::UniqueDescriptor file(
      ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR));
  if (!file.valid() || ::fchmod(file.get(), probe->expected_mode) != 0) {
    return;
  }
  probe->identity = node_identity(path);
  probe->installed = S_ISREG(probe->identity.mode);
}

/**
 * @brief Proves listener setup never mutates a replacement target's mode.
 * @return True when the same replacement inode and exact fixture mode survive
 * listener completion plus generation-checked cleanup.
 * @throws std::bad_alloc If fixture or diagnostic allocation fails.
 * @note The callback replaces the pathname after the original socket
 * generation is armed, at the historical pathname-based chmod window.
 */
bool replacement_target_mode_is_unchanged() {
  const std::string path = socket_path();
  ReplacementModeProbe probe;
  {
    auto listener = internal::create_unix_listener_with_armed_hook_for_test(
        path, 4, install_fixed_mode_replacement, &probe);
    static_cast<void>(listener);
  }
  const NodeIdentity after = node_identity(path);
  const bool unchanged =
      probe.installed && S_ISREG(after.mode) &&
      same_identity(probe.identity, after) &&
      (after.mode & (S_IRWXU | S_IRWXG | S_IRWXO)) == probe.expected_mode;
  ::unlink(path.c_str());
  return unchanged;
}

}  // namespace

/**
 * @brief Exercises fail-closed Unix socket pathname generation ownership.
 * @param argc Argument count; accepts at most one focused regression mode.
 * @param argv Argument vector containing an optional focused mode.
 * @return Zero when every existing/replaced/concurrent path remains correct.
 * @throws std::bad_alloc If fixture allocation fails.
 * @throws std::system_error If async test setup fails.
 * @note Behavioral failures return nonzero through `PS_IPC_CHECK`.
 */
int main(int argc, char* argv[]) {
  if (argc == 2) {
    const std::string_view mode(argv[1]);
    if (mode == "prearm-allocation") {
      PS_IPC_CHECK(prearm_allocation_failure_is_clean());
      return 0;
    }
    if (mode == "replacement-mode") {
      PS_IPC_CHECK(replacement_target_mode_is_unchanged());
      return 0;
    }
    return 2;
  }
  if (argc != 1) {
    return 2;
  }
  PS_IPC_CHECK(live_listener_rejection());
  PS_IPC_CHECK(stale_socket_rejection());
  PS_IPC_CHECK(regular_file_rejection());
  PS_IPC_CHECK(replacement_generation_is_preserved());
  PS_IPC_CHECK(concurrent_bind_has_one_winner());
  PS_IPC_CHECK(clean_restart_is_available());
  PS_IPC_CHECK(prearm_allocation_failure_is_clean());
  PS_IPC_CHECK(replacement_target_mode_is_unchanged());
  return 0;
}
