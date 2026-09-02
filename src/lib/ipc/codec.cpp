#include "ipc/codec.hpp"

#include <algorithm>
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
#include <atomic>
#endif
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ps::ipc::internal {
namespace {

#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
/** @brief Process-global observer used only by the noninstalled test runtime.
 */
std::atomic<codec_test::DecoderCountObserver> g_decoder_count_observer{nullptr};

/**
 * @brief Publishes one successfully fenced count to the test observer.
 * @param kind Exact allocation-capable collection boundary.
 * @param count Validated count about to control allocation or iteration.
 * @throws Nothing.
 * @note Production compilation removes this helper and every call site.
 */
void observe_decoder_count(codec_test::DecoderCountKind kind,
                           std::uint64_t count) noexcept {
  const auto observer =
      g_decoder_count_observer.load(std::memory_order_acquire);
  if (observer != nullptr) {
    observer(kind, count);
  }
}
#endif

/**
 * @brief Internal typed exception used to unwind malformed codec state.
 * @note It never crosses an exported codec boundary.
 */
class CodecFailure final : public std::runtime_error {
 public:
  /**
   * @brief Captures one recoverable codec category and diagnostic.
   * @param code Stable non-success failure category.
   * @param message Bounded human-readable diagnostic.
   * @throws std::bad_alloc If exception message allocation fails.
   * @note Exported codec functions translate this into `Status`.
   */
  CodecFailure(ErrorCode code, const std::string& message)
      : std::runtime_error(message), code_(code) {}

  /**
   * @brief Returns the stable failure category.
   * @return Category captured at construction.
   * @throws Nothing.
   * @note The value is independent of exception message text.
   */
  [[nodiscard]] ErrorCode code() const noexcept { return code_; }

 private:
  /** @brief Stable recoverable category. */
  ErrorCode code_;
};

/**
 * @brief Validates canonical UTF-8 byte structure.
 * @param value Candidate text bytes.
 * @return True when every scalar is non-overlong, non-surrogate, and bounded.
 * @throws Nothing.
 */
bool valid_utf8(const std::string& value) noexcept {
  std::size_t index = 0U;
  while (index < value.size()) {
    const auto first = static_cast<std::uint8_t>(value[index]);
    if (first <= 0x7fU) {
      ++index;
      continue;
    }
    std::size_t continuation_count = 0U;
    std::uint32_t scalar = 0U;
    std::uint32_t minimum = 0U;
    if (first >= 0xc2U && first <= 0xdfU) {
      continuation_count = 1U;
      scalar = first & 0x1fU;
      minimum = 0x80U;
    } else if (first >= 0xe0U && first <= 0xefU) {
      continuation_count = 2U;
      scalar = first & 0x0fU;
      minimum = 0x800U;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      continuation_count = 3U;
      scalar = first & 0x07U;
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (continuation_count > value.size() - index - 1U) {
      return false;
    }
    for (std::size_t continuation = 0U; continuation < continuation_count;
         ++continuation) {
      const auto byte =
          static_cast<std::uint8_t>(value[index + continuation + 1U]);
      if ((byte & 0xc0U) != 0x80U) {
        return false;
      }
      scalar = (scalar << 6U) | (byte & 0x3fU);
    }
    if (scalar < minimum || scalar > 0x10ffffU ||
        (scalar >= 0xd800U && scalar <= 0xdfffU)) {
      return false;
    }
    index += continuation_count + 1U;
  }
  return true;
}

/**
 * @brief Bounded little-endian payload builder.
 *
 * @note Every append checks the complete frame limit before mutation.
 */
class Encoder final {
 public:
  /**
   * @brief Appends one raw byte.
   * @param value Exact byte.
   * @throws CodecFailure If the complete frame bound would be exceeded.
   * @throws std::bad_alloc If payload growth fails.
   * @note Mutation occurs only after the bound check.
   */
  void u8(std::uint8_t value) { append(&value, sizeof(value)); }

  /**
   * @brief Appends one uint16 in little-endian order.
   * @param value Unsigned value to encode.
   * @throws CodecFailure If the complete frame bound would be exceeded.
   * @throws std::bad_alloc If payload growth fails.
   * @note Host endianness does not affect the wire bytes.
   */
  void u16(std::uint16_t value) {
    for (std::size_t index = 0U; index < 2U; ++index) {
      u8(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
    }
  }

  /**
   * @brief Appends one uint32 in little-endian order.
   * @param value Unsigned value to encode.
   * @throws CodecFailure If the complete frame bound would be exceeded.
   * @throws std::bad_alloc If payload growth fails.
   * @note Host endianness does not affect the wire bytes.
   */
  void u32(std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index) {
      u8(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
    }
  }

  /**
   * @brief Appends one uint64 in little-endian order.
   * @param value Unsigned value to encode.
   * @throws CodecFailure If the complete frame bound would be exceeded.
   * @throws std::bad_alloc If payload growth fails.
   * @note Host endianness does not affect the wire bytes.
   */
  void u64(std::uint64_t value) {
    for (std::size_t index = 0U; index < 8U; ++index) {
      u8(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
    }
  }

  /**
   * @brief Appends one signed integer without host-endian dependence.
   * @param value Signed value whose exact representation is preserved.
   * @throws CodecFailure If the complete frame bound would be exceeded.
   * @throws std::bad_alloc If payload growth fails.
   * @note The implementation copies bits without signed conversion.
   */
  void i64(std::int64_t value) {
    std::uint64_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    u64(bits);
  }

  /**
   * @brief Appends one binary64 value preserving exact bits.
   * @param value Floating value whose exact representation is preserved.
   * @throws CodecFailure If the complete frame bound would be exceeded.
   * @throws std::bad_alloc If payload growth fails.
   * @note NaN payloads and signed zero are not canonicalized by the codec.
   */
  void f64(double value) {
    std::uint64_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    u64(bits);
  }

  /**
   * @brief Appends a strict zero-or-one boolean.
   * @param value Boolean to encode.
   * @throws CodecFailure If the complete frame bound would be exceeded.
   * @throws std::bad_alloc If payload growth fails.
   * @note False is zero and true is one.
   */
  void boolean(bool value) { u8(value ? 1U : 0U); }

  /**
   * @brief Appends uint32 length-framed text.
   * @param value Exact string bytes.
   * @param maximum Semantic maximum before frame accounting.
   * @throws CodecFailure If text or complete payload is invalid/oversized.
   * @throws std::bad_alloc If payload or diagnostic allocation fails.
   * @note Text must contain canonical structurally valid UTF-8.
   */
  void text(const std::string& value, std::size_t maximum) {
    if (value.size() > maximum ||
        value.size() > std::numeric_limits<std::uint32_t>::max() ||
        !valid_utf8(value)) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire text exceeds its bound or is invalid UTF-8");
    }
    u32(static_cast<std::uint32_t>(value.size()));
    append(value.data(), value.size());
  }

  /**
   * @brief Appends uint32 length-framed bytes.
   * @param value Exact owned byte vector.
   * @throws CodecFailure If the vector or complete frame is oversized.
   * @throws std::bad_alloc If payload or diagnostic allocation fails.
   * @note Empty vectors are encoded with a zero length.
   */
  void bytes(const std::vector<std::uint8_t>& value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw CodecFailure(ErrorCode::ResourceExhausted,
                         "wire byte vector exceeds uint32");
    }
    u32(static_cast<std::uint32_t>(value.size()));
    append(value.data(), value.size());
  }

  /**
   * @brief Moves out the complete bounded payload.
   * @return Owned accumulated bytes.
   * @throws Nothing.
   * @note The encoder is not reused after this call.
   */
  [[nodiscard]] std::vector<std::uint8_t> finish() {
    return std::move(payload_);
  }

 private:
  /**
   * @brief Appends raw bytes after overflow and frame-limit checks.
   * @param data Byte range, possibly null only when `size` is zero.
   * @param size Exact byte count.
   * @throws CodecFailure If the complete frame bound would be exceeded.
   * @throws std::bad_alloc If payload growth fails.
   * @note No bytes are published before size validation.
   */
  void append(const void* data, std::size_t size) {
    if (size > kMaximumFramePayloadBytes - payload_.size()) {
      throw CodecFailure(ErrorCode::ResourceExhausted,
                         "wire payload exceeds maximum frame size");
    }
    const auto* first = static_cast<const std::uint8_t*>(data);
    if (size != 0U) {
      payload_.insert(payload_.end(), first, first + size);
    }
  }

  /** @brief Accumulated bounded payload. */
  std::vector<std::uint8_t> payload_;
};

/**
 * @brief Minimum encoded bytes required by one count-controlled collection.
 *
 * `minimum_entry_bytes` is the smallest legal structural representation of
 * one entry, excluding semantic validation. `required_suffix_bytes` is the
 * smallest fixed wire suffix that must remain after the complete collection.
 *
 * @note Contracts intentionally exclude variable payload bytes so valid
 * frames are never rejected by a conservative overestimate.
 */
struct CountContract final {
  /** @brief Smallest encoded bytes consumed by one collection entry. */
  std::size_t minimum_entry_bytes;
  /** @brief Smallest fixed structural suffix following the collection. */
  std::size_t required_suffix_bytes;
};

/** @brief Workflow nodes plus the final output-count field. */
constexpr CountContract kWorkflowNodesContract{20U, 4U};
/** @brief Node inputs plus parameter and final output count fields. */
constexpr CountContract kWorkflowInputsContract{12U, 8U};
/** @brief Boolean-minimum parameters plus the final output-count field. */
constexpr CountContract kWorkflowParametersContract{6U, 4U};
/** @brief Named workflow outputs with no fixed trailing document bytes. */
constexpr CountContract kWorkflowOutputsContract{16U, 0U};
/** @brief Value axes plus Value fixed fields and minimum diagnostics. */
constexpr CountContract kValueAxesContract{32U, 65U};
/** @brief Value facets plus Value bytes and minimum diagnostics. */
constexpr CountContract kValueFacetsContract{12U, 56U};
/** @brief Backend map entries plus fixed remaining diagnostic fields. */
constexpr CountContract kDiagnosticBackendsContract{9U, 40U};
/** @brief Fallback text prefixes plus timing/digest count fields. */
constexpr CountContract kDiagnosticReasonsContract{4U, 12U};
/** @brief Operation timings plus both following digest length fields. */
constexpr CountContract kDiagnosticTimingsContract{18U, 8U};
/** @brief Named minimum-rank Values plus minimum diagnostics. */
constexpr CountContract kResultValuesContract{54U, 52U};
/** @brief Method text prefixes plus fixed daemon-capacity fields. */
constexpr CountContract kDaemonMethodsContract{4U, 24U};

/**
 * @brief Evaluates one structural count contract without overflow.
 * @param count Candidate entry count.
 * @param remaining_bytes Exact unread bytes following the count field.
 * @param contract Minimum per-entry and fixed-suffix wire bytes.
 * @return True exactly when the encoded collection can structurally fit.
 * @throws Nothing.
 * @note Subtraction precedes division, and no count multiplication occurs.
 */
bool count_fits(std::uint64_t count, std::size_t remaining_bytes,
                CountContract contract) noexcept {
  return contract.minimum_entry_bytes != 0U &&
         contract.required_suffix_bytes <= remaining_bytes &&
         count <= static_cast<std::uint64_t>(
                      (remaining_bytes - contract.required_suffix_bytes) /
                      contract.minimum_entry_bytes);
}

#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
/**
 * @brief Returns the production byte contract selected by one test-only kind.
 * @param kind Exact count-controlled decoder boundary.
 * @return Matching minimum-entry and required-suffix contract.
 * @throws Nothing.
 */
CountContract count_contract(codec_test::DecoderCountKind kind) noexcept {
  switch (kind) {
    case codec_test::DecoderCountKind::WorkflowNodes:
      return kWorkflowNodesContract;
    case codec_test::DecoderCountKind::WorkflowInputs:
      return kWorkflowInputsContract;
    case codec_test::DecoderCountKind::WorkflowParameters:
      return kWorkflowParametersContract;
    case codec_test::DecoderCountKind::WorkflowOutputs:
      return kWorkflowOutputsContract;
    case codec_test::DecoderCountKind::ValueAxes:
      return kValueAxesContract;
    case codec_test::DecoderCountKind::ValueFacets:
      return kValueFacetsContract;
    case codec_test::DecoderCountKind::DiagnosticBackends:
      return kDiagnosticBackendsContract;
    case codec_test::DecoderCountKind::DiagnosticFallbackReasons:
      return kDiagnosticReasonsContract;
    case codec_test::DecoderCountKind::DiagnosticOperationTimings:
      return kDiagnosticTimingsContract;
    case codec_test::DecoderCountKind::ResultNamedValues:
      return kResultValuesContract;
    case codec_test::DecoderCountKind::DaemonMethods:
      return kDaemonMethodsContract;
  }
  return CountContract{0U, 0U};
}
#endif

/**
 * @brief Bounds-checking little-endian payload reader.
 *
 * @note Every read either advances exactly once or throws without publishing a
 * partially decoded public object.
 */
class Decoder final {
 public:
  /**
   * @brief Binds one complete immutable frame payload.
   * @param payload Borrowed bytes that outlive this decoder.
   * @throws CodecFailure If payload already exceeds the frame bound.
   * @note Decoding never mutates the borrowed bytes.
   */
  explicit Decoder(const std::vector<std::uint8_t>& payload)
      : payload_(payload) {
    if (payload.size() > kMaximumFramePayloadBytes) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire payload exceeds maximum frame size");
    }
  }

  /**
   * @brief Reads one raw byte.
   * @return Next byte.
   * @throws CodecFailure If the payload is truncated.
   * @note Offset advances exactly once after validation.
   */
  [[nodiscard]] std::uint8_t u8() {
    require(1U);
    return payload_[offset_++];
  }

  /**
   * @brief Reads one little-endian uint16.
   * @return Decoded unsigned value.
   * @throws CodecFailure If the payload is truncated.
   * @note Host endianness does not affect the result.
   */
  [[nodiscard]] std::uint16_t u16() {
    std::uint16_t value = 0U;
    for (std::size_t index = 0U; index < 2U; ++index) {
      value |= static_cast<std::uint16_t>(u8()) << (index * 8U);
    }
    return value;
  }

  /**
   * @brief Reads one little-endian uint32.
   * @return Decoded unsigned value.
   * @throws CodecFailure If the payload is truncated.
   * @note Host endianness does not affect the result.
   */
  [[nodiscard]] std::uint32_t u32() {
    std::uint32_t value = 0U;
    for (std::size_t index = 0U; index < 4U; ++index) {
      value |= static_cast<std::uint32_t>(u8()) << (index * 8U);
    }
    return value;
  }

  /**
   * @brief Reads one little-endian uint64.
   * @return Decoded unsigned value.
   * @throws CodecFailure If the payload is truncated.
   * @note Host endianness does not affect the result.
   */
  [[nodiscard]] std::uint64_t u64() {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
      value |= static_cast<std::uint64_t>(u8()) << (index * 8U);
    }
    return value;
  }

  /**
   * @brief Reads one bit-preserving signed integer.
   * @return Decoded signed value.
   * @throws CodecFailure If the payload is truncated.
   * @note Bits are copied without unsigned-to-signed conversion.
   */
  [[nodiscard]] std::int64_t i64() {
    const std::uint64_t bits = u64();
    std::int64_t value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  /**
   * @brief Reads one bit-preserving binary64 value.
   * @return Decoded floating value.
   * @throws CodecFailure If the payload is truncated.
   * @note NaN payloads and signed zero are preserved.
   */
  [[nodiscard]] double f64() {
    const std::uint64_t bits = u64();
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  /**
   * @brief Reads a strict zero-or-one boolean.
   * @return Decoded boolean.
   * @throws CodecFailure If truncated or not encoded as zero/one.
   * @note No other integer representation is accepted.
   */
  [[nodiscard]] bool boolean() {
    const std::uint8_t value = u8();
    if (value > 1U) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire boolean is not zero or one");
    }
    return value != 0U;
  }

  /**
   * @brief Reads bounded uint32 length-framed text.
   * @param maximum Inclusive semantic byte limit.
   * @return Owned decoded string.
   * @throws CodecFailure If truncated, oversized, or invalid UTF-8.
   * @throws std::bad_alloc If string allocation fails.
   * @note Offset publication completes before UTF-8 validation only inside
   * this private decoder, which is discarded when decoding fails.
   */
  [[nodiscard]] std::string text(std::size_t maximum) {
    const std::uint32_t size = u32();
    if (size > maximum) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire text exceeds its method bound");
    }
    require(size);
    std::string value(reinterpret_cast<const char*>(payload_.data() + offset_),
                      size);
    offset_ += size;
    if (!valid_utf8(value)) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire text is invalid UTF-8");
    }
    return value;
  }

  /**
   * @brief Reads bounded uint32 length-framed bytes.
   * @param maximum Inclusive semantic byte limit.
   * @return Owned decoded bytes.
   * @throws CodecFailure If truncated or oversized.
   * @throws std::bad_alloc If vector allocation fails.
   * @note No partially decoded public object is published on failure.
   */
  [[nodiscard]] std::vector<std::uint8_t> bytes(std::size_t maximum) {
    const std::uint32_t size = u32();
    if (size > maximum) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire byte vector exceeds its method bound");
    }
    require(size);
    std::vector<std::uint8_t> value(payload_.begin() + offset_,
                                    payload_.begin() + offset_ + size);
    offset_ += size;
    return value;
  }

  /**
   * @brief Returns the exact unread payload byte count.
   * @return Bytes from the current offset through the complete payload end.
   * @throws Nothing.
   * @note Decoder invariants keep `offset_` within `payload_`, so subtraction
   * cannot underflow.
   */
  [[nodiscard]] std::size_t remaining() const noexcept {
    return payload_.size() - offset_;
  }

  /**
   * @brief Fences a wire count against unread minimum entry and suffix bytes.
   * @param count Decoded collection count after its semantic maximum check.
   * @param minimum_entry_bytes Smallest structural bytes for one entry;
   * must be nonzero.
   * @param required_suffix_bytes Smallest fixed bytes following the complete
   * collection.
   * @throws CodecFailure If the unread payload cannot structurally contain the
   * requested entries and suffix.
   * @note Subtraction and division avoid multiplication overflow. Call before
   * every reserve, resize, insertion-capable loop, or count-controlled loop.
   */
  void require_count_fits(std::uint64_t count, std::size_t minimum_entry_bytes,
                          std::size_t required_suffix_bytes) const {
    if (!count_fits(
            count, remaining(),
            CountContract{minimum_entry_bytes, required_suffix_bytes})) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire collection count exceeds remaining bytes");
    }
  }

  /**
   * @brief Rejects any trailing method bytes.
   * @throws CodecFailure Unless every payload byte was consumed.
   * @note Call exactly once after the method-selected body is decoded.
   */
  void require_end() const {
    if (offset_ != payload_.size()) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire payload has trailing bytes");
    }
  }

 private:
  /**
   * @brief Ensures the next exact byte range exists.
   * @param size Required unread byte count.
   * @throws CodecFailure If the payload is truncated.
   * @note The read offset is unchanged.
   */
  void require(std::size_t size) const {
    if (size > remaining()) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire payload is truncated");
    }
  }

  /** @brief Borrowed complete payload. */
  const std::vector<std::uint8_t>& payload_;
  /** @brief Next unread byte. */
  std::size_t offset_ = 0U;
};

