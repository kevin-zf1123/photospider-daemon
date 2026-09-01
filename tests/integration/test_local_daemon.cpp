#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
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
