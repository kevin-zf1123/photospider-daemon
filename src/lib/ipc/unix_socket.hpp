#pragma once

#include <sys/types.h>

#include <cstdint>
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
 * @brief Move-only state machine for one conditional socket-node cleanup.
 *
 * @note The lifecycle is `Empty -> Prepared -> Armed -> Consumed`. `Prepared`
 * owns every allocation-backed path component before bind but has no socket
 * generation and never unlinks. `Armed` follows a successful generation
 * capture and removes only an exact device/inode match through the fixed parent
 * descriptor. Moves transfer the current state and leave the source `Empty`.
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
   * @brief Prepares allocation-backed cleanup state before listener bind.
   * @param parent_directory Fixed descriptor for the bound entry's parent.
   * @param parent_path Path used to revalidate the parent directory identity.
   * @param leaf_name Single socket filename relative to `parent_directory`.
   * @param parent_identity Device/inode of the fixed parent directory.
   * @throws Nothing.
   * @note The caller must transfer already-allocated strings with rvalue
   * references. Valid ownership enters `Prepared`, which preserves every path
   * until `arm()` receives a successfully captured socket generation.
   */
  SocketNodeGuard(UniqueDescriptor parent_directory, std::string&& parent_path,
                  std::string&& leaf_name,
                  SocketNodeIdentity parent_identity) noexcept;

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
   * @brief Returns the fixed parent descriptor without transferring it.
   * @return Nonnegative owned descriptor while prepared or armed, else -1.
   * @throws Nothing.
   * @note The borrowed descriptor remains owned by this guard.
   */
  [[nodiscard]] int parent_descriptor() const noexcept {
    return parent_directory_.get();
  }

  /**
   * @brief Returns the prepared parent pathname.
   * @return Immutable borrowed allocation-backed parent path.
   * @throws Nothing.
   * @note The reference remains valid until this guard is moved or destroyed.
   */
  [[nodiscard]] const std::string& parent_path() const noexcept {
    return parent_path_;
  }

  /**
   * @brief Returns the prepared socket leaf name.
   * @return Immutable borrowed allocation-backed leaf component.
   * @throws Nothing.
   * @note The reference remains valid until this guard is moved or destroyed.
   */
  [[nodiscard]] const std::string& leaf_name() const noexcept {
    return leaf_name_;
  }

  /**
   * @brief Arms cleanup with one successfully captured socket generation.
   * @param node_identity Device/inode observed after successful bind.
   * @throws Nothing.
   * @note Only `Prepared` transitions to `Armed`; every other state is
   * unchanged. No allocation or pathname operation occurs.
   */
  void arm(SocketNodeIdentity node_identity) noexcept;

  /**
   * @brief Abandons an unverified bind without unlinking its pathname.
   * @throws Nothing.
   * @note Only `Prepared` transitions to `Consumed`. This fail-closed rollback
   * closes descriptor ownership later but preserves a path whose generation
   * could not be captured, so a replacement is never guessed to be ours.
   */
  void abandon_unverified_bind() noexcept;

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
  /** @brief Exact allocation/identity/cleanup state of this guard. */
  enum class State : std::uint8_t {
    /** @brief No parent or pathname ownership is actionable. */
    Empty = 0U,
    /** @brief Pre-bind path state exists but cannot authorize unlink. */
    Prepared = 1U,
    /** @brief One captured socket generation authorizes checked cleanup. */
    Armed = 2U,
    /** @brief Cleanup or fail-closed abandonment has been consumed. */
    Consumed = 3U,
  };

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
  /** @brief Current state in the documented one-way cleanup lifecycle. */
  State state_ = State::Empty;
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

/** @brief Internal outcome class for one same-user accept attempt. */
enum class AcceptDisposition : std::uint8_t {
  /** @brief A verified stream descriptor is published. */
  Accepted = 0U,
  /** @brief One connected peer failed the supported-platform uid match. */
  PeerRejected = 1U,
  /** @brief Accept, stream preparation, or credential syscall failed. */
  FatalFailure = 2U,
};

/**
 * @brief Typed same-user accept outcome without diagnostic-string branching.
 *
 * @note `Accepted` uniquely carries a valid descriptor and success status.
 * `PeerRejected` and `FatalFailure` carry no descriptor and one non-Ok status.
 */
struct SameUserAcceptResult final {
  /** @brief Stable internal control-flow disposition. */
  AcceptDisposition disposition = AcceptDisposition::FatalFailure;
  /** @brief Verified accepted stream only for `Accepted`. */
  UniqueDescriptor descriptor;
  /** @brief Success for `Accepted`, otherwise typed rejection/failure. */
  Status status;
};

/**
 * @brief Validates one pathname before any socket or filesystem syscall.
 * @param path Exact caller-supplied bytes.
 * @return Success or `InvalidArgument` for empty, over-bound, or embedded-NUL
 * path bytes.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note The check is shared by connect/listen and preserves `std::string`
 * length semantics instead of allowing POSIX `c_str()` prefix truncation.
 */
[[nodiscard]] Status validate_unix_socket_path(const std::string& path);