/**
 * @brief Reports whether a method enum value belongs to IPC v3.
 * @param method Candidate method value.
 * @return True exactly for the nine closed method codes.
 * @throws Nothing.
 * @note This validates values constructed in memory before encoding.
 */
bool valid_method(Method method) noexcept {
  const auto raw = static_cast<std::uint8_t>(method);
  return raw >= static_cast<std::uint8_t>(Method::SessionCreate) &&
         raw <= static_cast<std::uint8_t>(Method::DaemonShutdown);
}

/**
 * @brief Validates one nonzero process-scoped Session identity.
 * @param id Candidate SessionId.
 * @throws CodecFailure If either identity component is zero.
 * @note This validates wire shape, not registry liveness.
 */
void require_session_id(SessionId id) {
  if (id.instance == 0U || id.value == 0U) {
    throw CodecFailure(ErrorCode::InvalidArgument,
                       "wire SessionId contains zero");
  }
}

/**
 * @brief Validates one nonzero process-scoped Job identity.
 * @param id Candidate JobId.
 * @throws CodecFailure If either identity component is zero.
 * @note This validates wire shape, not registry liveness.
 */
void require_job_id(JobId id) {
  if (id.instance == 0U || id.value == 0U) {
    throw CodecFailure(ErrorCode::InvalidArgument, "wire JobId contains zero");
  }
}

