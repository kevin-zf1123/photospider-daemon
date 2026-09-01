#include "ipc/unix_socket.hpp"

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
Result<UniqueDescriptor> create_unix_listener(const std::string& path,
                                              int backlog) {
  auto address = socket_address(path);
  if (!address.ok() || backlog <= 0) {
    return Result<UniqueDescriptor>(
        address.ok()
            ? Status::failure(ErrorCode::InvalidArgument,
                              "Unix-domain listener backlog must be positive")
            : address.status());
  }
  struct stat existing{};
  if (::lstat(path.c_str(), &existing) == 0) {
    if (!S_ISSOCK(existing.st_mode)) {
      return Result<UniqueDescriptor>(
          Status::failure(ErrorCode::InvalidArgument,
                          "listener path exists and is not a socket"));
    }
    if (::unlink(path.c_str()) != 0) {
      return Result<UniqueDescriptor>(errno_status("could not replace socket"));
    }
  } else if (errno != ENOENT) {
    return Result<UniqueDescriptor>(
        errno_status("could not inspect socket path"));
  }

  UniqueDescriptor descriptor(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (!descriptor.valid()) {
    return Result<UniqueDescriptor>(errno_status("could not create listener"));
  }
  if (::bind(descriptor.get(),
             reinterpret_cast<const sockaddr*>(&address.value()),
             sizeof(sockaddr_un)) != 0) {
    return Result<UniqueDescriptor>(
        errno_status("could not bind local socket"));
  }
  if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
    remove_socket_node(path);
    return Result<UniqueDescriptor>(
        errno_status("could not restrict socket mode"));
  }
  if (::listen(descriptor.get(), backlog) != 0) {
    remove_socket_node(path);
    return Result<UniqueDescriptor>(errno_status("could not listen locally"));
  }
  return Result<UniqueDescriptor>(std::move(descriptor));
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

/**
 * @brief Implements type-checked socket-node removal.
 * @copydetails remove_socket_node
 */
void remove_socket_node(const std::string& path) noexcept {
  struct stat existing{};
  if (!path.empty() && ::lstat(path.c_str(), &existing) == 0 &&
      S_ISSOCK(existing.st_mode)) {
    ::unlink(path.c_str());
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
