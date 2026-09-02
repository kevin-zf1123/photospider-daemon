#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ipc/codec.hpp"
#include "ipc/frame.hpp"
#include "ipc/unix_socket.hpp"
#include "photospider/ipc/client.hpp"
#include "support/test_support.hpp"

namespace {

/**
 * @brief Builds one complete public compiler source for codec round trips.
 * @return Two-node identity workflow with one named output.
 * @throws std::bad_alloc If source-container allocation fails.
 * @note The source contains no internal compiler representation.
 */
ps::WorkflowDocument document() {
  ps::WorkflowDocument value;
  value.nodes = {
      ps::WorkflowNode{1U, "core.constant", {}, {{"value", 3.0}}},
      ps::WorkflowNode{2U,
                       "core.identity",
                       {ps::WorkflowInput{1U, "value"}},
                       {{"label", std::string("roundtrip")}}},
  };
  value.outputs = {ps::WorkflowOutput{"value", 2U, "value"}};
  return value;
}

/**
 * @brief Returns one process-unique fake-server Unix socket path.
 * @return Uncreated bounded path under `/tmp`.
 * @throws std::bad_alloc If path construction fails.
 * @note The helper is called only from the single test thread.
 */
std::string socket_path() {
  static std::uint32_t sequence = 0U;
  return "/tmp/psd-codec-" + std::to_string(::getpid()) + "-" +
         std::to_string(sequence++) + ".sock";
}

/**
 * @brief Closed failed-response variants served to the public Client.
 *
 * @note Every variant is a complete non-Ok response. Only `FailedSentinel`
 * uses the protocol-error sentinel; the other variants deliberately violate
 * one ordinary response-correlation component.
 */
enum class PublicClientResponseKind : std::uint8_t {
  /** @brief Legal zero-id/daemon.info failed protocol sentinel. */
  FailedSentinel,
  /** @brief Ordinary response with a wrong nonzero request id. */
  WrongRequestId,
  /** @brief Ordinary response with the expected id but wrong method. */
  WrongMethod,
};

/**
 * @brief Owns one fake-server thread and its exceptional-path Client wakeup.
 *
 * The normal path joins explicitly after the public call. If setup, encoding,
 * or result inspection throws, destruction first disconnects the Client so a
 * responder blocked in request I/O observes EOF, then joins the thread.
 *
 * @note The referenced Client must outlive this owner. The responder never
 * accesses the Client object itself.
 */
class PublicClientServerThread final {
 public:
  /**
   * @brief Takes ownership of one joinable fake-server thread.
   * @param client Nonnull Client used only to interrupt exceptional-path I/O.
   * @param thread Joinable responder thread transferred to this owner.
   * @throws Nothing.
   * @note `client` must outlive this object.
   */
  PublicClientServerThread(ps::ipc::Client* client, std::thread thread) noexcept
      : client_(client), thread_(std::move(thread)) {}

