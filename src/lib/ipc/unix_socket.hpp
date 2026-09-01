#pragma once

#include <string>

#include "photospider/core/status.hpp"

namespace ps::ipc::internal {

/**
 * @brief Move-only exact ownership of one POSIX descriptor.
 *
 * @note Reset/destruction closes at most once.
 */
class UniqueDescriptor final {
 public:
  /**
   * @brief Constructs empty descriptor ownership.
   * @throws Nothing.
   * @note `valid()` is false until ownership is assigned.
   */
  UniqueDescriptor() noexcept = default;
  /**
   * @brief Takes exact ownership of one descriptor.
   * @param descriptor Descriptor to close later, or a negative empty value.
   * @throws Nothing.
   * @note The caller must not close a nonnegative descriptor after transfer.
   */
  explicit UniqueDescriptor(int descriptor) noexcept
      : descriptor_(descriptor) {}
  /**
   * @brief Closes the owned descriptor at most once.
   * @throws Nothing.
   * @note Close errors are intentionally ignored during teardown.
   */
  ~UniqueDescriptor() noexcept;

  UniqueDescriptor(const UniqueDescriptor&) = delete;
  UniqueDescriptor& operator=(const UniqueDescriptor&) = delete;
  /**
   * @brief Transfers descriptor ownership from another object.
   * @param other Source left without descriptor ownership.
   * @throws Nothing.
   * @note No descriptor is duplicated.
   */
  UniqueDescriptor(UniqueDescriptor&& other) noexcept;
  /**
   * @brief Replaces ownership after closing the current descriptor.
   * @param other Source left without descriptor ownership.
   * @return This ownership object.
   * @throws Nothing.
   * @note Self-assignment preserves the current descriptor.
   */
  UniqueDescriptor& operator=(UniqueDescriptor&& other) noexcept;

  /**
   * @brief Returns the owned descriptor without transferring it.
   * @return Nonnegative descriptor, or -1 when empty.
   * @throws Nothing.
   * @note The borrowed descriptor remains owned by this object.
   */
  [[nodiscard]] int get() const noexcept { return descriptor_; }
  /**
   * @brief Reports whether a descriptor is owned.
   * @return True exactly when `get()` is nonnegative.
   * @throws Nothing.
   * @note This does not probe operating-system liveness.
   */
  [[nodiscard]] bool valid() const noexcept { return descriptor_ >= 0; }
  /**
   * @brief Closes current ownership and takes a replacement.
   * @param descriptor Replacement descriptor, or -1 for empty ownership.
   * @throws Nothing.
   * @note Close errors are intentionally ignored.
   */
  void reset(int descriptor = -1) noexcept;
  /**
   * @brief Releases ownership without closing the descriptor.
   * @return Former descriptor, or -1 when already empty.
   * @throws Nothing.
   * @note The caller becomes responsible for closing a nonnegative result.
   */
  [[nodiscard]] int release() noexcept;

 private:
  /** @brief Owned POSIX descriptor or -1. */
  int descriptor_ = -1;
};

/**
 * @brief Connects one local stream to an explicit Unix-domain socket path.
 * @param path Exact local filesystem socket path.
 * @return Connected owned descriptor or typed argument/transport failure.
 * @throws std::bad_alloc If path or diagnostic allocation fails.
 * @note No discovery, retry, remote endpoint, or daemon start is performed.
 */
[[nodiscard]] Result<UniqueDescriptor> connect_unix_socket(
    const std::string& path);

/**
 * @brief Creates a same-user listener with filesystem mode 0600.
 * @param path Exact local socket path.
 * @param backlog Positive pending-connection bound.
 * @return Bound listening descriptor or typed failure.
 * @throws std::bad_alloc If path or diagnostic allocation fails.
 * @note An existing socket node is replaced; any other filesystem object is
 * rejected without removal.
 */
[[nodiscard]] Result<UniqueDescriptor> create_unix_listener(
    const std::string& path, int backlog);

/**
 * @brief Accepts one connection and verifies the peer uid equals this process.
 * @param listener Valid listening descriptor.
 * @return Connected descriptor or typed accept/peer failure.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note A peer rejected by the same-user check is closed before return.
 */
[[nodiscard]] Result<UniqueDescriptor> accept_same_user(int listener);

/**
 * @brief Requests read/write interruption on a descriptor idempotently.
 * @param descriptor Descriptor to interrupt; negative values are ignored.
 * @throws Nothing.
 * @note This does not transfer or close descriptor ownership.
 */
void shutdown_descriptor(int descriptor) noexcept;

/**
 * @brief Removes an exact socket filesystem node when it is still a socket.
 * @param path Exact path previously bound by this process.
 * @throws Nothing.
 * @note Non-socket filesystem objects are never removed.
 */
void remove_socket_node(const std::string& path) noexcept;

}  // namespace ps::ipc::internal
