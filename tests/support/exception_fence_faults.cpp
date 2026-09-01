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
  for (FaultSlot& slot : fault_slots()) {
    slot.remaining.store(0U, std::memory_order_relaxed);
    slot.action.store(ExceptionFenceFaultAction::None,
                      std::memory_order_relaxed);
    slot.hits.store(0U, std::memory_order_relaxed);
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