/**
 * @brief Validates one lifecycle/status response snapshot.
 * @param status Candidate public JobStatus.
 * @throws CodecFailure If identities, state, or outcome disagree.
 * @note Running and successful states carry canonical success; cancellation
 * carries the Cancelled category.
 */
void validate_job_status(const JobStatus& status) {
  require_job_id(status.job_id);
  require_session_id(status.session_id);
  if (status.job_id.instance != status.session_id.instance) {
    throw CodecFailure(ErrorCode::InvalidArgument,
                       "wire JobStatus identity domains differ");
  }
  switch (status.state) {
    case JobState::Queued:
    case JobState::Running:
    case JobState::Succeeded:
      if (!status.outcome.ok()) {
        throw CodecFailure(ErrorCode::InvalidArgument,
                           "wire non-failed Job state carries failure");
      }
      return;
    case JobState::Failed:
      if (status.outcome.ok() || status.outcome.code == ErrorCode::Cancelled) {
        throw CodecFailure(ErrorCode::InvalidArgument,
                           "wire failed Job carries invalid outcome");
      }
      return;
    case JobState::Cancelled:
      if (status.outcome.code != ErrorCode::Cancelled) {
        throw CodecFailure(ErrorCode::InvalidArgument,
                           "wire cancelled Job lacks cancellation outcome");
      }
      return;
  }
  throw CodecFailure(ErrorCode::InvalidArgument, "wire Job state is unknown");
}

/**
 * @brief Validates the exact local daemon capability response.
 * @param info Candidate DaemonInfo payload.
 * @throws CodecFailure If invariant fields disagree with IPC v3.
 * @throws std::bad_alloc If expected method inventory allocation fails.
 * @note Live counts are observations and therefore are not cross-validated.
 */
