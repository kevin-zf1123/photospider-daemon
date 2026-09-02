#include "orchestration/service.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/compiler/compiler.hpp"
#include "photospider/execution/execution.hpp"
#include "photospider/plugin/operation_registry.hpp"

#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
#include "support/exception_fence_faults.hpp"
#endif

#ifndef PHOTOSPIDER_DAEMON_VERSION
#define PHOTOSPIDER_DAEMON_VERSION "0.2.0"
#endif

namespace ps::ipc::internal {
namespace {

/**
 * @brief Converts a response to one allocation-free canonical failure.
 * @param response Correlated response whose success-only payload is discarded.
 * @param code Intended non-success category; `Ok` is normalized to `Internal`.
 * @throws Nothing.
 * @note Request correlation and method are preserved. Every string/container is
 * cleared in place, so the fallback does not allocate while removing any
 * partially populated method-specific success payload.
 */
void fail_response_without_allocation(Response& response,
                                      ErrorCode code) noexcept {
  response.status.code = code == ErrorCode::Ok ? ErrorCode::Internal : code;
  response.status.message.clear();

  response.session_id.instance = 0U;
  response.session_id.value = 0U;
  response.job_id.instance = 0U;
  response.job_id.value = 0U;

  response.job_status.job_id.instance = 0U;
  response.job_status.job_id.value = 0U;
  response.job_status.session_id.instance = 0U;
  response.job_status.session_id.value = 0U;
  response.job_status.state = JobState::Queued;
  response.job_status.outcome.code = ErrorCode::Ok;
  response.job_status.outcome.message.clear();

  response.execution_result.values.clear();
  auto& diagnostics = response.execution_result.diagnostics;
  diagnostics.execute_us = 0U;
  diagnostics.selected_backends.clear();
  diagnostics.transfer_count = 0U;
  diagnostics.transfer_bytes = 0U;
  diagnostics.peak_live_bytes = 0U;
  diagnostics.fallback_reasons.clear();
  diagnostics.operation_timings.clear();
  diagnostics.plan_digest.clear();
  diagnostics.result_digest.clear();

  response.daemon_info.protocol_version = 0U;
  response.daemon_info.instance_id = 0U;
  response.daemon_info.service_version.clear();
  response.daemon_info.transport.clear();
  response.daemon_info.methods.clear();
  response.daemon_info.active_sessions = 0U;
  response.daemon_info.active_jobs = 0U;
  response.daemon_info.maximum_concurrency = 0U;
  response.daemon_info.maximum_sessions = 0U;
  response.shutdown_after_write = false;
}

/** @brief Immutable namespace record owning one kernel GraphContext. */
struct SessionRecord final {
  /** @brief Ephemeral namespace identifier. */
  SessionId id;
  /** @brief Independently owned immutable source graph. */
  std::shared_ptr<GraphContext> graph;
};

/** @brief Synchronized state for one queued/running/terminal execution. */
struct JobRecord final {
  /** @brief Serializes lifecycle, outcome, and result publication. */
  mutable std::mutex mutex;
  /** @brief Wakes namespace close after worker or queued-close termination. */
  std::condition_variable terminal_changed;
  /** @brief Immutable ephemeral execution identifier. */
  JobId id;
  /** @brief Immutable owning namespace identifier. */
  SessionId session_id;
  /** @brief Retained source graph even when the namespace closes mid-run. */
  std::shared_ptr<GraphContext> graph;
  /** @brief Public local planning/execution controls. */
  JobSubmitOptions options;
  /** @brief Current forward-only state. */
  JobState state = JobState::Queued;
  /** @brief Terminal success/failure category. */
  Status outcome;
  /** @brief Cooperative kernel cancellation source. */
  CancellationSource cancellation;
  /** @brief In-memory successful result retained until release. */
  std::optional<ExecutionResult> result;
};

/**
 * @brief Returns whether one execution lifecycle is terminal.
 * @param state Closed forward-only lifecycle state.
 * @return True for Succeeded, Failed, or Cancelled.
 * @throws Nothing.
 * @note Queued and Running are the only nonterminal states.
 */
bool terminal(JobState state) noexcept {
  return state == JobState::Succeeded || state == JobState::Failed ||
         state == JobState::Cancelled;
}

/**
 * @brief Generates one nonzero non-security process-instance token.
 * @return Random-device-derived ephemeral value.
 * @throws Any exception raised by the platform random device.
 * @note The value prevents prior-process identifiers from matching new maps;
 * it is not used for authentication or native-code admission.
 */
std::uint64_t make_instance_id() {
  std::random_device source;
  std::uint64_t value = 0U;
  while (value == 0U) {
    value = (static_cast<std::uint64_t>(source()) << 32U) ^
            static_cast<std::uint64_t>(source());
  }
  return value;
}

/**
 * @brief Worker-backed bounded registry for all ephemeral executions.
 *
 * @note Queue capacity and worker count are global daemon bounds. Namespace
 * names do not alter admission or resource allocation.
 */
class JobRegistry final {
 public:
  /**
   * @brief Starts fixed worker loops over shared public kernel facilities.
   * @param compiler Thread-safe typed compiler.
   * @param execution Shared bounded kernel execution context.
   * @param maximum_concurrency Positive worker count.
   * @param maximum_jobs Positive retained-record bound.
   * @param gpu_enabled Whether submit may opt into local GPU planning.
   * @param instance_id Nonzero process-lifetime identifier domain.
   * @throws std::invalid_argument If pointers or bounds are invalid.
   * @throws std::bad_alloc If worker ownership allocation fails.
   * @throws std::system_error If a fixed worker cannot be started.
   * @note Partial construction cancels and joins every started worker.
   */
  JobRegistry(Compiler* compiler, ExecutionContext* execution,
              std::uint32_t maximum_concurrency, std::uint32_t maximum_jobs,
              bool gpu_enabled, std::uint64_t instance_id)
      : compiler_(compiler),
        execution_(execution),
        maximum_jobs_(maximum_jobs),
        gpu_enabled_(gpu_enabled),
        instance_id_(instance_id) {
    if (!compiler_ || !execution_ || maximum_concurrency == 0U ||
        maximum_jobs_ == 0U || instance_id_ == 0U) {
      throw std::invalid_argument(
          "execution registry configuration is invalid");
    }
    try {
      workers_.reserve(maximum_concurrency);
      for (std::uint32_t index = 0U; index < maximum_concurrency; ++index) {
        workers_.emplace_back([this] { worker_loop(); });
      }
    } catch (...) {
      stop();
      throw;
    }
  }

