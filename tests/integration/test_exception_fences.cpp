#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "ipc/codec.hpp"
#include "ipc/frame.hpp"
#include "ipc/unix_socket.hpp"
#include "orchestration/service.hpp"
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
  PS_IPC_CHECK(registration_failure_regression());
  return 0;
}
