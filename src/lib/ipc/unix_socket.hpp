#pragma once

#include <sys/types.h>

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

  /**
   * @brief Forbids duplicating exact descriptor ownership.
   * @param other Source owner that cannot be copied.
   * @throws Nothing; the operation is deleted.
   * @note One descriptor has at most one closing owner.
   */
  UniqueDescriptor(const UniqueDescriptor& other) = delete;
  /**
   * @brief Forbids copy assignment of exact descriptor ownership.
   * @param other Source owner that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Use move assignment for explicit transfer.
   */
  UniqueDescriptor& operator=(const UniqueDescriptor& other) = delete;
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

/** @brief Exact filesystem device/inode generation of one directory entry. */
struct SocketNodeIdentity final {
  /** @brief Filesystem device number. */
  dev_t device = 0;
  /** @brief Filesystem inode number. */
  ino_t inode = 0;
};

/**
 * @brief Move-only conditional cleanup ownership for one bound socket node.
 *
 * @note Cleanup uses a fixed parent-directory descriptor and removes only the
 * currently observed socket entry whose device/inode still match this guard.
 */
class SocketNodeGuard final {
 public:
  /**
   * @brief Constructs inactive cleanup ownership.
   * @throws Nothing.
   * @note `remove()` is a no-op until a bound generation is supplied.
   */
  SocketNodeGuard() noexcept = default;

  /**
   * @brief Takes one verified bound-generation cleanup capability.
   * @param parent_directory Fixed descriptor for the bound entry's parent.
   * @param parent_path Path used to revalidate the parent directory identity.
   * @param leaf_name Single socket filename relative to `parent_directory`.
   * @param parent_identity Device/inode of the fixed parent directory.
   * @param node_identity Device/inode captured after successful bind.
   * @throws Nothing after caller-owned string argument construction.
   * @note All ownership transfers to this guard. The guard is active only when
   * both identities and the descriptor are nonempty.
   */
  SocketNodeGuard(UniqueDescriptor parent_directory, std::string parent_path,
                  std::string leaf_name, SocketNodeIdentity parent_identity,
                  SocketNodeIdentity node_identity) noexcept;

  /**
   * @brief Conditionally removes the still-matching socket node.
   * @throws Nothing.
   * @note Inconclusive parent/node state and every mismatch preserve the
   * current pathname. The attempt occurs at most once.
   */
  ~SocketNodeGuard() noexcept;

  /**
   * @brief Forbids duplicating one socket-generation cleanup capability.
   * @param other Source guard that cannot be copied.
   * @throws Nothing; the operation is deleted.
   */
  SocketNodeGuard(const SocketNodeGuard& other) = delete;
  /**
   * @brief Forbids assigning duplicate cleanup ownership.
   * @param other Source guard that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  SocketNodeGuard& operator=(const SocketNodeGuard& other) = delete;
  /**
   * @brief Transfers one conditional cleanup capability.
   * @param other Source left inactive and without its parent descriptor.
   * @throws Nothing.
   */
  SocketNodeGuard(SocketNodeGuard&& other) noexcept;
  /**
   * @brief Replaces this capability after conditionally cleaning its node.
   * @param other Source left inactive and without its parent descriptor.
   * @return This guard.
   * @throws Nothing.
   */
  SocketNodeGuard& operator=(SocketNodeGuard&& other) noexcept;

  /**
   * @brief Attempts identity-matched socket-node removal exactly once.
   * @throws Nothing.
   * @note Portable POSIX keeps final `fstatat` and `unlinkat` as separate
   * operations. This method never intentionally unlinks an observed mismatch
   * and makes no atomic compare-and-unlink claim against a hostile same-uid
   * writer racing the final component.
   */
  void remove() noexcept;

 private:
  /** @brief Fixed parent-directory descriptor capability. */
  UniqueDescriptor parent_directory_;
  /** @brief Parent pathname used only for identity revalidation. */
  std::string parent_path_;
  /** @brief Single nonempty entry name relative to the fixed parent. */
  std::string leaf_name_;
  /** @brief Captured fixed parent device/inode. */
  SocketNodeIdentity parent_identity_;
  /** @brief Captured bound socket device/inode generation. */
  SocketNodeIdentity node_identity_;
  /** @brief True until the one cleanup attempt is consumed. */
  bool active_ = false;
};

/**
 * @brief Complete ownership returned after one listener bind succeeds.
 *
 * @note Descriptor and pathname-generation cleanup move together; destroying
 * this value closes the listener and conditionally removes only its node.
 */
struct BoundUnixListener final {
  /** @brief Bound listening descriptor ownership. */
  UniqueDescriptor descriptor;
  /** @brief Exact bound pathname-generation cleanup ownership. */
  SocketNodeGuard socket_node;
};

/**
 * @brief Connects one local stream to an explicit Unix-domain socket path.
 * @param path Exact local filesystem socket path.
 * @return Connected owned descriptor or typed argument/transport failure.
 * @throws std::bad_alloc If path or diagnostic allocation fails.
 * @note Darwin configures descriptor-level `SO_NOSIGPIPE` before connect. A
 * configuration failure closes the descriptor and publishes no connection.
 * No discovery, retry, remote endpoint, or daemon start is performed.
 */
[[nodiscard]] Result<UniqueDescriptor> connect_unix_socket(
    const std::string& path);

/**
 * @brief Creates a same-user listener with filesystem mode 0600.
 * @param path Exact local socket path.
 * @param backlog Positive pending-connection bound.
 * @return Bound descriptor plus exact socket-node generation guard, or typed
 * failure.
 * @throws std::bad_alloc If path or diagnostic allocation fails.
 * @note Every existing filesystem entry, including a stale/live socket, is
 * rejected without removal. When absent, concurrent binds are arbitrated by
 * the operating system; no stale-node recovery is attempted.
 */
[[nodiscard]] Result<BoundUnixListener> create_unix_listener(
    const std::string& path, int backlog);

/**
 * @brief Accepts one connection and verifies the peer uid equals this process.
 * @param listener Valid listening descriptor.
 * @return Connected descriptor or typed accept/peer failure.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note Darwin configures descriptor-level `SO_NOSIGPIPE` immediately after
 * accept. A configuration failure and a rejected peer are closed before
 * return.
 */
[[nodiscard]] Result<UniqueDescriptor> accept_same_user(int listener);

/**
 * @brief Requests read/write interruption on a descriptor idempotently.
 * @param descriptor Descriptor to interrupt; negative values are ignored.
 * @throws Nothing.
 * @note This does not transfer or close descriptor ownership.
 */
void shutdown_descriptor(int descriptor) noexcept;

#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
/**
 * @brief Applies production connected-stream SIGPIPE preparation in tests.
 * @param descriptor Descriptor whose ownership transfers to this function.
 * @return Prepared owned descriptor or typed platform failure.
 * @throws std::bad_alloc If failure diagnostic allocation fails.
 * @note Failure closes the supplied descriptor. This seam is compiled only
 * into the noninstalled test runtime so Darwin can verify `setsockopt`
 * failure ownership without changing production syscalls.
 */
[[nodiscard]] Result<UniqueDescriptor> prepare_stream_for_test(int descriptor);
#endif

}  // namespace ps::ipc::internal