  /**
   * @brief Cancels all retained work and joins every worker.
   * @throws Nothing.
   * @note No Job or result survives registry destruction.
   */
  ~JobRegistry() noexcept { stop(); }

  /**
   * @brief Forbids duplicating Job records, queues, and worker ownership.
   * @param other Source registry that cannot be copied.
   * @throws Nothing; the operation is deleted.
   * @note One registry is the sole owner of each ephemeral JobId domain.
   */
  JobRegistry(const JobRegistry& other) = delete;
  /**
   * @brief Forbids assigning active Job/worker ownership.
   * @param other Source registry that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Worker joins and result cleanup remain exactly once.
   */
  JobRegistry& operator=(const JobRegistry& other) = delete;

  /**
   * @brief Admits one new execution for an already validated namespace.
   * @param session Immutable namespace/source record.
   * @param options Public local controls.
   * @return New identifier or global backpressure failure.
   * @throws std::bad_alloc If record or queue allocation fails.
   * @note Map publication is rolled back if queue publication fails.
   */
  Result<JobId> submit(const std::shared_ptr<SessionRecord>& session,
                       JobSubmitOptions options) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return Result<JobId>(Status::failure(ErrorCode::Cancelled,
                                           "daemon orchestration is stopping"));
    }
    if (jobs_.size() >= maximum_jobs_) {
      return Result<JobId>(
          Status::failure(ErrorCode::ResourceExhausted,
                          "global retained execution bound is exhausted"));
    }
    if (next_id_ == 0U) {
      return Result<JobId>(Status::failure(ErrorCode::ResourceExhausted,
                                           "ephemeral execution id exhausted"));
    }
    auto record = std::make_shared<JobRecord>();
    record->id.instance = instance_id_;
    record->id.value = next_id_++;
    record->session_id = session->id;
    record->graph = session->graph;
    record->options = options;
    const auto inserted = jobs_.emplace(record->id.value, record);
    if (!inserted.second) {
      return Result<JobId>(Status::failure(
          ErrorCode::Internal, "ephemeral execution id was duplicated"));
    }
    try {
      queue_.push_back(record);
    } catch (...) {
      jobs_.erase(inserted.first);
      throw;
    }
    ready_.notify_one();
    return Result<JobId>(record->id);
  }

