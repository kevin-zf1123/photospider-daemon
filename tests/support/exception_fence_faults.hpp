#pragma once

#include <cstdint>

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
  /** @brief Raise one type outside the `std::exception` hierarchy. */
  UnknownException,
};

/**
 * @brief Clears every armed one-shot fault and observation count.
 * @throws Nothing.
 * @note Call only while no exception-test handler is using the controller.
 */
void reset_exception_fence_faults() noexcept;

/**
 * @brief Arms one test-only point to raise exactly once.
 * @param point Exact instrumented boundary.
 * @param action Exception category; `None` leaves the point observational.
 * @throws Nothing.
 * @note Configuration is published before a handler thread is started.
 */
void arm_exception_fence_fault(ExceptionFenceFaultPoint point,
                               ExceptionFenceFaultAction action) noexcept;

/**
 * @brief Installs or clears deterministic shutdown-response observers.
 * @param ready Callback after Service dispatch and acceptance capture.
 * @param write Callback after a real normal-response write attempt.
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
 * @throws Nothing.
 * @note Install before submit and clear only after the observed worker exits
 * its callback. This seam exists only in the noninstalled test runtime.
 */
void install_job_running_observer(JobRunningObserver observer) noexcept;

/**
 * @brief Invokes the installed post-Running test observer.
 * @throws Nothing; every observer exception is fenced.
 * @note Production orchestration objects contain no corresponding callback.
 */
void observe_job_running() noexcept;

/**
 * @brief Invokes the installed post-dispatch response observer.
 * @param accepted_shutdown Whether Service accepted daemon shutdown.
 * @throws Any exception raised by the installed test observer.
 * @note Production server objects contain no corresponding callback.
 */
void observe_shutdown_response_ready(bool accepted_shutdown);

/**
 * @brief Invokes the installed real-write outcome observer.
 * @param accepted_shutdown Whether Service accepted daemon shutdown.
 * @param status Exact product `write_frame` result.
 * @throws Any exception raised by the installed test observer.
 * @note Production server objects contain no corresponding callback.
 */
void observe_shutdown_response_write(bool accepted_shutdown,
                                     const Status& status);

/**
 * @brief Records one boundary hit and raises an armed one-shot exception.
 * @param point Exact instrumented boundary.
 * @throws std::bad_alloc For `BadAlloc`.
 * @throws std::exception subtype For `StandardException`.
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

}  // namespace ps::ipc::test