void validate_daemon_info(const DaemonInfo& info) {
  if (info.protocol_version != kProtocolVersion || info.instance_id == 0U ||
      info.service_version.empty() || info.transport != "unix-domain" ||
      info.maximum_concurrency == 0U || info.maximum_sessions == 0U ||
      info.methods != method_inventory()) {
    throw CodecFailure(ErrorCode::InvalidArgument,
                       "wire daemon.info payload is inconsistent");
  }
}

/**
 * @brief Encodes one stable public status.
 * @param encoder Nonnull bounded destination.
 * @param status Canonical success or stable failure category/diagnostic.
 * @throws CodecFailure If the category or success diagnostic is invalid.
 * @throws std::bad_alloc If payload growth fails.
 * @note Empty failure messages remain valid for allocation-fence fallback.
 */
void encode_status(Encoder* encoder, const Status& status) {
  const auto raw = static_cast<std::uint8_t>(status.code);
  if (raw > static_cast<std::uint8_t>(ErrorCode::Internal) ||
      (status.ok() && !status.message.empty())) {
    throw CodecFailure(ErrorCode::InvalidArgument,
                       "wire status is not canonical");
  }
  encoder->u8(static_cast<std::uint8_t>(status.code));
  encoder->text(status.message, 4096U);
}

/**
 * @brief Decodes one validated stable public status.
 * @param decoder Nonnull bounded source.
 * @return Canonical success or stable failure category/diagnostic.
 * @throws CodecFailure If category, text, or success form is invalid.
 * @throws std::bad_alloc If diagnostic allocation fails.
 * @note Empty failure messages remain valid for allocation-fence fallback.
 */
Status decode_status(Decoder* decoder) {
  const std::uint8_t raw = decoder->u8();
  if (raw > static_cast<std::uint8_t>(ErrorCode::Internal)) {
    throw CodecFailure(ErrorCode::InvalidArgument,
                       "wire status error code is unknown");
  }
  const ErrorCode code = static_cast<ErrorCode>(raw);
  std::string message = decoder->text(4096U);
  if (code == ErrorCode::Ok && !message.empty()) {
    throw CodecFailure(ErrorCode::InvalidArgument,
                       "wire success status contains a diagnostic");
  }
  return code == ErrorCode::Ok ? Status::success()
                               : Status::failure(code, std::move(message));
}

/**
 * @brief Encodes one closed source parameter variant.
 * @param encoder Nonnull bounded destination.
 * @param value Typed source parameter.
 * @throws CodecFailure If text or complete payload is invalid/oversized.
 * @throws std::bad_alloc If payload growth fails.
 * @note Variant alternatives are tagged one through four.
 */
void encode_parameter(Encoder* encoder, const ParameterValue& value) {
  if (const auto* integer = std::get_if<std::int64_t>(&value)) {
    encoder->u8(1U);
    encoder->i64(*integer);
  } else if (const auto* floating = std::get_if<double>(&value)) {
    encoder->u8(2U);
    encoder->f64(*floating);
  } else if (const auto* boolean = std::get_if<bool>(&value)) {
    encoder->u8(3U);
    encoder->boolean(*boolean);
  } else {
    encoder->u8(4U);
    encoder->text(std::get<std::string>(value), 8192U);
  }
}

/**
 * @brief Decodes one closed source parameter variant.
 * @param decoder Nonnull bounded source.
 * @return Typed source parameter.
 * @throws CodecFailure If tag, text, or payload is malformed.
 * @throws std::bad_alloc If string allocation fails.
 * @note No internal compiler representation is constructed.
 */
ParameterValue decode_parameter(Decoder* decoder) {
  switch (decoder->u8()) {
    case 1U:
      return decoder->i64();
    case 2U:
      return decoder->f64();
    case 3U:
      return decoder->boolean();
    case 4U:
      return decoder->text(8192U);
    default:
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire parameter tag is unknown");
  }
}

/**
 * @brief Encodes a complete public WorkflowDocument without internal IR.
 * @param encoder Nonnull bounded destination.
 * @param document Public compiler source model.
 * @throws CodecFailure If counts, text, or complete payload are invalid.
 * @throws std::bad_alloc If payload growth fails.
 * @note Compiler semantic validation remains a daemon-side operation.
 */
void encode_document(Encoder* encoder, const WorkflowDocument& document) {
  if (document.nodes.size() > 65536U || document.outputs.size() > 4096U) {
    throw CodecFailure(ErrorCode::InvalidArgument,
                       "workflow document count exceeds wire bounds");
  }
  encoder->u32(document.schema_version);
  encoder->u32(static_cast<std::uint32_t>(document.nodes.size()));
  for (const WorkflowNode& node : document.nodes) {
    if (node.inputs.size() > 1024U || node.parameters.size() > 1024U) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "workflow node count exceeds wire bounds");
    }
    encoder->u64(node.id);
    encoder->text(node.operation, 1024U);
    encoder->u32(static_cast<std::uint32_t>(node.inputs.size()));
    for (const WorkflowInput& input : node.inputs) {
      encoder->u64(input.source_node);
      encoder->text(input.source_port, 64U);
    }
    encoder->u32(static_cast<std::uint32_t>(node.parameters.size()));
    for (const auto& parameter : node.parameters) {
      encoder->text(parameter.first, 1024U);
      encode_parameter(encoder, parameter.second);
    }
  }
  encoder->u32(static_cast<std::uint32_t>(document.outputs.size()));
  for (const WorkflowOutput& output : document.outputs) {
    encoder->text(output.name, 1024U);
    encoder->u64(output.node_id);
    encoder->text(output.port, 64U);
  }
}

/**
 * @brief Decodes a bounded public WorkflowDocument without compiling it.
 * @param decoder Nonnull bounded source.
 * @return Fully owned public compiler source model.
 * @throws CodecFailure If counts, duplicates, text, or payload are malformed.
 * @throws std::bad_alloc If document allocation fails.
 * @note Every collection count is fenced against its minimum entries and
 * required suffix before reserve, map insertion, or iteration. The caller
 * compiles only after complete decoding succeeds.
 */
WorkflowDocument decode_document(Decoder* decoder) {
  WorkflowDocument document;
  document.schema_version = decoder->u32();
  const std::uint32_t node_count = decoder->u32();
  if (node_count > 65536U) {
    throw CodecFailure(ErrorCode::InvalidArgument,
                       "wire workflow has too many nodes");
  }
  decoder->require_count_fits(node_count,
                              kWorkflowNodesContract.minimum_entry_bytes,
                              kWorkflowNodesContract.required_suffix_bytes);
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
  observe_decoder_count(codec_test::DecoderCountKind::WorkflowNodes,
                        node_count);
#endif
  document.nodes.reserve(node_count);
  for (std::uint32_t index = 0U; index < node_count; ++index) {
    WorkflowNode node;
    node.id = decoder->u64();
    node.operation = decoder->text(1024U);
    const std::uint32_t input_count = decoder->u32();
    if (input_count > 1024U) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire workflow has too many inputs");
    }
    decoder->require_count_fits(input_count,
                                kWorkflowInputsContract.minimum_entry_bytes,
                                kWorkflowInputsContract.required_suffix_bytes);
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
    observe_decoder_count(codec_test::DecoderCountKind::WorkflowInputs,
                          input_count);
#endif
    node.inputs.reserve(input_count);
    for (std::uint32_t input = 0U; input < input_count; ++input) {
      node.inputs.push_back(WorkflowInput{decoder->u64(), decoder->text(64U)});
    }
    const std::uint32_t parameter_count = decoder->u32();
    if (parameter_count > 1024U) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire workflow has too many parameters");
    }
    decoder->require_count_fits(
        parameter_count, kWorkflowParametersContract.minimum_entry_bytes,
        kWorkflowParametersContract.required_suffix_bytes);
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
    observe_decoder_count(codec_test::DecoderCountKind::WorkflowParameters,
                          parameter_count);