  /**
   * @brief Returns one non-destructive execution status snapshot.
   * @param id Exact process-scoped ephemeral identifier.
   * @return Copied status or `NotFound` for stale/missing identity.
   * @throws std::bad_alloc If result or diagnostic allocation fails.
   * @note Status observation does not consume terminal state.
   */
  Result<JobStatus> status(JobId id) const {
    auto record = find(id);
    if (!record) {
      return Result<JobStatus>(Status::failure(
          ErrorCode::NotFound, "ephemeral execution does not exist"));
    }
    std::lock_guard<std::mutex> lock(record->mutex);
    return Result<JobStatus>(JobStatus{record->id, record->session_id,
                                       record->state, record->outcome});
  }

  /**
   * @brief Requests cooperative cancellation or accepts terminal idempotence.
   * @param id Exact process-scoped ephemeral identifier.
   * @return Success for accepted/already-terminal work or `NotFound`.
   * @throws std::bad_alloc If a failure diagnostic allocation fails.
   * @note The Running transition remains observable before cancellation ends.
   */
  Status cancel(JobId id) {
    auto record = find(id);
    if (!record) {
      return Status::failure(ErrorCode::NotFound,
                             "ephemeral execution does not exist");
    }
    std::lock_guard<std::mutex> lock(record->mutex);
    if (terminal(record->state)) {
      return Status::success();
    }
    record->cancellation.cancel();
    return Status::success();
  }

  /**
   * @brief Returns one copied successful in-memory result.
   * @param id Exact process-scoped ephemeral identifier.
   * @return Copied result or precise missing/not-ready/terminal failure.
   * @throws std::bad_alloc If result or diagnostic allocation fails.
   * @note Observation is non-destructive until explicit release.
   */
  Result<ExecutionResult> result(JobId id) const {
    auto record = find(id);
    if (!record) {
      return Result<ExecutionResult>(Status::failure(
          ErrorCode::NotFound, "ephemeral execution does not exist"));
    }
    std::lock_guard<std::mutex> lock(record->mutex);
    if (record->state == JobState::Succeeded && record->result.has_value()) {
      return Result<ExecutionResult>(*record->result);
    }
    if (record->state == JobState::Failed ||
        record->state == JobState::Cancelled) {
      return Result<ExecutionResult>(
          record->outcome.ok()
              ? Status::failure(ErrorCode::Internal,
                                "terminal execution lacks failure")
              : record->outcome);
    }
    return Result<ExecutionResult>(Status::failure(
        ErrorCode::InvalidArgument, "execution result is not ready"));
  }

