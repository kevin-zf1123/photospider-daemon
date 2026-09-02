#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ipc/codec.hpp"
#include "ipc/frame.hpp"
#include "ipc/unix_socket.hpp"
#include "orchestration/service.hpp"
#include "photospider/ipc/client.hpp"
#include "server/server.hpp"
#include "support/exception_fence_faults.hpp"
#include "support/server_run_guard.hpp"
#include "support/test_support.hpp"

namespace internal = ps::ipc::internal;

namespace {

/** @brief Interval between public Job-status observations. */
constexpr std::chrono::milliseconds kJobStatusPollInterval{5};

/** @brief Deadline budget for observing one expected public Job state. */
constexpr std::chrono::seconds kJobStatusTimeout{4};

/** @brief Long bounded operation delay used to occupy execution workers. */
constexpr std::int64_t kWorkerSaturationDelayMilliseconds = 5000;

/** @brief Maximum accepted close latency for cancelling long operations. */
constexpr std::chrono::seconds kSessionCloseTimeout{2};

/** @brief Short proof window for one close that must remain blocked. */
constexpr std::chrono::milliseconds kCloseBlockedObservation{100};

/**
 * @brief Holds the first Job after Running publication and before compilation.
 *
 * @note Destruction releases and waits for an entered observer, making early
 * test returns safe before ServerRunGuard begins worker shutdown.
 */
class JobRunningGate final {
 public:
  /**
   * @brief Releases an entered worker and waits until it leaves the callback.
   * @throws Nothing.
   */
  ~JobRunningGate() noexcept {
    try {
      release();
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [this] { return !entered_ || exited_; });
    } catch (...) {
    }
  }

  /**
   * @brief Blocks exactly the first observed Job-Running boundary.
   * @throws std::system_error If test synchronization fails.
   * @note Later Jobs pass immediately so another worker can execute Session B.
   */
  void hold_first() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (entered_) {
      return;
    }
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] { return released_; });
    exited_ = true;
    changed_.notify_all();
  }

  /**
   * @brief Waits for the first worker to enter the held boundary.
   * @param timeout Maximum bounded wait.
   * @return True when Running was observed before the deadline.
   * @throws std::system_error If test synchronization fails.
   */
  bool wait_until_entered(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] { return entered_; });
  }

  /**
   * @brief Releases the held worker idempotently.
   * @throws std::system_error If test synchronization fails.
   */
  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    changed_.notify_all();
  }

 private:
  /** @brief Serializes deterministic gate state. */
  std::mutex mutex_;
  /** @brief Wakes test and worker on entry/release/exit. */
  std::condition_variable changed_;
  /** @brief Whether the first Running boundary entered. */
  bool entered_ = false;
  /** @brief Whether the held worker may continue. */
  bool released_ = false;
  /** @brief Whether the held observer callback has returned. */
  bool exited_ = false;
};

/**
 * @brief Holds the first Session create after capacity reservation.
 *
 * @note Later creates pass immediately, allowing the test to prove that one
 * pending compiler boundary neither holds lifecycle serialization nor becomes
 * visible as an active Session.
 */
class PendingSessionCreateGate final {
 public:
  /**
   * @brief Creates an idle gate that will hold the first observed create.
   * @throws Nothing.
   */
  PendingSessionCreateGate() noexcept = default;

  /**
   * @brief Releases and drains an entered observer during failure cleanup.
   * @throws Nothing.
   */
  ~PendingSessionCreateGate() noexcept {
    try {
      release();
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [this] { return !entered_ || exited_; });
    } catch (...) {
    }
  }

  /**
   * @brief Forbids duplicating address-stable synchronization state.
   * @param other Source gate that cannot be copied.
   * @throws Nothing; the operation is deleted.
   */
  PendingSessionCreateGate(const PendingSessionCreateGate& other) = delete;
  /**
   * @brief Forbids assigning address-stable synchronization state.
   * @param other Source gate that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  PendingSessionCreateGate& operator=(const PendingSessionCreateGate& other) =
      delete;
  /**
   * @brief Forbids moving state borrowed by a process-global observer.
   * @param other Source gate that cannot be moved.
   * @throws Nothing; the operation is deleted.
   */
  PendingSessionCreateGate(PendingSessionCreateGate&& other) = delete;
  /**
   * @brief Forbids move-assigning borrowed synchronization state.
   * @param other Source gate that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  PendingSessionCreateGate& operator=(PendingSessionCreateGate&& other) =
      delete;

  /**
   * @brief Blocks exactly the first post-reservation create boundary.
   * @return No value.
   * @throws std::system_error If test synchronization fails.
   */
  void hold_first() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (entered_) {
      return;
    }
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] { return released_; });
    exited_ = true;
    changed_.notify_all();
  }

  /**
   * @brief Waits for the first pending create to enter the observer.
   * @param timeout Maximum bounded wait.
   * @return True when entry is observed before the deadline.
   * @throws std::system_error If test synchronization fails.
   */
  bool wait_until_entered(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] { return entered_; });
  }

  /**
   * @brief Releases the held pending create idempotently.
   * @return No value.
   * @throws std::system_error If test synchronization fails.
   */
  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    changed_.notify_all();
  }

 private:
  /** @brief Serializes deterministic gate state. */
  std::mutex mutex_;
  /** @brief Wakes the test and held create on state changes. */
  std::condition_variable changed_;
  /** @brief Whether the first pending create reached the observer. */
  bool entered_ = false;
  /** @brief Whether the held create may continue. */
  bool released_ = false;
  /** @brief Whether the held observer callback returned. */
  bool exited_ = false;
};

/**
 * @brief Holds one filtered result handler after shared-record lookup.
 *
 * @note A separate no-throw retirement callback observes final JobRecord
 * destruction after every handler, registry, worker, and local shared owner has
 * released the same record.
 */
class JobResultAfterFindGate final {
 public:
  /**
   * @brief Creates an idle after-find and retirement observation gate.
   * @throws Nothing.
   */
  JobResultAfterFindGate() noexcept = default;

  /**
   * @brief Releases and drains an entered result observer during cleanup.
   * @throws Nothing.
   */
  ~JobResultAfterFindGate() noexcept { drain(); }

  /**
   * @brief Forbids duplicating address-stable synchronization state.
   * @param other Source gate that cannot be copied.
   * @throws Nothing; the operation is deleted.
   */
  JobResultAfterFindGate(const JobResultAfterFindGate& other) = delete;

  /**
   * @brief Forbids assigning address-stable synchronization state.
   * @param other Source gate that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  JobResultAfterFindGate& operator=(const JobResultAfterFindGate& other) =
      delete;

  /**
   * @brief Forbids moving state borrowed by process-global test observers.
   * @param other Source gate that cannot be moved.
   * @throws Nothing; the operation is deleted.
   */
  JobResultAfterFindGate(JobResultAfterFindGate&& other) = delete;

  /**
   * @brief Forbids move-assigning borrowed synchronization state.
   * @param other Source gate that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  JobResultAfterFindGate& operator=(JobResultAfterFindGate&& other) = delete;

  /**
   * @brief Blocks the single controller-filtered result handler.
   * @param id Exact observed JobId, retained for test diagnostics.
   * @return No value.
   * @throws std::system_error If test synchronization fails.
   */
  void hold(ps::ipc::JobId id) {
    std::unique_lock<std::mutex> lock(mutex_);
    observed_id_ = id;
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] { return released_; });
    exited_ = true;
    changed_.notify_all();
  }

  /**
   * @brief Waits until the result handler reaches the after-find boundary.
   * @param timeout Maximum bounded wait.
   * @return True when the filtered handler entered before the deadline.
   * @throws std::system_error If test synchronization fails.
   */
  bool wait_until_entered(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] { return entered_; });
  }

  /**
   * @brief Releases the held result handler idempotently.
   * @return No value.
   * @throws std::system_error If test synchronization fails.
   */
  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    changed_.notify_all();
  }

  /**
   * @brief Records final destruction of the filtered JobRecord.
   * @param id Exact retired JobId.
   * @return No value.
   * @throws Nothing; synchronization failures are swallowed in the observer.
   */
  void record_retirement(ps::ipc::JobId id) noexcept {
    try {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        retired_id_ = id;
        retired_ = true;
      }
      changed_.notify_all();
    } catch (...) {
    }
  }

  /**
   * @brief Waits until final matching JobRecord destruction is observed.
   * @param timeout Maximum bounded wait.
   * @return True when retirement occurred before the deadline.
   * @throws std::system_error If test synchronization fails.
   */
  bool wait_until_retired(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] { return retired_; });
  }

  /**
   * @brief Reports whether entry and retirement observed the same target id.
   * @return True only when both complete nonzero identifiers match.
   * @throws Nothing.
   */
  bool observed_same_job() const noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      return observed_id_.instance != 0U && observed_id_.value != 0U &&
             observed_id_.instance == retired_id_.instance &&
             observed_id_.value == retired_id_.value;
    } catch (...) {
      return false;
    }
  }

  /**
   * @brief Releases and waits for an entered callback without throwing.
   * @return No value.
   * @throws Nothing.
   */
  void drain() noexcept {
    try {
      release();
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [this] { return !entered_ || exited_; });
    } catch (...) {
    }
  }

 private:
  /** @brief Serializes after-find and retirement observations. */
  mutable std::mutex mutex_;
  /** @brief Wakes test, result handler, and cleanup on state changes. */
  std::condition_variable changed_;
  /** @brief JobId seen strictly after registry lookup. */
  ps::ipc::JobId observed_id_;
  /** @brief JobId seen at final shared-owner retirement. */
  ps::ipc::JobId retired_id_;
  /** @brief Whether the filtered result handler entered. */
  bool entered_ = false;
  /** @brief Whether the held result handler may continue. */
  bool released_ = false;
  /** @brief Whether the after-find observer returned. */
  bool exited_ = false;
  /** @brief Whether final JobRecord destruction was observed. */
  bool retired_ = false;
};

/**
 * @brief Process-lifetime deterministic gate for one filtered Job boundary.
 *
 * @note The gate is reused only after `reset()` while no prior callback is
 * active. Process-lifetime storage prevents a callback from observing freed
 * synchronization state during failure cleanup.
 */
class JobArbitrationGate final {
 public:
  /** @brief Constructs one idle deterministic gate. @throws Nothing. */
  JobArbitrationGate() noexcept = default;

  /** @brief Releases and drains any entered callback. @throws Nothing. */
  ~JobArbitrationGate() noexcept { drain(); }

  /**
   * @brief Forbids copying synchronization state.
   * @param other Source gate that cannot be copied.
   * @throws Nothing; the operation is deleted.
   */
  JobArbitrationGate(const JobArbitrationGate& other) = delete;

  /**
   * @brief Forbids assigning synchronization state.
   * @param other Source gate that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  JobArbitrationGate& operator=(const JobArbitrationGate& other) = delete;

  /**
   * @brief Forbids moving process-lifetime synchronization state.
   * @param other Source gate that cannot be moved.
   * @throws Nothing; the operation is deleted.
   */
  JobArbitrationGate(JobArbitrationGate&& other) = delete;

  /**
   * @brief Forbids move-assigning synchronization state.
   * @param other Source gate that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  JobArbitrationGate& operator=(JobArbitrationGate&& other) = delete;

  /**
   * @brief Reinitializes the gate before one externally serialized install.
   * @return No value.
   * @throws std::system_error If mutex synchronization fails.
   * @note No callback may be active while reset runs.
   */
  void reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    observed_id_ = {};
    entered_ = false;
    released_ = false;
    exited_ = false;
  }

  /**
   * @brief Holds one no-throw test-runtime callback until released.
   * @param id Exact JobId delivered by the filtered controller.
   * @return No value.
   * @throws Nothing; synchronization failures are contained.
   */
  void hold(ps::ipc::JobId id) noexcept {
    try {
      std::unique_lock<std::mutex> lock(mutex_);
      observed_id_ = id;
      entered_ = true;
      changed_.notify_all();
      changed_.wait(lock, [this] { return released_; });
      exited_ = true;
      changed_.notify_all();
    } catch (...) {
      try {
        std::lock_guard<std::mutex> lock(mutex_);
        exited_ = true;
      } catch (...) {
      }
      changed_.notify_all();
    }
  }

  /**
   * @brief Waits for the filtered callback to enter.
   * @param timeout Maximum bounded wait.
   * @return True when entry occurs before the deadline.
   * @throws std::system_error If condition-variable synchronization fails.
   */
  bool wait_until_entered(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] { return entered_; });
  }

  /**
   * @brief Waits for the filtered callback to return from its hold.
   * @param timeout Maximum bounded wait.
   * @return True when callback exit occurs before the deadline.
   * @throws std::system_error If condition-variable synchronization fails.
   */
  bool wait_until_exited(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] { return exited_; });
  }

  /**
   * @brief Releases an entered or future callback idempotently.
   * @return No value.
   * @throws Nothing; synchronization failures are contained.
   */
  void release() noexcept {
    try {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
      }
      changed_.notify_all();
    } catch (...) {
    }
  }

  /**
   * @brief Reports whether the callback observed the expected JobId.
   * @param id Expected complete process-scoped JobId.
   * @return True only when the observed identifier matches exactly.
   * @throws Nothing.
   */
  bool observed(ps::ipc::JobId id) const noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      return observed_id_.instance == id.instance &&
             observed_id_.value == id.value;
    } catch (...) {
      return false;
    }
  }

  /**
   * @brief Releases and waits for any entered callback during cleanup.
   * @return No value.
   * @throws Nothing.
   */
  void drain() noexcept {
    release();
    try {
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [this] { return !entered_ || exited_; });
    } catch (...) {
    }
  }

 private:
  /** @brief Serializes entry, release, exit, and identity observations. */
  mutable std::mutex mutex_;
  /** @brief Wakes the test thread and observed callback. */
  std::condition_variable changed_;
  /** @brief Exact JobId delivered by the installed controller. */
  ps::ipc::JobId observed_id_;
  /** @brief Whether the target callback entered. */
  bool entered_ = false;
  /** @brief Whether the callback may return. */
  bool released_ = false;
  /** @brief Whether the callback completed its hold. */
  bool exited_ = false;
};

/**
 * @brief Returns process-lifetime final-publication gate storage.
 * @return Mutable gate shared by one externally serialized regression.
 * @throws std::system_error If platform synchronization construction fails.
 */
JobArbitrationGate& job_final_publication_gate() {
  static JobArbitrationGate gate;
  return gate;
}

/**
 * @brief Returns process-lifetime Session-close arbitration gate storage.
 * @return Mutable gate shared by one externally serialized regression.
 * @throws std::system_error If platform synchronization construction fails.
 */
JobArbitrationGate& session_close_cancellation_gate() {
  static JobArbitrationGate gate;
  return gate;
}

/** @brief Borrowed active Running gate for one scoped regression. */
std::atomic<JobRunningGate*> g_job_running_gate{nullptr};