#endif
    for (std::uint32_t parameter = 0U; parameter < parameter_count;
         ++parameter) {
      std::string key = decoder->text(1024U);
      if (!node.parameters.emplace(key, decode_parameter(decoder)).second) {
        throw CodecFailure(ErrorCode::InvalidArgument,
                           "wire workflow duplicates a parameter key");
      }
    }
    document.nodes.push_back(std::move(node));
  }
  const std::uint32_t output_count = decoder->u32();
  if (output_count > 4096U) {
    throw CodecFailure(ErrorCode::InvalidArgument,
                       "wire workflow has too many outputs");
  }
  decoder->require_count_fits(output_count,
                              kWorkflowOutputsContract.minimum_entry_bytes,
                              kWorkflowOutputsContract.required_suffix_bytes);
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
  observe_decoder_count(codec_test::DecoderCountKind::WorkflowOutputs,
                        output_count);
#endif
  document.outputs.reserve(output_count);
  for (std::uint32_t index = 0U; index < output_count; ++index) {
    document.outputs.push_back(WorkflowOutput{
        decoder->text(1024U), decoder->u64(), decoder->text(64U)});
  }
  return document;
}

/**
 * @brief Encodes one immutable public runtime Value.
 * @param encoder Nonnull bounded destination.
 * @param value Valid public runtime Value.
 * @throws CodecFailure If Value shape/facets or payload bounds are invalid.
 * @throws std::bad_alloc If payload growth fails.
 * @note No durable result identity or internal execution object is encoded.
 */
void encode_value(Encoder* encoder, const Value& value) {
  if (!value.valid() || value.descriptor().shape.size() > 8U) {
    throw CodecFailure(ErrorCode::InvalidArgument,
                       "result contains an invalid Value");
  }
  encoder->u32(static_cast<std::uint32_t>(value.descriptor().element_type));
  encoder->u8(static_cast<std::uint8_t>(value.descriptor().shape.size()));
  for (std::uint64_t extent : value.descriptor().shape) {
    encoder->u64(extent);
  }
  for (const RegionDimension& dimension : value.region().dimensions()) {
    encoder->u64(dimension.offset);
    encoder->u64(dimension.extent);
  }
  encoder->u64(value.layout().byte_offset);
  for (std::int64_t stride : value.layout().byte_strides) {
    encoder->i64(stride);
  }
  if (value.facets().size() > 64U) {
    throw CodecFailure(ErrorCode::ResourceExhausted,
                       "result Value has too many facets");
  }
  encoder->u8(static_cast<std::uint8_t>(value.facets().size()));
  for (const ValueFacet& facet : value.facets()) {
    encoder->text(facet.key, 256U);
    encoder->u32(facet.version);
    encoder->bytes(facet.payload);
  }
  encoder->bytes(value.bytes());
}

/**
 * @brief Decodes and republishes one fully validated runtime Value.
 * @param decoder Nonnull bounded source.
 * @return Fully validated immutable public Value.
 * @throws CodecFailure If type, shape, Region, layout, facets, or bytes fail.
 * @throws std::bad_alloc If Value construction allocation fails.
 * @note Rank and facet counts are fenced against remaining Value/result bytes
 * before reserve. Publication occurs only through `Value::create` after
 * complete decode.
 */
Value decode_value(Decoder* decoder) {
  const std::uint32_t raw_type = decoder->u32();
  if (raw_type < static_cast<std::uint32_t>(ElementType::UInt8) ||
      raw_type > static_cast<std::uint32_t>(ElementType::Float64)) {
    throw CodecFailure(ErrorCode::InvalidArgument,
                       "wire Value element type is unknown");
  }
  const std::uint8_t rank = decoder->u8();
  if (rank == 0U || rank > 8U) {
    throw CodecFailure(ErrorCode::InvalidArgument,
                       "wire Value rank is outside 1..8");
  }
  decoder->require_count_fits(rank, kValueAxesContract.minimum_entry_bytes,
                              kValueAxesContract.required_suffix_bytes);
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
  observe_decoder_count(codec_test::DecoderCountKind::ValueAxes, rank);
#endif
  std::vector<std::uint64_t> shape;
  std::vector<RegionDimension> dimensions;
  std::vector<std::int64_t> strides;
  shape.reserve(rank);
  dimensions.reserve(rank);
  strides.reserve(rank);
  for (std::uint8_t axis = 0U; axis < rank; ++axis) {
    shape.push_back(decoder->u64());
  }
  for (std::uint8_t axis = 0U; axis < rank; ++axis) {
    dimensions.push_back(RegionDimension{decoder->u64(), decoder->u64()});
  }
  const std::uint64_t offset = decoder->u64();
  for (std::uint8_t axis = 0U; axis < rank; ++axis) {
    strides.push_back(decoder->i64());
  }
  const std::uint8_t facet_count = decoder->u8();
  if (facet_count > 64U) {
    throw CodecFailure(ErrorCode::InvalidArgument,
                       "wire Value has too many facets");
  }
  decoder->require_count_fits(facet_count,
                              kValueFacetsContract.minimum_entry_bytes,
                              kValueFacetsContract.required_suffix_bytes);
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
  observe_decoder_count(codec_test::DecoderCountKind::ValueFacets, facet_count);
#endif
  std::vector<ValueFacet> facets;
  facets.reserve(facet_count);
  for (std::uint8_t index = 0U; index < facet_count; ++index) {
    facets.push_back(ValueFacet{decoder->text(256U), decoder->u32(),
                                decoder->bytes(64U * 1024U)});
  }
  auto result = Value::create(
      ValueDescriptor{static_cast<ElementType>(raw_type), std::move(shape)},
      Region(std::move(dimensions)), StridedLayout{offset, std::move(strides)},
      decoder->bytes(kMaximumFramePayloadBytes), std::move(facets));
  if (!result.ok()) {
    throw CodecFailure(result.status().code, result.status().message);
  }
  return result.take_value();
}

/**
 * @brief Encodes raw execution diagnostics without internal IR.
 * @param encoder Nonnull bounded destination.
 * @param diagnostics Raw local timing/resource observations.
 * @throws CodecFailure If counts, enums, text, or frame size are invalid.
 * @throws std::bad_alloc If payload growth fails.
 * @note Digests are non-security reproducibility text only.
 */
void encode_diagnostics(Encoder* encoder,
                        const ExecutionDiagnostics& diagnostics) {
  if (diagnostics.selected_backends.size() > 65536U ||
      diagnostics.operation_timings.size() > 131072U ||
      diagnostics.fallback_reasons.size() > 65536U) {
    throw CodecFailure(ErrorCode::ResourceExhausted,
                       "execution diagnostics exceed wire bounds");
  }
  encoder->u64(diagnostics.execute_us);
  encoder->u32(
      static_cast<std::uint32_t>(diagnostics.selected_backends.size()));
  for (const auto& backend : diagnostics.selected_backends) {
    if (backend.first == 0U ||
        (backend.second != Backend::Cpu && backend.second != Backend::Gpu)) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "execution backend diagnostic is invalid");
    }
    encoder->u64(backend.first);
    encoder->u8(static_cast<std::uint8_t>(backend.second));
  }
  encoder->u64(diagnostics.transfer_count);
  encoder->u64(diagnostics.transfer_bytes);
  encoder->u64(diagnostics.peak_live_bytes);
  encoder->u32(static_cast<std::uint32_t>(diagnostics.fallback_reasons.size()));
  for (const std::string& reason : diagnostics.fallback_reasons) {
    encoder->text(reason, 4096U);
  }
  encoder->u32(
      static_cast<std::uint32_t>(diagnostics.operation_timings.size()));
  for (const OperationTiming& timing : diagnostics.operation_timings) {
    if (timing.node_id == 0U ||
        (timing.backend != Backend::Cpu && timing.backend != Backend::Gpu) ||
        static_cast<std::uint8_t>(timing.outcome) >
            static_cast<std::uint8_t>(ErrorCode::Internal)) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "execution timing diagnostic is invalid");
    }
    encoder->u64(timing.node_id);
    encoder->u8(static_cast<std::uint8_t>(timing.backend));
    encoder->u64(timing.duration_us);
    encoder->u8(static_cast<std::uint8_t>(timing.outcome));
  }
  encoder->text(diagnostics.plan_digest, 128U);
  encoder->text(diagnostics.result_digest, 128U);
}