  /**
   * @brief Erases one terminal execution and releases its result.
   * @param id Exact process-scoped ephemeral identifier.
   * @return Success, `NotFound`, or nonterminal rejection.
   * @throws std::bad_alloc If a failure diagnostic allocation fails.
   * @note Released identity cannot be observed or reused.
   */
  Status release(JobId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = jobs_.find(id.value);
    if (id.instance != instance_id_ || id.value == 0U ||
        iterator == jobs_.end()) {
      return Status::failure(ErrorCode::NotFound,
                             "ephemeral execution does not exist");
    }
    {
      std::lock_guard<std::mutex> record_lock(iterator->second->mutex);
      if (!terminal(iterator->second->state)) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "running execution cannot be released");
      }
      iterator->second->result.reset();
    }
    jobs_.erase(iterator);
    return Status::success();
  }

  /**
   * @brief Cancels and removes every execution for one closing namespace.
   * @param session_id Namespace being closed.
   * @throws std::bad_alloc If queued/running snapshot allocation fails before
   * any queue or map mutation.
   * @note The registry mutex is the linearization boundary between records
   * still owned by `queue_` and records already popped by a worker. Queued
   * records are removed from both containers and synchronously complete
   * `Queued -> Running -> Cancelled`; only popped/running records require
   * cooperative cancellation and a terminal wait. Record mutexes are never
   * acquired while the registry mutex is held.
   */
  void close_session(SessionId session_id) {
    std::vector<std::shared_ptr<JobRecord>> queued;
    std::vector<std::shared_ptr<JobRecord>> running;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queued.reserve(queue_.size());
      running.reserve(jobs_.size());
      for (auto iterator = queue_.begin(); iterator != queue_.end();) {
        const auto& record = *iterator;
        if (record->session_id.instance != session_id.instance ||
            record->session_id.value != session_id.value) {
          ++iterator;
          continue;
        }
        queued.push_back(record);
        iterator = queue_.erase(iterator);
      }
      for (const auto& record : queued) {
        const auto iterator = jobs_.find(record->id.value);
        if (iterator != jobs_.end() && iterator->second == record) {
          jobs_.erase(iterator);
        }
      }
      for (auto iterator = jobs_.begin(); iterator != jobs_.end();) {
        const auto& record = iterator->second;
        if (record->session_id.instance != session_id.instance ||
            record->session_id.value != session_id.value) {
          ++iterator;
          continue;
        }
        running.push_back(record);
        iterator = jobs_.erase(iterator);
      }
    }

    for (const auto& record : queued) {
      static_cast<void>(record->cancellation.cancel());
      std::lock_guard<std::mutex> record_lock(record->mutex);
      record->result.reset();
      record->state = JobState::Running;
      record->outcome.code = ErrorCode::Cancelled;
      record->outcome.message.clear();
      record->state = JobState::Cancelled;
      record->terminal_changed.notify_all();
    }
    for (const auto& record : running) {
      static_cast<void>(record->cancellation.cancel());
    }
    for (const auto& record : running) {
      std::unique_lock<std::mutex> record_lock(record->mutex);
      record->result.reset();
      record->terminal_changed.wait(
          record_lock, [&record] { return terminal(record->state); });
    }
  }

  /**
   * @brief Returns the current retained execution count.
   * @return Number of queued, running, and terminal records in the map.
   * @throws Nothing.
   * @note The observation may change immediately after return.
   */
  [[nodiscard]] std::uint64_t size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.size();
  }

 private:
  /**
   * @brief Finds one retained record with shared lifetime.
   * @param id Exact process-scoped ephemeral identifier.
   * @return Shared record, or null for stale/missing identity.
   * @throws Nothing.
   * @note Shared ownership lets a worker finish after registry removal.
   */
  std::shared_ptr<JobRecord> find(JobId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = jobs_.find(id.value);
    return id.instance != instance_id_ || id.value == 0U ||
                   iterator == jobs_.end()
               ? nullptr
               : iterator->second;
  }

  /**
   * @brief Pops queued records and performs compile then local execution.
   * @throws Nothing across the fixed worker-thread boundary.
   * @note Stop wakes the loop and prevents later queued publication.
   */
  void worker_loop() noexcept {
    for (;;) {
      std::shared_ptr<JobRecord> record;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (stopping_) {
          return;
        }
        record = std::move(queue_.front());
        queue_.pop_front();
      }
      execute(record);
    }
  }

  /**
   * @brief Advances one record through Running and exactly one terminal state.
   * @param record Shared record retained across namespace close.
   * @throws Nothing across the worker boundary.
   * @note Cancellation is rechecked before compilation, before execution, and
   * before result publication.
   */
  void execute(const std::shared_ptr<JobRecord>& record) noexcept {
    try {
      {
        std::lock_guard<std::mutex> lock(record->mutex);
        if (record->state != JobState::Queued) {
          record->state = JobState::Failed;
          record->outcome = Status::failure(
              ErrorCode::Internal, "execution left queued state unexpectedly");
          record->terminal_changed.notify_all();
          return;
        }
        record->state = JobState::Running;
      }

      if (record->cancellation.token().cancelled()) {
        finish_cancelled(record, "execution cancelled before compilation");
        return;
      }

      const bool allow_gpu = gpu_enabled_ && record->options.allow_gpu;
      PlanningOptions planning;
      planning.allow_gpu = allow_gpu;
      auto compiled = compiler_->compile(*record->graph, planning);
      if (!compiled.ok()) {
        finish_failure(record, compiled.status());
        return;
      }
      if (record->cancellation.token().cancelled()) {
        finish_cancelled(record, "execution cancelled during compilation");
        return;
      }
      auto executed = execution_->execute(
          compiled.value().plan, record->cancellation.token(),
          ExecutionOptions{record->options.maximum_parallelism});
      if (!executed.ok()) {
        if (executed.status().code == ErrorCode::Cancelled ||
            record->cancellation.token().cancelled()) {
          finish_cancelled(record, executed.status().message);
        } else {
          finish_failure(record, executed.status());
        }
        return;
      }
      std::lock_guard<std::mutex> lock(record->mutex);
      if (record->cancellation.token().cancelled()) {
        record->state = JobState::Cancelled;
        record->outcome = Status::failure(
            ErrorCode::Cancelled,
            "cancelled execution result publication was rejected");
        record->result.reset();
      } else {
        record->result = executed.take_value();
        record->outcome = Status::success();
        record->state = JobState::Succeeded;
      }
      record->terminal_changed.notify_all();
    } catch (const std::bad_alloc&) {
      finish_failure_safely(record, ErrorCode::ResourceExhausted,
                            "execution orchestration allocation failed");
    } catch (const std::exception& error) {
      finish_failure_safely(record, ErrorCode::Internal, error.what());
    } catch (...) {
      finish_failure_safely(record, ErrorCode::Internal,
                            "execution orchestration raised an exception");
    }
  }

  /**
   * @brief Publishes one non-cancellation terminal failure.
   * @param record Shared execution record.
   * @param status Kernel/compiler/orchestration failure to publish.
   * @throws std::bad_alloc If a cancellation diagnostic allocation fails.
   * @note A concurrently observed cancellation takes terminal precedence.
   */
  void finish_failure(const std::shared_ptr<JobRecord>& record, Status status) {
    std::lock_guard<std::mutex> lock(record->mutex);
    if (record->cancellation.token().cancelled()) {
      record->state = JobState::Cancelled;
      record->outcome = Status::failure(ErrorCode::Cancelled,
                                        "execution cancellation was requested");
    } else {
      record->state = JobState::Failed;
      record->outcome = std::move(status);
    }
    record->result.reset();
    record->terminal_changed.notify_all();
  }

  /**
   * @brief Publishes one cancellation terminal state.
   * @param record Shared execution record.
   * @param reason Best available cancellation diagnostic.
   * @throws std::bad_alloc If diagnostic allocation fails.
   * @note Result storage is cleared before terminal waiters are awakened.
   */
  void finish_cancelled(const std::shared_ptr<JobRecord>& record,
                        const std::string& reason) {
    std::lock_guard<std::mutex> lock(record->mutex);
    record->state = JobState::Cancelled;
    record->outcome =
        Status::failure(ErrorCode::Cancelled,
                        reason.empty() ? "execution was cancelled" : reason);
    record->result.reset();
    record->terminal_changed.notify_all();
  }

  /**
   * @brief Best-effort exception-fence terminal publication.
   * @param record Shared execution record.
   * @param code Failure category when cancellation is not active.
   * @param reason Best available diagnostic.
   * @throws Nothing.
   * @note If diagnostic allocation also fails, a no-allocation empty-message
   * terminal status still wakes namespace close and releases the result.
   */
  void finish_failure_safely(const std::shared_ptr<JobRecord>& record,
                             ErrorCode code,
                             const std::string& reason) noexcept {
    try {
      finish_failure(record, Status::failure(code, reason));
    } catch (...) {
      try {
        std::lock_guard<std::mutex> lock(record->mutex);
        const bool cancelled = record->cancellation.token().cancelled();
        record->state = cancelled ? JobState::Cancelled : JobState::Failed;
        record->outcome.code = cancelled ? ErrorCode::Cancelled : code;
        record->outcome.message.clear();
        record->result.reset();
        record->terminal_changed.notify_all();
      } catch (...) {
      }
    }
  }

  /**
   * @brief Idempotently cancels state, wakes, and joins all workers.
   * @throws Nothing under fixed worker-ownership invariants.
   * @note Map, queue, and results are cleared only after every worker joins.
   */
  void stop() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) {
        return;
      }
      stopping_ = true;
      for (const auto& entry : jobs_) {
        entry.second->cancellation.cancel();
      }
    }
    ready_.notify_all();
    for (std::thread& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers_.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    jobs_.clear();
  }

  /** @brief Shared typed compiler. */
  Compiler* compiler_;
  /** @brief Shared bounded local kernel execution context. */
  ExecutionContext* execution_;
  /** @brief Fixed retained-record bound. */
  std::uint32_t maximum_jobs_;
  /** @brief Fixed optional local GPU availability. */
  bool gpu_enabled_;
  /** @brief Nonzero owning daemon-instance token. */
  std::uint64_t instance_id_;
  /** @brief Serializes map, queue, stop, and id generation. */
  mutable std::mutex mutex_;
  /** @brief Wakes fixed worker loops. */
  std::condition_variable ready_;
  /** @brief Retained record map by ephemeral id. */
  std::map<std::uint64_t, std::shared_ptr<JobRecord>> jobs_;
  /** @brief FIFO queue of admitted records. */
  std::deque<std::shared_ptr<JobRecord>> queue_;
  /** @brief Fixed worker set. */
  std::vector<std::thread> workers_;
  /** @brief Next nonzero id; wrap to zero means exhausted. */
  std::uint64_t next_id_ = 1U;
  /** @brief Monotonic stop flag. */
  bool stopping_ = false;
};

}  // namespace