/** @brief Borrowed active pending-create gate for one scoped regression. */
std::atomic<PendingSessionCreateGate*> g_pending_session_create_gate{nullptr};

/** @brief Borrowed active result/retirement gate for one scoped regression. */
std::atomic<JobResultAfterFindGate*> g_job_result_after_find_gate{nullptr};

/**
 * @brief Bridges one filtered final-publication seam into static gate storage.
 * @param id Exact JobId being finalized.
 * @param point Exact point already filtered by the controller.
 * @return No value.
 * @throws Nothing.
 */
void hold_job_final_publication(
    ps::ipc::JobId id, ps::ipc::test::JobFinalPublicationPoint point) noexcept {
  static_cast<void>(point);
  try {
    job_final_publication_gate().hold(id);
  } catch (...) {
  }
}

/**
 * @brief Bridges one filtered close-cancellation seam into static gate state.
 * @param id Exact popped/running JobId being arbitrated.
 * @return No value.
 * @throws Nothing.
 */
void hold_session_close_cancellation(ps::ipc::JobId id) noexcept {
  try {
    session_close_cancellation_gate().hold(id);
  } catch (...) {
  }
}

/**
 * @brief Bridges the noninstalled post-Running observer into its gate.
 * @throws Any test synchronization exception, fenced by the observer boundary.
 */
void hold_first_running_job() {
  JobRunningGate* gate = g_job_running_gate.load(std::memory_order_acquire);
  if (gate) {
    gate->hold_first();
  }
}

/**
 * @brief Bridges the noninstalled pending-create observer into its gate.
 * @return No value.
 * @throws Any test synchronization exception, fenced by the observer boundary.
 */
void hold_first_pending_session_create() {
  PendingSessionCreateGate* gate =
      g_pending_session_create_gate.load(std::memory_order_acquire);
  if (gate) {
    gate->hold_first();
  }
}

/**
 * @brief Bridges one filtered noninstalled after-find observer into its gate.
 * @param id Exact JobId whose shared record was retained.
 * @return No value.
 * @throws Any synchronization exception, fenced by the observer boundary.
 */
void hold_job_result_after_find(ps::ipc::JobId id) {
  JobResultAfterFindGate* gate =
      g_job_result_after_find_gate.load(std::memory_order_acquire);
  if (gate) {
    gate->hold(id);
  }
}

/**
 * @brief Bridges one filtered JobRecord retirement into its no-throw gate.
 * @param id Exact retired JobId.
 * @return No value.
 * @throws Nothing.
 */
void record_job_retirement(ps::ipc::JobId id) noexcept {
  JobResultAfterFindGate* gate =
      g_job_result_after_find_gate.load(std::memory_order_acquire);
  if (gate) {
    gate->record_retirement(id);
  }
}

/** @brief Scoped installation of the noninstalled Job-Running observer. */
class JobRunningHookScope final {
 public:
  /**
   * @brief Installs one borrowed deterministic gate.
   * @param gate Nonnull gate that outlives this scope.
   * @throws std::invalid_argument If gate is null.
   */
  explicit JobRunningHookScope(JobRunningGate* gate) {
    if (!gate) {
      throw std::invalid_argument("JobRunningHookScope requires a gate");
    }
    g_job_running_gate.store(gate, std::memory_order_release);
    ps::ipc::test::install_job_running_observer(&hold_first_running_job);
  }

  /**
   * @brief Clears the observer before the gate releases or is destroyed.
   * @throws Nothing.
   */
  ~JobRunningHookScope() noexcept {
    ps::ipc::test::install_job_running_observer(nullptr);
    g_job_running_gate.store(nullptr, std::memory_order_release);
  }

  /**
   * @brief Forbids duplicate process-global hook ownership.
   * @param other Source scope that cannot be copied.
   * @throws Nothing; the operation is deleted.
   */
  JobRunningHookScope(const JobRunningHookScope& other) = delete;
  /**
   * @brief Forbids assigning process-global hook ownership.
   * @param other Source scope that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  JobRunningHookScope& operator=(const JobRunningHookScope& other) = delete;
};

/**
 * @brief Scoped one-shot installation of a final-publication observer.
 *
 * @note Fixed process-lifetime gate storage is reset before publication and
 * drained after the controller is cleared, preventing callback UAF or reuse
 * across sequential regressions.
 */
class JobFinalPublicationHookScope final {
 public:
  /**
   * @brief Installs one exact JobId and final-publication point.
   * @param id Already-published target JobId.
   * @param point Exact worker boundary to hold.
   * @throws std::invalid_argument If the identifier is zero.
   * @throws std::system_error If gate reset synchronization fails.
   */
  JobFinalPublicationHookScope(ps::ipc::JobId id,
                               ps::ipc::test::JobFinalPublicationPoint point) {
    if (id.instance == 0U || id.value == 0U) {
      throw std::invalid_argument(
          "JobFinalPublicationHookScope requires a valid id");
    }
    job_final_publication_gate().reset();
    ps::ipc::test::install_job_final_publication_observer(
        id, point, &hold_job_final_publication);
  }

  /** @brief Clears, releases, and drains the one-shot seam. @throws Nothing. */
  ~JobFinalPublicationHookScope() noexcept {
    ps::ipc::test::install_job_final_publication_observer(
        {}, ps::ipc::test::JobFinalPublicationPoint::BeforeRecordLock, nullptr);
    try {
      job_final_publication_gate().drain();
    } catch (...) {
    }
  }

  /**
   * @brief Forbids duplicate controller ownership.
   * @param other Source scope that cannot be copied.
   * @throws Nothing; the operation is deleted.
   */
  JobFinalPublicationHookScope(const JobFinalPublicationHookScope& other) =
      delete;

  /**
   * @brief Forbids assigning duplicate controller ownership.
   * @param other Source scope that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  JobFinalPublicationHookScope& operator=(
      const JobFinalPublicationHookScope& other) = delete;

  /**
   * @brief Forbids moving process-global controller ownership.
   * @param other Source scope that cannot be moved.
   * @throws Nothing; the operation is deleted.
   */
  JobFinalPublicationHookScope(JobFinalPublicationHookScope&& other) = delete;

  /**
   * @brief Forbids move-assigning process-global controller ownership.
   * @param other Source scope that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  JobFinalPublicationHookScope& operator=(
      JobFinalPublicationHookScope&& other) = delete;
};

/**
 * @brief Scoped one-shot installation of Session-close cancellation observer.
 */
class SessionCloseCancellationHookScope final {
 public:
  /**
   * @brief Installs one exact popped/running JobId filter.
   * @param id Already-published target JobId.
   * @throws std::invalid_argument If the identifier is zero.
   * @throws std::system_error If gate reset synchronization fails.
   */
  explicit SessionCloseCancellationHookScope(ps::ipc::JobId id) {
    if (id.instance == 0U || id.value == 0U) {
      throw std::invalid_argument(
          "SessionCloseCancellationHookScope requires a valid id");
    }
    session_close_cancellation_gate().reset();
    ps::ipc::test::install_session_close_cancellation_observer(
        id, &hold_session_close_cancellation);
  }

  /** @brief Clears, releases, and drains the one-shot seam. @throws Nothing. */
  ~SessionCloseCancellationHookScope() noexcept {
    ps::ipc::test::install_session_close_cancellation_observer({}, nullptr);
    try {
      session_close_cancellation_gate().drain();
    } catch (...) {
    }
  }

  /**
   * @brief Forbids duplicate controller ownership.
   * @param other Source scope that cannot be copied.
   * @throws Nothing; the operation is deleted.
   */
  SessionCloseCancellationHookScope(
      const SessionCloseCancellationHookScope& other) = delete;

  /**
   * @brief Forbids assigning duplicate controller ownership.
   * @param other Source scope that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  SessionCloseCancellationHookScope& operator=(
      const SessionCloseCancellationHookScope& other) = delete;

  /**
   * @brief Forbids moving process-global controller ownership.
   * @param other Source scope that cannot be moved.
   * @throws Nothing; the operation is deleted.
   */
  SessionCloseCancellationHookScope(SessionCloseCancellationHookScope&& other) =
      delete;

  /**
   * @brief Forbids move-assigning process-global controller ownership.
   * @param other Source scope that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  SessionCloseCancellationHookScope& operator=(
      SessionCloseCancellationHookScope&& other) = delete;
};

/** @brief Scoped installation of the noninstalled pending-create observer. */
class PendingSessionCreateHookScope final {
 public:
  /**
   * @brief Installs one borrowed deterministic pending-create gate.
   * @param gate Nonnull gate that outlives this scope.
   * @throws std::invalid_argument If gate is null.
   */
  explicit PendingSessionCreateHookScope(PendingSessionCreateGate* gate) {
    if (!gate) {
      throw std::invalid_argument(
          "PendingSessionCreateHookScope requires a gate");
    }
    g_pending_session_create_gate.store(gate, std::memory_order_release);
    ps::ipc::test::install_session_create_pending_observer(
        &hold_first_pending_session_create);
  }

  /**
   * @brief Clears the observer before the gate retires.
   * @throws Nothing.
   */
  ~PendingSessionCreateHookScope() noexcept {
    ps::ipc::test::install_session_create_pending_observer(nullptr);
    g_pending_session_create_gate.store(nullptr, std::memory_order_release);
  }

  /**
   * @brief Forbids duplicate process-global hook ownership.
   * @param other Source scope that cannot be copied.
   * @throws Nothing; the operation is deleted.
   */
  PendingSessionCreateHookScope(const PendingSessionCreateHookScope& other) =
      delete;
  /**
   * @brief Forbids assigning process-global hook ownership.
   * @param other Source scope that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  PendingSessionCreateHookScope& operator=(
      const PendingSessionCreateHookScope& other) = delete;
  /**
   * @brief Forbids moving one process-global observer installation.
   * @param other Source scope that cannot be moved.
   * @throws Nothing; the operation is deleted.
   */
  PendingSessionCreateHookScope(PendingSessionCreateHookScope&& other) = delete;
  /**
   * @brief Forbids move-assigning process-global observer ownership.
   * @param other Source scope that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  PendingSessionCreateHookScope& operator=(
      PendingSessionCreateHookScope&& other) = delete;
};

/**
 * @brief Scoped installation of filtered result and retirement observers.
 *
 * @note The scope releases/drains an entered result callback before clearing
 * borrowed gate storage, so early assertion returns cannot strand a handler.
 */
class JobResultLifetimeHookScope final {
 public:
  /**
   * @brief Installs one exact JobId filter over a nonnull borrowed gate.
   * @param id Target JobId, already published by submit.
   * @param gate Address-stable gate that outlives this scope.
   * @throws std::invalid_argument If id or gate is invalid.
   */
  JobResultLifetimeHookScope(ps::ipc::JobId id, JobResultAfterFindGate* gate)
      : gate_(gate) {
    if (!gate_ || id.instance == 0U || id.value == 0U) {
      throw std::invalid_argument(
          "JobResultLifetimeHookScope requires a valid id and gate");
    }
    g_job_result_after_find_gate.store(gate_, std::memory_order_release);
    ps::ipc::test::install_job_record_retirement_observer(
        id, &record_job_retirement);
    ps::ipc::test::install_job_result_after_find_observer(
        id, &hold_job_result_after_find);
  }

  /**
   * @brief Releases the handler and clears both observers before gate teardown.
   * @throws Nothing.
   */
  ~JobResultLifetimeHookScope() noexcept {
    gate_->drain();
    ps::ipc::test::install_job_result_after_find_observer({}, nullptr);
    ps::ipc::test::install_job_record_retirement_observer({}, nullptr);
    g_job_result_after_find_gate.store(nullptr, std::memory_order_release);
  }

  /**
   * @brief Forbids duplicate process-global hook ownership.
   * @param other Source scope that cannot be copied.
   * @throws Nothing; the operation is deleted.
   */
  JobResultLifetimeHookScope(const JobResultLifetimeHookScope& other) = delete;

  /**
   * @brief Forbids assigning process-global hook ownership.
   * @param other Source scope that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  JobResultLifetimeHookScope& operator=(
      const JobResultLifetimeHookScope& other) = delete;

  /**
   * @brief Forbids moving one address-bound observer installation.
   * @param other Source scope that cannot be moved.
   * @throws Nothing; the operation is deleted.
   */
  JobResultLifetimeHookScope(JobResultLifetimeHookScope&& other) = delete;

  /**
   * @brief Forbids move-assigning process-global observer ownership.
   * @param other Source scope that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  JobResultLifetimeHookScope& operator=(JobResultLifetimeHookScope&& other) =
      delete;

 private:
  /** @brief Borrowed gate released and drained during scope teardown. */
  JobResultAfterFindGate* gate_;
};

/**
 * @brief Owns one async RPC whose cleanup first releases its deterministic
 * gate.
 * @tparam T Complete RPC result type.
 * @tparam Gate Address-stable gate type exposing idempotent `release()`.
 *
 * @note This guard prevents an early test return from blocking in
 * `std::future` destruction while the matching RPC still waits on the gate.
 */
template <typename T, typename Gate = JobRunningGate>
class GateReleasingFuture final {
 public:
  /**
   * @brief Launches one asynchronous RPC with fail-safe gate ownership.
   * @tparam Function Nullary callable returning T.
   * @param gate Nonnull gate released during unfinished cleanup.
   * @param function RPC callable copied or moved into `std::async`.
   * @throws Any exception raised while launching the async task.
   */
  template <typename Function>
  GateReleasingFuture(Gate* gate, Function&& function)
      : gate_(gate),
        future_(
            std::async(std::launch::async, std::forward<Function>(function))) {
    if (!gate_) {
      throw std::invalid_argument("GateReleasingFuture requires a gate");
    }
  }

  /**
   * @brief Releases the gate and waits if the result was not consumed.
   * @throws Nothing.
   */
  ~GateReleasingFuture() noexcept {
    if (!future_.valid()) {
      return;
    }
    try {
      gate_->release();
      future_.wait();
    } catch (...) {
    }
  }

  /**
   * @brief Forbids duplicate future/gate cleanup ownership.
   * @param other Source guard that cannot be copied.
   * @throws Nothing; the operation is deleted.
   */
  GateReleasingFuture(const GateReleasingFuture& other) = delete;
  /**
   * @brief Forbids assigning duplicate future/gate cleanup ownership.
   * @param other Source guard that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  GateReleasingFuture& operator=(const GateReleasingFuture& other) = delete;

  /**
   * @brief Waits for bounded RPC readiness without releasing the gate.
   * @param timeout Maximum observation interval.
   * @return True when the async result is ready.
   * @throws std::future_error If the result was already consumed.
   */
  bool ready_within(std::chrono::milliseconds timeout) {
    return future_.wait_for(timeout) == std::future_status::ready;
  }

  /**
   * @brief Consumes the complete asynchronous RPC result.
   * @return Result returned by the callable.
   * @throws Any exception propagated from the callable.
   */
  T get() { return future_.get(); }

