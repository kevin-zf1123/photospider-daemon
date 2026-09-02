#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
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
namespace test = ps::ipc::test;

namespace {

/** @brief Child exit code emitted if a handler crosses its noexcept fence. */
constexpr int kTerminateExitCode = 86;

/** @brief Deadline for observing one handler cleanup transition. */
constexpr std::chrono::seconds kHandlerCleanupTimeout{2};

/**
 * @brief Builds one deterministic valid source document.
 * @return Three-node addition workflow with one named output.
 * @throws std::bad_alloc If source storage allocation fails.
 * @note The fixture uses only installed public kernel source types.
 */
ps::WorkflowDocument addition_document() {
  ps::WorkflowDocument document;
  document.nodes = {
      ps::WorkflowNode{1U, "core.constant", {}, {{"value", 1.0}}},
      ps::WorkflowNode{2U, "core.constant", {}, {{"value", 2.0}}},
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
 * @brief Returns a process-unique Unix-domain socket path.
 * @return Uncreated path under `/tmp`.
 * @throws std::bad_alloc If path construction fails.
 * @note A monotonic sequence separates all cases in one process.
 */
std::string socket_path() {
  static std::atomic<std::uint32_t> sequence{0U};
  return "/tmp/psd-fence-" + std::to_string(::getpid()) + "-" +
         std::to_string(sequence.fetch_add(1U)) + ".sock";
}

/**
 * @brief Waits for the exact active-handler count.
 * @param server Running server.
 * @param expected Expected atomic handler count.
 * @return True when observed before the fixed deadline.
 * @throws Nothing.
 */
bool wait_handler_count(internal::Server* server, std::uint32_t expected) {
  const auto deadline =
      std::chrono::steady_clock::now() + kHandlerCleanupTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (server->active_handler_count() == expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return server->active_handler_count() == expected;
}

/**
 * @brief Reports whether one server socket node is absent.
 * @param path Exact path to inspect.
 * @return True only for `ENOENT`.
 * @throws Nothing.
 */
bool socket_node_absent(const std::string& path) noexcept {
  struct stat state{};
  errno = 0;
  return ::lstat(path.c_str(), &state) != 0 && errno == ENOENT;
}

/**
 * @brief Checks that every method-specific success payload is empty.
 * @param response Exception-fenced failed response.
 * @return True when no success-only field remains populated.
 * @throws Nothing.
 * @note Correlation and the top-level failure status are intentionally kept.
 */
bool success_payload_is_empty(const internal::Response& response) noexcept {
  const auto& diagnostics = response.execution_result.diagnostics;
  return response.session_id.instance == 0U &&
         response.session_id.value == 0U && response.job_id.instance == 0U &&
         response.job_id.value == 0U &&
         response.job_status.job_id.instance == 0U &&
         response.job_status.job_id.value == 0U &&
         response.job_status.session_id.instance == 0U &&
         response.job_status.session_id.value == 0U &&
         response.job_status.outcome.message.empty() &&
         response.execution_result.values.empty() &&
         diagnostics.execute_us == 0U &&
         diagnostics.selected_backends.empty() &&
         diagnostics.transfer_count == 0U && diagnostics.transfer_bytes == 0U &&
         diagnostics.peak_live_bytes == 0U &&
         diagnostics.fallback_reasons.empty() &&
         diagnostics.operation_timings.empty() &&
         diagnostics.plan_digest.empty() && diagnostics.result_digest.empty() &&
         response.daemon_info.protocol_version == 0U &&
         response.daemon_info.instance_id == 0U &&
         response.daemon_info.service_version.empty() &&
         response.daemon_info.transport.empty() &&
         response.daemon_info.methods.empty() &&
         response.daemon_info.active_sessions == 0U &&
         response.daemon_info.active_jobs == 0U &&
         response.daemon_info.maximum_concurrency == 0U &&
         response.daemon_info.maximum_sessions == 0U &&
         !response.shutdown_after_write;
}

/**
 * @brief Exercises one no-payload dispatch primary/fallback double fault.
 * @param action Primary exception category.
 * @param expected Expected typed catch category.
 * @return True when correlation, status, and payload cleanup are exact.
 * @throws std::bad_alloc If fixture or Service construction fails.
 * @note `session.close` succeeds before the injected primary exception.
 */
bool dispatch_close_double_fault(test::ExceptionFenceFaultAction action,
                                 ps::ErrorCode expected) {
  test::reset_exception_fence_faults();
  internal::Service service(internal::ServiceConfig{1U, 4U, 2U, false});
  internal::Request create;
  create.request_id = 1U;
  create.method = internal::Method::SessionCreate;
  create.document = addition_document();
  const internal::Response created = service.dispatch(create);
  if (!created.status.ok()) {
    return false;
  }

  test::reset_exception_fence_faults();
  test::arm_exception_fence_fault(
      test::ExceptionFenceFaultPoint::DispatchPrimary, action);
  test::arm_exception_fence_fault(
      test::ExceptionFenceFaultPoint::DispatchFailureStatus,
      test::ExceptionFenceFaultAction::BadAlloc);
  internal::Request close;
  close.request_id = 91U;
  close.method = internal::Method::SessionClose;
  close.session_id = created.session_id;
  const internal::Response response = service.dispatch(close);
  const bool passed =
      response.request_id == close.request_id &&
      response.method == internal::Method::SessionClose &&
      !response.status.ok() && response.status.code == expected &&
      response.status.message.empty() && success_payload_is_empty(response) &&
      test::exception_fence_fault_hits(
          test::ExceptionFenceFaultPoint::DispatchPrimary) == 1U &&
      test::exception_fence_fault_hits(
          test::ExceptionFenceFaultPoint::DispatchFailureStatus) == 1U;
  test::reset_exception_fence_faults();
  return passed;
}

/**
 * @brief Verifies all dispatch catch categories and payload clearing.
 * @return True when bad-alloc, standard, and unknown exceptions are fenced.
 * @throws std::bad_alloc If fixture or Service construction fails.
 */
bool dispatch_double_fault_regression() {
  return dispatch_close_double_fault(test::ExceptionFenceFaultAction::BadAlloc,
                                     ps::ErrorCode::ResourceExhausted) &&
         dispatch_close_double_fault(
             test::ExceptionFenceFaultAction::StandardException,
             ps::ErrorCode::Internal) &&
         dispatch_close_double_fault(
             test::ExceptionFenceFaultAction::UnknownException,
             ps::ErrorCode::Internal);
}

/**
 * @brief Proves shutdown dispatch commits only after the primary fault seam.
 * @return True when failed shutdown leaves admission open and a later shutdown
 * commits normally.
 * @throws std::bad_alloc If Service or response staging allocation fails.
 * @note The one-shot primary fault is injected after response staging but must
 * precede both the admission fence and `shutdown_after_write` publication.
 */
bool shutdown_dispatch_commit_regression() {
  test::reset_exception_fence_faults();
  internal::Service service(internal::ServiceConfig{1U, 4U, 2U, false});
  test::arm_exception_fence_fault(
      test::ExceptionFenceFaultPoint::DispatchPrimary,
      test::ExceptionFenceFaultAction::BadAlloc);

  internal::Request first_shutdown;
  first_shutdown.request_id = 201U;
  first_shutdown.method = internal::Method::DaemonShutdown;
  const internal::Response failed = service.dispatch(first_shutdown);
  if (failed.status.ok() || failed.shutdown_after_write ||
      failed.status.code != ps::ErrorCode::ResourceExhausted) {
    test::reset_exception_fence_faults();
    return false;
  }

  internal::Request info;
  info.request_id = 202U;
  info.method = internal::Method::DaemonInfo;
  const internal::Response admitted = service.dispatch(info);
  if (!admitted.status.ok() || admitted.daemon_info.protocol_version != 3U) {
    test::reset_exception_fence_faults();
    return false;
  }

  internal::Request second_shutdown;
  second_shutdown.request_id = 203U;
  second_shutdown.method = internal::Method::DaemonShutdown;
  const internal::Response accepted = service.dispatch(second_shutdown);
  const bool passed =
      accepted.status.ok() && accepted.shutdown_after_write &&
      test::exception_fence_fault_hits(
          test::ExceptionFenceFaultPoint::DispatchPrimary) == 3U;
  test::reset_exception_fence_faults();
  return passed;
}

/**
 * @brief Coordinates accepted shutdown responses before their real writes.
 * @note Handler callbacks block only after Service acceptance; the test closes
 * peer descriptors, then releases all callbacks to observe actual failures.
 */
class ShutdownResponseBarrier final {
 public:
  /**
   * @brief Sets the exact number of shutdown responses to coordinate.
   * @param expected Positive accepted-response count.
   * @throws Nothing.
   */
  explicit ShutdownResponseBarrier(std::uint32_t expected) noexcept
      : expected_(expected) {}

  /**
   * @brief Forbids duplicating synchronization state.
   * @param other Source barrier that cannot be copied.
   * @throws Nothing; the operation is deleted.
   */
  ShutdownResponseBarrier(const ShutdownResponseBarrier& other) = delete;
  /**
   * @brief Forbids assigning synchronization state.
   * @param other Source barrier that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  ShutdownResponseBarrier& operator=(const ShutdownResponseBarrier& other) =
      delete;
  /**
   * @brief Forbids moving address-stable callback state.
   * @param other Source barrier that cannot be moved.
   * @throws Nothing; the operation is deleted.
   */
  ShutdownResponseBarrier(ShutdownResponseBarrier&& other) = delete;
  /**
   * @brief Forbids move-assigning address-stable callback state.
   * @param other Source barrier that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  ShutdownResponseBarrier& operator=(ShutdownResponseBarrier&& other) = delete;

  /**
   * @brief Blocks one accepted response until the test releases the barrier.
   * @param accepted_shutdown Whether Service accepted this shutdown.
   * @throws std::system_error If test synchronization fails.
   * @note Non-shutdown and failed-shutdown responses return immediately.
   */
  void wait_after_accept(bool accepted_shutdown) {
    if (!accepted_shutdown) {
      return;
    }
    accepted_.fetch_add(1U, std::memory_order_acq_rel);
    changed_.notify_all();
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this] { return released_; });
  }

  /**
   * @brief Waits until every expected shutdown response is held.
   * @param timeout Maximum bounded wait.
   * @return True when the exact expected count arrived before the deadline.
   * @throws std::system_error If test synchronization fails.
   */
  bool wait_for_all(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] {
      return accepted_.load(std::memory_order_acquire) == expected_;
    });
  }