/**
 * @brief Complete private implementation of the local orchestration service.
 * @note Member order keeps compiler/execution alive until workers have joined.
 */
struct Service::Impl final {
  /**
   * @brief Constructs compiler, execution, and worker state in dependency
   * order.
   * @param requested Positive fixed process-local resource bounds.
   * @throws std::invalid_argument If bounds are zero.
   * @throws std::bad_alloc If state allocation fails.
   * @throws std::system_error If a fixed worker cannot be created.
   * @note Construction always starts with empty Session and Job registries.
   */
  explicit Impl(ServiceConfig requested)
      : config(requested),
        instance_id(make_instance_id()),
        operations(make_default_operation_registry()),
        compiler(operations),
        execution(operations,
                  ExecutionContextConfig{
                      requested.maximum_concurrency, requested.gpu_enabled,
                      requested.maximum_jobs, 256U * 1024U * 1024U}),
        jobs(&compiler, &execution, requested.maximum_concurrency,
             requested.maximum_jobs, requested.gpu_enabled, instance_id) {
    if (config.maximum_concurrency == 0U || config.maximum_jobs == 0U ||
        config.maximum_sessions == 0U) {
      throw std::invalid_argument("service bounds must be positive");
    }
  }

  /**
   * @brief Creates and compiler-validates one immutable namespace.
   * @param document Complete public compiler source document.
   * @return New ephemeral SessionId or validation/resource failure.
   * @throws std::bad_alloc If graph, compiler, or registry allocation fails.
   * @note No internal IR is retained in or accepted from the request.
   */
  Result<SessionId> create_session(const WorkflowDocument& document) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
    {
      std::lock_guard<std::mutex> sessions_lock(sessions_mutex);
      if (sessions.size() >= config.maximum_sessions) {
        Status capacity;
        capacity.code = ErrorCode::ResourceExhausted;
        return Result<SessionId>(std::move(capacity));
      }
      if (next_session_id == 0U) {
        return Result<SessionId>(Status::failure(
            ErrorCode::ResourceExhausted, "ephemeral namespace id exhausted"));
      }
    }
    auto candidate = std::make_shared<GraphContext>(document);
    auto validation = compiler.compile(*candidate);
    if (!validation.ok()) {
      return Result<SessionId>(validation.status());
    }
    auto record = std::make_shared<SessionRecord>();
    record->id.instance = instance_id;
    record->id.value = next_session_id;
    record->graph = std::move(candidate);
    std::lock_guard<std::mutex> sessions_lock(sessions_mutex);
    if (!sessions.emplace(record->id.value, record).second) {
      return Result<SessionId>(Status::failure(
          ErrorCode::Internal, "ephemeral namespace id was duplicated"));
    }
    ++next_session_id;
    return Result<SessionId>(record->id);
  }

  /**
   * @brief Closes a namespace atomically against new submit admission.
   * @param id Exact process-scoped namespace identifier.
   * @return Success or `NotFound` for stale/missing identity.
   * @throws std::bad_alloc If closing-record snapshot allocation fails.
   * @note Queued executions are removed and synchronously cancelled; popped
   * or running executions are cooperatively cancelled and awaited. Unrelated
   * namespace work never participates in this close wait.
   */
  Status close_session(SessionId id) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
    {
      std::lock_guard<std::mutex> sessions_lock(sessions_mutex);
      if (id.instance != instance_id || id.value == 0U ||
          sessions.find(id.value) == sessions.end()) {
        return Status::failure(ErrorCode::NotFound,
                               "ephemeral namespace does not exist");
      }
    }
    jobs.close_session(id);
    {
      std::lock_guard<std::mutex> sessions_lock(sessions_mutex);
      sessions.erase(id.value);
    }
    return Status::success();
  }

  /**
   * @brief Admits a new execution atomically against namespace close.
   * @param id Exact process-scoped namespace identifier.
   * @param options Public local planning/execution controls.
   * @return New JobId or precise identity/backpressure failure.
   * @throws std::bad_alloc If record or queue allocation fails.
   * @note The lifecycle lock prevents admission after close validation.
   */
  Result<JobId> submit(SessionId id, JobSubmitOptions options) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);
    std::shared_ptr<SessionRecord> session;
    {
      std::lock_guard<std::mutex> sessions_lock(sessions_mutex);
      const auto iterator = sessions.find(id.value);
      if (id.instance != instance_id || id.value == 0U ||
          iterator == sessions.end()) {
        return Result<JobId>(Status::failure(
            ErrorCode::NotFound, "ephemeral namespace does not exist"));
      }
      session = iterator->second;
    }
    return jobs.submit(session, options);
  }

  /**
   * @brief Returns the current namespace count.
   * @return Current registry size.
   * @throws Nothing.
   * @note The observation may change immediately after return.
   */
  std::uint64_t session_count() const noexcept {
    std::lock_guard<std::mutex> lock(sessions_mutex);
    return sessions.size();
  }

  /** @brief Fixed service configuration. */
  ServiceConfig config;
  /** @brief Nonzero non-security process-lifetime identity. */
  std::uint64_t instance_id;
  /** @brief Frozen built-in operation set. */
  std::shared_ptr<OperationRegistry> operations;
  /** @brief Typed public kernel compiler. */
  Compiler compiler;
  /** @brief Shared local kernel execution resources. */
  ExecutionContext execution;
  /** @brief Global ephemeral execution registry. */
  JobRegistry jobs;
  /** @brief Serializes close against submit. */
  std::mutex lifecycle_mutex;
  /** @brief Serializes namespace registry. */
  mutable std::mutex sessions_mutex;
  /** @brief Ephemeral namespace map. */
  std::map<std::uint64_t, std::shared_ptr<SessionRecord>> sessions;
  /** @brief Next nonzero namespace id. */
  std::uint64_t next_session_id = 1U;
  /**
   * @brief Monotonic admission fence raised at shutdown's no-throw commit.
   * @note Pre-commit dispatch failure leaves this false.
   */
  std::atomic<bool> shutting_down{false};
};