/**
 * @brief Decodes bounded raw execution diagnostics.
 * @param decoder Nonnull bounded source.
 * @return Fully owned raw local timing/resource observations.
 * @throws CodecFailure If counts, duplicates, enums, text, or payload fail.
 * @throws std::bad_alloc If diagnostic storage allocation fails.
 * @note Backend, fallback-reason, and timing counts are fenced before
 * insertion, reserve, or iteration. No performance verdict or release
 * evidence is reconstructed.
 */
ExecutionDiagnostics decode_diagnostics(Decoder* decoder) {
  ExecutionDiagnostics diagnostics;
  diagnostics.execute_us = decoder->u64();
  const std::uint32_t backend_count = decoder->u32();
  if (backend_count > 65536U) {
    throw CodecFailure(ErrorCode::InvalidArgument,
                       "wire diagnostics have too many backends");
  }
  decoder->require_count_fits(
      backend_count, kDiagnosticBackendsContract.minimum_entry_bytes,
      kDiagnosticBackendsContract.required_suffix_bytes);
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
  observe_decoder_count(codec_test::DecoderCountKind::DiagnosticBackends,
                        backend_count);
#endif
  for (std::uint32_t index = 0U; index < backend_count; ++index) {
    const std::uint64_t node_id = decoder->u64();
    const std::uint8_t raw_backend = decoder->u8();
    if (node_id == 0U ||
        (raw_backend != static_cast<std::uint8_t>(Backend::Cpu) &&
         raw_backend != static_cast<std::uint8_t>(Backend::Gpu)) ||
        !diagnostics.selected_backends
             .emplace(node_id, static_cast<Backend>(raw_backend))
             .second) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire diagnostics backend map is malformed");
    }
  }
  diagnostics.transfer_count = decoder->u64();
  diagnostics.transfer_bytes = decoder->u64();
  diagnostics.peak_live_bytes = decoder->u64();
  const std::uint32_t reason_count = decoder->u32();
  if (reason_count > 65536U) {
    throw CodecFailure(ErrorCode::InvalidArgument,
                       "wire diagnostics have too many fallback reasons");
  }
  decoder->require_count_fits(reason_count,
                              kDiagnosticReasonsContract.minimum_entry_bytes,
                              kDiagnosticReasonsContract.required_suffix_bytes);
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
  observe_decoder_count(codec_test::DecoderCountKind::DiagnosticFallbackReasons,
                        reason_count);
#endif
  diagnostics.fallback_reasons.reserve(reason_count);
  for (std::uint32_t index = 0U; index < reason_count; ++index) {
    diagnostics.fallback_reasons.push_back(decoder->text(4096U));
  }
  const std::uint32_t timing_count = decoder->u32();
  if (timing_count > 131072U) {
    throw CodecFailure(ErrorCode::InvalidArgument,
                       "wire diagnostics have too many operation timings");
  }
  decoder->require_count_fits(timing_count,
                              kDiagnosticTimingsContract.minimum_entry_bytes,
                              kDiagnosticTimingsContract.required_suffix_bytes);
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
  observe_decoder_count(
      codec_test::DecoderCountKind::DiagnosticOperationTimings, timing_count);
#endif
  diagnostics.operation_timings.reserve(timing_count);
  for (std::uint32_t index = 0U; index < timing_count; ++index) {
    OperationTiming timing;
    timing.node_id = decoder->u64();
    const std::uint8_t raw_backend = decoder->u8();
    if (timing.node_id == 0U ||
        (raw_backend != static_cast<std::uint8_t>(Backend::Cpu) &&
         raw_backend != static_cast<std::uint8_t>(Backend::Gpu))) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire timing backend is unknown");
    }
    timing.backend = static_cast<Backend>(raw_backend);
    timing.duration_us = decoder->u64();
    const std::uint8_t raw_outcome = decoder->u8();
    if (raw_outcome > static_cast<std::uint8_t>(ErrorCode::Internal)) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire timing outcome is unknown");
    }
    timing.outcome = static_cast<ErrorCode>(raw_outcome);
    diagnostics.operation_timings.push_back(timing);
  }
  diagnostics.plan_digest = decoder->text(128U);
  diagnostics.result_digest = decoder->text(128U);
  return diagnostics;
}

/**
 * @brief Encodes complete copied named Values and raw diagnostics.
 * @param encoder Nonnull bounded destination.
 * @param result Public kernel execution result.
 * @throws CodecFailure If counts, Values, diagnostics, or frame size fail.
 * @throws std::bad_alloc If payload growth fails.
 * @note Result names are emitted in sorted map order.
 */
void encode_execution_result(Encoder* encoder, const ExecutionResult& result) {
  if (result.values.size() > 4096U) {
    throw CodecFailure(ErrorCode::ResourceExhausted,
                       "execution result has too many named Values");
  }
  encoder->u32(static_cast<std::uint32_t>(result.values.size()));
  for (const auto& value : result.values) {
    encoder->text(value.first, 1024U);
    encode_value(encoder, value.second);
  }
  encode_diagnostics(encoder, result.diagnostics);
}

/**
 * @brief Decodes complete copied named Values and raw diagnostics.
 * @param decoder Nonnull bounded source.
 * @return Fully owned public kernel execution result.
 * @throws CodecFailure If counts, names, Values, or diagnostics fail.
 * @throws std::bad_alloc If result allocation fails.
 * @note The named-Value count is fenced against minimum Values and complete
 * diagnostics before map insertion. Duplicate names reject the complete
 * response without publication.
 */
ExecutionResult decode_execution_result(Decoder* decoder) {
  ExecutionResult result;
  const std::uint32_t value_count = decoder->u32();
  if (value_count > 4096U) {
    throw CodecFailure(ErrorCode::InvalidArgument,
                       "wire result has too many named Values");
  }
  decoder->require_count_fits(value_count,
                              kResultValuesContract.minimum_entry_bytes,
                              kResultValuesContract.required_suffix_bytes);
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
  observe_decoder_count(codec_test::DecoderCountKind::ResultNamedValues,
                        value_count);
#endif
  for (std::uint32_t index = 0U; index < value_count; ++index) {
    std::string name = decoder->text(1024U);
    if (!result.values.emplace(name, decode_value(decoder)).second) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire result duplicates a Value name");
    }
  }
  result.diagnostics = decode_diagnostics(decoder);
  return result;
}

/**
 * @brief Decodes and validates one closed method code.
 * @param raw Candidate wire byte.
 * @return Exact version-three Method.
 * @throws CodecFailure If outside the nine-method range.
 * @note No extension or compatibility method range exists.
 */
Method decode_method(std::uint8_t raw) {
  if (raw < static_cast<std::uint8_t>(Method::SessionCreate) ||
      raw > static_cast<std::uint8_t>(Method::DaemonShutdown)) {
    throw CodecFailure(ErrorCode::InvalidArgument,
                       "wire method code is unknown");
  }
  return static_cast<Method>(raw);
}