  /**
   * @brief Releases every held response callback exactly once.
   * @throws Nothing; a synchronization failure leaves test teardown to the
   * surrounding Server run guard.
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
   * @brief Records one real accepted-shutdown response write outcome.
   * @param accepted_shutdown Whether the response accepted shutdown.
   * @param status Exact `write_frame` result.
   * @throws Nothing.
   */
  void observe_write(bool accepted_shutdown,
                     const ps::Status& status) noexcept {
    if (accepted_shutdown && !status.ok()) {
      write_failures_.fetch_add(1U, std::memory_order_relaxed);
    }
  }

  /**
   * @brief Returns the number of accepted responses that reached the barrier.
   * @return Monotonic accepted count.
   * @throws Nothing.
   */
  [[nodiscard]] std::uint32_t accepted_count() const noexcept {
    return accepted_.load(std::memory_order_acquire);
  }

  /**
   * @brief Returns the number of observed real write failures.
   * @return Monotonic failed-write count.
   * @throws Nothing.
   */
  [[nodiscard]] std::uint32_t write_failure_count() const noexcept {
    return write_failures_.load(std::memory_order_acquire);
  }

 private:
  /** @brief Exact accepted-response count required to release the test. */
  const std::uint32_t expected_;
  /** @brief Serializes the release predicate and condition wait. */
  std::mutex mutex_;
  /** @brief Wakes the test and held response handlers. */
  std::condition_variable changed_;
  /** @brief Number of accepted responses that reached the hook. */
  std::atomic<std::uint32_t> accepted_{0U};
  /** @brief Number of accepted response writes that returned non-Ok. */
  std::atomic<std::uint32_t> write_failures_{0U};
  /** @brief Whether held handlers may attempt their real writes. */
  bool released_ = false;
};

