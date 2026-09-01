#pragma once

#include <memory>
#include <string>

#include "photospider/ipc/protocol.hpp"

namespace ps::ipc {

/**
 * @brief Sequential client for the exact local IPC version-three surface.
 *
 * @note Independent Client objects may be used concurrently. One Client
 * permits at most one outstanding call and performs no automatic retry.
 */
class Client final {
 public:
  /**
   * @brief Creates a disconnected client with request sequence one.
   * @throws std::bad_alloc If private state allocation fails.
   */
  Client();

  /**
   * @brief Closes the owned local connection exactly once.
   * @throws Nothing.
   * @note Destruction does not close a Session or shut down the daemon.
   */
  ~Client() noexcept;

  /**
   * @brief Forbids duplicating one sequential connection/request sequence.
   * @param other Source client that cannot be copied.
   * @throws Nothing; the operation is deleted.
   * @note Use a distinct Client for another concurrent connection.
   */
  Client(const Client& other) = delete;
  /**
   * @brief Forbids copy assignment of descriptor/request ownership.
   * @param other Source client that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Move assignment is the only ownership-transfer operation.
   */
  Client& operator=(const Client& other) = delete;

  /**
   * @brief Transfers local connection ownership and request sequence.
   * @param other Source left in a disconnected valid state.
   * @throws Nothing.
   * @note No request is sent and no connection is duplicated.
   */
  Client(Client&& other) noexcept;
  /**
   * @brief Replaces this connection with transferred ownership.
   * @param other Source left in a disconnected valid state.
   * @return This client.
   * @throws Nothing.
   * @note Any connection formerly owned by this client is closed.
   */
  Client& operator=(Client&& other) noexcept;

  /**
   * @brief Connects to one explicit Unix-domain socket path.
   * @param socket_path Local filesystem socket path.
   * @return Success or typed transport/argument failure.
   * @throws std::bad_alloc If path or diagnostic allocation fails.
   * @note No discovery, daemon start, remote endpoint, or retry is performed.
   */
  [[nodiscard]] Status connect(const std::string& socket_path);

  /**
   * @brief Idempotently closes only this transport connection.
   * @throws Nothing.
   * @note Daemon Session and Job state is not changed.
   */
  void disconnect() noexcept;

  /**
   * @brief Reports whether this object currently owns a descriptor.
   * @return True when a local stream descriptor is owned.
   * @throws Nothing.
   * @note This does not probe peer liveness.
   */
  [[nodiscard]] bool connected() const noexcept;

  /**
   * @brief Calls `session.create` with a complete public WorkflowDocument.
   * @param document Format-neutral compiler source copied onto the wire.
   * @return New ephemeral logical namespace identifier.
   * @throws std::bad_alloc If encoding/result allocation fails.
   * @note The kernel sees only the public WorkflowDocument.
   */
  [[nodiscard]] Result<SessionId> session_create(
      const WorkflowDocument& document);

  /**
   * @brief Calls `session.close` and invalidates its daemon-owned state.
   * @param session_id Existing namespace identifier.
   * @return Success or typed missing/transport failure.
   * @throws std::bad_alloc If encoding/result allocation fails.
   * @note The daemon cancels unfinished executions and releases all results.
   */
  [[nodiscard]] Status session_close(SessionId session_id);

  /**
   * @brief Calls `job.submit` for one existing logical namespace.
   * @param session_id Existing namespace identifier.
   * @param options Public local planning/execution controls.
   * @return New execution identifier or bounded-backpressure failure.
   * @throws std::bad_alloc If encoding/result allocation fails.
   * @note A transport ambiguity is never retried automatically.
   */
  [[nodiscard]] Result<JobId> job_submit(SessionId session_id,
                                         const JobSubmitOptions& options = {});

  /**
   * @brief Calls `job.status` without consuming retained state.
   * @param job_id Existing execution identifier.
   * @return Current forward-only lifecycle snapshot or typed failure.
   * @throws std::bad_alloc If encoding/result allocation fails.
   * @note Observation does not release a terminal record.
   */
  [[nodiscard]] Result<JobStatus> job_status(JobId job_id);

  /**
   * @brief Calls `job.cancel` using kernel cooperative best-effort
   * cancellation.
   * @param job_id Existing execution identifier.
   * @return Success when the request was accepted or already terminal.
   * @throws std::bad_alloc If encoding/result allocation fails.
   * @note Cancellation is cooperative and best effort.
   */
  [[nodiscard]] Status job_cancel(JobId job_id);

  /**
   * @brief Calls `job.result` for one succeeded execution.
   * @param job_id Existing execution identifier.
   * @return Copied in-memory named Values and raw diagnostics.
   * @throws std::bad_alloc If encoding/result allocation fails.
   * @note Result observation is non-destructive until `job.release`.
   */
  [[nodiscard]] Result<ExecutionResult> job_result(JobId job_id);

  /**
   * @brief Calls `job.release` for one terminal execution.
   * @param job_id Existing terminal identifier.
   * @return Success after its retained result and status are erased.
   * @throws std::bad_alloc If encoding/result allocation fails.
   * @note A running execution cannot be released.
   */
  [[nodiscard]] Status job_release(JobId job_id);

  /**
   * @brief Calls `daemon.info` for exact methods and local live counts.
   * @return Capability snapshot or typed transport/protocol failure.
   * @throws std::bad_alloc If encoding/result allocation fails.
   * @note Counts may change immediately after return.
   */
  [[nodiscard]] Result<DaemonInfo> daemon_info();

  /**
   * @brief Calls `daemon.shutdown` and requests graceful local process exit.
   * @return Success after the daemon accepts shutdown.
   * @throws std::bad_alloc If encoding/result allocation fails.
   * @note The current connection becomes unusable after the response.
   */
  [[nodiscard]] Status daemon_shutdown();

 private:
  /** @brief Opaque descriptor, request sequence, and call serialization. */
  struct Impl;
  /** @brief Unique client transport ownership. */
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps::ipc
