#pragma once

#include <cstdint>

#include "photospider/ipc/protocol.hpp"

namespace ps {
struct Status;
}

namespace ps::ipc::test {

/** @brief Test-only observer after shutdown acceptance and before encoding. */
using ShutdownResponseReadyObserver = void (*)(bool accepted_shutdown);

/** @brief Test-only observer after one real shutdown response write attempt. */
using ShutdownResponseWriteObserver = void (*)(bool accepted_shutdown,
                                               const Status& status);

/** @brief Test-only observer after a Job publishes Running before compilation.
 */
using JobRunningObserver = void (*)();

/**
 * @brief Test-only observer after a successful Job-result find and before the
 * record mutex.
 * @param id Exact filtered JobId whose shared record has been retained.
 * @return No value.
 * @throws Any exception raised by test synchronization; the invocation seam
 * fences it so the result handler remains exception-safe.
 * @note The controller invokes this observer at most once for its installed
 * JobId and exists only in the noninstalled test runtime.
 */
using JobResultAfterFindObserver = void (*)(JobId id);

/**
 * @brief Test-only no-throw observer when one filtered JobRecord retires.
 * @param id Exact retired JobId.
 * @return No value.
 * @throws Nothing by function type contract.
 * @note The fixed controller also counts matching retirement before callback.
 */
using JobRecordRetirementObserver = void (*)(JobId id) noexcept;

/**
 * @brief Test-only observer after Session-create capacity reservation.
 * @return No value.
 * @throws Any exception raised by test synchronization; the invocation seam
 * fences it so production-equivalent control flow can continue.
 * @note The borrowed callback exists only in the noninstalled test runtime.
 */
using SessionCreatePendingObserver = void (*)();

/** @brief Deterministic exception-fence points in the test-only runtime. */
enum class ExceptionFenceFaultPoint : std::uint32_t {
  /** @brief Throws after one Service method has produced its response. */
  DispatchPrimary = 0U,
  /** @brief Throws before dispatch constructs its catch-path diagnostic. */
  DispatchFailureStatus,
  /** @brief Throws while an accepted descriptor enters registration. */
  ConnectionRegistration,
  /** @brief Throws after registration and before handler frame input. */
  HandlerPrimary,
  /** @brief Throws after dispatch and before normal response encoding. */
  ResponseEncode,
  /** @brief Throws before a handler catch constructs its diagnostic. */
  HandlerFailureStatus,
  /** @brief Throws before a best-effort protocol failure is encoded. */
  ProtocolFailureEncode,
  /** @brief Throws after encoding and before a failure frame is written. */
  ProtocolFailureWrite,
  /** @brief Throws before Session-close snapshot allocation or mutation. */
  SessionCloseSnapshot,
  /** @brief Throws after Session-create capacity reservation. */
  SessionCreateCandidate,
  /** @brief Throws immediately before Session map publication. */
  SessionCreatePublication,
  /** @brief Throws after a worker publishes Running. */
  JobPrimary,
  /** @brief Throws before worker failure status materialization. */
  JobFailureStatus,
  /** @brief One-past-the-end value used only to size fixed test state. */
  Count,
};

/** @brief Exact exception category raised by one armed fault point. */
enum class ExceptionFenceFaultAction : std::uint32_t {
  /** @brief Observe the point without raising an exception. */
  None = 0U,
  /** @brief Raise `std::bad_alloc`. */
  BadAlloc,
  /** @brief Raise one allocation-free `std::exception` subtype. */
  StandardException,
  /** @brief Raise a standard exception whose `what()` result is null. */
  NullDiagnostic,
  /** @brief Raise one type outside the `std::exception` hierarchy. */
  UnknownException,
};

/**
 * @brief Clears every observer, armed one-shot fault, and observation count.
 * @return No value.
 * @throws Nothing.
 * @note Call only while no exception-test handler is using the controller.
 */
void reset_exception_fence_faults() noexcept;

/**
 * @brief Arms one test-only point to raise exactly once.
 * @param point Exact instrumented boundary.
 * @param action Exception category; `None` leaves the point observational.
 * @return No value.
 * @throws Nothing.
 * @note Configuration is published before a handler thread is started.
 */
void arm_exception_fence_fault(ExceptionFenceFaultPoint point,
                               ExceptionFenceFaultAction action) noexcept;

/**
 * @brief Installs or clears deterministic shutdown-response observers.
 * @param ready Callback after Service dispatch and acceptance capture.
 * @param write Callback after a real normal-response write attempt.
 * @return No value.
 * @throws Nothing.
 * @note Install before starting handler threads and clear only after every
 * handler joins. These callbacks exist only in the noninstalled test runtime.
 */
void install_shutdown_response_observers(
    ShutdownResponseReadyObserver ready,
    ShutdownResponseWriteObserver write) noexcept;

/**
 * @brief Installs or clears the deterministic Job-Running observer.
 * @param observer Callback after Running publication, or null to clear.
 * @return No value.
 * @throws Nothing.
 * @note Install before submit and clear only after the observed worker exits
 * its callback. This seam exists only in the noninstalled test runtime.
 */
void install_job_running_observer(JobRunningObserver observer) noexcept;

/**
 * @brief Installs or clears one filtered, one-shot Job-result after-find seam.
 * @param id Exact JobId to observe; ignored when `observer` is null.
 * @param observer Callback after successful find and before record locking, or
 * null to clear.
 * @return No value.
 * @throws Nothing.
 * @note Install after submit and before starting the target result request.
 * Reconfiguration is valid only after the prior observed handler exits.
 */
void install_job_result_after_find_observer(
    JobId id, JobResultAfterFindObserver observer) noexcept;

/**
 * @brief Installs or clears one filtered JobRecord-retirement observer/counter.
 * @param id Exact JobId whose final shared-owner release is observed.
 * @param observer No-throw callback after the matching count increments, or
 * null to clear.
 * @return No value.
 * @throws Nothing.
 * @note The controller is allocation-free and exists only in the noninstalled
 * test runtime.
 */
void install_job_record_retirement_observer(
    JobId id, JobRecordRetirementObserver observer) noexcept;

/**
 * @brief Installs or clears the pending Session-create observer.
 * @param observer Callback after capacity reservation, or null to clear.
 * @return No value.
 * @throws Nothing.
 * @note Install before create and clear only after every observed create has
 * left the callback. This seam exists only in the noninstalled test runtime.
 */
void install_session_create_pending_observer(
    SessionCreatePendingObserver observer) noexcept;

/**
 * @brief Invokes the installed post-Running test observer.
 * @return No value.
 * @throws Nothing; every observer exception is fenced.
 * @note Production orchestration objects contain no corresponding callback.
 */
void observe_job_running() noexcept;

/**
 * @brief Invokes the installed one-shot observer for a matching retained Job.
 * @param id JobId whose `find()` just returned shared ownership.
 * @return No value.
 * @throws Nothing; every observer exception is fenced.
 * @note The production runtime has no corresponding call or controller.
 */
void observe_job_result_after_find(JobId id) noexcept;

/**
 * @brief Counts and reports retirement of one matching JobRecord.
 * @param id JobId owned by the retiring record.
 * @return No value.
 * @throws Nothing.
 * @note Invocation occurs only from the noninstalled test-runtime destructor.
 */
void observe_job_record_retirement(JobId id) noexcept;

/**
 * @brief Invokes the installed pending Session-create observer.
 * @return No value.
 * @throws Nothing; every observer exception is fenced.
 * @note Production orchestration objects contain no corresponding callback.
 */
void observe_session_create_pending() noexcept;

/**
 * @brief Invokes the installed post-dispatch response observer.
 * @param accepted_shutdown Whether Service accepted daemon shutdown.
 * @return No value.
 * @throws Any exception raised by the installed test observer.
 * @note Production server objects contain no corresponding callback.
 */
void observe_shutdown_response_ready(bool accepted_shutdown);

/**
 * @brief Invokes the installed real-write outcome observer.
 * @param accepted_shutdown Whether Service accepted daemon shutdown.
 * @param status Exact product `write_frame` result.
 * @return No value.
 * @throws Any exception raised by the installed test observer.
 * @note Production server objects contain no corresponding callback.
 */
void observe_shutdown_response_write(bool accepted_shutdown,
                                     const Status& status);

/**
 * @brief Records one boundary hit and raises an armed one-shot exception.
 * @param point Exact instrumented boundary.
 * @return No value.
 * @throws std::bad_alloc For `BadAlloc`.
 * @throws std::exception subtype For `StandardException` or
 * `NullDiagnostic`.
 * @throws A private nonstandard type for `UnknownException`.
 * @note The point is disarmed before the exception is raised.
 */
void hit_exception_fence_fault(ExceptionFenceFaultPoint point);

/**
 * @brief Returns how often one instrumented boundary was reached.
 * @param point Exact instrumented boundary.
 * @return Monotonic hit count since the last reset.
 * @throws Nothing.
 * @note Observation is safe after the matching handler has completed.
 */
[[nodiscard]] std::uint32_t exception_fence_fault_hits(
    ExceptionFenceFaultPoint point) noexcept;

/**
 * @brief Returns the exact matching JobRecord retirement count.
 * @param id Installed filter identity.
 * @return Count since installation/reset, or zero for another identity.
 * @throws Nothing.
 * @note A count of one proves every registry/handler/worker shared owner has
 * released the single record.
 */
[[nodiscard]] std::uint32_t job_record_retirement_count(JobId id) noexcept;

}  // namespace ps::ipc::test