/**
 * @brief Decodes and validates one closed Job state.
 * @param raw Candidate wire byte.
 * @return Exact five-state JobState.
 * @throws CodecFailure If outside the closed state range.
 * @note Lifecycle transition validity is checked with the status payload.
 */
JobState decode_job_state(std::uint8_t raw) {
  if (raw < static_cast<std::uint8_t>(JobState::Queued) ||
      raw > static_cast<std::uint8_t>(JobState::Cancelled)) {
    throw CodecFailure(ErrorCode::InvalidArgument, "wire job state is unknown");
  }
  return static_cast<JobState>(raw);
}

/**
 * @brief Translates an internal codec exception into one failed Result.
 * @tparam T Exported result value type.
 * @param failure Captured internal validation failure.
 * @return Failed Result with stable category and copied diagnostic.
 * @throws std::bad_alloc If diagnostic copy allocation fails.
 * @note No partially decoded value is retained.
 */
template <typename T>
Result<T> codec_failure_result(const CodecFailure& failure) {
  return Result<T>(Status::failure(failure.code(), failure.what()));
}

/**
 * @brief Correlation recovered without trusting a malformed request body.
 * @note The default pair is the sole failed-response sentinel and is never a
 * valid request or successful response identity.
 */
struct RecoveredCorrelation final {
  /** @brief Valid nonzero request id, or sentinel zero. */
  std::uint64_t request_id = 0U;
  /** @brief Valid request method, or sentinel daemon.info. */
  Method method = Method::DaemonInfo;
};

/**
 * @brief Recovers only the fixed valid v3 request header from malformed bytes.
 * @param payload Candidate request payload.
 * @return Valid correlation or the documented zero/daemon.info sentinel.
 * @throws Nothing.
 * @note The function reads at most eleven bytes and never allocates.
 */
RecoveredCorrelation recover_correlation(
    const std::vector<std::uint8_t>* payload) noexcept {
  RecoveredCorrelation correlation;
  if (!payload || payload->size() < 11U ||
      ((*payload)[0] | (static_cast<std::uint16_t>((*payload)[1]) << 8U)) !=
          kProtocolVersion) {
    return correlation;
  }
  std::uint64_t request_id = 0U;
  for (std::size_t index = 0U; index < 8U; ++index) {
    request_id |= static_cast<std::uint64_t>((*payload)[index + 2U])
                  << (index * 8U);
  }
  const auto method = static_cast<Method>((*payload)[10U]);
  if (request_id == 0U || !valid_method(method)) {
    return correlation;
  }
  correlation.request_id = request_id;
  correlation.method = method;
  return correlation;
}

}  // namespace

#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
namespace codec_test {

/** @copydoc install_decoder_count_observer */
void install_decoder_count_observer(DecoderCountObserver observer) noexcept {
  g_decoder_count_observer.store(observer, std::memory_order_release);
}

/** @copydoc decoder_count_fits */
bool decoder_count_fits(DecoderCountKind kind, std::uint64_t count,
                        std::size_t remaining_bytes) noexcept {
  return count_fits(count, remaining_bytes, count_contract(kind));
}

}  // namespace codec_test
#endif

/**
 * @brief Implements complete version-three request encoding.
 * @copydetails encode_request
 */
Result<std::vector<std::uint8_t>> encode_request(const Request& request) {
  try {
    if (request.request_id == 0U || !valid_method(request.method)) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire request id or method is invalid");
    }
    Encoder encoder;
    encoder.u16(kProtocolVersion);
    encoder.u64(request.request_id);
    encoder.u8(static_cast<std::uint8_t>(request.method));
    switch (request.method) {
      case Method::SessionCreate:
        encode_document(&encoder, request.document);
        break;
      case Method::SessionClose:
        require_session_id(request.session_id);
        encoder.u64(request.session_id.instance);
        encoder.u64(request.session_id.value);
        break;
      case Method::JobSubmit:
        require_session_id(request.session_id);
        encoder.u64(request.session_id.instance);
        encoder.u64(request.session_id.value);
        encoder.boolean(request.submit_options.allow_gpu);
        encoder.u32(request.submit_options.maximum_parallelism);
        break;
      case Method::JobStatus:
      case Method::JobCancel:
      case Method::JobResult:
      case Method::JobRelease:
        require_job_id(request.job_id);
        encoder.u64(request.job_id.instance);
        encoder.u64(request.job_id.value);
        break;
      case Method::DaemonInfo:
      case Method::DaemonShutdown:
        break;
    }
    return Result<std::vector<std::uint8_t>>(encoder.finish());
  } catch (const CodecFailure& failure) {
    return codec_failure_result<std::vector<std::uint8_t>>(failure);
  }
}

/**
 * @brief Implements complete version-three request decoding.
 * @copydetails decode_request
 */
Result<Request> decode_request(const std::vector<std::uint8_t>& payload) {
  try {
    Decoder decoder(payload);
    if (decoder.u16() != kProtocolVersion) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire protocol version is not three");
    }
    Request request;
    request.request_id = decoder.u64();
    if (request.request_id == 0U) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire request id must be nonzero");
    }
    request.method = decode_method(decoder.u8());
    switch (request.method) {
      case Method::SessionCreate:
        request.document = decode_document(&decoder);
        break;
      case Method::SessionClose:
        request.session_id.instance = decoder.u64();
        request.session_id.value = decoder.u64();
        require_session_id(request.session_id);
        break;
      case Method::JobSubmit:
        request.session_id.instance = decoder.u64();
        request.session_id.value = decoder.u64();
        request.submit_options.allow_gpu = decoder.boolean();
        request.submit_options.maximum_parallelism = decoder.u32();
        require_session_id(request.session_id);
        break;
      case Method::JobStatus:
      case Method::JobCancel:
      case Method::JobResult:
      case Method::JobRelease:
        request.job_id.instance = decoder.u64();
        request.job_id.value = decoder.u64();
        require_job_id(request.job_id);
        break;
      case Method::DaemonInfo:
      case Method::DaemonShutdown:
        break;
    }
    decoder.require_end();
    return Result<Request>(std::move(request));
  } catch (const CodecFailure& failure) {
    return codec_failure_result<Request>(failure);
  } catch (const std::invalid_argument& failure) {
    return Result<Request>(
        Status::failure(ErrorCode::InvalidArgument, failure.what()));
  }
}

/**
 * @brief Implements complete version-three response encoding.
 * @copydetails encode_response
 */
