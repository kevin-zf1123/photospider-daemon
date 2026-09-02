#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "photospider/ipc/protocol.hpp"

namespace ps::ipc::internal {

/** @brief Closed method codes for the exact version-three RPC surface. */
enum class Method : std::uint8_t {
  SessionCreate = 1U,
  SessionClose = 2U,
  JobSubmit = 3U,
  JobStatus = 4U,
  JobCancel = 5U,
  JobResult = 6U,
  JobRelease = 7U,
  DaemonInfo = 8U,
  DaemonShutdown = 9U,
};

#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
namespace codec_test {

/**
 * @brief Identifies one allocation-capable decoded wire-count boundary.
 *
 * @note This private test-only inventory is absent from installed production
 * objects and exists solely to prove malformed counts fail before reserve,
 * container insertion, or count-controlled iteration.
 */
enum class DecoderCountKind : std::uint8_t {
  /** @brief Workflow node vector count. */
  WorkflowNodes,
  /** @brief Per-node input vector count. */
  WorkflowInputs,
  /** @brief Per-node parameter map count. */
  WorkflowParameters,
  /** @brief Workflow output vector count. */
  WorkflowOutputs,
  /** @brief Value shape, Region, and stride rank. */
  ValueAxes,
  /** @brief Value facet vector count. */
  ValueFacets,
  /** @brief Selected-backend map count. */
  DiagnosticBackends,
  /** @brief Fallback-reason vector count. */
  DiagnosticFallbackReasons,
  /** @brief Operation-timing vector count. */
  DiagnosticOperationTimings,
  /** @brief Named execution-result Value map count. */
  ResultNamedValues,
  /** @brief Daemon method-inventory vector count. */
  DaemonMethods,
};

/**
 * @brief Observes a count only after its semantic and remaining-byte fences.
 * @param kind Exact allocation-capable collection boundary.
 * @param count Validated wire count about to control allocation or iteration.
 * @throws Nothing.
 * @note Observers must not call codec entry points and run synchronously on the
 * decoding thread.
 */
using DecoderCountObserver = void (*)(DecoderCountKind kind,
                                      std::uint64_t count) noexcept;

/**
 * @brief Installs or clears the process-global decoder count observer.
 * @param observer Callback to install, or null to clear.
 * @throws Nothing.
 * @note Tests must serialize installation and clear the observer before
 * teardown. The seam is compiled only into the noninstalled test runtime.
 */
void install_decoder_count_observer(DecoderCountObserver observer) noexcept;

/**
 * @brief Evaluates the production structural byte contract for one count.
 * @param kind Exact count-controlled collection boundary.
 * @param count Candidate entry count.
 * @param remaining_bytes Exact unread bytes available after the count field.
 * @return True exactly when the entries and fixed suffix can structurally fit.
 * @throws Nothing.
 * @note This arithmetic probe shares the production overflow-safe predicate;
 * semantic maxima remain the responsibility of each actual decoder call site.
 */
[[nodiscard]] bool decoder_count_fits(DecoderCountKind kind,
                                      std::uint64_t count,
                                      std::size_t remaining_bytes) noexcept;

}  // namespace codec_test
#endif

/**
 * @brief Fully decoded version-three request.
 *
 * @note Only fields selected by `method` are authoritative.
 */
struct Request final {
  /** @brief Nonzero connection-local correlation id. */
  std::uint64_t request_id = 0U;
  /** @brief Exact routed method. */
  Method method = Method::DaemonInfo;
  /** @brief Source document for SessionCreate. */
  WorkflowDocument document;
  /** @brief Session argument for SessionClose/JobSubmit. */
  SessionId session_id;
  /** @brief Job argument for Job methods. */
  JobId job_id;
  /** @brief Submit controls for JobSubmit. */
  JobSubmitOptions submit_options;
};

/**
 * @brief Fully owned version-three response before framing.
 *
 * @note Only the method-selected successful payload is encoded.
 */
struct Response final {
  /** @brief Correlation id copied from a valid request. */
  std::uint64_t request_id = 0U;
  /** @brief Routed method copied from a valid request. */
  Method method = Method::DaemonInfo;
  /** @brief Canonical success or sole typed failure. */
  Status status;
  /** @brief SessionCreate result. */
  SessionId session_id;
  /** @brief JobSubmit result. */
  JobId job_id;
  /** @brief JobStatus result. */
  JobStatus job_status;
  /** @brief JobResult result. */
  ExecutionResult execution_result;
  /** @brief DaemonInfo result. */
  DaemonInfo daemon_info;
  /** @brief Whether a successful response should stop the server after write.
   */
  bool shutdown_after_write = false;
};

/**
 * @brief Encodes one typed request to a bounded binary payload.
 * @param request Complete method-selected request.
 * @return Versioned payload or validation/size failure.
 * @throws std::bad_alloc If output allocation fails.
 */
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_request(
    const Request& request);

/**
 * @brief Decodes and validates one complete binary request payload.
 * @param payload Frame payload bytes.
 * @return Owned request or malformed-frame failure.
 * @throws std::bad_alloc If decoded storage allocation fails.
 * @note Count fields are fenced against remaining bytes before allocation,
 * insertion, or iteration; structural mismatches return InvalidArgument.
 */
[[nodiscard]] Result<Request> decode_request(
    const std::vector<std::uint8_t>& payload);

/**
 * @brief Encodes one complete method-correlated response payload.
 * @param response Complete method-selected response.
 * @return Versioned bounded payload or validation/size failure.
 * @throws std::bad_alloc If output allocation fails.
 * @note Only the successful payload selected by `response.method` is emitted.
 */
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_response(
    const Response& response);

/**
 * @brief Decodes one method-correlated response for a client call.
 * @param payload Complete frame payload bytes.
 * @param expected_method Method sent by the client.
 * @param expected_request_id Nonzero correlation id sent by the client.
 * @return Owned response or malformed/correlation failure.
 * @throws std::bad_alloc If decoded storage allocation fails.
 * @note Method/correlation mismatches invalidate the client connection. Count
 * fields are fenced against remaining bytes before allocation or iteration.
 */
[[nodiscard]] Result<Response> decode_response(
    const std::vector<std::uint8_t>& payload, Method expected_method,
    std::uint64_t expected_request_id);

/**
 * @brief Encodes one typed protocol/backpressure error before normal routing.
 * @param status Required non-success status.
 * @param request_payload Optional malformed request payload used only to
 * recover a valid correlation header.
 * @return Bounded error response using recovered correlation or the documented
 * sentinel `request_id=0`, `method=daemon.info`.
 * @throws std::bad_alloc If response allocation fails.
 * @note No successful method payload or service mutation is encoded.
 */
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_protocol_error(
    const Status& status,
    const std::vector<std::uint8_t>* request_payload = nullptr);

/**
 * @brief Decodes a server-generated typed pre-routing protocol error.
 * @param payload Complete bounded response payload.
 * @return Failed Response with recovered or sentinel correlation.
 * @throws std::bad_alloc If diagnostic allocation fails.
 * @note Success responses and malformed sentinel combinations are rejected.
 */
[[nodiscard]] Result<Response> decode_protocol_error(
    const std::vector<std::uint8_t>& payload);

/**
 * @brief Returns the canonical dotted wire name of one method.
 * @param method Closed version-three method code.
 * @return Process-lifetime string literal, or `unknown` for invalid input.
 * @throws Nothing.
 * @note The returned name is diagnostic and capability metadata.
 */
[[nodiscard]] const char* method_name(Method method) noexcept;

/**
 * @brief Returns the exact sorted nine-method version-three inventory.
 * @return Owned lexicographically sorted dotted method names.
 * @throws std::bad_alloc If result allocation fails.
 * @note This is the sole advertised RPC surface.
 */
[[nodiscard]] std::vector<std::string> method_inventory();

}  // namespace ps::ipc::internal
