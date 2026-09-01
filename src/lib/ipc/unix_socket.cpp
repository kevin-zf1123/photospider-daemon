#include "ipc/unix_socket.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

namespace ps::ipc::internal {
namespace {

/** @brief Parent pathname plus one final socket entry name. */
struct SocketPathParts final {
  /** @brief Parent directory path, following the bind pathname spelling. */
  std::string parent_path;
  /** @brief Nonempty single final component. */
  std::string leaf_name;
};

/**
 * @brief Validates and copies one filesystem path into `sockaddr_un`.
 * @param path Exact local socket filesystem path.
 * @return Populated address or typed invalid-argument failure.
 * @throws std::bad_alloc If failure diagnostic allocation fails.
 * @note The terminating null byte must fit in `sun_path`.
 */
Result<sockaddr_un> socket_address(const std::string& path) {
  sockaddr_un address{};
  if (path.empty() || path.size() >= sizeof(address.sun_path)) {
    return Result<sockaddr_un>(Status::failure(
        ErrorCode::InvalidArgument, "Unix-domain socket path is invalid"));
  }
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
  return Result<sockaddr_un>(address);
}

/**
 * @brief Splits one validated socket pathname for fixed-dirfd operations.
 * @param path Exact local socket pathname.
 * @return Parent path and single leaf component, or argument failure.
 * @throws std::bad_alloc If component storage or diagnostics allocation fails.
 * @note Relative leaf-only paths use `.` as their parent. Empty, `.`, and `..`
 * leaves are rejected before any filesystem effect.
 */
Result<SocketPathParts> split_socket_path(const std::string& path) {
  const std::size_t separator = path.find_last_of('/');
  SocketPathParts parts;
  if (separator == std::string::npos) {
    parts.parent_path = ".";
    parts.leaf_name = path;
  } else {
    parts.parent_path = separator == 0U ? "/" : path.substr(0U, separator);
    parts.leaf_name = path.substr(separator + 1U);
  }
  if (parts.leaf_name.empty() || parts.leaf_name == "." ||
      parts.leaf_name == "..") {
    return Result<SocketPathParts>(Status::failure(
        ErrorCode::InvalidArgument, "Unix-domain socket leaf is invalid"));
  }
  return Result<SocketPathParts>(std::move(parts));
}

/**
 * @brief Builds one errno-backed local transport status.
 * @param action Stable description of the failed operation.
 * @return Internal failure containing the current errno diagnostic.
 * @throws std::bad_alloc If diagnostic allocation fails.
 * @note Call immediately after the failing system call.
 */
Status errno_status(const std::string& action) {
  return Status::failure(ErrorCode::Internal,
                         action + ": " + std::strerror(errno));
}

/**
 * @brief Compares one stat observation to a captured device/inode identity.
 * @param state Current filesystem observation.
 * @param identity Previously captured generation.
 * @return True only for exact device and inode equality.
 * @throws Nothing.
 */
bool same_identity(const struct stat& state,
                   SocketNodeIdentity identity) noexcept {
  return state.st_dev == identity.device && state.st_ino == identity.inode;
}

/**
 * @brief Confirms the parent pathname still resolves to the fixed directory.
 * @param parent_path Parent path spelling used by bind.
 * @param identity Fixed parent descriptor's captured generation.
 * @return True only for a current directory with matching device/inode.
 * @throws Nothing.
 * @note `stat` intentionally follows platform aliases such as Darwin `/tmp`;
 * replacement with another target fails the identity comparison.
 */
bool parent_path_matches(const std::string& parent_path,
                         SocketNodeIdentity identity) noexcept {
  struct stat current{};
  return ::stat(parent_path.c_str(), &current) == 0 &&
         S_ISDIR(current.st_mode) && same_identity(current, identity);
}

/**
 * @brief Configures one connected-stream descriptor against process SIGPIPE.
 * @param descriptor Newly created client or newly accepted stream descriptor.
 * @return Success, or typed transport failure from Darwin `setsockopt`.
 * @throws std::bad_alloc If failure diagnostic allocation fails.
 * @note Darwin uses descriptor-level `SO_NOSIGPIPE`; Linux retains per-send
 * `MSG_NOSIGNAL` in the frame writer. No process signal disposition changes.
 */
Status configure_no_sigpipe(int descriptor) {
#if defined(__APPLE__)
  const int enabled = 1;
  if (::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                   sizeof(enabled)) != 0) {
    return errno_status("could not suppress local socket SIGPIPE");
  }
#else
  static_cast<void>(descriptor);
#endif
  return Status::success();
}

/**
 * @brief Applies SIGPIPE configuration while owning rollback closure.
 * @param descriptor Exact descriptor ownership to configure.
 * @return Prepared ownership or typed transport failure.
 * @throws std::bad_alloc If failure diagnostic allocation fails.
 * @note Any failure destroys the local owner before returning to the caller.
 */
Result<UniqueDescriptor> prepare_stream(UniqueDescriptor descriptor) {
  Status status = configure_no_sigpipe(descriptor.get());
  return status.ok() ? Result<UniqueDescriptor>(std::move(descriptor))
                     : Result<UniqueDescriptor>(std::move(status));
}

/**
 * @brief Verifies a connected peer belongs to the current effective uid.
 * @param descriptor Connected local stream descriptor.
 * @return Success only after a supported platform peer-uid check passes.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note Unsupported platforms fail closed instead of accepting an unchecked
 * peer.
 */
Status verify_same_user(int descriptor) {
#if defined(__APPLE__) || defined(__FreeBSD__)
  uid_t uid = 0;
  gid_t gid = 0;
  if (::getpeereid(descriptor, &uid, &gid) != 0) {
    return errno_status("could not read local peer identity");
  }
  static_cast<void>(gid);
  if (uid != ::geteuid()) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "local peer uid does not match daemon uid");
  }