/**
 * @brief Connects one local stream to an explicit Unix-domain socket path.
 * @param path Exact local filesystem socket path.
 * @return Connected owned descriptor or typed argument/transport failure.
 * @throws std::bad_alloc If path or diagnostic allocation fails.
 * @note The returned descriptor is close-on-exec. Linux requests this
 * atomically at socket creation; platforms without that facility use checked
 * `fcntl`, whose create-to-flag interval is not atomic against a concurrent
 * fork. Darwin also configures descriptor-level `SO_NOSIGPIPE` before connect.
 * Any preparation failure closes the descriptor and publishes no connection.
 * No discovery, retry, remote endpoint, or daemon start is performed.
 */
[[nodiscard]] Result<UniqueDescriptor> connect_unix_socket(
    const std::string& path);

/**
 * @brief Creates one local listener with generation-checked pathname cleanup.
 * @param path Exact local socket path.
 * @param backlog Positive pending-connection bound.
 * @return Bound descriptor plus exact socket-node generation guard, or typed
 * failure.
 * @throws std::bad_alloc If pre-bind path or later diagnostic allocation fails.
 * @note Listener and fixed-parent descriptors are close-on-exec. Every
 * existing filesystem entry, including a stale/live socket, is rejected
 * without removal. When absent, concurrent binds are arbitrated by the
 * operating system; no stale-node recovery is attempted. Socket-node mode
 * follows the caller's directory and process umask and is not an
 * authentication boundary. Callers select a suitably private parent
 * directory; accepted peers are separately checked by `accept_same_user()`.
 */
[[nodiscard]] Result<BoundUnixListener> create_unix_listener(
    const std::string& path, int backlog);

/**
 * @brief Accepts one connection and verifies the peer uid equals this process.
 * @param listener Valid listening descriptor.
 * @return Explicit accepted, peer-rejected, or fatal-failure disposition.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note Accepted descriptors are close-on-exec. Linux requests the flag
 * atomically with `accept4`; unsupported platforms use checked `fcntl`, whose
 * accept-to-flag interval is not atomic against a concurrent fork. Darwin
 * configures descriptor-level `SO_NOSIGPIPE` immediately after accept. Every
 * preparation failure and rejected peer is closed before return.
 */
[[nodiscard]] SameUserAcceptResult accept_same_user(int listener);

/**
 * @brief Requests read/write interruption on a descriptor idempotently.
 * @param descriptor Descriptor to interrupt; negative values are ignored.
 * @throws Nothing.
 * @note This does not transfer or close descriptor ownership.
 */
void shutdown_descriptor(int descriptor) noexcept;

#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
/**
 * @brief Injects one peer-uid rejection after the next successful accept.
 * @param ambient_errno Errno value left behind on the accepting thread.
 * @throws Nothing.
 * @note This seam exists only in the noninstalled test runtime and is consumed
 * exactly once after stream preparation but before real peer verification.
 */
void reject_next_peer_for_test(int ambient_errno) noexcept;

/**
 * @brief Injects one fatal accept failure before the next accept syscall.
 * @param error_number Positive errno value represented by the typed failure.
 * @throws Nothing.
 * @note This seam exists only in the noninstalled test runtime and is consumed
 * exactly once without accepting or publishing a descriptor.
 */
void fail_next_accept_for_test(int error_number) noexcept;

/**
 * @brief Callback type for one noninstalled post-arm pathname replacement.
 * @param path Exact listener pathname whose original generation is armed.
 * @param context Opaque caller-owned test state.
 * @throws Any exception raised by the callback; armed cleanup remains active.
 * @note The callback type exists only in the noninstalled test runtime.
 */
using ArmedSocketNodeHookForTest = void (*)(const std::string& path,
                                            void* context);

/**
 * @brief Injects allocation failure at listener guard-state preparation.
 * @param path Exact local socket path.
 * @param backlog Positive pending-connection bound.
 * @return A normal listener result only when validation fails before the
 * injection point.
 * @throws std::bad_alloc At the exact guard preparation boundary.
 * @note This seam is compiled only into the noninstalled test runtime. It
 * models allocation failure immediately before the inactive socket-node guard
 * state is prepared and must never enter an installed product object.
 */
[[nodiscard]] Result<BoundUnixListener>
create_unix_listener_with_prearm_allocation_failure_for_test(
    const std::string& path, int backlog);

/**
 * @brief Runs one callback after socket-generation ownership is armed.
 * @param path Exact local socket path.
 * @param backlog Positive pending-connection bound.
 * @param hook Non-null callback invoked after generation capture and guard arm.
 * @param context Opaque caller-owned callback state.
 * @return Bound listener ownership or typed setup failure.
 * @throws std::bad_alloc If path or diagnostic allocation fails.
 * @throws Any exception raised by `hook`; armed cleanup remains active.
 * @note This noninstalled seam lets tests replace the pathname exactly before
 * subsequent listener setup, without changing production object code.
 */
[[nodiscard]] Result<BoundUnixListener>
create_unix_listener_with_armed_hook_for_test(const std::string& path,
                                              int backlog,
                                              ArmedSocketNodeHookForTest hook,
                                              void* context);

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