Result<std::vector<std::uint8_t>> encode_response(const Response& response) {
  try {
    if (response.request_id == 0U || !valid_method(response.method)) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "wire response id or method is invalid");
    }
    Encoder encoder;
    encoder.u16(kProtocolVersion);
    encoder.u64(response.request_id);
    encoder.u8(static_cast<std::uint8_t>(response.method));
    encode_status(&encoder, response.status);
    if (response.status.ok()) {
      switch (response.method) {
        case Method::SessionCreate:
          require_session_id(response.session_id);
          encoder.u64(response.session_id.instance);
          encoder.u64(response.session_id.value);
          break;
        case Method::JobSubmit:
          require_job_id(response.job_id);
          encoder.u64(response.job_id.instance);
          encoder.u64(response.job_id.value);
          break;
        case Method::JobStatus:
          validate_job_status(response.job_status);
          encoder.u64(response.job_status.job_id.instance);
          encoder.u64(response.job_status.job_id.value);
          encoder.u64(response.job_status.session_id.instance);
          encoder.u64(response.job_status.session_id.value);
          encoder.u8(static_cast<std::uint8_t>(response.job_status.state));
          encode_status(&encoder, response.job_status.outcome);
          break;
        case Method::JobResult:
          encode_execution_result(&encoder, response.execution_result);
          break;
        case Method::DaemonInfo:
          validate_daemon_info(response.daemon_info);
          encoder.u16(response.daemon_info.protocol_version);
          encoder.u64(response.daemon_info.instance_id);
          encoder.text(response.daemon_info.service_version, 128U);
          encoder.text(response.daemon_info.transport, 64U);
          if (response.daemon_info.methods.size() > 16U) {
            throw CodecFailure(ErrorCode::InvalidArgument,
                               "daemon method inventory is too large");
          }
          encoder.u8(
              static_cast<std::uint8_t>(response.daemon_info.methods.size()));
          for (const std::string& method : response.daemon_info.methods) {
            encoder.text(method, 64U);
          }
          encoder.u64(response.daemon_info.active_sessions);
          encoder.u64(response.daemon_info.active_jobs);
          encoder.u32(response.daemon_info.maximum_concurrency);
          encoder.u32(response.daemon_info.maximum_sessions);
          break;
        case Method::SessionClose:
        case Method::JobCancel:
        case Method::JobRelease:
        case Method::DaemonShutdown:
          break;
      }
    }
    return Result<std::vector<std::uint8_t>>(encoder.finish());
  } catch (const CodecFailure& failure) {
    return codec_failure_result<std::vector<std::uint8_t>>(failure);
  }
}

/**
 * @brief Implements correlated version-three response decoding.
 * @copydetails decode_response
 */
Result<Response> decode_response(const std::vector<std::uint8_t>& payload,
                                 Method expected_method,
                                 std::uint64_t expected_request_id) {
  try {
    if (expected_request_id == 0U || !valid_method(expected_method)) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "expected response correlation is invalid");
    }
    Decoder decoder(payload);
    if (decoder.u16() != kProtocolVersion) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "response protocol version is not three");
    }
    Response response;
    response.request_id = decoder.u64();
    response.method = decode_method(decoder.u8());
    if (response.request_id != expected_request_id ||
        response.method != expected_method) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "response correlation does not match request");
    }
    response.status = decode_status(&decoder);
    if (response.status.ok()) {
      switch (response.method) {
        case Method::SessionCreate:
          response.session_id.instance = decoder.u64();
          response.session_id.value = decoder.u64();
          require_session_id(response.session_id);
          break;
        case Method::JobSubmit:
          response.job_id.instance = decoder.u64();
          response.job_id.value = decoder.u64();
          require_job_id(response.job_id);
          break;
        case Method::JobStatus:
          response.job_status.job_id.instance = decoder.u64();
          response.job_status.job_id.value = decoder.u64();
          response.job_status.session_id.instance = decoder.u64();
          response.job_status.session_id.value = decoder.u64();
          response.job_status.state = decode_job_state(decoder.u8());
          response.job_status.outcome = decode_status(&decoder);
          validate_job_status(response.job_status);
          break;
        case Method::JobResult:
          response.execution_result = decode_execution_result(&decoder);
          break;
        case Method::DaemonInfo: {
          response.daemon_info.protocol_version = decoder.u16();
          response.daemon_info.instance_id = decoder.u64();
          response.daemon_info.service_version = decoder.text(128U);
          response.daemon_info.transport = decoder.text(64U);
          const std::uint8_t method_count = decoder.u8();
          if (method_count > 16U) {
            throw CodecFailure(ErrorCode::InvalidArgument,
                               "response has too many methods");
          }
          decoder.require_count_fits(
              method_count, kDaemonMethodsContract.minimum_entry_bytes,
              kDaemonMethodsContract.required_suffix_bytes);
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
          observe_decoder_count(codec_test::DecoderCountKind::DaemonMethods,
                                method_count);
#endif
          response.daemon_info.methods.reserve(method_count);
          for (std::uint8_t index = 0U; index < method_count; ++index) {
            response.daemon_info.methods.push_back(decoder.text(64U));
          }
          response.daemon_info.active_sessions = decoder.u64();
          response.daemon_info.active_jobs = decoder.u64();
          response.daemon_info.maximum_concurrency = decoder.u32();
          response.daemon_info.maximum_sessions = decoder.u32();
          validate_daemon_info(response.daemon_info);
          break;
        }
        case Method::SessionClose:
        case Method::JobCancel:
        case Method::JobRelease:
        case Method::DaemonShutdown:
          break;
      }
    }
    decoder.require_end();
    return Result<Response>(std::move(response));
  } catch (const CodecFailure& failure) {
    return codec_failure_result<Response>(failure);
  } catch (const std::invalid_argument& failure) {
    return Result<Response>(
        Status::failure(ErrorCode::InvalidArgument, failure.what()));
  }
}

/**
 * @brief Implements pre-routing typed error response encoding.
 * @copydetails encode_protocol_error
 */
Result<std::vector<std::uint8_t>> encode_protocol_error(
    const Status& status, const std::vector<std::uint8_t>* request_payload) {
  try {
    if (status.ok()) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "protocol error response cannot carry success");
    }
    const RecoveredCorrelation correlation =
        recover_correlation(request_payload);
    Encoder encoder;
    encoder.u16(kProtocolVersion);
    encoder.u64(correlation.request_id);
    encoder.u8(static_cast<std::uint8_t>(correlation.method));
    encode_status(&encoder, status);
    return Result<std::vector<std::uint8_t>>(encoder.finish());
  } catch (const CodecFailure& failure) {
    return codec_failure_result<std::vector<std::uint8_t>>(failure);
  }
}

/**
 * @brief Implements pre-routing typed error response decoding.
 * @copydetails decode_protocol_error
 */
Result<Response> decode_protocol_error(
    const std::vector<std::uint8_t>& payload) {
  try {
    Decoder decoder(payload);
    if (decoder.u16() != kProtocolVersion) {
      throw CodecFailure(ErrorCode::InvalidArgument,
                         "protocol error response version is not three");
    }
    Response response;
    response.request_id = decoder.u64();
    response.method = decode_method(decoder.u8());
    response.status = decode_status(&decoder);
    decoder.require_end();
    if (response.status.ok() ||
        (response.request_id == 0U && response.method != Method::DaemonInfo)) {
      throw CodecFailure(
          ErrorCode::InvalidArgument,
          "protocol error response status or sentinel is invalid");
    }
    return Result<Response>(std::move(response));
  } catch (const CodecFailure& failure) {
    return codec_failure_result<Response>(failure);
  }
}

/**
 * @brief Implements canonical method-name lookup.
 * @copydetails method_name
 */
const char* method_name(Method method) noexcept {
  switch (method) {
    case Method::SessionCreate:
      return "session.create";
    case Method::SessionClose:
      return "session.close";
    case Method::JobSubmit:
      return "job.submit";
    case Method::JobStatus:
      return "job.status";
    case Method::JobCancel:
      return "job.cancel";
    case Method::JobResult:
      return "job.result";
    case Method::JobRelease:
      return "job.release";
    case Method::DaemonInfo:
      return "daemon.info";
    case Method::DaemonShutdown:
      return "daemon.shutdown";
  }
  return "unknown";
}

/**
 * @brief Implements exact sorted method inventory construction.
 * @copydetails method_inventory
 */
std::vector<std::string> method_inventory() {
  std::vector<std::string> methods = {
      method_name(Method::DaemonInfo),    method_name(Method::DaemonShutdown),
      method_name(Method::JobCancel),     method_name(Method::JobRelease),
      method_name(Method::JobResult),     method_name(Method::JobStatus),
      method_name(Method::JobSubmit),     method_name(Method::SessionClose),
      method_name(Method::SessionCreate),
  };
  std::sort(methods.begin(), methods.end());
  return methods;
}

}  // namespace ps::ipc::internal