  /**
   * @brief Disconnects the Client if needed and joins the responder.
   * @throws Nothing under the owner-thread and joinable-state invariants.
   * @note Client disconnection is idempotent and occurs before a fallback join.
   */
  ~PublicClientServerThread() noexcept {
    client_->disconnect();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  /**
   * @brief Forbids duplicating Client/thread cleanup ownership.
   * @param other Source owner that cannot be copied.
   * @throws Nothing; the operation is deleted.
   */
  PublicClientServerThread(const PublicClientServerThread& other) = delete;

  /**
   * @brief Forbids copy assignment of Client/thread cleanup ownership.
   * @param other Source owner that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  PublicClientServerThread& operator=(const PublicClientServerThread& other) =
      delete;

  /**
   * @brief Joins the responder after a completed public Client call.
   * @throws std::system_error If the platform thread join fails.
   * @note Repeated calls are harmless after the first successful join.
   */
  void join() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  /** @brief Borrowed Client used to wake request I/O during unwinding. */
  ps::ipc::Client* client_;
  /** @brief Sole ownership of the one-shot responder thread. */
  std::thread thread_;
};

/**
 * @brief Captures public Client behavior after one complete failed response.
 *
 * @note `server_completed` distinguishes a Client result from fixture setup or
 * transport failure, making every correlation assertion independently
 * falsifiable.
 */
struct PublicClientObservation final {
  /** @brief Whether the responder validated the request and wrote its frame. */
  bool server_completed = false;
  /** @brief Whether `Client::daemon_info()` unexpectedly returned a value. */
  bool result_ok = false;
  /** @brief Returned failure category, or `Ok` for an unexpected value. */
  ps::ErrorCode status_code = ps::ErrorCode::Ok;
  /** @brief Descriptor ownership observed immediately after the public call. */
  bool connected_after_call = false;
};

/**
 * @brief Encodes one complete failed fake-server response.
 * @param kind Legal sentinel or one ordinary correlation mismatch.
 * @param request Completely decoded daemon.info request from the Client.
 * @return Bounded response payload or typed fixture-encoding failure.
 * @throws std::bad_alloc If response or diagnostic allocation fails.
 * @note Ordinary mismatch variants use `encode_response`, remain non-Ok, and
 * carry no success-only payload.
 */
ps::Result<std::vector<std::uint8_t>> encode_public_client_response(
    PublicClientResponseKind kind, const ps::ipc::internal::Request& request) {
  using ps::ErrorCode;
  using ps::Status;
  using ps::ipc::internal::encode_protocol_error;
  using ps::ipc::internal::encode_response;
  using ps::ipc::internal::Method;
  using ps::ipc::internal::Response;

  const Status failure = Status::failure(ErrorCode::ResourceExhausted,
                                         "one-shot capacity rejection");
  if (kind == PublicClientResponseKind::FailedSentinel) {
    return encode_protocol_error(failure);
  }

  Response response;
  response.request_id = request.request_id;
  response.method = request.method;
  response.status = failure;
  if (kind == PublicClientResponseKind::WrongRequestId) {
    response.request_id = request.request_id + 1U;
  } else if (kind == PublicClientResponseKind::WrongMethod) {
    response.method = Method::DaemonShutdown;
  }
  return encode_response(response);
}

/**
 * @brief Runs one complete public daemon.info call against a fake Unix server.
 * @param kind Failed response correlation served after the request is decoded.
 * @return Explicit server, result, status, and connection observations.
 * @throws std::bad_alloc If fixture, protocol, or result allocation fails.
 * @throws std::system_error If the one-shot responder thread cannot start or
 * join.
 * @note The request traverses public Client, frame I/O, and the real codec. All
 * descriptors use RAII; exceptional paths disconnect before joining.
 */
PublicClientObservation observe_public_client_response(
    PublicClientResponseKind kind) {
  using ps::ipc::Client;
  using ps::ipc::internal::accept_same_user;
  using ps::ipc::internal::create_unix_listener;
  using ps::ipc::internal::decode_request;
  using ps::ipc::internal::Method;
  using ps::ipc::internal::read_frame;
  using ps::ipc::internal::write_frame;

  const std::string path = socket_path();
  auto listener = create_unix_listener(path, 1);
  if (!listener.ok()) {
    return {};
  }
  Client client;
  if (!client.connect(path).ok()) {
    return {};
  }

  bool server_completed = false;
  PublicClientServerThread server(&client, std::thread([&] {
    try {
      auto accepted = accept_same_user(listener.value().descriptor.get());
      if (accepted.disposition !=
          ps::ipc::internal::AcceptDisposition::Accepted) {
        return;
      }
      auto request_frame = read_frame(accepted.descriptor.get());
      if (!request_frame.ok()) {
        return;
      }
      auto request = decode_request(request_frame.value());
      if (!request.ok() || request.value().request_id != 1U ||
          request.value().method != Method::DaemonInfo) {
        return;
      }
      auto response = encode_public_client_response(kind, request.value());
      if (!response.ok() ||
          !write_frame(accepted.descriptor.get(), response.value()).ok()) {
        return;
      }
      server_completed = true;
    } catch (...) {
    }
  }));
  auto info = client.daemon_info();
  server.join();
  return PublicClientObservation{
      server_completed, info.ok(),
      info.ok() ? ps::ErrorCode::Ok : info.status().code, client.connected()};
}

}  // namespace

/**
 * @brief Exercises v3 request/response round trips and malformed frame fences.
 * @return Zero when all checks pass.
 * @throws std::bad_alloc If test setup allocation fails.
 * @throws std::system_error If the one-shot responder thread cannot start or
 * join.
 * @note Behavioral failures otherwise return nonzero through `PS_IPC_CHECK`.
 */
int main() {
  using ps::Backend;
  using ps::ElementType;
  using ps::ErrorCode;
  using ps::OperationTiming;
  using ps::Region;
  using ps::Status;
  using ps::StridedLayout;
  using ps::Value;
  using ps::ValueDescriptor;
  using ps::ValueFacet;
  using ps::ipc::Client;
  using ps::ipc::JobId;
  using ps::ipc::JobState;
  using ps::ipc::SessionId;
  using ps::ipc::internal::decode_protocol_error;
  using ps::ipc::internal::decode_request;
  using ps::ipc::internal::decode_response;
  using ps::ipc::internal::encode_protocol_error;
  using ps::ipc::internal::encode_request;
  using ps::ipc::internal::encode_response;
  using ps::ipc::internal::Method;
  using ps::ipc::internal::method_inventory;
  using ps::ipc::internal::read_frame;
  using ps::ipc::internal::Request;
  using ps::ipc::internal::Response;
  using ps::ipc::internal::write_frame;

  Request request;
  request.request_id = 7U;
  request.method = Method::SessionCreate;
  request.document = document();
  auto encoded = encode_request(request);
  PS_IPC_CHECK(encoded.ok());
  auto decoded = decode_request(encoded.value());
  PS_IPC_CHECK(decoded.ok());
  PS_IPC_CHECK(decoded.value().request_id == 7U);
  PS_IPC_CHECK(decoded.value().document.nodes.size() == 2U);
  PS_IPC_CHECK(decoded.value().document.outputs.front().name == "value");

  const Status malformed_status =
      Status::failure(ErrorCode::InvalidArgument, "malformed request fixture");
  auto correlated_protocol_error =
      encode_protocol_error(malformed_status, &encoded.value());
  PS_IPC_CHECK(correlated_protocol_error.ok());
  auto decoded_correlated_protocol_error =
      decode_protocol_error(correlated_protocol_error.value());
  PS_IPC_CHECK(decoded_correlated_protocol_error.ok());
  PS_IPC_CHECK(decoded_correlated_protocol_error.value().request_id == 7U);
  PS_IPC_CHECK(decoded_correlated_protocol_error.value().method ==
               Method::SessionCreate);
  PS_IPC_CHECK(decoded_correlated_protocol_error.value().status.code ==
               ErrorCode::InvalidArgument);
  auto sentinel_protocol_error = encode_protocol_error(malformed_status);
  PS_IPC_CHECK(sentinel_protocol_error.ok());
  auto decoded_sentinel_protocol_error =
      decode_protocol_error(sentinel_protocol_error.value());
  PS_IPC_CHECK(decoded_sentinel_protocol_error.ok());
  PS_IPC_CHECK(decoded_sentinel_protocol_error.value().request_id == 0U);
  PS_IPC_CHECK(decoded_sentinel_protocol_error.value().method ==
               Method::DaemonInfo);
  PS_IPC_CHECK(!encode_protocol_error(Status::success()).ok());

  Response response;
  response.request_id = 9U;
  response.method = Method::JobResult;
  response.execution_result.values.emplace("answer", Value::from_float64(42.0));
  auto faceted =
      Value::create(ValueDescriptor{ElementType::UInt8, {1U}},
                    Region::whole({1U}), StridedLayout{0U, {1}}, {7U},
                    {ValueFacet{"test.semantic", 3U, {4U, 5U}}});
  PS_IPC_CHECK(faceted.ok());
  response.execution_result.values.emplace("faceted", faceted.take_value());
  response.execution_result.diagnostics.plan_digest = "0123456789abcdef";
  response.execution_result.diagnostics.result_digest = "fedcba9876543210";
  response.execution_result.diagnostics.selected_backends.emplace(1U,
                                                                  Backend::Cpu);
  response.execution_result.diagnostics.operation_timings.push_back(
      OperationTiming{1U, Backend::Cpu, 5U, ErrorCode::Ok});
  auto encoded_response = encode_response(response);
  PS_IPC_CHECK(encoded_response.ok());
  auto decoded_response =
      decode_response(encoded_response.value(), Method::JobResult, 9U);
  PS_IPC_CHECK(decoded_response.ok());
  PS_IPC_CHECK(decoded_response.value()
                   .execution_result.values.at("answer")
                   .as_float64()
                   .value() == 42.0);
  const Value& decoded_faceted =
      decoded_response.value().execution_result.values.at("faceted");
  PS_IPC_CHECK(decoded_faceted.facets().size() == 1U);
  PS_IPC_CHECK(decoded_faceted.facets().front().key == "test.semantic");
  PS_IPC_CHECK(decoded_faceted.facets().front().version == 3U);
  PS_IPC_CHECK(decoded_faceted.facets().front().payload ==
               std::vector<std::uint8_t>({4U, 5U}));
  PS_IPC_CHECK(
      decoded_response.value().execution_result.diagnostics.plan_digest ==
      "0123456789abcdef");

  std::vector<std::uint8_t> wrong_version = encoded.value();
  wrong_version[0] = 2U;
  PS_IPC_CHECK(!decode_request(wrong_version).ok());
  std::vector<std::uint8_t> trailing = encoded.value();
  trailing.push_back(0U);
  PS_IPC_CHECK(!decode_request(trailing).ok());
  PS_IPC_CHECK(!decode_request({}).ok());
  std::vector<std::uint8_t> invalid_utf8 = encoded.value();
  PS_IPC_CHECK(invalid_utf8.size() > 31U);
  invalid_utf8[31U] = 0xc0U;
  PS_IPC_CHECK(!decode_request(invalid_utf8).ok());

  Request zero_session;
  zero_session.request_id = 10U;
  zero_session.method = Method::SessionClose;
  PS_IPC_CHECK(!encode_request(zero_session).ok());

  Request invalid_method;
  invalid_method.request_id = 11U;
  invalid_method.method = static_cast<Method>(255U);
  PS_IPC_CHECK(!encode_request(invalid_method).ok());

  Response zero_session_response;
  zero_session_response.request_id = 12U;
  zero_session_response.method = Method::SessionCreate;
  PS_IPC_CHECK(!encode_response(zero_session_response).ok());

  Response status_response;
  status_response.request_id = 13U;
  status_response.method = Method::JobStatus;
  status_response.job_status.job_id = JobId{77U, 1U};
  status_response.job_status.session_id = SessionId{77U, 2U};
  status_response.job_status.state = JobState::Queued;
  auto encoded_status = encode_response(status_response);
  PS_IPC_CHECK(encoded_status.ok());
  PS_IPC_CHECK(encoded_status.value().size() > 49U);
  std::vector<std::uint8_t> incoherent_status = encoded_status.value();
  incoherent_status[48U] = static_cast<std::uint8_t>(JobState::Cancelled);
  PS_IPC_CHECK(
      !decode_response(incoherent_status, Method::JobStatus, 13U).ok());

  Response invalid_info;
  invalid_info.request_id = 14U;
  invalid_info.method = Method::DaemonInfo;
  PS_IPC_CHECK(!encode_response(invalid_info).ok());

  const auto methods = method_inventory();
  PS_IPC_CHECK(methods.size() == 9U);
  PS_IPC_CHECK(methods.front() == "daemon.info");
  PS_IPC_CHECK(methods.back() == "session.create");

  int descriptors[2]{-1, -1};
  PS_IPC_CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
  PS_IPC_CHECK(write_frame(descriptors[0], encoded.value()).ok());
  auto framed = read_frame(descriptors[1]);
  PS_IPC_CHECK(framed.ok());
  PS_IPC_CHECK(framed.value() == encoded.value());
  ::close(descriptors[0]);
  ::close(descriptors[1]);

  PS_IPC_CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
  const std::uint8_t oversized_header[4] = {0xffU, 0xffU, 0xffU, 0xffU};
  PS_IPC_CHECK(
      ::write(descriptors[0], oversized_header, sizeof(oversized_header)) ==
      static_cast<ssize_t>(sizeof(oversized_header)));
  auto oversized = read_frame(descriptors[1]);
  PS_IPC_CHECK(!oversized.ok());
  PS_IPC_CHECK(oversized.status().code == ErrorCode::InvalidArgument);
  ::close(descriptors[0]);
  ::close(descriptors[1]);

  PS_IPC_CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
  const std::uint8_t truncated_header[4] = {0U, 0U, 0U, 4U};
  PS_IPC_CHECK(
      ::write(descriptors[0], truncated_header, sizeof(truncated_header)) ==
      static_cast<ssize_t>(sizeof(truncated_header)));
  const std::uint8_t one_byte = 1U;
  PS_IPC_CHECK(::write(descriptors[0], &one_byte, 1U) == 1);
  ::close(descriptors[0]);
  auto truncated = read_frame(descriptors[1]);
  PS_IPC_CHECK(!truncated.ok());
  PS_IPC_CHECK(truncated.status().code == ErrorCode::InvalidArgument);
  ::close(descriptors[1]);

  Client original_client;
  Client moved_client(std::move(original_client));
  auto moved_from_call = original_client.daemon_info();
  PS_IPC_CHECK(!moved_from_call.ok());
  PS_IPC_CHECK(moved_from_call.status().code == ErrorCode::InvalidArgument);
  PS_IPC_CHECK(!moved_client.connected());
  const auto sentinel =
      observe_public_client_response(PublicClientResponseKind::FailedSentinel);
  PS_IPC_CHECK(sentinel.server_completed);
  PS_IPC_CHECK(!sentinel.result_ok);
  PS_IPC_CHECK(sentinel.status_code == ErrorCode::ResourceExhausted);
  PS_IPC_CHECK(!sentinel.connected_after_call);

  const auto wrong_request_id =
      observe_public_client_response(PublicClientResponseKind::WrongRequestId);
  PS_IPC_CHECK(wrong_request_id.server_completed);
  PS_IPC_CHECK(!wrong_request_id.result_ok);
  PS_IPC_CHECK(wrong_request_id.status_code == ErrorCode::InvalidArgument);
  PS_IPC_CHECK(!wrong_request_id.connected_after_call);

  const auto wrong_method =
      observe_public_client_response(PublicClientResponseKind::WrongMethod);
  PS_IPC_CHECK(wrong_method.server_completed);
  PS_IPC_CHECK(!wrong_method.result_ok);
  PS_IPC_CHECK(wrong_method.status_code == ErrorCode::InvalidArgument);
  PS_IPC_CHECK(!wrong_method.connected_after_call);
  return 0;
}