/** @brief Borrowed active shutdown-response barrier for test callbacks. */
ShutdownResponseBarrier* g_shutdown_response_barrier = nullptr;

/**
 * @brief Bridges the test-runtime post-dispatch observer to the active barrier.
 * @param accepted_shutdown Whether Service accepted daemon shutdown.
 * @throws std::system_error If barrier synchronization fails.
 */
void wait_at_shutdown_response_barrier(bool accepted_shutdown) {
  if (g_shutdown_response_barrier) {
    g_shutdown_response_barrier->wait_after_accept(accepted_shutdown);
  }
}

/**
 * @brief Bridges the test-runtime real-write observer to the active barrier.
 * @param accepted_shutdown Whether Service accepted daemon shutdown.
 * @param status Exact product `write_frame` result.
 * @throws Nothing.
 */
void record_shutdown_response_write(bool accepted_shutdown,
                                    const ps::Status& status) noexcept {
  if (g_shutdown_response_barrier) {
    g_shutdown_response_barrier->observe_write(accepted_shutdown, status);
  }
}

/**
 * @brief Owns one process-global shutdown-response observer installation.
 * @note Declare before Server so callbacks clear only after all handlers join.
 */
class ShutdownResponseObserverScope final {
 public:
  /**
   * @brief Installs callbacks bound to one address-stable barrier.
   * @param barrier Nonnull barrier that outlives this scope.
   * @throws std::invalid_argument If `barrier` is null.
   */
  explicit ShutdownResponseObserverScope(ShutdownResponseBarrier* barrier) {
    if (!barrier) {
      throw std::invalid_argument("shutdown response barrier is null");
    }
    g_shutdown_response_barrier = barrier;
    test::install_shutdown_response_observers(wait_at_shutdown_response_barrier,
                                              record_shutdown_response_write);
  }

