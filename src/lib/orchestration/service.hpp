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
  /** @brief Positive number of concurrent compile/execute worker loops. */
  std::uint32_t maximum_concurrency = 1U;
  /** @brief Maximum retained queued/running/terminal records. */
  std::uint32_t maximum_jobs = 1024U;
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

  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;

  /**
   * @brief Routes one already decoded request through the exact nine methods.
   * @param request Complete version-three request.
   * @return Correlated response with typed failure or method payload.
   * @throws Nothing; allocation and kernel exceptions are fenced into status.
   */
  [[nodiscard]] Response dispatch(const Request& request) noexcept;

 private:
  /** @brief Opaque registries, workers, compiler, and execution resources. */
  struct Impl;
  /** @brief Unique process-local service state. */
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps::ipc::internal
