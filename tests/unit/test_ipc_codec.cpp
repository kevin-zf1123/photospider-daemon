#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ipc/codec.hpp"
#include "ipc/frame.hpp"
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

}  // namespace

/**
 * @brief Exercises v3 request/response round trips and malformed frame fences.
 * @return Zero when all checks pass.
 * @throws std::bad_alloc If test setup allocation fails.
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
  return 0;
}