  /**
   * @brief Clears test-runtime callbacks before the barrier retires.
   * @throws Nothing.
   */
  ~ShutdownResponseObserverScope() noexcept {
    test::install_shutdown_response_observers(nullptr, nullptr);
    g_shutdown_response_barrier = nullptr;
  }

  /**
   * @brief Forbids duplicate process-global observer ownership.
   * @param other Source scope that cannot be copied.
   * @throws Nothing; the operation is deleted.
   */
  ShutdownResponseObserverScope(const ShutdownResponseObserverScope& other) =
      delete;
  /**
   * @brief Forbids assigning process-global observer ownership.
   * @param other Source scope that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  ShutdownResponseObserverScope& operator=(
      const ShutdownResponseObserverScope& other) = delete;
  /**
   * @brief Forbids moving one process-global callback installation.
   * @param other Source scope that cannot be moved.
   * @throws Nothing; the operation is deleted.
   */
  ShutdownResponseObserverScope(ShutdownResponseObserverScope&& other) = delete;
  /**
   * @brief Forbids move-assigning process-global callback ownership.
   * @param other Source scope that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  ShutdownResponseObserverScope& operator=(
      ShutdownResponseObserverScope&& other) = delete;
};

/**
 * @brief Sends one complete shutdown request without reading its response.
 * @param path Exact bound local socket path.
 * @param request_id Nonzero request correlation id.
 * @return Connected descriptor retained by the caller, or typed failure.
 * @throws std::bad_alloc If request encoding allocation fails.
 * @note The caller closes the descriptor only after the server acceptance
 * barrier is reached, creating a deterministic real write failure.
 */
ps::Result<internal::UniqueDescriptor> send_shutdown_without_read(
    const std::string& path, std::uint64_t request_id) {
  auto connection = internal::connect_unix_socket(path);
  if (!connection.ok()) {
    return ps::Result<internal::UniqueDescriptor>(connection.status());
  }
  internal::Request request;
  request.request_id = request_id;
  request.method = internal::Method::DaemonShutdown;
  auto encoded = internal::encode_request(request);
  if (!encoded.ok()) {
    return ps::Result<internal::UniqueDescriptor>(encoded.status());
  }
  const ps::Status written =
      internal::write_frame(connection.value().get(), encoded.value());
  if (!written.ok()) {
    return ps::Result<internal::UniqueDescriptor>(written);
  }
  return ps::Result<internal::UniqueDescriptor>(connection.take_value());
}

/**
 * @brief Exercises accepted shutdown after one or more peer-close ack failures.
 * @param request_count Number of concurrently held accepted shutdowns.
 * @return True when every real write fails and the server fully settles.
 * @throws std::bad_alloc If test/server staging allocation fails.
 * @throws std::system_error If server threads or synchronization fail.
 * @note A bounded rescue stop runs only after recording a failed invariant, so
 * teardown cannot mask whether accepted shutdown stopped the server itself.
 */
bool shutdown_write_failure_regression(std::uint32_t request_count) {
  const std::string path = socket_path();
  ShutdownResponseBarrier barrier(request_count);
  ShutdownResponseObserverScope observer_scope(&barrier);
  internal::ServerConfig config{
      path, internal::ServiceConfig{1U, 8U, 2U, false}, 8, 8U, {}, {}};
  internal::Server server(std::move(config));
  ps::ipc::test::ServerRunGuard server_run(&server);
  std::vector<internal::UniqueDescriptor> peers;
  peers.reserve(request_count);
  bool requests_sent = true;
  for (std::uint32_t index = 0U; index < request_count; ++index) {
    auto peer = send_shutdown_without_read(path, index + 1U);
    if (!peer.ok()) {
      requests_sent = false;
      break;
    }
    peers.push_back(peer.take_value());
  }
  bool all_accepted = false;
  if (requests_sent) {
    all_accepted = barrier.wait_for_all(kHandlerCleanupTimeout);
  }
  for (auto& peer : peers) {
    peer.reset();
  }
  barrier.release();
  const bool completed =
      all_accepted && server_run.ready_within(kHandlerCleanupTimeout);
  if (!completed) {
    server.request_stop();
  }
  const ps::Status run_status = server_run.join();
  return requests_sent && all_accepted && completed && run_status.ok() &&
         barrier.accepted_count() == request_count &&
         barrier.write_failure_count() == request_count &&
         server.active_handler_count() == 0U &&
         server.active_connection_count_for_test() == 0U &&
         server.retained_handler_count() == 0U && socket_node_absent(path);
}

/**
 * @brief Exercises accepted shutdown when normal response encoding allocates.
 * @return True when the client observes failure and the server still settles.
 * @throws std::bad_alloc If test/server staging allocation fails.
 * @throws std::system_error If the server test thread cannot start.
 */
bool shutdown_encode_failure_regression() {
  test::reset_exception_fence_faults();
  test::arm_exception_fence_fault(
      test::ExceptionFenceFaultPoint::ResponseEncode,
      test::ExceptionFenceFaultAction::BadAlloc);
  const std::string path = socket_path();
  internal::Server server(
      internal::ServerConfig{path,
                             internal::ServiceConfig{1U, 4U, 2U, false},
                             4,
                             2U,
                             {},
                             {}});
  ps::ipc::test::ServerRunGuard server_run(&server);
  ps::ipc::Client client;
  const bool connected = client.connect(path).ok();
  ps::Status shutdown = ps::Status::failure(ps::ErrorCode::Internal,
                                            "shutdown client did not connect");
  if (connected) {
    shutdown = client.daemon_shutdown();
  }
  const bool completed =
      connected && server_run.ready_within(kHandlerCleanupTimeout);
  if (!completed) {
    server.request_stop();
  }
  const ps::Status run_status = server_run.join();
  const bool passed =
      connected && !shutdown.ok() && completed && run_status.ok() &&
      test::exception_fence_fault_hits(
          test::ExceptionFenceFaultPoint::ResponseEncode) == 1U &&
      server.active_handler_count() == 0U &&
      server.active_connection_count_for_test() == 0U &&
      server.retained_handler_count() == 0U && socket_node_absent(path);
  test::reset_exception_fence_faults();
  return passed;
}

/**
 * @brief Runs one handler primary/fallback double fault in this test process.
 * @param action Primary handler exception category.
 * @param expected Expected typed catch category.
 * @return True only when the server survives and returns a failed sentinel.
 * @throws Nothing; setup or protocol exceptions become a failed assertion.
 * @note CTest invokes each case as an independent process. A pre-fix noexcept
 * escape therefore terminates only that test process, never the CTest owner.
 */
bool handler_double_fault_case(test::ExceptionFenceFaultAction action,
                               ps::ErrorCode expected) noexcept {
  std::set_terminate([] { std::_Exit(kTerminateExitCode); });
  try {
    test::reset_exception_fence_faults();
    test::arm_exception_fence_fault(
        test::ExceptionFenceFaultPoint::HandlerPrimary, action);
    test::arm_exception_fence_fault(
        test::ExceptionFenceFaultPoint::HandlerFailureStatus,
        test::ExceptionFenceFaultAction::BadAlloc);
    const std::string path = socket_path();
    internal::Server server(
        internal::ServerConfig{path,
                               internal::ServiceConfig{1U, 4U, 2U, false},
                               4,
                               2U,
                               {},
                               {}});
    ps::ipc::test::ServerRunGuard server_run(&server);
    auto connection = internal::connect_unix_socket(path);
    if (!connection.ok()) {
      server.request_stop();
      static_cast<void>(server_run.join());
      return false;
    }
    auto frame = internal::read_frame(connection.value().get());
    if (!frame.ok()) {
      server.request_stop();
      static_cast<void>(server_run.join());
      return false;
    }
    auto response = internal::decode_protocol_error(frame.value());
    auto closed = internal::read_frame(connection.value().get());
    const bool handler_finished = wait_handler_count(&server, 0U);
    server.request_stop();
    const ps::Status run_status = server_run.join();
    const bool passed =
        response.ok() && response.value().request_id == 0U &&
        response.value().method == internal::Method::DaemonInfo &&
        response.value().status.code == expected &&
        response.value().status.message.empty() && !closed.ok() &&
        closed.status().code == ps::ErrorCode::NotFound && handler_finished &&
        server.active_connection_count_for_test() == 0U &&
        server.retained_handler_count() == 0U && run_status.ok() &&
        socket_node_absent(path) &&
        test::exception_fence_fault_hits(
            test::ExceptionFenceFaultPoint::HandlerPrimary) == 1U &&
        test::exception_fence_fault_hits(
            test::ExceptionFenceFaultPoint::HandlerFailureStatus) == 1U;
    return passed;
  } catch (...) {
    return false;
  }
}

/**
 * @brief Selects one independently registered handler double-fault case.
 * @param mode Exact command-line case name supplied by CTest.
 * @return True only for a known case whose typed behavior is exact.
 * @throws Nothing.
 * @note Separate CTest processes preserve the regression signal if an old
 * implementation terminates at the noexcept handler boundary.
 */
bool run_handler_mode(const char* mode) noexcept {
  if (!mode) {
    return false;
  }
  if (std::strcmp(mode, "handler-bad-alloc") == 0) {
    return handler_double_fault_case(test::ExceptionFenceFaultAction::BadAlloc,
                                     ps::ErrorCode::ResourceExhausted);
  }
  if (std::strcmp(mode, "handler-standard") == 0) {
    return handler_double_fault_case(
        test::ExceptionFenceFaultAction::StandardException,
        ps::ErrorCode::Internal);
  }
  if (std::strcmp(mode, "handler-unknown") == 0) {
    return handler_double_fault_case(
        test::ExceptionFenceFaultAction::UnknownException,
        ps::ErrorCode::Internal);
  }
  return false;
}

/** @brief Optional injected failure after registration response creation. */
enum class ProtocolFailureMode {
  /** @brief Allow the typed failure response to reach the peer. */
  None,
  /** @brief Fail the best-effort protocol-error encoder. */
  Encode,
  /** @brief Fail immediately before the best-effort frame write. */
  Write,
};

/**
 * @brief Exercises one active-descriptor registration allocation failure.
 * @param mode Optional secondary protocol-response failure.
 * @return True when typed attempt, correlation, and cleanup are exact.
 * @throws std::bad_alloc If test setup allocation fails.
 * @throws std::system_error If the server test thread cannot start.
 */
bool registration_failure_case(ProtocolFailureMode mode) {
  test::reset_exception_fence_faults();
  test::arm_exception_fence_fault(
      test::ExceptionFenceFaultPoint::ConnectionRegistration,
      test::ExceptionFenceFaultAction::BadAlloc);
  if (mode == ProtocolFailureMode::Encode) {
    test::arm_exception_fence_fault(
        test::ExceptionFenceFaultPoint::ProtocolFailureEncode,
        test::ExceptionFenceFaultAction::BadAlloc);
  } else if (mode == ProtocolFailureMode::Write) {
    test::arm_exception_fence_fault(
        test::ExceptionFenceFaultPoint::ProtocolFailureWrite,
        test::ExceptionFenceFaultAction::BadAlloc);
  }

  const std::string path = socket_path();
  bool transport_passed = false;
  bool cleanup_passed = false;
  {
    internal::Server server(
        internal::ServerConfig{path,
                               internal::ServiceConfig{1U, 4U, 2U, false},
                               4,
                               2U,
                               {},
                               {}});
    ps::ipc::test::ServerRunGuard server_run(&server);
    auto connection = internal::connect_unix_socket(path);
    if (!connection.ok()) {
      server.request_stop();
      static_cast<void>(server_run.join());
      return false;
    }
    auto frame = internal::read_frame(connection.value().get());
    if (mode == ProtocolFailureMode::None) {
      if (frame.ok()) {
        auto response = internal::decode_protocol_error(frame.value());
        auto closed = internal::read_frame(connection.value().get());
        transport_passed =
            response.ok() && response.value().request_id == 0U &&
            response.value().method == internal::Method::DaemonInfo &&
            response.value().status.code == ps::ErrorCode::ResourceExhausted &&
            response.value().status.message.empty() && !closed.ok() &&
            closed.status().code == ps::ErrorCode::NotFound;
      }
    } else {
      transport_passed =
          !frame.ok() && frame.status().code == ps::ErrorCode::NotFound;
    }
    const bool handler_finished = wait_handler_count(&server, 0U);
    server.request_stop();
    const ps::Status run_status = server_run.join();
    cleanup_passed = handler_finished && server.active_handler_count() == 0U &&
                     server.active_connection_count_for_test() == 0U &&
                     server.retained_handler_count() == 0U && run_status.ok() &&
                     socket_node_absent(path);
  }

  bool rebound = false;
  try {
    internal::Server rebound_server(
        internal::ServerConfig{path,
                               internal::ServiceConfig{1U, 4U, 2U, false},
                               4,
                               2U,
                               {},
                               {}});
    rebound = true;
  } catch (...) {
    rebound = false;
  }
  const std::uint32_t encode_hits = test::exception_fence_fault_hits(
      test::ExceptionFenceFaultPoint::ProtocolFailureEncode);
  const std::uint32_t write_hits = test::exception_fence_fault_hits(
      test::ExceptionFenceFaultPoint::ProtocolFailureWrite);
  const bool attempts_passed =
      test::exception_fence_fault_hits(
          test::ExceptionFenceFaultPoint::ConnectionRegistration) == 1U &&
      encode_hits == 1U &&
      write_hits == (mode == ProtocolFailureMode::Encode ? 0U : 1U);
  test::reset_exception_fence_faults();
  return transport_passed && cleanup_passed && rebound && attempts_passed &&
         socket_node_absent(path);
}

/**
 * @brief Verifies typed registration rejection and secondary response fences.
 * @return True for delivered, encode-failed, and write-failed cleanup cases.
 * @throws std::bad_alloc If test setup allocation fails.
 * @throws std::system_error If the server test thread cannot start.
 */
bool registration_failure_regression() {
  return registration_failure_case(ProtocolFailureMode::None) &&
         registration_failure_case(ProtocolFailureMode::Encode) &&
         registration_failure_case(ProtocolFailureMode::Write);
}

}  // namespace

/**
 * @brief Runs deterministic daemon exception-fence regressions.
 * @param argc Process argument count.
 * @param argv Optional one-case handler selector registered by CTest.
 * @return Zero on success and one on the first failed invariant.
 * @throws Nothing; unexpected exceptions produce a process failure.
 * @note Handler modes are separate processes so a historical noexcept escape
 * cannot terminate the parent CTest process or suppress sibling regressions.
 */
int main(int argc, char** argv) {
  if (argc == 2) {
    return run_handler_mode(argv[1]) ? 0 : 1;
  }
  if (argc != 1) {
    return 2;
  }
  PS_IPC_CHECK(dispatch_double_fault_regression());
  PS_IPC_CHECK(shutdown_dispatch_commit_regression());
  PS_IPC_CHECK(shutdown_write_failure_regression(1U));
  PS_IPC_CHECK(shutdown_write_failure_regression(4U));
  PS_IPC_CHECK(shutdown_encode_failure_regression());
  PS_IPC_CHECK(registration_failure_regression());
  return 0;
}