/**
 * @brief Implements empty ephemeral service construction.
 * @copydetails Service::Service
 */
Service::Service(ServiceConfig config)
    : impl_(std::make_unique<Impl>(config)) {}

/**
 * @brief Implements cancellation, join, and state release.
 * @copydetails Service::~Service
 */
Service::~Service() noexcept = default;

/**
 * @brief Implements exception-fenced exact-method dispatch.
 * @copydetails Service::dispatch
 */
Response Service::dispatch(const Request& request) noexcept {
  Response response;
  response.request_id = request.request_id;
  response.method = request.method;
  bool accept_shutdown = false;
  try {
    if (request.method != Method::DaemonShutdown &&
        impl_->shutting_down.load(std::memory_order_acquire)) {
      response.status = Status::failure(
          ErrorCode::Cancelled, "daemon orchestration is shutting down");
      return response;
    }
    switch (request.method) {
      case Method::SessionCreate: {
        auto result = impl_->create_session(request.document);
        response.status = result.status();
        if (result.ok()) {
          response.session_id = result.value();
        }
        break;
      }
      case Method::SessionClose:
        response.status = impl_->close_session(request.session_id);
        break;
      case Method::JobSubmit: {
        auto result = impl_->submit(request.session_id, request.submit_options);
        response.status = result.status();
        if (result.ok()) {
          response.job_id = result.value();
        }
        break;
      }
      case Method::JobStatus: {
        auto result = impl_->jobs.status(request.job_id);
        response.status = result.status();
        if (result.ok()) {
          response.job_status = result.value();
        }
        break;
      }
      case Method::JobCancel:
        response.status = impl_->jobs.cancel(request.job_id);
        break;
      case Method::JobResult: {
        auto result = impl_->jobs.result(request.job_id);
        response.status = result.status();
        if (result.ok()) {
          response.execution_result = result.value();
        }
        break;
      }
      case Method::JobRelease:
        response.status = impl_->jobs.release(request.job_id);
        break;
      case Method::DaemonInfo:
        response.status = Status::success();
        response.daemon_info.protocol_version = kProtocolVersion;
        response.daemon_info.instance_id = impl_->instance_id;
        response.daemon_info.service_version = PHOTOSPIDER_DAEMON_VERSION;
        response.daemon_info.transport = "unix-domain";
        response.daemon_info.methods = method_inventory();
        response.daemon_info.active_sessions = impl_->session_count();
        response.daemon_info.active_jobs = impl_->jobs.size();
        response.daemon_info.maximum_concurrency =
            impl_->config.maximum_concurrency;
        response.daemon_info.maximum_sessions = impl_->config.maximum_sessions;
        break;
      case Method::DaemonShutdown:
        response.status = Status::success();
        accept_shutdown = true;
        break;
    }
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
    test::hit_exception_fence_fault(
        test::ExceptionFenceFaultPoint::DispatchPrimary);
#endif
    if (accept_shutdown) {
      impl_->shutting_down.store(true, std::memory_order_release);
      response.shutdown_after_write = true;
    }
  } catch (const std::bad_alloc&) {
    fail_response_without_allocation(response, ErrorCode::ResourceExhausted);
    try {
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
      test::hit_exception_fence_fault(
          test::ExceptionFenceFaultPoint::DispatchFailureStatus);
#endif
      response.status =
          Status::failure(ErrorCode::ResourceExhausted,
                          "daemon orchestration allocation failed");
    } catch (...) {
      fail_response_without_allocation(response, ErrorCode::ResourceExhausted);
    }
  } catch (const std::exception& error) {
    fail_response_without_allocation(response, ErrorCode::Internal);
    try {
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
      test::hit_exception_fence_fault(
          test::ExceptionFenceFaultPoint::DispatchFailureStatus);
#endif
      response.status = Status::failure(ErrorCode::Internal, error.what());
    } catch (...) {
      fail_response_without_allocation(response, ErrorCode::Internal);
    }
  } catch (...) {
    fail_response_without_allocation(response, ErrorCode::Internal);
    try {
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
      test::hit_exception_fence_fault(
          test::ExceptionFenceFaultPoint::DispatchFailureStatus);
#endif
      response.status = Status::failure(
          ErrorCode::Internal,
          "daemon orchestration raised a nonstandard exception");
    } catch (...) {
      fail_response_without_allocation(response, ErrorCode::Internal);
    }
  }
  return response;
}

}  // namespace ps::ipc::internal