 private:
  /** @brief Borrowed gate released before unfinished wait cleanup. */
  Gate* gate_;
  /** @brief Sole asynchronous RPC result owner. */
  std::future<T> future_;
};

/**
 * @brief Releases a held callback gate before async RPC cleanup.
 * @tparam Gate Address-stable gate type exposing no-throw `release()`.
 *
 * @note Declare this guard after result/close futures. Its destructor then
 * releases the worker before those futures release their own gates and wait,
 * preventing early-return cleanup from deadlocking.
 */
template <typename Gate>
class GateReleaseGuard final {
 public:
  /**
   * @brief Retains one nonnull gate for fail-safe release.
   * @param gate Gate whose callback may hold a worker or handler.
   * @throws std::invalid_argument If gate is null.
   */
  explicit GateReleaseGuard(Gate* gate) : gate_(gate) {
    if (!gate_) {
      throw std::invalid_argument("GateReleaseGuard requires a gate");
    }
  }

  /** @brief Releases the borrowed gate idempotently. @throws Nothing. */
  ~GateReleaseGuard() noexcept { gate_->release(); }

  /**
   * @brief Forbids duplicate release ownership.
   * @param other Source guard that cannot be copied.
   * @throws Nothing; the operation is deleted.
   */
  GateReleaseGuard(const GateReleaseGuard& other) = delete;

  /**
   * @brief Forbids assigning duplicate release ownership.
   * @param other Source guard that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  GateReleaseGuard& operator=(const GateReleaseGuard& other) = delete;

  /**
   * @brief Forbids moving release ownership.
   * @param other Source guard that cannot be moved.
   * @throws Nothing; the operation is deleted.
   */
  GateReleaseGuard(GateReleaseGuard&& other) = delete;

  /**
   * @brief Forbids move-assigning release ownership.
   * @param other Source guard that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  GateReleaseGuard& operator=(GateReleaseGuard&& other) = delete;

 private:
  /** @brief Borrowed process-lifetime gate released during cleanup. */
  Gate* gate_;
};

/**
 * @brief Builds one deterministic addition source document.
 * @param left First scalar operand.
 * @param right Second scalar operand.
 * @return Three-node public workflow with named `sum` output.
 * @throws std::bad_alloc If source storage allocation fails.
 * @note The document contains no internal compiler representation.
 */
ps::WorkflowDocument addition_document(double left, double right) {
  ps::WorkflowDocument document;
  document.nodes = {
      ps::WorkflowNode{1U, "core.constant", {}, {{"value", left}}},
      ps::WorkflowNode{2U, "core.constant", {}, {{"value", right}}},
      ps::WorkflowNode{
          3U,
          "math.add",
          {ps::WorkflowInput{1U, "value"}, ps::WorkflowInput{2U, "value"}},
          {}},
  };
  document.outputs = {ps::WorkflowOutput{"sum", 3U, "value"}};
  return document;
}

/**
 * @brief Builds one cooperative delay source document.
 * @param milliseconds Bounded delay parameter supplied to the operation.
 * @return Two-node public workflow with named `value` output.
 * @throws std::bad_alloc If source storage allocation fails.
 * @note The delay provides deterministic cancellation windows for tests.
 */
ps::WorkflowDocument delayed_document(std::int64_t milliseconds) {
  ps::WorkflowDocument document;
  document.nodes = {
      ps::WorkflowNode{1U, "core.constant", {}, {{"value", 7.0}}},
      ps::WorkflowNode{2U,
                       "core.delay",
                       {ps::WorkflowInput{1U, "value"}},
                       {{"milliseconds", milliseconds}}},
  };
  document.outputs = {ps::WorkflowOutput{"value", 2U, "value"}};
  return document;
}

/**
 * @brief Builds a wire-valid source with an unregistered operation key.
 * @return One-node public workflow that fails only during kernel compilation.
 * @throws std::bad_alloc If source storage allocation fails.
 * @note Encoding accepts this source; no Session may publish after compile
 * failure.
 */
ps::WorkflowDocument compiler_invalid_document() {
  ps::WorkflowDocument document;
  document.nodes = {
      ps::WorkflowNode{1U, "missing.operation", {}, {}},
  };
  document.outputs = {ps::WorkflowOutput{"value", 1U, "value"}};
  return document;
}

/**
 * @brief Polls one execution until a terminal state or bounded timeout.
 * @param client Connected sequential client.
 * @param id Existing execution identifier.
 * @return Terminal status.
 * @throws std::runtime_error On RPC failure or timeout.
 * @note Polling is test-only, uses one steady-clock deadline, and never changes
 * Job state.
 */
ps::ipc::JobStatus wait_terminal(ps::ipc::Client* client, ps::ipc::JobId id) {
  const auto deadline = std::chrono::steady_clock::now() + kJobStatusTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto status = client->job_status(id);
    if (!status.ok()) {
      throw std::runtime_error(status.status().message);
    }
    if (status.value().state == ps::ipc::JobState::Succeeded ||
        status.value().state == ps::ipc::JobState::Failed ||
        status.value().state == ps::ipc::JobState::Cancelled) {
      return status.value();
    }
    std::this_thread::sleep_for(kJobStatusPollInterval);
  }
  throw std::runtime_error("execution did not reach terminal state");
}

/**
 * @brief Observes a terminal Job through real IPC without elapsed sleeps.
 * @param client Connected sequential client.
 * @param id Existing execution identifier.
 * @return Terminal status.
 * @throws std::runtime_error On RPC failure or deadline expiry.
 * @note Each status round trip yields through the real socket/server boundary;
 * the helper is reserved for deterministic barrier regressions that must not
 * establish ordering with `sleep_for`.
 */
ps::ipc::JobStatus wait_terminal_without_sleep(ps::ipc::Client* client,
                                               ps::ipc::JobId id) {
  const auto deadline = std::chrono::steady_clock::now() + kJobStatusTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto status = client->job_status(id);
    if (!status.ok()) {
      throw std::runtime_error(status.status().message);
    }
    if (status.value().state == ps::ipc::JobState::Succeeded ||
        status.value().state == ps::ipc::JobState::Failed ||
        status.value().state == ps::ipc::JobState::Cancelled) {
      return status.value();
    }
  }
  throw std::runtime_error("execution did not reach terminal state");
}

/**
 * @brief Polls until one exact nonterminal/terminal state is observed.
 * @param client Connected sequential client.
 * @param id Existing execution identifier.
 * @param expected State to observe.
 * @return Matching snapshot.
 * @throws std::runtime_error On RPC failure, early terminal state, or timeout.
 * @note Polling is test-only, uses one steady-clock deadline, and never changes
 * Job state.
 */
ps::ipc::JobStatus wait_state(ps::ipc::Client* client, ps::ipc::JobId id,
                              ps::ipc::JobState expected) {
  const auto deadline = std::chrono::steady_clock::now() + kJobStatusTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto status = client->job_status(id);
    if (!status.ok()) {
      throw std::runtime_error(status.status().message);
    }
    if (status.value().state == expected) {
      return status.value();
    }
    if (status.value().state == ps::ipc::JobState::Succeeded ||
        status.value().state == ps::ipc::JobState::Failed ||
        status.value().state == ps::ipc::JobState::Cancelled) {
      throw std::runtime_error("execution reached terminal state too early");
    }
    std::this_thread::sleep_for(kJobStatusPollInterval);
  }
  throw std::runtime_error("execution did not reach expected state");
}

/**
 * @brief Waits until Session close removes one Job from public observation.
 * @param client Connected client whose handler is independent from close.
 * @param id Job owned by the Session being closed.
 * @param timeout Maximum bounded wait.
 * @return True when status becomes `NotFound`, false on timeout.
 * @throws std::runtime_error If another typed failure is observed.
 * @note Removal proves close reached the JobRegistry mutation/wait boundary.
 */
bool wait_job_not_found(ps::ipc::Client* client, ps::ipc::JobId id,
                        std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto status = client->job_status(id);
    if (!status.ok()) {
      if (status.status().code == ps::ErrorCode::NotFound) {
        return true;
      }
      throw std::runtime_error(status.status().message);
    }
    std::this_thread::sleep_for(kJobStatusPollInterval);
  }
  return false;
}

/**
 * @brief Observes one exact Job-registry count without elapsed sleeps.
 * @param client Connected client whose handler is independent from close.
 * @param expected Exact active Job count to observe.
 * @param timeout Maximum bounded wait.
 * @return True when `daemon.info` reports the expected count before deadline.
 * @throws std::runtime_error If the real IPC observation fails.
 * @note Repeated round trips yield through the socket/server boundary; only
 * condition-variable and future barriers establish test ordering.
 */
