#include "support/exception_fence_faults.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <exception>
#include <new>

namespace ps::ipc::test {
namespace {

/** @brief Local name used by the fixed-size controller declaration. */
using FaultPoint = ExceptionFenceFaultPoint;

/** @brief Fixed number of independently instrumented test boundaries. */
constexpr auto kFaultPointCount = static_cast<std::size_t>(FaultPoint::Count);

/** @brief Allocation-free standard exception used by the controller. */
class InjectedStandardException final : public std::exception {
 public:
  /**
   * @brief Returns a process-lifetime diagnostic literal.
   * @return Static injected-fault description.
   * @throws Nothing.
   */
  [[nodiscard]] const char* what() const noexcept override {
    return "injected exception-fence standard exception";
  }
};

/** @brief Allocation-free standard exception with no diagnostic pointer. */
class InjectedNullDiagnosticException final : public std::exception {
 public:
  /**
   * @brief Returns no diagnostic bytes for the injected failure.
   * @return Null, intentionally.
   * @throws Nothing.
   */
  [[nodiscard]] const char* what() const noexcept override { return nullptr; }
};

/** @brief Nonstandard exception type for catch-all boundary coverage. */
struct InjectedUnknownException final {};

/** @brief One fixed atomically published one-shot fault slot. */
struct FaultSlot final {
  /** @brief Total observations since reset. */
  std::atomic<std::uint32_t> hits{0U};
  /** @brief One when armed and zero after the single trigger. */
  std::atomic<std::uint32_t> remaining{0U};
  /** @brief Exception category published before `remaining`. */
  std::atomic<ExceptionFenceFaultAction> action{
      ExceptionFenceFaultAction::None};
};

/**
 * @brief Returns process-lifetime fixed fault-controller storage.
 * @return Mutable slots shared by the exception-test runtime and test.
 * @throws Nothing.
 * @note No dynamic allocation is used by controller state.
 */
std::array<FaultSlot, kFaultPointCount>& fault_slots() noexcept {
  static std::array<FaultSlot, kFaultPointCount> slots;
  return slots;
}

/** @brief Installed post-dispatch response observer, or null. */
std::atomic<ShutdownResponseReadyObserver> response_ready_observer{nullptr};

/** @brief Installed real-response-write observer, or null. */
std::atomic<ShutdownResponseWriteObserver> response_write_observer{nullptr};

/** @brief Installed post-Running execution observer, or null. */
std::atomic<JobRunningObserver> job_running_observer{nullptr};

/** @brief Installed post-reservation Session-create observer, or null. */
std::atomic<SessionCreatePendingObserver> session_create_pending_observer{};

/**
 * @brief Converts a valid fault point to its fixed array index.
 * @param point Instrumented boundary.
 * @return Zero-based index, possibly equal to `kFaultPointCount` for Count.
 * @throws Nothing.
 */
std::size_t fault_index(ExceptionFenceFaultPoint point) noexcept {
  return static_cast<std::size_t>(point);
}

}  // namespace

void reset_exception_fence_faults() noexcept {
  response_ready_observer.store(nullptr, std::memory_order_release);
  response_write_observer.store(nullptr, std::memory_order_release);
  job_running_observer.store(nullptr, std::memory_order_release);
  session_create_pending_observer.store(nullptr, std::memory_order_release);
  for (FaultSlot& slot : fault_slots()) {
    slot.remaining.store(0U, std::memory_order_relaxed);
    slot.action.store(ExceptionFenceFaultAction::None,
                      std::memory_order_relaxed);
    slot.hits.store(0U, std::memory_order_relaxed);
  }
}

void install_shutdown_response_observers(
    ShutdownResponseReadyObserver ready,
    ShutdownResponseWriteObserver write) noexcept {
  response_write_observer.store(write, std::memory_order_relaxed);
  response_ready_observer.store(ready, std::memory_order_release);
}

/** @copydetails install_job_running_observer */
void install_job_running_observer(JobRunningObserver observer) noexcept {
  job_running_observer.store(observer, std::memory_order_release);
}

/** @copydetails install_session_create_pending_observer */
void install_session_create_pending_observer(
    SessionCreatePendingObserver observer) noexcept {
  session_create_pending_observer.store(observer, std::memory_order_release);
}

/** @copydetails observe_job_running */
void observe_job_running() noexcept {
  const JobRunningObserver observer =
      job_running_observer.load(std::memory_order_acquire);
  if (!observer) {
    return;
  }
  try {
    observer();
  } catch (...) {
  }
}

/** @copydetails observe_session_create_pending */
void observe_session_create_pending() noexcept {
  const SessionCreatePendingObserver observer =
      session_create_pending_observer.load(std::memory_order_acquire);
  if (!observer) {
    return;
  }
  try {
    observer();
  } catch (...) {
  }
}

void observe_shutdown_response_ready(bool accepted_shutdown) {
  const ShutdownResponseReadyObserver observer =
      response_ready_observer.load(std::memory_order_acquire);
  if (observer) {
    observer(accepted_shutdown);
  }
}

void observe_shutdown_response_write(bool accepted_shutdown,
                                     const Status& status) {
  const ShutdownResponseWriteObserver observer =
      response_write_observer.load(std::memory_order_acquire);
  if (observer) {
    observer(accepted_shutdown, status);
  }
}

void arm_exception_fence_fault(ExceptionFenceFaultPoint point,
                               ExceptionFenceFaultAction action) noexcept {
  const std::size_t index = fault_index(point);
  if (index >= kFaultPointCount) {
    return;
  }
  FaultSlot& slot = fault_slots()[index];
  slot.action.store(action, std::memory_order_relaxed);
  slot.remaining.store(action == ExceptionFenceFaultAction::None ? 0U : 1U,
                       std::memory_order_release);
}

void hit_exception_fence_fault(ExceptionFenceFaultPoint point) {
  const std::size_t index = fault_index(point);
  if (index >= kFaultPointCount) {
    return;
  }
  FaultSlot& slot = fault_slots()[index];
  slot.hits.fetch_add(1U, std::memory_order_relaxed);
  std::uint32_t expected = 1U;
  if (!slot.remaining.compare_exchange_strong(
          expected, 0U, std::memory_order_acq_rel, std::memory_order_acquire)) {
    return;
  }
  switch (slot.action.load(std::memory_order_acquire)) {
    case ExceptionFenceFaultAction::None:
      return;
    case ExceptionFenceFaultAction::BadAlloc:
      throw std::bad_alloc();
    case ExceptionFenceFaultAction::StandardException:
      throw InjectedStandardException();
    case ExceptionFenceFaultAction::NullDiagnostic:
      throw InjectedNullDiagnosticException();
    case ExceptionFenceFaultAction::UnknownException:
      throw InjectedUnknownException();
  }
}

std::uint32_t exception_fence_fault_hits(
    ExceptionFenceFaultPoint point) noexcept {
  const std::size_t index = fault_index(point);
  return index < kFaultPointCount
             ? fault_slots()[index].hits.load(std::memory_order_acquire)
             : 0U;
}

}  // namespace ps::ipc::test