#elif defined(__linux__)
  struct ucred credentials{};
  socklen_t size = sizeof(credentials);
  if (::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &size) !=
      0) {
    return errno_status("could not read local peer identity");
  }
  if (credentials.uid != ::geteuid()) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "local peer uid does not match daemon uid");
  }
#else
  static_cast<void>(descriptor);
  return Status::failure(ErrorCode::InvalidArgument,
                         "local peer uid verification is unavailable");
#endif
  return Status::success();
}

}  // namespace

/**
 * @brief Implements exact descriptor teardown.
 * @copydetails UniqueDescriptor::~UniqueDescriptor
 */
UniqueDescriptor::~UniqueDescriptor() noexcept {
  reset();
}

/**
 * @brief Implements transfer-only descriptor move construction.
 * @copydetails UniqueDescriptor::UniqueDescriptor(UniqueDescriptor&&)
 */
UniqueDescriptor::UniqueDescriptor(UniqueDescriptor&& other) noexcept
    : descriptor_(other.release()) {}

/**
 * @brief Implements transfer-only descriptor move assignment.
 * @copydetails UniqueDescriptor::operator=(UniqueDescriptor&&)
 */
UniqueDescriptor& UniqueDescriptor::operator=(
    UniqueDescriptor&& other) noexcept {
  if (this != &other) {
    reset(other.release());
  }
  return *this;
}

/**
 * @brief Implements close-then-replace descriptor ownership.
 * @copydetails UniqueDescriptor::reset
 */
void UniqueDescriptor::reset(int descriptor) noexcept {
  if (descriptor_ >= 0) {
    ::close(descriptor_);
  }
  descriptor_ = descriptor;
}

/**
 * @brief Implements ownership release without closing.
 * @copydetails UniqueDescriptor::release
 */
int UniqueDescriptor::release() noexcept {
  return std::exchange(descriptor_, -1);
}

/**
 * @brief Implements verified socket-generation cleanup construction.
 * @copydetails SocketNodeGuard::SocketNodeGuard(UniqueDescriptor, std::string,
 * std::string, SocketNodeIdentity, SocketNodeIdentity)
 */
SocketNodeGuard::SocketNodeGuard(UniqueDescriptor parent_directory,
                                 std::string parent_path, std::string leaf_name,
                                 SocketNodeIdentity parent_identity,
                                 SocketNodeIdentity node_identity) noexcept
    : parent_directory_(std::move(parent_directory)),
      parent_path_(std::move(parent_path)),
      leaf_name_(std::move(leaf_name)),
      parent_identity_(parent_identity),
      node_identity_(node_identity),
      active_(parent_directory_.valid() && !parent_path_.empty() &&
              !leaf_name_.empty()) {}

/**
 * @brief Implements one conditional cleanup attempt at guard destruction.
 * @copydetails SocketNodeGuard::~SocketNodeGuard
 */