bool wait_active_job_count_without_sleep(ps::ipc::Client* client,
                                         std::uint64_t expected,
                                         std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto info = client->daemon_info();
    if (!info.ok()) {
      throw std::runtime_error(info.status().message);
    }
    if (info.value().active_jobs == expected) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Returns a process-unique short Unix-domain socket path.
 * @return Path under `/tmp` scoped by pid and monotonic test sequence.
 * @throws std::bad_alloc If path construction fails.
 * @note The function does not create a filesystem node.
 */
std::string socket_path() {
  static std::atomic<std::uint32_t> sequence{0U};
  return "/tmp/psd-v3-" + std::to_string(::getpid()) + "-" +
         std::to_string(sequence.fetch_add(1U)) + ".sock";
}

/**
 * @brief Appends one little-endian uint32 to a test payload.
 * @param payload Nonnull destination bytes.
 * @param value Unsigned value.
 * @throws std::bad_alloc If destination growth fails.
 */
void append_u32(std::vector<std::uint8_t>* payload, std::uint32_t value) {
  for (std::size_t index = 0U; index < 4U; ++index) {
    payload->push_back(
        static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
  }
}

/**
 * @brief Appends one little-endian uint64 to a test payload.
 * @param payload Nonnull destination bytes.
 * @param value Unsigned value.
 * @throws std::bad_alloc If destination growth fails.
 */
void append_u64(std::vector<std::uint8_t>* payload, std::uint64_t value) {
  for (std::size_t index = 0U; index < 8U; ++index) {
    payload->push_back(
        static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
  }
}

/**
 * @brief Appends bounded uint32-length-framed test text.
 * @param payload Nonnull destination bytes.
 * @param value Exact ASCII fixture bytes.
 * @throws std::bad_alloc If destination growth fails.
 */
void append_text(std::vector<std::uint8_t>* payload, const std::string& value) {
  append_u32(payload, static_cast<std::uint32_t>(value.size()));
  payload->insert(payload->end(), value.begin(), value.end());
}

/**
 * @brief Appends one tagged Float64 source parameter value.
 * @param payload Nonnull destination bytes.
 * @param value Exact binary64 value.
 * @throws std::bad_alloc If destination growth fails.
 */
void append_float64_parameter(std::vector<std::uint8_t>* payload,
                              double value) {
  payload->push_back(2U);
  std::uint64_t bits = 0U;
  std::memcpy(&bits, &value, sizeof(bits));
  append_u64(payload, bits);
}

/**
 * @brief Builds a syntactically framed request with a duplicate parameter key.
 * @param request_id Nonzero correlation id.
 * @return Malformed SessionCreate payload rejected before service mutation.
 * @throws std::bad_alloc If payload construction fails.
 */
std::vector<std::uint8_t> duplicate_parameter_request(
    std::uint64_t request_id) {
  using ps::ipc::internal::Method;
  std::vector<std::uint8_t> payload;
  payload.push_back(3U);
  payload.push_back(0U);
  append_u64(&payload, request_id);
  payload.push_back(static_cast<std::uint8_t>(Method::SessionCreate));
  append_u32(&payload, 1U);
  append_u32(&payload, 1U);
  append_u64(&payload, 1U);
  append_text(&payload, "core.constant");
  append_u32(&payload, 0U);
  append_u32(&payload, 2U);
  append_text(&payload, "value");
  append_float64_parameter(&payload, 1.0);
  append_text(&payload, "value");
  append_float64_parameter(&payload, 2.0);
  append_u32(&payload, 1U);
  append_text(&payload, "value");
  append_u64(&payload, 1U);
  append_text(&payload, "value");
  return payload;
}

/**
 * @brief Builds a frame whose valid request header is followed by body EOF.
 * @param request_id Nonzero correlation id encoded in the complete header.
 * @param method Known request method encoded in the complete header.
 * @return Four-byte declared length plus exactly eleven request-header bytes.
 * @throws std::bad_alloc If fixture storage allocation fails.
 * @note The declared payload is one byte longer than the returned payload.
 */
std::vector<std::uint8_t> correlated_truncated_frame(
    std::uint64_t request_id, ps::ipc::internal::Method method) {
  std::vector<std::uint8_t> bytes{0U, 0U, 0U, 12U, 3U, 0U};
  append_u64(&bytes, request_id);
  bytes.push_back(static_cast<std::uint8_t>(method));
  return bytes;
}

/**
 * @brief Captures one deterministic post-bind construction-failure outcome.
 *
 * @note Every field is computed after test-owned cleanup, so later cases can
 * reuse process descriptor and filesystem state.
 */
struct ConstructionFailureObservation final {
  /** @brief True when the injected `std::bad_alloc` propagated. */
  bool exception_propagated = false;
  /** @brief True when the listener descriptor returned to the free set. */
  bool descriptor_released = false;
  /** @brief True when no socket filesystem node remained. */
  bool socket_node_removed = false;
  /** @brief True when the same exact path could be bound again. */
  bool rebound = false;
};

/**
 * @brief Finds the lowest currently available descriptor number.
 * @return Descriptor number observed through a temporary `/dev/null` open.
 * @throws std::runtime_error If the probe cannot open `/dev/null`.
 * @note The probe closes its descriptor before returning.
 */
int next_available_descriptor() {
  const int descriptor = ::open("/dev/null", O_RDONLY);
  if (descriptor < 0) {
    throw std::runtime_error("could not probe descriptor inventory");
  }
  ::close(descriptor);
  return descriptor;
}

/**
 * @brief Injects one post-bind constructor failure and audits cleanup.
 * @param stage Exact private server construction stage that throws.
 * @return Descriptor/node/rebind observations after bounded cleanup.
 * @throws std::bad_alloc If test configuration allocation fails.
 * @throws std::runtime_error If descriptor probing fails.
 * @note A leaked descriptor/socket node is removed only after its failure is
 * recorded so the following test case remains isolated.
 */
ConstructionFailureObservation observe_construction_failure(
    ps::ipc::internal::ServerConstructionStage stage) {
  using ps::ipc::internal::Server;
  using ps::ipc::internal::ServerConfig;
  const std::string path = socket_path();
  const int descriptor_before = next_available_descriptor();
  ServerConfig config{
      path, ps::ipc::internal::ServiceConfig{1U, 4U, 2U, false}, 4, 2U, {}, {}};
  config.construction_hook = [stage](auto observed) {
    if (observed == stage) {
      throw std::bad_alloc();
    }
  };

  ConstructionFailureObservation observation;
  try {
    Server rejected(std::move(config));
  } catch (const std::bad_alloc&) {
    observation.exception_propagated = true;
  }

  struct stat existing{};
  errno = 0;
  observation.socket_node_removed =
      ::lstat(path.c_str(), &existing) != 0 && errno == ENOENT;
  const int descriptor_after = next_available_descriptor();
  observation.descriptor_released = descriptor_after == descriptor_before;

  if (!observation.socket_node_removed) {
    ::unlink(path.c_str());
  }
  if (!observation.descriptor_released) {
    ::close(descriptor_before);
  }
  try {
    Server rebound(
        ServerConfig{path,
                     ps::ipc::internal::ServiceConfig{1U, 4U, 2U, false},
                     4,
                     2U,
                     {},
                     {}});
    observation.rebound = true;
  } catch (...) {
    observation.rebound = false;
  }
  return observation;
}

/**
 * @brief Sends one malformed payload and reads the server typed error.
 * @param path Bound local server socket path.
 * @param payload Complete malformed frame payload.
 * @return Decoded pre-routing failure response.
 * @throws std::runtime_error If transport/response handling fails.
 * @note The helper verifies the server closes after one typed response.
 */
ps::ipc::internal::Response malformed_payload_response(
    const std::string& path, const std::vector<std::uint8_t>& payload) {
  using ps::ipc::internal::connect_unix_socket;
  using ps::ipc::internal::decode_protocol_error;
  using ps::ipc::internal::read_frame;
  using ps::ipc::internal::write_frame;
  auto connection = connect_unix_socket(path);
  if (!connection.ok() ||
      !write_frame(connection.value().get(), payload).ok()) {
    throw std::runtime_error("could not send malformed request payload");
  }
  auto frame = read_frame(connection.value().get());
  if (!frame.ok()) {
    throw std::runtime_error("server did not return a protocol error frame");
  }
  auto response = decode_protocol_error(frame.value());
  if (!response.ok()) {
    throw std::runtime_error(response.status().message);
  }
  auto closed = read_frame(connection.value().get());
  if (closed.ok() || closed.status().code != ps::ErrorCode::NotFound) {
    throw std::runtime_error("server did not close malformed connection");
  }
  return response.take_value();
}

/**
 * @brief Sends raw framed-stream bytes and reads one typed protocol error.
 * @param path Bound local server socket path.
 * @param bytes Exact raw bytes including any frame header.
 * @param finish_writes Whether to half-close writes to expose truncation.
 * @return Decoded sentinel/recovered failure response.
 * @throws std::runtime_error If transport or response handling fails.
 * @note The helper never asks the client codec to allocate the declared size
 * and verifies controlled EOF after the typed response.
 */
ps::ipc::internal::Response malformed_stream_response(
    const std::string& path, const std::vector<std::uint8_t>& bytes,
    bool finish_writes) {
  using ps::ipc::internal::connect_unix_socket;
  using ps::ipc::internal::decode_protocol_error;
  using ps::ipc::internal::read_frame;
  auto connection = connect_unix_socket(path);
  if (!connection.ok()) {
    throw std::runtime_error("could not connect malformed stream client");
  }
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const ssize_t count = ::write(connection.value().get(),
                                  bytes.data() + offset, bytes.size() - offset);
    if (count <= 0) {
      throw std::runtime_error("could not write malformed stream bytes");
    }
    offset += static_cast<std::size_t>(count);
  }
  if (finish_writes) {
    ::shutdown(connection.value().get(), SHUT_WR);
  }
  auto frame = read_frame(connection.value().get());
  if (!frame.ok()) {
    throw std::runtime_error("server did not return a stream protocol error");
  }
  auto response = decode_protocol_error(frame.value());
  if (!response.ok()) {
    throw std::runtime_error(response.status().message);
  }
  auto closed = read_frame(connection.value().get());
  if (closed.ok() || closed.status().code != ps::ErrorCode::NotFound) {
    throw std::runtime_error("server did not close malformed stream");
  }
  return response.take_value();
}

/**
 * @brief Waits until a server active-handler count reaches one expected value.
 * @param server Running server.
 * @param expected Exact target count.
 * @throws std::runtime_error If the bounded wait expires.
 * @note Polling is test-only and never mutates server lifecycle.
 */
void wait_handler_count(ps::ipc::internal::Server* server,
                        std::uint32_t expected) {
  for (int attempt = 0; attempt < 400; ++attempt) {
    if (server->active_handler_count() == expected) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  throw std::runtime_error("server handler count did not settle");
}

/**
 * @brief Settles old handlers and explicitly drives accept-loop record reaping.
 * @param server Running test-runtime Server.
 * @param path Exact bound socket path.
 * @throws std::runtime_error If active settlement or the trigger RPC fails.
 * @throws std::bad_alloc If client/response allocation fails.
 * @note Runtime reaping occurs before a newly accepted handler is recorded.
 * Waiting for active zero first makes every old completion flag observable;
 * the trigger connection then causes those old records to be joined/erased.
 */
void settle_handlers_and_trigger_reap(ps::ipc::internal::Server* server,
                                      const std::string& path) {
  wait_handler_count(server, 0U);
  ps::ipc::Client trigger;
  if (!trigger.connect(path).ok() || !trigger.daemon_info().ok()) {
    throw std::runtime_error("could not trigger finished-handler reaping");
  }
  trigger.disconnect();
  wait_handler_count(server, 0U);
}

/**
 * @brief Proves pending Session creation reserves only capacity, not locks or
 * identity.
 * @return Zero when capacity, lifecycle responsiveness, visibility, id order,
 * and cleanup are exact.
 * @throws std::bad_alloc If test/server/RPC staging allocation fails.
 * @throws std::system_error If asynchronous or barrier synchronization fails.
 * @note Coordination uses only the noninstalled post-reservation observer and
 * bounded future waits; no elapsed sleep establishes a lifecycle state.
 */
int verify_pending_session_create_reservation() {
  using ps::ErrorCode;
  using ps::ipc::Client;
  using ps::ipc::JobId;
  using ps::ipc::SessionId;

  const std::string path = socket_path();
  internal::Server server(
      internal::ServerConfig{path,
                             internal::ServiceConfig{1U, 8U, 2U, false},
                             32,
                             32U,
                             {},
                             {}});
  ps::ipc::test::ServerRunGuard server_run(&server);
  Client observer;
  Client create_a_client;
  Client submit_b_client;
  Client capacity_client;
  Client close_b_client;
  Client create_c_client;
  PS_IPC_CHECK(observer.connect(path).ok());
  PS_IPC_CHECK(create_a_client.connect(path).ok());
  PS_IPC_CHECK(submit_b_client.connect(path).ok());
  PS_IPC_CHECK(capacity_client.connect(path).ok());
  PS_IPC_CHECK(close_b_client.connect(path).ok());
  PS_IPC_CHECK(create_c_client.connect(path).ok());

  auto session_b = observer.session_create(addition_document(1.0, 2.0));
  PS_IPC_CHECK(session_b.ok());
  PS_IPC_CHECK(session_b.value().value == 1U);

  PendingSessionCreateGate pending_gate;
  PendingSessionCreateHookScope pending_hook(&pending_gate);
  GateReleasingFuture<ps::Result<SessionId>, PendingSessionCreateGate>
      pending_a(&pending_gate, [&] {
        return create_a_client.session_create(addition_document(3.0, 4.0));
      });
  PS_IPC_CHECK(pending_gate.wait_until_entered(kSessionCloseTimeout));

  auto pending_info = observer.daemon_info();
  PS_IPC_CHECK(pending_info.ok());
  PS_IPC_CHECK(pending_info.value().active_sessions == 1U);
  PS_IPC_CHECK(pending_info.value().maximum_sessions == 2U);

  GateReleasingFuture<ps::Result<JobId>, PendingSessionCreateGate> submit_b(
      &pending_gate,
      [&] { return submit_b_client.job_submit(session_b.value()); });
  PS_IPC_CHECK(submit_b.ready_within(kSessionCloseTimeout));
  auto job_b = submit_b.get();
  PS_IPC_CHECK(job_b.ok());

  GateReleasingFuture<ps::Result<SessionId>, PendingSessionCreateGate>
      at_capacity(&pending_gate, [&] {
        return capacity_client.session_create(addition_document(5.0, 6.0));
      });
  PS_IPC_CHECK(at_capacity.ready_within(kSessionCloseTimeout));
  auto rejected = at_capacity.get();
  PS_IPC_CHECK(!rejected.ok());
  PS_IPC_CHECK(rejected.status().code == ErrorCode::ResourceExhausted);

  GateReleasingFuture<ps::Status, PendingSessionCreateGate> close_b(
      &pending_gate,
      [&] { return close_b_client.session_close(session_b.value()); });
  PS_IPC_CHECK(close_b.ready_within(kSessionCloseTimeout));
  PS_IPC_CHECK(close_b.get().ok());
  auto removed_job = observer.job_status(job_b.value());
  PS_IPC_CHECK(!removed_job.ok());
  PS_IPC_CHECK(removed_job.status().code == ErrorCode::NotFound);

  GateReleasingFuture<ps::Result<SessionId>, PendingSessionCreateGate> create_c(
      &pending_gate, [&] {
        return create_c_client.session_create(addition_document(7.0, 8.0));
      });
  PS_IPC_CHECK(create_c.ready_within(kSessionCloseTimeout));
  auto session_c = create_c.get();
  PS_IPC_CHECK(session_c.ok());
  PS_IPC_CHECK(session_c.value().value == 2U);
  auto c_info = observer.daemon_info();
  PS_IPC_CHECK(c_info.ok());
  PS_IPC_CHECK(c_info.value().active_sessions == 1U);

  pending_gate.release();
  PS_IPC_CHECK(pending_a.ready_within(kSessionCloseTimeout));
  auto session_a = pending_a.get();
  PS_IPC_CHECK(session_a.ok());
  PS_IPC_CHECK(session_a.value().value == 3U);
  auto published_info = observer.daemon_info();
  PS_IPC_CHECK(published_info.ok());
  PS_IPC_CHECK(published_info.value().active_sessions == 2U);

  PS_IPC_CHECK(observer.session_close(session_c.value()).ok());
  PS_IPC_CHECK(observer.session_close(session_a.value()).ok());
  auto settled = observer.daemon_info();
  PS_IPC_CHECK(settled.ok());
  PS_IPC_CHECK(settled.value().active_sessions == 0U);
  PS_IPC_CHECK(settled.value().active_jobs == 0U);
  PS_IPC_CHECK(observer.daemon_shutdown().ok());
  PS_IPC_CHECK(server_run.join().ok());
  PS_IPC_CHECK(server.active_handler_count() == 0U);
  PS_IPC_CHECK(server.retained_handler_count() == 0U);
  return 0;
}

/**
 * @brief Proves every pre-publication Session-create failure rolls back its
 * reservation without consuming an identifier.
 * @return Zero after compiler validation, injected candidate, publication,
 * first-id, and final-capacity assertions pass.
 * @throws std::bad_alloc If Service or request staging allocation fails.
 * @note The publication fault fires immediately before the Session map
 * insertion that may allocate; all controls exist only in the test runtime.
 */
int verify_session_create_failure_rollback() {
  using ps::ErrorCode;

  ps::ipc::test::reset_exception_fence_faults();
  internal::Service service(internal::ServiceConfig{1U, 4U, 1U, false});
  internal::Request create;
  create.request_id = 401U;
  create.method = internal::Method::SessionCreate;
  create.document = compiler_invalid_document();
  const internal::Response invalid = service.dispatch(create);
  PS_IPC_CHECK(!invalid.status.ok());
  PS_IPC_CHECK(invalid.status.code == ErrorCode::NotFound);
  PS_IPC_CHECK(invalid.session_id.instance == 0U);
  PS_IPC_CHECK(invalid.session_id.value == 0U);

  ps::ipc::test::reset_exception_fence_faults();
  ps::ipc::test::arm_exception_fence_fault(
      ps::ipc::test::ExceptionFenceFaultPoint::SessionCreateCandidate,
      ps::ipc::test::ExceptionFenceFaultAction::StandardException);
  create.request_id = 402U;
  create.document = addition_document(2.0, 3.0);
  const internal::Response candidate_failure = service.dispatch(create);
  PS_IPC_CHECK(!candidate_failure.status.ok());
  PS_IPC_CHECK(candidate_failure.status.code == ErrorCode::Internal);
  PS_IPC_CHECK(candidate_failure.session_id.instance == 0U);
  PS_IPC_CHECK(candidate_failure.session_id.value == 0U);
  PS_IPC_CHECK(
      ps::ipc::test::exception_fence_fault_hits(
          ps::ipc::test::ExceptionFenceFaultPoint::SessionCreateCandidate) ==
      1U);

  ps::ipc::test::reset_exception_fence_faults();
  ps::ipc::test::arm_exception_fence_fault(
      ps::ipc::test::ExceptionFenceFaultPoint::SessionCreatePublication,
      ps::ipc::test::ExceptionFenceFaultAction::BadAlloc);
  create.request_id = 403U;
  const internal::Response publication_failure = service.dispatch(create);
  PS_IPC_CHECK(!publication_failure.status.ok());
  PS_IPC_CHECK(publication_failure.status.code == ErrorCode::ResourceExhausted);
  PS_IPC_CHECK(publication_failure.session_id.instance == 0U);
  PS_IPC_CHECK(publication_failure.session_id.value == 0U);
  PS_IPC_CHECK(
      ps::ipc::test::exception_fence_fault_hits(
          ps::ipc::test::ExceptionFenceFaultPoint::SessionCreatePublication) ==
      1U);

  ps::ipc::test::reset_exception_fence_faults();
  internal::Request info;
  info.request_id = 404U;
  info.method = internal::Method::DaemonInfo;
  const internal::Response empty = service.dispatch(info);
  PS_IPC_CHECK(empty.status.ok());
  PS_IPC_CHECK(empty.daemon_info.active_sessions == 0U);
  create.request_id = 405U;
  const internal::Response first = service.dispatch(create);
  PS_IPC_CHECK(first.status.ok());
  PS_IPC_CHECK(first.session_id.value == 1U);
  internal::Request close;
  close.request_id = 406U;
  close.method = internal::Method::SessionClose;
  close.session_id = first.session_id;
  PS_IPC_CHECK(service.dispatch(close).status.ok());
  info.request_id = 407U;
  const internal::Response settled = service.dispatch(info);
  PS_IPC_CHECK(settled.status.ok());
  PS_IPC_CHECK(settled.daemon_info.active_sessions == 0U);
  PS_IPC_CHECK(settled.daemon_info.active_jobs == 0U);
  ps::ipc::test::reset_exception_fence_faults();
  return 0;
}

/** @brief Registry-reference removal used by one in-flight result regression.
 */
enum class JobRecordRemoval : std::uint32_t {
  /** @brief Remove the terminal Job through `job.release`. */
  Release = 1U,
  /** @brief Remove the terminal Job while closing its Session. */
  SessionClose = 2U,
};

/**
 * @brief Proves an already-found result reader survives registry removal.
 *
 * A public result request is held strictly after `JobRegistry::find` retained
 * the target shared record and before it acquires the record mutex. A second
 * real handler releases the terminal Job or closes its Session, and a third
 * handler proves all fresh status/result/release lookups are `NotFound`.
 * Releasing the barrier must let the old reader copy the exact value, after
 * which the retirement observer proves the last shared owner destroys the
 * record.
 *
 * @param removal Exact release or Session-close mutation under test.
 * @return Zero when result linearization, fresh lookup, and retirement pass.
 * @throws std::bad_alloc If test/server/RPC staging allocation fails.
 * @throws std::system_error If asynchronous or barrier synchronization fails.
 * @throws std::runtime_error If a real IPC terminal observation fails.
 * @note No sleep establishes ordering; one-shot JobId filtering and condition-
 * variable/future barriers provide every happens-before edge.
 */
int verify_in_flight_result_survives_removal(JobRecordRemoval removal) {
  using ps::ErrorCode;
  using ps::ExecutionResult;
  using ps::ipc::Client;
  using ps::ipc::JobState;

  ps::ipc::test::reset_exception_fence_faults();
  const std::string path = socket_path();
  internal::Server server(
      internal::ServerConfig{path,
                             internal::ServiceConfig{1U, 8U, 2U, false},
                             16,
                             16U,
                             {},
                             {}});
  ps::ipc::test::ServerRunGuard server_run(&server);
  Client coordinator;
  Client result_client;
  Client mutation_client;
  Client fresh_client;
  PS_IPC_CHECK(coordinator.connect(path).ok());
  PS_IPC_CHECK(result_client.connect(path).ok());
  PS_IPC_CHECK(mutation_client.connect(path).ok());
  PS_IPC_CHECK(fresh_client.connect(path).ok());

  auto session = coordinator.session_create(addition_document(2.0, 3.0));
  PS_IPC_CHECK(session.ok());
  auto job = coordinator.job_submit(session.value());
  PS_IPC_CHECK(job.ok());
  const auto terminal_status =
      wait_terminal_without_sleep(&coordinator, job.value());
  PS_IPC_CHECK(terminal_status.state == JobState::Succeeded);

  JobResultAfterFindGate result_gate;
  JobResultLifetimeHookScope lifetime_hook(job.value(), &result_gate);
  GateReleasingFuture<ps::Result<ExecutionResult>, JobResultAfterFindGate>
      in_flight_result(&result_gate,
                       [&] { return result_client.job_result(job.value()); });
  PS_IPC_CHECK(result_gate.wait_until_entered(kSessionCloseTimeout));

  const ps::Status removed =
      removal == JobRecordRemoval::Release
          ? mutation_client.job_release(job.value())
          : mutation_client.session_close(session.value());
  PS_IPC_CHECK(removed.ok());

  auto fresh_status = fresh_client.job_status(job.value());
  auto fresh_result = fresh_client.job_result(job.value());
  const ps::Status fresh_release = fresh_client.job_release(job.value());
  PS_IPC_CHECK(!fresh_status.ok());
  PS_IPC_CHECK(fresh_status.status().code == ErrorCode::NotFound);
  PS_IPC_CHECK(!fresh_result.ok());
  PS_IPC_CHECK(fresh_result.status().code == ErrorCode::NotFound);
  PS_IPC_CHECK(!fresh_release.ok());
  PS_IPC_CHECK(fresh_release.code == ErrorCode::NotFound);
  PS_IPC_CHECK(ps::ipc::test::job_record_retirement_count(job.value()) == 0U);

  result_gate.release();
  PS_IPC_CHECK(in_flight_result.ready_within(kSessionCloseTimeout));
  auto retained_result = in_flight_result.get();
  PS_IPC_CHECK(retained_result.ok());
  const auto sum = retained_result.value().values.find("sum");
  PS_IPC_CHECK(sum != retained_result.value().values.end());
  auto scalar = sum->second.as_float64();
  PS_IPC_CHECK(scalar.ok());
  PS_IPC_CHECK(scalar.value() == 5.0);
  PS_IPC_CHECK(result_gate.wait_until_retired(kSessionCloseTimeout));
  PS_IPC_CHECK(result_gate.observed_same_job());
  PS_IPC_CHECK(ps::ipc::test::job_record_retirement_count(job.value()) == 1U);

  auto after_removal = fresh_client.daemon_info();
  PS_IPC_CHECK(after_removal.ok());
  PS_IPC_CHECK(after_removal.value().active_jobs == 0U);
  if (removal == JobRecordRemoval::Release) {
    PS_IPC_CHECK(after_removal.value().active_sessions == 1U);
    PS_IPC_CHECK(mutation_client.session_close(session.value()).ok());
  } else {
    PS_IPC_CHECK(after_removal.value().active_sessions == 0U);
  }
  auto settled = fresh_client.daemon_info();
  PS_IPC_CHECK(settled.ok());
  PS_IPC_CHECK(settled.value().active_sessions == 0U);
  PS_IPC_CHECK(settled.value().active_jobs == 0U);
  PS_IPC_CHECK(fresh_client.daemon_shutdown().ok());
  PS_IPC_CHECK(server_run.join().ok());
  PS_IPC_CHECK(server.active_handler_count() == 0U);
  PS_IPC_CHECK(server.retained_handler_count() == 0U);
  return 0;
}

/**
 * @brief Proves worker publication linearizes before Session-close cancel.
 *
 * The worker is held while owning the JobRecord mutex after observing no
 * cancellation. An old result handler then retains the shared record, and
 * close erases registry visibility. Close must not reach its cancellation
 * decision until the worker publishes `Succeeded` and releases the record
 * mutex. The old reader subsequently deep-copies the exact result while every
 * fresh lookup remains `NotFound`.
 *
 * @return Zero when worker-first publication and retained-reader semantics
 * pass.
 * @throws std::bad_alloc If test/server/RPC staging allocation fails.
 * @throws std::system_error If asynchronous or barrier synchronization fails.
 * @throws std::runtime_error If a real IPC observation fails.
 * @note No elapsed sleep establishes ordering. JobId-filtered one-shot hooks,
 * condition variables, real IPC observations, and bounded futures provide all
 * happens-before edges.
 */
int verify_worker_wins_session_close_arbitration() {
  using ps::ErrorCode;
  using ps::ExecutionResult;
  using ps::ipc::Client;

  ps::ipc::test::reset_exception_fence_faults();
  const std::string path = socket_path();
  internal::Server server(
      internal::ServerConfig{path,
                             internal::ServiceConfig{1U, 8U, 2U, false},
                             16,
                             16U,
                             {},
                             {}});
  ps::ipc::test::ServerRunGuard server_run(&server);
  JobRunningGate running_gate;
  JobRunningHookScope running_hook(&running_gate);
  Client coordinator;
  Client result_client;
  Client close_client;
  Client fresh_client;
  PS_IPC_CHECK(coordinator.connect(path).ok());
  PS_IPC_CHECK(result_client.connect(path).ok());
  PS_IPC_CHECK(close_client.connect(path).ok());
  PS_IPC_CHECK(fresh_client.connect(path).ok());

  auto session = coordinator.session_create(addition_document(2.0, 3.0));
  PS_IPC_CHECK(session.ok());
  auto job = coordinator.job_submit(session.value());
  PS_IPC_CHECK(job.ok());
  PS_IPC_CHECK(running_gate.wait_until_entered(kSessionCloseTimeout));

  JobFinalPublicationHookScope final_hook(
      job.value(),
      ps::ipc::test::JobFinalPublicationPoint::AfterCancellationCheck);
  SessionCloseCancellationHookScope close_hook(job.value());
  running_gate.release();
  PS_IPC_CHECK(
      job_final_publication_gate().wait_until_entered(kSessionCloseTimeout));
  PS_IPC_CHECK(job_final_publication_gate().observed(job.value()));

  JobResultAfterFindGate result_gate;
  JobResultLifetimeHookScope lifetime_hook(job.value(), &result_gate);
  GateReleasingFuture<ps::Result<ExecutionResult>, JobResultAfterFindGate>
      retained_result(&result_gate,
                      [&] { return result_client.job_result(job.value()); });
  PS_IPC_CHECK(result_gate.wait_until_entered(kSessionCloseTimeout));

  GateReleasingFuture<ps::Status, JobArbitrationGate> close_future(
      &session_close_cancellation_gate(),
      [&] { return close_client.session_close(session.value()); });
  GateReleaseGuard<JobArbitrationGate> worker_release_guard(
      &job_final_publication_gate());

  const bool registry_erased = wait_active_job_count_without_sleep(
      &fresh_client, 0U, kSessionCloseTimeout);
  auto fresh_status = fresh_client.job_status(job.value());
  auto fresh_result = fresh_client.job_result(job.value());
  const ps::Status fresh_release = fresh_client.job_release(job.value());
  const bool close_reached_cancellation =
      session_close_cancellation_gate().wait_until_entered(
          kCloseBlockedObservation);
  const bool close_blocked =
      !close_future.ready_within(kCloseBlockedObservation);

  job_final_publication_gate().release();
  const bool worker_exited =
      job_final_publication_gate().wait_until_exited(kSessionCloseTimeout);
  session_close_cancellation_gate().release();
  const bool close_ready = close_future.ready_within(kSessionCloseTimeout);

  PS_IPC_CHECK(registry_erased);
  PS_IPC_CHECK(!fresh_status.ok());
  PS_IPC_CHECK(fresh_status.status().code == ErrorCode::NotFound);
  PS_IPC_CHECK(!fresh_result.ok());
  PS_IPC_CHECK(fresh_result.status().code == ErrorCode::NotFound);
  PS_IPC_CHECK(!fresh_release.ok());
  PS_IPC_CHECK(fresh_release.code == ErrorCode::NotFound);
  PS_IPC_CHECK(!close_reached_cancellation);
  PS_IPC_CHECK(!session_close_cancellation_gate().observed(job.value()));
  PS_IPC_CHECK(close_blocked);
  PS_IPC_CHECK(worker_exited);
  PS_IPC_CHECK(close_ready);
  PS_IPC_CHECK(close_future.get().ok());

  result_gate.release();
  PS_IPC_CHECK(retained_result.ready_within(kSessionCloseTimeout));
  auto result = retained_result.get();
  PS_IPC_CHECK(result.ok());
  const auto sum = result.value().values.find("sum");
  PS_IPC_CHECK(sum != result.value().values.end());
  auto scalar = sum->second.as_float64();
  PS_IPC_CHECK(scalar.ok());
  PS_IPC_CHECK(scalar.value() == 5.0);
  PS_IPC_CHECK(result_gate.wait_until_retired(kSessionCloseTimeout));
  PS_IPC_CHECK(result_gate.observed_same_job());
  PS_IPC_CHECK(ps::ipc::test::job_record_retirement_count(job.value()) == 1U);

  auto settled = fresh_client.daemon_info();
  PS_IPC_CHECK(settled.ok());
  PS_IPC_CHECK(settled.value().active_sessions == 0U);
  PS_IPC_CHECK(settled.value().active_jobs == 0U);
  PS_IPC_CHECK(fresh_client.daemon_shutdown().ok());
  PS_IPC_CHECK(server_run.join().ok());
  PS_IPC_CHECK(server.active_handler_count() == 0U);
  PS_IPC_CHECK(server.retained_handler_count() == 0U);
  return 0;
}

/**
 * @brief Proves Session-close cancellation linearizes before worker publish.
 *
 * Close is held at the nonterminal cancellation decision while owning the
 * JobRecord mutex. The worker then completes kernel execution and reaches the
 * final-publication pre-lock seam, but cannot finalize until close requests
 * cancellation and releases the mutex. Both close and an old retained result
 * reader must observe `Cancelled`, with no stale successful result.
 *
 * @return Zero when close-first cancellation and result suppression pass.
 * @throws std::bad_alloc If test/server/RPC staging allocation fails.
 * @throws std::system_error If asynchronous or barrier synchronization fails.
 * @throws std::runtime_error If a real IPC observation fails.
 * @note The regression uses no sleep; all ordering comes from one-shot hooks,
 * condition variables, real IPC observations, and bounded future deadlines.
 */
int verify_session_close_wins_final_publication_arbitration() {
  using ps::ErrorCode;
  using ps::ExecutionResult;
  using ps::ipc::Client;

  ps::ipc::test::reset_exception_fence_faults();
  const std::string path = socket_path();
  internal::Server server(
      internal::ServerConfig{path,
                             internal::ServiceConfig{1U, 8U, 2U, false},
                             16,
                             16U,
                             {},
                             {}});
  ps::ipc::test::ServerRunGuard server_run(&server);
  JobRunningGate running_gate;
  JobRunningHookScope running_hook(&running_gate);
  Client coordinator;
  Client result_client;
  Client close_client;
  Client fresh_client;
  PS_IPC_CHECK(coordinator.connect(path).ok());
  PS_IPC_CHECK(result_client.connect(path).ok());
  PS_IPC_CHECK(close_client.connect(path).ok());
  PS_IPC_CHECK(fresh_client.connect(path).ok());

  auto session = coordinator.session_create(addition_document(10.0, 20.0));
  PS_IPC_CHECK(session.ok());
  auto job = coordinator.job_submit(session.value());
  PS_IPC_CHECK(job.ok());
  PS_IPC_CHECK(running_gate.wait_until_entered(kSessionCloseTimeout));

  JobFinalPublicationHookScope final_hook(
      job.value(), ps::ipc::test::JobFinalPublicationPoint::BeforeRecordLock);
  SessionCloseCancellationHookScope close_hook(job.value());
  JobResultAfterFindGate result_gate;
  JobResultLifetimeHookScope lifetime_hook(job.value(), &result_gate);
  GateReleasingFuture<ps::Result<ExecutionResult>, JobResultAfterFindGate>
      retained_result(&result_gate,
                      [&] { return result_client.job_result(job.value()); });
  PS_IPC_CHECK(result_gate.wait_until_entered(kSessionCloseTimeout));

  GateReleasingFuture<ps::Status, JobArbitrationGate> close_future(
      &session_close_cancellation_gate(),
      [&] { return close_client.session_close(session.value()); });
  GateReleaseGuard<JobRunningGate> running_release_guard(&running_gate);
  GateReleaseGuard<JobArbitrationGate> worker_release_guard(
      &job_final_publication_gate());

  const bool registry_erased = wait_active_job_count_without_sleep(
      &fresh_client, 0U, kSessionCloseTimeout);
  const bool close_entered =
      session_close_cancellation_gate().wait_until_entered(
          kSessionCloseTimeout);
  auto fresh_status = fresh_client.job_status(job.value());
  auto fresh_result = fresh_client.job_result(job.value());
  const ps::Status fresh_release = fresh_client.job_release(job.value());

  running_gate.release();
  const bool worker_entered =
      job_final_publication_gate().wait_until_entered(kSessionCloseTimeout);
  job_final_publication_gate().release();
  const bool worker_exited =
      job_final_publication_gate().wait_until_exited(kSessionCloseTimeout);
  const bool close_blocked =
      !close_future.ready_within(kCloseBlockedObservation);

  session_close_cancellation_gate().release();
  const bool close_ready = close_future.ready_within(kSessionCloseTimeout);

  PS_IPC_CHECK(registry_erased);
  PS_IPC_CHECK(close_entered);
  PS_IPC_CHECK(session_close_cancellation_gate().observed(job.value()));
  PS_IPC_CHECK(!fresh_status.ok());
  PS_IPC_CHECK(fresh_status.status().code == ErrorCode::NotFound);
  PS_IPC_CHECK(!fresh_result.ok());
  PS_IPC_CHECK(fresh_result.status().code == ErrorCode::NotFound);
  PS_IPC_CHECK(!fresh_release.ok());
  PS_IPC_CHECK(fresh_release.code == ErrorCode::NotFound);
  PS_IPC_CHECK(worker_entered);
  PS_IPC_CHECK(job_final_publication_gate().observed(job.value()));
  PS_IPC_CHECK(worker_exited);
  PS_IPC_CHECK(close_blocked);
  PS_IPC_CHECK(close_ready);
  PS_IPC_CHECK(close_future.get().ok());

  result_gate.release();
  PS_IPC_CHECK(retained_result.ready_within(kSessionCloseTimeout));
  auto result = retained_result.get();
  PS_IPC_CHECK(!result.ok());
  PS_IPC_CHECK(result.status().code == ErrorCode::Cancelled);
  PS_IPC_CHECK(result_gate.wait_until_retired(kSessionCloseTimeout));
  PS_IPC_CHECK(result_gate.observed_same_job());
  PS_IPC_CHECK(ps::ipc::test::job_record_retirement_count(job.value()) == 1U);

  auto settled = fresh_client.daemon_info();
  PS_IPC_CHECK(settled.ok());
  PS_IPC_CHECK(settled.value().active_sessions == 0U);
  PS_IPC_CHECK(settled.value().active_jobs == 0U);
  PS_IPC_CHECK(fresh_client.daemon_shutdown().ok());
  PS_IPC_CHECK(server_run.join().ok());
  PS_IPC_CHECK(server.active_handler_count() == 0U);
  PS_IPC_CHECK(server.retained_handler_count() == 0U);
  return 0;
}

}  // namespace

/**
 * @brief Exercises exact v3 methods, bounded Session/handler admission,
 * malformed/error fencing, multi-namespace execution, cancellation,
 * close/release cleanup, graceful shutdown/join, and restart loss.
 * @return Zero when all checks pass.
 * @throws std::bad_alloc If test setup allocation fails.
 * @throws std::runtime_error If bounded polling cannot observe expected state.
 * @note Behavioral failures otherwise return nonzero through `PS_IPC_CHECK`.
 */
int main() {
  using ps::ErrorCode;
  using ps::ipc::Client;
  using ps::ipc::JobId;
  using ps::ipc::JobState;
  using ps::ipc::SessionId;

  PS_IPC_CHECK(verify_session_create_failure_rollback() == 0);
  PS_IPC_CHECK(verify_pending_session_create_reservation() == 0);
  PS_IPC_CHECK(verify_worker_wins_session_close_arbitration() == 0);
  PS_IPC_CHECK(verify_session_close_wins_final_publication_arbitration() == 0);
  PS_IPC_CHECK(
      verify_in_flight_result_survives_removal(JobRecordRemoval::Release) == 0);
  PS_IPC_CHECK(verify_in_flight_result_survives_removal(
                   JobRecordRemoval::SessionClose) == 0);

  for (internal::ServerConstructionStage stage :
       {internal::ServerConstructionStage::AfterListenerBind,
        internal::ServerConstructionStage::BeforeHandlerStorage}) {
    const ConstructionFailureObservation observation =
        observe_construction_failure(stage);
    PS_IPC_CHECK(observation.exception_propagated);
    PS_IPC_CHECK(observation.descriptor_released);
    PS_IPC_CHECK(observation.socket_node_removed);
    PS_IPC_CHECK(observation.rebound);
  }

  {
    internal::Service service(internal::ServiceConfig{1U, 4U, 2U, false});
    internal::Request shutdown;
    shutdown.request_id = 1U;
    shutdown.method = internal::Method::DaemonShutdown;
    const internal::Response accepted = service.dispatch(shutdown);
    PS_IPC_CHECK(accepted.status.ok());
    PS_IPC_CHECK(accepted.shutdown_after_write);
    internal::Request late_info;
    late_info.request_id = 2U;
    late_info.method = internal::Method::DaemonInfo;
    const internal::Response rejected = service.dispatch(late_info);
    PS_IPC_CHECK(!rejected.status.ok());
    PS_IPC_CHECK(rejected.status.code == ErrorCode::Cancelled);
  }

  {
    bool rejected_zero_sessions = false;
    try {
      internal::Service invalid(internal::ServiceConfig{1U, 1U, 0U, false});
    } catch (const std::invalid_argument&) {
      rejected_zero_sessions = true;
    }
    PS_IPC_CHECK(rejected_zero_sessions);

    internal::Service service(internal::ServiceConfig{1U, 8U, 1U, false});
    internal::Request first_create;
    first_create.request_id = 10U;
    first_create.method = internal::Method::SessionCreate;
    first_create.document = addition_document(1.0, 2.0);
    const internal::Response first = service.dispatch(first_create);
    PS_IPC_CHECK(first.status.ok());
    internal::Request second_create = first_create;
    second_create.request_id = 11U;
    const internal::Response rejected = service.dispatch(second_create);
    PS_IPC_CHECK(!rejected.status.ok());
    PS_IPC_CHECK(rejected.status.code == ErrorCode::ResourceExhausted);
    PS_IPC_CHECK(rejected.session_id.instance == 0U);
    PS_IPC_CHECK(rejected.session_id.value == 0U);
    internal::Request info_request;
    info_request.request_id = 12U;
    info_request.method = internal::Method::DaemonInfo;
    const internal::Response at_capacity = service.dispatch(info_request);
    PS_IPC_CHECK(at_capacity.status.ok());
    PS_IPC_CHECK(at_capacity.daemon_info.active_sessions == 1U);
    PS_IPC_CHECK(at_capacity.daemon_info.maximum_sessions == 1U);
    internal::Request close_request;
    close_request.request_id = 13U;
    close_request.method = internal::Method::SessionClose;
    close_request.session_id = first.session_id;
    PS_IPC_CHECK(service.dispatch(close_request).status.ok());
    second_create.request_id = 14U;
    const internal::Response reused = service.dispatch(second_create);
    PS_IPC_CHECK(reused.status.ok());
    PS_IPC_CHECK(reused.session_id.value != first.session_id.value);
  }

  {
    internal::Service service(internal::ServiceConfig{1U, 8U, 2U, false});
    std::vector<std::future<internal::Response>> creates;
    for (std::uint64_t index = 0U; index < 8U; ++index) {
      creates.push_back(std::async(std::launch::async, [&service, index] {
        internal::Request request;
        request.request_id = 100U + index;
        request.method = internal::Method::SessionCreate;
        request.document = addition_document(static_cast<double>(index), 1.0);
        return service.dispatch(request);
      }));
    }
    std::vector<SessionId> admitted;
    std::size_t exhausted = 0U;
    for (auto& future : creates) {
      internal::Response response = future.get();
      if (response.status.ok()) {
        admitted.push_back(response.session_id);
      } else if (response.status.code == ErrorCode::ResourceExhausted) {
        ++exhausted;
      }
    }
    PS_IPC_CHECK(admitted.size() == 2U);
    PS_IPC_CHECK(exhausted == 6U);
    internal::Request info_request;
    info_request.request_id = 200U;
    info_request.method = internal::Method::DaemonInfo;
    PS_IPC_CHECK(service.dispatch(info_request).daemon_info.active_sessions ==
                 2U);
    for (std::size_t index = 0U; index < admitted.size(); ++index) {
      internal::Request close_request;
      close_request.request_id = 210U + index;
      close_request.method = internal::Method::SessionClose;
      close_request.session_id = admitted[index];
      PS_IPC_CHECK(service.dispatch(close_request).status.ok());
    }
  }

  {
    const std::string rejection_path = socket_path();
    internal::Server server(
        internal::ServerConfig{rejection_path,
                               internal::ServiceConfig{1U, 4U, 2U, false},
                               4,
                               4U,
                               {},
                               {}});
    internal::reject_next_peer_for_test(EBADF);
    ps::ipc::test::ServerRunGuard server_run(&server);
    auto rejected = internal::connect_unix_socket(rejection_path);
    PS_IPC_CHECK(rejected.ok());
    auto rejected_eof = internal::read_frame(rejected.value().get());
    PS_IPC_CHECK(!rejected_eof.ok());
    PS_IPC_CHECK(rejected_eof.status().code == ErrorCode::NotFound);
    PS_IPC_CHECK(!server_run.ready_within(std::chrono::milliseconds(100)));

    Client healthy;
    PS_IPC_CHECK(healthy.connect(rejection_path).ok());
    PS_IPC_CHECK(healthy.daemon_info().ok());
    PS_IPC_CHECK(healthy.daemon_shutdown().ok());
    PS_IPC_CHECK(server_run.join().ok());
  }

  {
    const std::string stop_path = socket_path();
    internal::Server server(
        internal::ServerConfig{stop_path,
                               internal::ServiceConfig{1U, 4U, 2U, false},
                               4,
                               4U,
                               {},
                               {}});
    ps::ipc::test::ServerRunGuard server_run(&server);
    server.request_stop();
    PS_IPC_CHECK(server_run.join().ok());
  }

  {
    const std::string failure_path = socket_path();
    internal::Server server(
        internal::ServerConfig{failure_path,
                               internal::ServiceConfig{1U, 4U, 2U, false},
                               4,
                               4U,
                               {},
                               {}});
    internal::fail_next_accept_for_test(EIO);
    ps::ipc::test::ServerRunGuard server_run(&server);
    const ps::Status& failure = server_run.join();
    PS_IPC_CHECK(!failure.ok());
    PS_IPC_CHECK(failure.code == ErrorCode::Internal);
  }

  {
    const std::string malformed_path = socket_path();
    internal::Server server(
        internal::ServerConfig{malformed_path,
                               internal::ServiceConfig{1U, 8U, 4U, false},
                               8,
                               4U,
                               {},
                               {}});
    ps::ipc::test::ServerRunGuard server_run(&server);

    const internal::Response oversized = malformed_stream_response(
        malformed_path, {0xffU, 0xffU, 0xffU, 0xffU}, false);
    PS_IPC_CHECK(oversized.status.code == ErrorCode::InvalidArgument);
    PS_IPC_CHECK(oversized.request_id == 0U);
    PS_IPC_CHECK(oversized.method == internal::Method::DaemonInfo);

    const internal::Response truncated =
        malformed_stream_response(malformed_path, {0U, 0U, 0U, 4U, 1U}, true);
    PS_IPC_CHECK(truncated.status.code == ErrorCode::InvalidArgument);
    PS_IPC_CHECK(truncated.request_id == 0U);

    const internal::Response correlated_truncation = malformed_stream_response(
        malformed_path,
        correlated_truncated_frame(304U, internal::Method::SessionCreate),
        true);
    PS_IPC_CHECK(correlated_truncation.status.code ==
                 ErrorCode::InvalidArgument);
    PS_IPC_CHECK(correlated_truncation.request_id == 304U);
    PS_IPC_CHECK(correlated_truncation.method ==
                 internal::Method::SessionCreate);

    internal::Request info_request;
    info_request.request_id = 301U;
    info_request.method = internal::Method::DaemonInfo;
    auto encoded_info = internal::encode_request(info_request);
    PS_IPC_CHECK(encoded_info.ok());
    std::vector<std::uint8_t> trailing = encoded_info.value();
    trailing.push_back(0U);
    const internal::Response trailing_response =
        malformed_payload_response(malformed_path, trailing);
    PS_IPC_CHECK(trailing_response.status.code == ErrorCode::InvalidArgument);
    PS_IPC_CHECK(trailing_response.request_id == 301U);
    PS_IPC_CHECK(trailing_response.method == internal::Method::DaemonInfo);

    std::vector<std::uint8_t> unknown_method = encoded_info.value();
    unknown_method[10U] = 0xffU;
    const internal::Response unknown_method_response =
        malformed_payload_response(malformed_path, unknown_method);
    PS_IPC_CHECK(unknown_method_response.status.code ==
                 ErrorCode::InvalidArgument);
    PS_IPC_CHECK(unknown_method_response.request_id == 0U);

    internal::Request source_request;
    source_request.request_id = 302U;
    source_request.method = internal::Method::SessionCreate;
    source_request.document = addition_document(1.0, 2.0);
    auto encoded_source = internal::encode_request(source_request);
    PS_IPC_CHECK(encoded_source.ok());
    std::vector<std::uint8_t> invalid_utf8 = encoded_source.value();
    PS_IPC_CHECK(invalid_utf8.size() > 31U);
    invalid_utf8[31U] = 0xc0U;
    const internal::Response invalid_utf8_response =
        malformed_payload_response(malformed_path, invalid_utf8);
    PS_IPC_CHECK(invalid_utf8_response.status.code ==
                 ErrorCode::InvalidArgument);
    PS_IPC_CHECK(invalid_utf8_response.request_id == 302U);
    PS_IPC_CHECK(invalid_utf8_response.method ==
                 internal::Method::SessionCreate);

    const internal::Response duplicate_response = malformed_payload_response(
        malformed_path, duplicate_parameter_request(303U));
    PS_IPC_CHECK(duplicate_response.status.code == ErrorCode::InvalidArgument);
    PS_IPC_CHECK(duplicate_response.request_id == 303U);
    PS_IPC_CHECK(duplicate_response.method == internal::Method::SessionCreate);

    settle_handlers_and_trigger_reap(&server, malformed_path);
    PS_IPC_CHECK(server.retained_handler_count() <= 1U);
    for (std::size_t iteration = 0U; iteration < 50U; ++iteration) {
      Client sequential;
      PS_IPC_CHECK(sequential.connect(malformed_path).ok());
      PS_IPC_CHECK(sequential.daemon_info().ok());
      sequential.disconnect();
      wait_handler_count(&server, 0U);
      PS_IPC_CHECK(server.retained_handler_count() <= 1U);
    }
    Client healthy;
    PS_IPC_CHECK(healthy.connect(malformed_path).ok());
    auto healthy_info = healthy.daemon_info();
    PS_IPC_CHECK(healthy_info.ok());
    PS_IPC_CHECK(healthy_info.value().active_sessions == 0U);
    PS_IPC_CHECK(server.retained_handler_count() <= 1U);
    PS_IPC_CHECK(healthy.daemon_shutdown().ok());
    PS_IPC_CHECK(server_run.join().ok());
    PS_IPC_CHECK(server.active_handler_count() == 0U);
    PS_IPC_CHECK(server.retained_handler_count() == 0U);
  }

  {
    const std::string bounded_path = socket_path();
    internal::Server server(
        internal::ServerConfig{bounded_path,
                               internal::ServiceConfig{1U, 4U, 2U, false},
                               4,
                               1U,
                               {},
                               {}});
    ps::ipc::test::ServerRunGuard server_run(&server);
    auto held = internal::connect_unix_socket(bounded_path);
    PS_IPC_CHECK(held.ok());
    internal::UniqueDescriptor held_connection = held.take_value();
    wait_handler_count(&server, 1U);
    auto rejected = internal::connect_unix_socket(bounded_path);
    PS_IPC_CHECK(rejected.ok());
    auto rejection_frame = internal::read_frame(rejected.value().get());
    PS_IPC_CHECK(rejection_frame.ok());
    auto rejection = internal::decode_protocol_error(rejection_frame.value());
    PS_IPC_CHECK(rejection.ok());
    PS_IPC_CHECK(rejection.value().request_id == 0U);
    PS_IPC_CHECK(rejection.value().status.code == ErrorCode::ResourceExhausted);
    held_connection.reset();
    wait_handler_count(&server, 0U);
    Client shutdown_client;
    PS_IPC_CHECK(shutdown_client.connect(bounded_path).ok());
    PS_IPC_CHECK(shutdown_client.daemon_shutdown().ok());
    PS_IPC_CHECK(server_run.join().ok());
    PS_IPC_CHECK(server.active_handler_count() == 0U);
    PS_IPC_CHECK(server.retained_handler_count() == 0U);
  }

  {
    const std::string exception_path = socket_path();
    std::atomic<bool> throw_once{true};
    internal::ServerConfig exception_config{
        exception_path,
        internal::ServiceConfig{1U, 4U, 2U, false},
        4,
        2U,
        {},
        {}};
    exception_config.handler_entry_hook = [&throw_once] {
      if (throw_once.exchange(false)) {
        throw std::runtime_error("fixture handler exception");
      }
    };
    internal::Server server(std::move(exception_config));
    ps::ipc::test::ServerRunGuard server_run(&server);
    const internal::Response exception_response =
        malformed_stream_response(exception_path, {}, false);
    PS_IPC_CHECK(exception_response.request_id == 0U);
    PS_IPC_CHECK(exception_response.status.code == ErrorCode::Internal);
    wait_handler_count(&server, 0U);
    Client recovered;
    PS_IPC_CHECK(recovered.connect(exception_path).ok());
    PS_IPC_CHECK(recovered.daemon_info().ok());
    PS_IPC_CHECK(recovered.daemon_shutdown().ok());
    PS_IPC_CHECK(server_run.join().ok());
    PS_IPC_CHECK(server.active_handler_count() == 0U);
    PS_IPC_CHECK(server.retained_handler_count() == 0U);
  }

  {
    // SessionCloseSnapshotFailureReopensAdmission regression.
    ps::ipc::test::reset_exception_fence_faults();
    const std::string snapshot_path = socket_path();
    internal::Server server(
        internal::ServerConfig{snapshot_path,
                               internal::ServiceConfig{1U, 8U, 2U, false},
                               8,
                               64U,
                               {},
                               {}});
    ps::ipc::test::ServerRunGuard server_run(&server);
    Client client;
    PS_IPC_CHECK(client.connect(snapshot_path).ok());
    auto session = client.session_create(addition_document(1.0, 2.0));
    PS_IPC_CHECK(session.ok());
    ps::ipc::test::arm_exception_fence_fault(
        ps::ipc::test::ExceptionFenceFaultPoint::SessionCloseSnapshot,
        ps::ipc::test::ExceptionFenceFaultAction::BadAlloc);
    auto failed_close = client.session_close(session.value());
    PS_IPC_CHECK(!failed_close.ok());
    PS_IPC_CHECK(failed_close.code == ErrorCode::ResourceExhausted);
    PS_IPC_CHECK(
        ps::ipc::test::exception_fence_fault_hits(
            ps::ipc::test::ExceptionFenceFaultPoint::SessionCloseSnapshot) ==
        1U);
    ps::ipc::test::reset_exception_fence_faults();
    auto retry_job = client.job_submit(session.value());
    PS_IPC_CHECK(retry_job.ok());
    PS_IPC_CHECK(wait_terminal(&client, retry_job.value()).state ==
                 JobState::Succeeded);
    PS_IPC_CHECK(client.session_close(session.value()).ok());
    auto removed = client.job_status(retry_job.value());
    PS_IPC_CHECK(!removed.ok());
    PS_IPC_CHECK(removed.status().code == ErrorCode::NotFound);
    PS_IPC_CHECK(client.daemon_shutdown().ok());
    PS_IPC_CHECK(server_run.join().ok());
    ps::ipc::test::reset_exception_fence_faults();
  }

  {
    // ClosingSessionDoesNotBlockUnrelatedLifecycle regression.
    const std::string closing_isolation_path = socket_path();
    internal::Server server(
        internal::ServerConfig{closing_isolation_path,
                               internal::ServiceConfig{2U, 8U, 2U, false},
                               16,
                               64U,
                               {},
                               {}});
    ps::ipc::test::ServerRunGuard server_run(&server);
    JobRunningGate running_gate;
    JobRunningHookScope running_hook(&running_gate);
    Client observer;
    Client close_a_client;
    Client session_b_client;
    Client repeated_close_client;
    Client capacity_client;
    PS_IPC_CHECK(observer.connect(closing_isolation_path).ok());
    PS_IPC_CHECK(close_a_client.connect(closing_isolation_path).ok());
    PS_IPC_CHECK(session_b_client.connect(closing_isolation_path).ok());
    PS_IPC_CHECK(repeated_close_client.connect(closing_isolation_path).ok());
    PS_IPC_CHECK(capacity_client.connect(closing_isolation_path).ok());

    auto session_a = observer.session_create(addition_document(10.0, 20.0));
    PS_IPC_CHECK(session_a.ok());
    auto job_a = observer.job_submit(session_a.value());
    PS_IPC_CHECK(job_a.ok());
    PS_IPC_CHECK(running_gate.wait_until_entered(kJobStatusTimeout));
    auto running_a = observer.job_status(job_a.value());
    PS_IPC_CHECK(running_a.ok());
    PS_IPC_CHECK(running_a.value().state == JobState::Running);

    GateReleasingFuture<ps::Status> close_a_future(&running_gate, [&] {
      return close_a_client.session_close(session_a.value());
    });
    PS_IPC_CHECK(
        wait_job_not_found(&observer, job_a.value(), kSessionCloseTimeout));
    PS_IPC_CHECK(!close_a_future.ready_within(kCloseBlockedObservation));

    const auto create_b_started = std::chrono::steady_clock::now();
    GateReleasingFuture<ps::Result<SessionId>> create_b_future(
        &running_gate, [&] {
          return session_b_client.session_create(addition_document(3.0, 4.0));
        });
    const bool create_b_ready =
        create_b_future.ready_within(kSessionCloseTimeout);
    if (!create_b_ready) {
      running_gate.release();
      auto close_a_cleanup = close_a_future.get();
      auto session_b_cleanup = create_b_future.get();
      if (session_b_cleanup.ok()) {
        static_cast<void>(
            session_b_client.session_close(session_b_cleanup.value()));
      }
      static_cast<void>(observer.daemon_shutdown());
      static_cast<void>(server_run.join());
      static_cast<void>(close_a_cleanup);
      PS_IPC_CHECK(create_b_ready);
    }
    auto session_b = create_b_future.get();
    PS_IPC_CHECK(session_b.ok());
    PS_IPC_CHECK(std::chrono::steady_clock::now() - create_b_started <
                 kSessionCloseTimeout);

    const auto submit_b_started = std::chrono::steady_clock::now();
    auto job_b = session_b_client.job_submit(session_b.value());
    PS_IPC_CHECK(job_b.ok());
    PS_IPC_CHECK(std::chrono::steady_clock::now() - submit_b_started <
                 kSessionCloseTimeout);
    PS_IPC_CHECK(wait_terminal(&session_b_client, job_b.value()).state ==
                 JobState::Succeeded);

    const auto submit_a_started = std::chrono::steady_clock::now();
    auto late_a_submit = observer.job_submit(session_a.value());
    PS_IPC_CHECK(!late_a_submit.ok());
    PS_IPC_CHECK(late_a_submit.status().code == ErrorCode::NotFound);
    PS_IPC_CHECK(std::chrono::steady_clock::now() - submit_a_started <
                 kSessionCloseTimeout);

    const auto repeated_close_started = std::chrono::steady_clock::now();
    auto repeated_close =
        repeated_close_client.session_close(session_a.value());
    PS_IPC_CHECK(!repeated_close.ok());
    PS_IPC_CHECK(repeated_close.code == ErrorCode::NotFound);
    PS_IPC_CHECK(std::chrono::steady_clock::now() - repeated_close_started <
                 kSessionCloseTimeout);

    const auto capacity_started = std::chrono::steady_clock::now();
    auto capacity_rejected =
        capacity_client.session_create(addition_document(5.0, 6.0));
    PS_IPC_CHECK(!capacity_rejected.ok());
    PS_IPC_CHECK(capacity_rejected.status().code ==
                 ErrorCode::ResourceExhausted);
    PS_IPC_CHECK(std::chrono::steady_clock::now() - capacity_started <
                 kSessionCloseTimeout);
    PS_IPC_CHECK(!close_a_future.ready_within(kCloseBlockedObservation));

    const auto close_b_started = std::chrono::steady_clock::now();
    PS_IPC_CHECK(session_b_client.session_close(session_b.value()).ok());
    PS_IPC_CHECK(std::chrono::steady_clock::now() - close_b_started <
                 kSessionCloseTimeout);

    running_gate.release();
    PS_IPC_CHECK(close_a_future.ready_within(kSessionCloseTimeout));
    PS_IPC_CHECK(close_a_future.get().ok());
    auto missing_a_status = observer.job_status(job_a.value());
    auto missing_a_result = observer.job_result(job_a.value());
    PS_IPC_CHECK(!missing_a_status.ok());
    PS_IPC_CHECK(missing_a_status.status().code == ErrorCode::NotFound);
    PS_IPC_CHECK(!missing_a_result.ok());
    PS_IPC_CHECK(missing_a_result.status().code == ErrorCode::NotFound);

    auto reused = observer.session_create(addition_document(7.0, 8.0));
    PS_IPC_CHECK(reused.ok());
    PS_IPC_CHECK(reused.value().value > session_a.value().value);
    PS_IPC_CHECK(reused.value().value > session_b.value().value);
    PS_IPC_CHECK(observer.session_close(reused.value()).ok());
    auto settled = observer.daemon_info();
    PS_IPC_CHECK(settled.ok());
    PS_IPC_CHECK(settled.value().active_sessions == 0U);
    PS_IPC_CHECK(settled.value().active_jobs == 0U);
    PS_IPC_CHECK(observer.daemon_shutdown().ok());
    PS_IPC_CHECK(server_run.join().ok());
    PS_IPC_CHECK(server.active_handler_count() == 0U);
    PS_IPC_CHECK(server.retained_handler_count() == 0U);
  }

  {
    // CloseCancelsRunningAndQueuedWithoutStalePublication regression.
    const std::string closing_path = socket_path();
    internal::Server server(
        internal::ServerConfig{closing_path,
                               internal::ServiceConfig{1U, 8U, 2U, false},
                               8,
                               64U,
                               {},
                               {}});
    ps::ipc::test::ServerRunGuard server_run(&server);
    Client client;
    PS_IPC_CHECK(client.connect(closing_path).ok());
    auto session = client.session_create(delayed_document(5000));
    PS_IPC_CHECK(session.ok());
    auto running = client.job_submit(session.value());
    PS_IPC_CHECK(running.ok());
    PS_IPC_CHECK(
        wait_state(&client, running.value(), JobState::Running).state ==
        JobState::Running);
    auto queued = client.job_submit(session.value());
    PS_IPC_CHECK(queued.ok());
    auto queued_status = client.job_status(queued.value());
    PS_IPC_CHECK(queued_status.ok());
    PS_IPC_CHECK(queued_status.value().state == JobState::Queued);

    PS_IPC_CHECK(client.session_close(session.value()).ok());
    for (const JobId id : {running.value(), queued.value()}) {
      auto missing_status = client.job_status(id);
      auto missing_result = client.job_result(id);
      PS_IPC_CHECK(!missing_status.ok());
      PS_IPC_CHECK(missing_status.status().code == ErrorCode::NotFound);
      PS_IPC_CHECK(!missing_result.ok());
      PS_IPC_CHECK(missing_result.status().code == ErrorCode::NotFound);
    }
    auto after_close = client.daemon_info();
    PS_IPC_CHECK(after_close.ok());
    PS_IPC_CHECK(after_close.value().active_sessions == 0U);
    PS_IPC_CHECK(after_close.value().active_jobs == 0U);
    PS_IPC_CHECK(client.daemon_shutdown().ok());
    PS_IPC_CHECK(server_run.join().ok());
  }

  {
    // QueuedCancellationAndSessionCloseIsolation regression.
    const std::string queued_cancel_path = socket_path();
    internal::Server server(
        internal::ServerConfig{queued_cancel_path,
                               internal::ServiceConfig{2U, 8U, 2U, false},
                               8,
                               64U,
                               {},
                               {}});
    ps::ipc::test::ServerRunGuard server_run(&server);
    Client client;
    PS_IPC_CHECK(client.connect(queued_cancel_path).ok());
    auto session_a = client.session_create(
        delayed_document(kWorkerSaturationDelayMilliseconds));
    auto session_b = client.session_create(
        delayed_document(kWorkerSaturationDelayMilliseconds));
    PS_IPC_CHECK(session_a.ok());
    PS_IPC_CHECK(session_b.ok());
    auto job_a = client.job_submit(session_a.value());
    auto job_b = client.job_submit(session_b.value());
    PS_IPC_CHECK(job_a.ok());
    PS_IPC_CHECK(job_b.ok());
    const auto running_a =
        wait_state(&client, job_a.value(), JobState::Running);
    const auto running_b =
        wait_state(&client, job_b.value(), JobState::Running);
    PS_IPC_CHECK(running_a.state == JobState::Running);
    PS_IPC_CHECK(running_a.session_id.instance == session_a.value().instance);
    PS_IPC_CHECK(running_a.session_id.value == session_a.value().value);
    PS_IPC_CHECK(running_b.state == JobState::Running);
    PS_IPC_CHECK(running_b.session_id.instance == session_b.value().instance);
    PS_IPC_CHECK(running_b.session_id.value == session_b.value().value);

    auto job_c = client.job_submit(session_b.value());
    PS_IPC_CHECK(job_c.ok());
    const auto queued = wait_state(&client, job_c.value(), JobState::Queued);
    PS_IPC_CHECK(queued.state == JobState::Queued);
    PS_IPC_CHECK(queued.session_id.instance == session_b.value().instance);
    PS_IPC_CHECK(queued.session_id.value == session_b.value().value);
    PS_IPC_CHECK(client.job_cancel(job_c.value()).ok());

    const auto close_started = std::chrono::steady_clock::now();
    PS_IPC_CHECK(client.session_close(session_a.value()).ok());
    PS_IPC_CHECK(std::chrono::steady_clock::now() - close_started <
                 kSessionCloseTimeout);
    auto missing_a_status = client.job_status(job_a.value());
    auto missing_a_result = client.job_result(job_a.value());
    PS_IPC_CHECK(!missing_a_status.ok());
    PS_IPC_CHECK(missing_a_status.status().code == ErrorCode::NotFound);
    PS_IPC_CHECK(!missing_a_result.ok());
    PS_IPC_CHECK(missing_a_result.status().code == ErrorCode::NotFound);

    auto surviving_b = client.job_status(job_b.value());
    PS_IPC_CHECK(surviving_b.ok());
    PS_IPC_CHECK(surviving_b.value().state == JobState::Running);
    PS_IPC_CHECK(surviving_b.value().session_id.instance ==
                 session_b.value().instance);
    PS_IPC_CHECK(surviving_b.value().session_id.value ==
                 session_b.value().value);
    auto pending_b_result = client.job_result(job_b.value());
    PS_IPC_CHECK(!pending_b_result.ok());
    PS_IPC_CHECK(pending_b_result.status().code == ErrorCode::InvalidArgument);

    PS_IPC_CHECK(wait_terminal(&client, job_c.value()).state ==
                 JobState::Cancelled);
    auto cancelled_result = client.job_result(job_c.value());
    PS_IPC_CHECK(!cancelled_result.ok());
    PS_IPC_CHECK(cancelled_result.status().code == ErrorCode::Cancelled);
    auto retained = client.daemon_info();
    PS_IPC_CHECK(retained.ok());
    PS_IPC_CHECK(retained.value().active_sessions == 1U);
    PS_IPC_CHECK(retained.value().active_jobs == 2U);

    PS_IPC_CHECK(client.session_close(session_b.value()).ok());
    for (const JobId id : {job_b.value(), job_c.value()}) {
      auto missing_status = client.job_status(id);
      auto missing_result = client.job_result(id);
      PS_IPC_CHECK(!missing_status.ok());
      PS_IPC_CHECK(missing_status.status().code == ErrorCode::NotFound);
      PS_IPC_CHECK(!missing_result.ok());
      PS_IPC_CHECK(missing_result.status().code == ErrorCode::NotFound);
    }
    auto released = client.daemon_info();
    PS_IPC_CHECK(released.ok());
    PS_IPC_CHECK(released.value().active_sessions == 0U);
    PS_IPC_CHECK(released.value().active_jobs == 0U);
    PS_IPC_CHECK(client.daemon_shutdown().ok());
    PS_IPC_CHECK(server_run.join().ok());
  }

  {
    // QueuedSessionCloseDoesNotWaitForUnrelatedRunningJob regression.
    const std::string queued_close_path = socket_path();
    internal::Server server(
        internal::ServerConfig{queued_close_path,
                               internal::ServiceConfig{1U, 2U, 2U, false},
                               8,
                               64U,
                               {},
                               {}});
    ps::ipc::test::ServerRunGuard server_run(&server);
    Client client;
    Client close_client;
    PS_IPC_CHECK(client.connect(queued_close_path).ok());
    PS_IPC_CHECK(close_client.connect(queued_close_path).ok());
    auto session_a = client.session_create(
        delayed_document(kWorkerSaturationDelayMilliseconds));
    auto session_b = client.session_create(
        delayed_document(kWorkerSaturationDelayMilliseconds));
    PS_IPC_CHECK(session_a.ok());
    PS_IPC_CHECK(session_b.ok());
    auto job_a = client.job_submit(session_a.value());
    PS_IPC_CHECK(job_a.ok());
    PS_IPC_CHECK(wait_state(&client, job_a.value(), JobState::Running).state ==
                 JobState::Running);
    auto job_b = client.job_submit(session_b.value());
    PS_IPC_CHECK(job_b.ok());
    PS_IPC_CHECK(wait_state(&client, job_b.value(), JobState::Queued).state ==
                 JobState::Queued);

    auto close_future = std::async(std::launch::async, [&] {
      return close_client.session_close(session_b.value());
    });
    const bool queued_close_completed =
        close_future.wait_for(kSessionCloseTimeout) ==
        std::future_status::ready;
    if (!queued_close_completed) {
      static_cast<void>(client.job_cancel(job_a.value()));
    }
    auto queued_close = close_future.get();
    PS_IPC_CHECK(queued_close_completed);
    PS_IPC_CHECK(queued_close.ok());

    auto surviving_a = client.job_status(job_a.value());
    PS_IPC_CHECK(surviving_a.ok());
    PS_IPC_CHECK(surviving_a.value().state == JobState::Running);
    auto missing_b_status = client.job_status(job_b.value());
    auto missing_b_result = client.job_result(job_b.value());
    PS_IPC_CHECK(!missing_b_status.ok());
    PS_IPC_CHECK(missing_b_status.status().code == ErrorCode::NotFound);
    PS_IPC_CHECK(!missing_b_result.ok());
    PS_IPC_CHECK(missing_b_result.status().code == ErrorCode::NotFound);

    auto reused_session = client.session_create(addition_document(3.0, 4.0));
    PS_IPC_CHECK(reused_session.ok());
    PS_IPC_CHECK(reused_session.value().value != session_b.value().value);
    PS_IPC_CHECK(client.session_close(reused_session.value()).ok());

    PS_IPC_CHECK(client.job_cancel(job_a.value()).ok());
    PS_IPC_CHECK(wait_terminal(&client, job_a.value()).state ==
                 JobState::Cancelled);
    PS_IPC_CHECK(client.session_close(session_a.value()).ok());
    auto released = client.daemon_info();
    PS_IPC_CHECK(released.ok());
    PS_IPC_CHECK(released.value().active_sessions == 0U);
    PS_IPC_CHECK(released.value().active_jobs == 0U);
    PS_IPC_CHECK(client.daemon_shutdown().ok());
    PS_IPC_CHECK(server_run.ready_within(kSessionCloseTimeout));
    PS_IPC_CHECK(server_run.join().ok());
  }

  const std::string path = socket_path();
  SessionId old_session;
  JobId old_job;
  std::uint64_t old_instance = 0U;
  {
    internal::Server server(
        internal::ServerConfig{path,
                               internal::ServiceConfig{2U, 32U, 16U, false},
                               8,
                               64U,
                               {},
                               {}});
    ps::ipc::test::ServerRunGuard server_run(&server);

    Client client;
    PS_IPC_CHECK(client.connect(path).ok());
    auto info = client.daemon_info();
    PS_IPC_CHECK(info.ok());
    PS_IPC_CHECK(info.value().protocol_version == 3U);
    PS_IPC_CHECK(info.value().instance_id != 0U);
    PS_IPC_CHECK(info.value().transport == "unix-domain");
    PS_IPC_CHECK(info.value().methods.size() == 9U);
    PS_IPC_CHECK(info.value().active_sessions == 0U);
    PS_IPC_CHECK(info.value().active_jobs == 0U);
    PS_IPC_CHECK(info.value().maximum_concurrency == 2U);
    PS_IPC_CHECK(info.value().maximum_sessions == 16U);

    // CompilerInvalidSessionCreateIsAtomic regression.
    auto before_invalid_create = client.daemon_info();
    PS_IPC_CHECK(before_invalid_create.ok());
    auto invalid_create = client.session_create(compiler_invalid_document());
    PS_IPC_CHECK(!invalid_create.ok());
    PS_IPC_CHECK(invalid_create.status().code == ErrorCode::NotFound);
    auto after_invalid_create = client.daemon_info();
    PS_IPC_CHECK(after_invalid_create.ok());
    PS_IPC_CHECK(after_invalid_create.value().active_sessions ==
                 before_invalid_create.value().active_sessions);
    PS_IPC_CHECK(after_invalid_create.value().active_jobs ==
                 before_invalid_create.value().active_jobs);
    auto first = client.session_create(addition_document(2.0, 3.0));
    auto second = client.session_create(addition_document(10.0, 20.0));
    PS_IPC_CHECK(first.ok());
    PS_IPC_CHECK(second.ok());
    PS_IPC_CHECK(first.value().value == 1U);
    PS_IPC_CHECK(first.value().value != second.value().value);
    old_session = first.value();
    auto first_job = client.job_submit(first.value());
    auto second_job = client.job_submit(second.value());
    PS_IPC_CHECK(first_job.ok());
    PS_IPC_CHECK(second_job.ok());
    old_job = first_job.value();
    PS_IPC_CHECK(wait_terminal(&client, first_job.value()).state ==
                 JobState::Succeeded);
    PS_IPC_CHECK(wait_terminal(&client, second_job.value()).state ==
                 JobState::Succeeded);
    auto first_result = client.job_result(first_job.value());
    auto second_result = client.job_result(second_job.value());
    PS_IPC_CHECK(first_result.ok());
    PS_IPC_CHECK(second_result.ok());
    PS_IPC_CHECK(first_result.value().values.at("sum").as_float64().value() ==
                 5.0);
    PS_IPC_CHECK(second_result.value().values.at("sum").as_float64().value() ==
                 30.0);
    PS_IPC_CHECK(client.job_release(first_job.value()).ok());
    // ReleaseMakesStatusAndResultNotFound regression.
    auto released_status = client.job_status(first_job.value());
    auto released_result = client.job_result(first_job.value());
    PS_IPC_CHECK(!released_status.ok());
    PS_IPC_CHECK(released_status.status().code == ErrorCode::NotFound);
    PS_IPC_CHECK(!released_result.ok());
    PS_IPC_CHECK(released_result.status().code == ErrorCode::NotFound);

    // RuntimeFailureGetsFreshJobIdAfterResubmit regression.
    auto failing_session = client.session_create(delayed_document(-1));
    PS_IPC_CHECK(failing_session.ok());
    auto failed_job = client.job_submit(failing_session.value());
    PS_IPC_CHECK(failed_job.ok());
    auto failed_status = wait_terminal(&client, failed_job.value());
    PS_IPC_CHECK(failed_status.state == JobState::Failed);
    PS_IPC_CHECK(failed_status.outcome.code == ErrorCode::InvalidArgument);
    auto failed_result = client.job_result(failed_job.value());
    PS_IPC_CHECK(!failed_result.ok());
    PS_IPC_CHECK(failed_result.status().code == ErrorCode::InvalidArgument);
    auto resubmitted_job = client.job_submit(failing_session.value());
    PS_IPC_CHECK(resubmitted_job.ok());
    PS_IPC_CHECK(resubmitted_job.value().instance ==
                 failed_job.value().instance);
    PS_IPC_CHECK(resubmitted_job.value().value != failed_job.value().value);
    auto resubmitted_status = wait_terminal(&client, resubmitted_job.value());
    PS_IPC_CHECK(resubmitted_status.state == JobState::Failed);
    PS_IPC_CHECK(resubmitted_status.outcome.code == ErrorCode::InvalidArgument);
    PS_IPC_CHECK(client.job_release(failed_job.value()).ok());
    PS_IPC_CHECK(client.job_release(resubmitted_job.value()).ok());
    PS_IPC_CHECK(client.session_close(failing_session.value()).ok());

    auto cancellable = client.session_create(delayed_document(500));
    PS_IPC_CHECK(cancellable.ok());
    auto cancellation_job = client.job_submit(cancellable.value());
    PS_IPC_CHECK(cancellation_job.ok());
    PS_IPC_CHECK(
        wait_state(&client, cancellation_job.value(), JobState::Running)
            .state == JobState::Running);
    PS_IPC_CHECK(client.job_cancel(cancellation_job.value()).ok());
    auto cancelled = wait_terminal(&client, cancellation_job.value());
    PS_IPC_CHECK(cancelled.state == JobState::Cancelled);
    PS_IPC_CHECK(cancelled.outcome.code == ErrorCode::Cancelled);
    PS_IPC_CHECK(!client.job_result(cancellation_job.value()).ok());

    auto closing = client.session_create(delayed_document(500));
    PS_IPC_CHECK(closing.ok());
    auto closing_job = client.job_submit(closing.value());
    PS_IPC_CHECK(closing_job.ok());
    PS_IPC_CHECK(client.session_close(closing.value()).ok());
    PS_IPC_CHECK(!client.job_status(closing_job.value()).ok());

    auto restart_session = client.session_create(addition_document(40.0, 2.0));
    PS_IPC_CHECK(restart_session.ok());
    auto restart_job = client.job_submit(restart_session.value());
    PS_IPC_CHECK(restart_job.ok());
    PS_IPC_CHECK(wait_terminal(&client, restart_job.value()).state ==
                 JobState::Succeeded);
    old_instance = info.value().instance_id;

    PS_IPC_CHECK(client.daemon_shutdown().ok());
    PS_IPC_CHECK(server_run.join().ok());
  }

  {
    internal::Server server(
        internal::ServerConfig{path,
                               internal::ServiceConfig{1U, 1U, 4U, false},
                               8,
                               64U,
                               {},
                               {}});
    ps::ipc::test::ServerRunGuard server_run(&server);
    Client client;
    PS_IPC_CHECK(client.connect(path).ok());
    auto info = client.daemon_info();
    PS_IPC_CHECK(info.ok());
    PS_IPC_CHECK(info.value().instance_id != 0U);
    PS_IPC_CHECK(info.value().instance_id != old_instance);
    PS_IPC_CHECK(info.value().active_sessions == 0U);
    PS_IPC_CHECK(info.value().active_jobs == 0U);
    PS_IPC_CHECK(!client.job_status(old_job).ok());
    PS_IPC_CHECK(!client.job_submit(old_session).ok());
    auto new_session = client.session_create(addition_document(1.0, 1.0));
    PS_IPC_CHECK(new_session.ok());
    PS_IPC_CHECK(new_session.value().value == old_session.value);
    PS_IPC_CHECK(new_session.value().instance != old_session.instance);
    PS_IPC_CHECK(!client.job_submit(old_session).ok());
    auto new_job = client.job_submit(new_session.value());
    PS_IPC_CHECK(new_job.ok());
    auto rejected_job = client.job_submit(new_session.value());
    PS_IPC_CHECK(!rejected_job.ok());
    PS_IPC_CHECK(rejected_job.status().code == ErrorCode::ResourceExhausted);
    PS_IPC_CHECK(new_job.value().value == old_job.value);
    PS_IPC_CHECK(new_job.value().instance != old_job.instance);
    PS_IPC_CHECK(!client.job_status(old_job).ok());
    PS_IPC_CHECK(client.daemon_shutdown().ok());
    PS_IPC_CHECK(server_run.join().ok());
  }
  return 0;
}
