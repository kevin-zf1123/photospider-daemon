#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>

#include "ipc/frame.hpp"
#include "ipc/unix_socket.hpp"
#include "support/test_support.hpp"

namespace {

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
 * @brief Exercises client and accepted writes after their peer closes.
 * @return True when both real-socket descriptors survive with typed failure.
 * @throws std::bad_alloc If transport/status storage allocation fails.
 * @note Both descriptors are created by production transport functions, so
 * platform-specific SIGPIPE suppression is exercised at its lifecycle seam.
 */
bool peer_close_write_regression() {
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
  auto accepted_result = accept_same_user(listener.descriptor.get());
  if (!client_result.ok() || !accepted_result.ok()) {
    return false;
  }
  UniqueDescriptor client = client_result.take_value();
  UniqueDescriptor accepted = accepted_result.take_value();
  accepted.reset();
  const bool client_failed = write_fails_after_peer_close(client.get());

  client_result = connect_unix_socket(path);
  accepted_result = accept_same_user(listener.descriptor.get());
  if (!client_result.ok() || !accepted_result.ok()) {
    return false;
  }
  client = client_result.take_value();
  accepted = accepted_result.take_value();
  client.reset();
  const bool accepted_failed = write_fails_after_peer_close(accepted.get());
  listener.descriptor.reset();
  listener.socket_node.remove();
  return client_failed && accepted_failed;
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
 * @return Zero when peer-close writes fail safely and setup rollback is exact.
 * @throws std::bad_alloc If fixture or transport diagnostic allocation fails.
 * @note Behavioral failures return nonzero through `PS_IPC_CHECK`.
 */
int main() {
  PS_IPC_CHECK(peer_close_write_regression());
  PS_IPC_CHECK(sigpipe_configuration_failure_regression());
  return 0;
}
