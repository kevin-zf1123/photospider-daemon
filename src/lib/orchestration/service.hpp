#pragma once

#include <cstdint>
#include <memory>

#include "ipc/codec.hpp"

namespace ps::ipc::internal {

/**
 * @brief Fixed process-local orchestration resource configuration.
 *
 * @note Bounds are global maintenance controls, not per-namespace quotas.
 */
struct ServiceConfig final {
  /**
   * @brief Captures fixed process-local resource bounds.
   * @param requested_concurrency Positive worker count validated by Service.
   * @param requested_jobs Positive retained-Job bound validated by Service.
   * @param requested_sessions Positive Session bound validated by Service.
   * @param requested_gpu Whether the optional local GPU lane is configured.
   * @throws Nothing.
   * @note Service performs positive-bound validation during construction.
   */
  explicit ServiceConfig(std::uint32_t requested_concurrency = 1U,
                         std::uint32_t requested_jobs = 1024U,
                         std::uint32_t requested_sessions = 128U,
                         bool requested_gpu = false) noexcept
      : maximum_concurrency(requested_concurrency),
        maximum_jobs(requested_jobs),
        maximum_sessions(requested_sessions),
        gpu_enabled(requested_gpu) {}

  /** @brief Positive number of concurrent compile/execute worker loops. */
  std::uint32_t maximum_concurrency = 1U;
  /** @brief Maximum retained queued/running/terminal records. */
  std::uint32_t maximum_jobs = 1024U;
  /** @brief Maximum simultaneously retained logical namespaces. */
  std::uint32_t maximum_sessions = 128U;
  /** @brief Whether the optional local kernel GPU lane is configured. */
  bool gpu_enabled = false;
};

/**
 * @brief In-memory local namespace and execution orchestration service.
 *
 * @note Construction starts empty; destruction cancels unfinished work, joins
 * worker loops, and releases every namespace/result. Nothing is persisted.
 */
class Service final {
 public:
  /**
   * @brief Creates empty registries and fixed kernel execution resources.
   * @param config Positive local concurrency/retention bounds.
   * @throws std::invalid_argument If configuration is invalid.
   * @throws std::bad_alloc If worker/registry allocation fails.
   */
  explicit Service(ServiceConfig config);

  /**
   * @brief Cancels, joins, and releases all ephemeral state.
   * @throws Nothing.
   * @note No Session, Job, or result survives destruction.
   */
  ~Service() noexcept;

  /**
   * @brief Forbids copying worker and registry ownership.
   * @param other Source service that cannot be copied.
   * @throws Nothing; the operation is deleted.
   * @note Construct a separate empty service for independent lifecycle state.
   */
  Service(const Service& other) = delete;
  /**
   * @brief Forbids assigning active orchestration ownership.
   * @param other Source service that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Session, Job, worker, and result lifetimes never transfer.
   */
  Service& operator=(const Service& other) = delete;

  /**
   * @brief Routes one already decoded request through the exact nine methods.
   * @param request Complete version-three request.
   * @return Correlated response with typed failure or method payload.
   * @throws Nothing; allocation and kernel exceptions are fenced into status.
   * @note If diagnostic construction also fails, the response retains its
   * correlation, carries a non-Ok empty-message status, and contains no
   * success-only payload.
   */
  [[nodiscard]] Response dispatch(const Request& request) noexcept;

 private:
  /** @brief Opaque registries, workers, compiler, and execution resources. */
  struct Impl;
  /** @brief Unique process-local service state. */
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps::ipc::internal
