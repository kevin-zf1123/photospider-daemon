#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>

#include "orchestration/service.hpp"
#include "photospider/ipc/client.hpp"
#include "server/server.hpp"
#include "support/test_support.hpp"

namespace {

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
 * @brief Polls one execution until a terminal state or bounded timeout.
 * @param client Connected sequential client.
 * @param id Existing execution identifier.
 * @return Terminal status.
 * @throws std::runtime_error On RPC failure or timeout.
 * @note Polling is test-only and never changes Job state.
 */
ps::ipc::JobStatus wait_terminal(ps::ipc::Client* client, ps::ipc::JobId id) {
  using namespace std::chrono_literals;
  for (int attempt = 0; attempt < 400; ++attempt) {
    auto status = client->job_status(id);
    if (!status.ok()) {
      throw std::runtime_error(status.status().message);
    }
    if (status.value().state == ps::ipc::JobState::Succeeded ||
        status.value().state == ps::ipc::JobState::Failed ||
        status.value().state == ps::ipc::JobState::Cancelled) {
      return status.value();
    }
    std::this_thread::sleep_for(5ms);
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
 * @note Polling is test-only and never changes Job state.
 */
ps::ipc::JobStatus wait_state(ps::ipc::Client* client, ps::ipc::JobId id,
                              ps::ipc::JobState expected) {
  using namespace std::chrono_literals;
  for (int attempt = 0; attempt < 400; ++attempt) {
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
    std::this_thread::sleep_for(5ms);
  }
  throw std::runtime_error("execution did not reach expected state");
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
 * @brief Starts a bound server's blocking accept loop asynchronously.
 * @param server Nonnull bound server that outlives the returned future.
 * @return Future resolving to the server run status.
 * @throws std::bad_alloc If asynchronous state allocation fails.
 * @throws std::system_error If the test thread cannot be started.
 * @note Exactly one async run is started per server.
 */
std::future<ps::Status> start_server(ps::ipc::internal::Server* server) {
  return std::async(std::launch::async, [server] { return server->run(); });
}

}  // namespace

/**
 * @brief Exercises exact v3 methods, multi-namespace execution, cancellation,
 * close/release cleanup, graceful shutdown, and restart loss.
 * @return Zero when all checks pass.
 * @throws std::bad_alloc If test setup allocation fails.
 * @throws std::runtime_error If bounded polling cannot observe expected state.
 * @note Behavioral failures otherwise return nonzero through `PS_IPC_CHECK`.
 */
int main() {
  using namespace ps;
  using namespace ps::ipc;

  {
    internal::Service service(internal::ServiceConfig{1U, 4U, false});
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

  const std::string path = socket_path();
  SessionId old_session;
  JobId old_job;
  std::uint64_t old_instance = 0U;
  {
    internal::Server server(internal::ServerConfig{
        path, internal::ServiceConfig{2U, 32U, false}, 8});
    auto server_result = start_server(&server);

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

    auto first = client.session_create(addition_document(2.0, 3.0));
    auto second = client.session_create(addition_document(10.0, 20.0));
    PS_IPC_CHECK(first.ok());
    PS_IPC_CHECK(second.ok());
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
    PS_IPC_CHECK(!client.job_status(first_job.value()).ok());

    auto saturation = client.session_create(delayed_document(500));
    PS_IPC_CHECK(saturation.ok());
    auto running_a = client.job_submit(saturation.value());
    auto running_b = client.job_submit(saturation.value());
    PS_IPC_CHECK(running_a.ok());
    PS_IPC_CHECK(running_b.ok());
    PS_IPC_CHECK(
        wait_state(&client, running_a.value(), JobState::Running).state ==
        JobState::Running);
    PS_IPC_CHECK(
        wait_state(&client, running_b.value(), JobState::Running).state ==
        JobState::Running);
    auto queued = client.job_submit(saturation.value());
    PS_IPC_CHECK(queued.ok());
    PS_IPC_CHECK(client.job_status(queued.value()).value().state ==
                 JobState::Queued);
    PS_IPC_CHECK(client.job_cancel(queued.value()).ok());
    PS_IPC_CHECK(client.job_status(queued.value()).value().state ==
                 JobState::Queued);
    PS_IPC_CHECK(client.job_cancel(running_a.value()).ok());
    PS_IPC_CHECK(client.job_cancel(running_b.value()).ok());
    PS_IPC_CHECK(wait_terminal(&client, running_a.value()).state ==
                 JobState::Cancelled);
    PS_IPC_CHECK(wait_terminal(&client, running_b.value()).state ==
                 JobState::Cancelled);
    PS_IPC_CHECK(wait_terminal(&client, queued.value()).state ==
                 JobState::Cancelled);
    PS_IPC_CHECK(client.session_close(saturation.value()).ok());

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
    PS_IPC_CHECK(server_result.get().ok());
  }

  {
    internal::Server server(internal::ServerConfig{
        path, internal::ServiceConfig{1U, 1U, false}, 8});
    auto server_result = start_server(&server);
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
    PS_IPC_CHECK(server_result.get().ok());
  }
  return 0;
}