SocketNodeGuard::~SocketNodeGuard() noexcept {
  remove();
}

/**
 * @brief Implements transfer-only socket-node cleanup construction.
 * @copydetails SocketNodeGuard::SocketNodeGuard(SocketNodeGuard&&)
 */
SocketNodeGuard::SocketNodeGuard(SocketNodeGuard&& other) noexcept {
  *this = std::move(other);
}

/**
 * @brief Implements cleanup-then-transfer move assignment.
 * @copydetails SocketNodeGuard::operator=(SocketNodeGuard&&)
 */
SocketNodeGuard& SocketNodeGuard::operator=(SocketNodeGuard&& other) noexcept {
  if (this != &other) {
    remove();
    parent_directory_ = std::move(other.parent_directory_);
    parent_path_ = std::move(other.parent_path_);
    leaf_name_ = std::move(other.leaf_name_);
    parent_identity_ = other.parent_identity_;
    node_identity_ = other.node_identity_;
    active_ = std::exchange(other.active_, false);
  }
  return *this;
}

/**
 * @brief Implements fail-closed fixed-dirfd generation cleanup.
 * @copydetails SocketNodeGuard::remove
 */
void SocketNodeGuard::remove() noexcept {
  if (!std::exchange(active_, false) || !parent_directory_.valid()) {
    return;
  }
  struct stat fixed_parent{};
  if (::fstat(parent_directory_.get(), &fixed_parent) != 0 ||
      !S_ISDIR(fixed_parent.st_mode) ||
      !same_identity(fixed_parent, parent_identity_) ||
      !parent_path_matches(parent_path_, parent_identity_)) {
    return;
  }
  struct stat current{};
  if (::fstatat(parent_directory_.get(), leaf_name_.c_str(), &current,
                AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISSOCK(current.st_mode) || !same_identity(current, node_identity_)) {
    return;
  }
  static_cast<void>(::unlinkat(parent_directory_.get(), leaf_name_.c_str(), 0));
}

/**
 * @brief Implements one explicit Unix-domain client connection.
 * @copydetails connect_unix_socket
 */
Result<UniqueDescriptor> connect_unix_socket(const std::string& path) {
  auto address = socket_address(path);
  if (!address.ok()) {
    return Result<UniqueDescriptor>(address.status());
  }
  UniqueDescriptor descriptor(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (!descriptor.valid()) {
    return Result<UniqueDescriptor>(
        errno_status("could not create local socket"));
  }
  auto prepared = prepare_stream(std::move(descriptor));
  if (!prepared.ok()) {
    return Result<UniqueDescriptor>(prepared.status());
  }
  descriptor = prepared.take_value();
  if (::connect(descriptor.get(),
                reinterpret_cast<const sockaddr*>(&address.value()),
                sizeof(sockaddr_un)) != 0) {
    return Result<UniqueDescriptor>(
        errno_status("could not connect local socket"));
  }
  return Result<UniqueDescriptor>(std::move(descriptor));
}

/**
 * @brief Implements creation of a restricted same-user listener.
 * @copydetails create_unix_listener
 */
Result<BoundUnixListener> create_unix_listener(const std::string& path,
                                               int backlog) {
  auto address = socket_address(path);
  if (!address.ok() || backlog <= 0) {
    return Result<BoundUnixListener>(
        address.ok()
            ? Status::failure(ErrorCode::InvalidArgument,
                              "Unix-domain listener backlog must be positive")
            : address.status());
  }
  auto parts = split_socket_path(path);
  if (!parts.ok()) {
    return Result<BoundUnixListener>(parts.status());
  }

  UniqueDescriptor parent_directory(::open(parts.value().parent_path.c_str(),
                                           O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!parent_directory.valid()) {
    return Result<BoundUnixListener>(
        errno_status("could not open socket parent directory"));
  }
  struct stat parent_state{};
  if (::fstat(parent_directory.get(), &parent_state) != 0) {
    return Result<BoundUnixListener>(
        errno_status("could not inspect socket parent directory"));
  }
  if (!S_ISDIR(parent_state.st_mode)) {
    return Result<BoundUnixListener>(Status::failure(
        ErrorCode::InvalidArgument, "socket parent is not a directory"));
  }
  const SocketNodeIdentity parent_identity{parent_state.st_dev,
                                           parent_state.st_ino};
  if (!parent_path_matches(parts.value().parent_path, parent_identity)) {
    return Result<BoundUnixListener>(
        Status::failure(ErrorCode::InvalidArgument,
                        "socket parent directory identity is not stable"));
  }

  struct stat existing{};
  if (::fstatat(parent_directory.get(), parts.value().leaf_name.c_str(),
                &existing, AT_SYMLINK_NOFOLLOW) == 0) {
    return Result<BoundUnixListener>(Status::failure(
        ErrorCode::InvalidArgument, "listener path already exists"));
  } else if (errno != ENOENT) {
    return Result<BoundUnixListener>(
        errno_status("could not inspect socket path"));
  }

  UniqueDescriptor descriptor(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (!descriptor.valid()) {
    return Result<BoundUnixListener>(errno_status("could not create listener"));
  }
  if (::bind(descriptor.get(),
             reinterpret_cast<const sockaddr*>(&address.value()),
             sizeof(sockaddr_un)) != 0) {
    return Result<BoundUnixListener>(
        errno_status("could not bind local socket"));
  }

  struct stat node_state{};
  if (::fstatat(parent_directory.get(), parts.value().leaf_name.c_str(),
                &node_state, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISSOCK(node_state.st_mode) ||
      !parent_path_matches(parts.value().parent_path, parent_identity)) {
    return Result<BoundUnixListener>(Status::failure(
        ErrorCode::Internal,
        "could not prove the bound socket pathname generation"));
  }
  const int parent_descriptor = parent_directory.get();
  SocketNodeGuard socket_node(
      std::move(parent_directory), parts.value().parent_path,
      parts.value().leaf_name, parent_identity,
      SocketNodeIdentity{node_state.st_dev, node_state.st_ino});

  if (::fchmodat(parent_descriptor, parts.value().leaf_name.c_str(),
                 S_IRUSR | S_IWUSR, 0) != 0) {
    return Result<BoundUnixListener>(
        errno_status("could not restrict socket mode"));
  }
  struct stat restricted_state{};
  if (::fstatat(parent_descriptor, parts.value().leaf_name.c_str(),
                &restricted_state, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISSOCK(restricted_state.st_mode) ||
      !same_identity(restricted_state, SocketNodeIdentity{node_state.st_dev,
                                                          node_state.st_ino}) ||
      (restricted_state.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) !=
          (S_IRUSR | S_IWUSR)) {
    return Result<BoundUnixListener>(Status::failure(
        ErrorCode::Internal, "socket mode or generation changed during bind"));
  }
  if (::listen(descriptor.get(), backlog) != 0) {
    return Result<BoundUnixListener>(errno_status("could not listen locally"));
  }
  return Result<BoundUnixListener>(
      BoundUnixListener{std::move(descriptor), std::move(socket_node)});
}

/**
 * @brief Implements same-user connection acceptance.
 * @copydetails accept_same_user
 */
Result<UniqueDescriptor> accept_same_user(int listener) {
  int descriptor = -1;
  do {
    descriptor = ::accept(listener, nullptr, nullptr);
  } while (descriptor < 0 && errno == EINTR);
  if (descriptor < 0) {
    return Result<UniqueDescriptor>(errno_status("local accept failed"));
  }
  UniqueDescriptor owned(descriptor);
  auto prepared = prepare_stream(std::move(owned));
  if (!prepared.ok()) {
    return Result<UniqueDescriptor>(prepared.status());
  }
  owned = prepared.take_value();
  Status status = verify_same_user(descriptor);
  if (!status.ok()) {
    return Result<UniqueDescriptor>(std::move(status));
  }
  return Result<UniqueDescriptor>(std::move(owned));
}

/**
 * @brief Implements best-effort stream interruption.
 * @copydetails shutdown_descriptor
 */
void shutdown_descriptor(int descriptor) noexcept {
  if (descriptor >= 0) {
    ::shutdown(descriptor, SHUT_RDWR);
  }
}

#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
/**
 * @brief Implements noninstalled SIGPIPE preparation failure verification.
 * @copydetails prepare_stream_for_test
 */
Result<UniqueDescriptor> prepare_stream_for_test(int descriptor) {
  return prepare_stream(UniqueDescriptor(descriptor));
}
#endif

}  // namespace ps::ipc::internal
