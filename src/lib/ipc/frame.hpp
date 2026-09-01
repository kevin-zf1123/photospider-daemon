#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "photospider/core/status.hpp"
#include "photospider/ipc/protocol.hpp"

namespace ps::ipc::internal {

/** @brief Exact byte count of the fixed IPC v3 request correlation header. */
inline constexpr std::size_t kRequestCorrelationPrefixBytes = 11U;

/**
 * @brief Fixed-size progress retained while one frame payload is received.
 *
 * @note The prefix never allocates and is sufficient only for protocol-error
 * correlation recovery; it is not a second frame buffer.
 */
struct FrameReadProgress final {
  /** @brief First received request-payload bytes, capped at eleven. */
  std::array<std::uint8_t, kRequestCorrelationPrefixBytes> payload_prefix{};
  /** @brief Number of initialized bytes in `payload_prefix`. */
  std::size_t payload_prefix_size = 0U;
};

/**
 * @brief Reads one four-byte big-endian length-prefixed binary payload.
 * @param descriptor Connected local stream descriptor.
 * @param progress Optional fixed correlation-prefix progress destination.
 * @return Complete bounded payload or typed EOF/malformed/transport failure.
 * @throws std::bad_alloc If payload allocation fails.
 * @note EOF before a new header returns `NotFound`; mid-frame EOF is invalid.
 * Payload storage grows only after bytes are received and never reserves the
 * complete peer-declared length in advance.
 */
[[nodiscard]] Result<std::vector<std::uint8_t>> read_frame(
    int descriptor, FrameReadProgress* progress = nullptr);

/**
 * @brief Writes one complete bounded length-prefixed binary payload.
 * @param descriptor Connected local stream descriptor.
 * @param payload Nonempty payload no larger than the protocol maximum.
 * @return Success or typed transport/argument failure.
 * @throws std::bad_alloc If a transport diagnostic allocation fails.
 * @note Interrupted partial writes resume the same frame and are never retried
 * as a distinct request.
 */
[[nodiscard]] Status write_frame(int descriptor,
                                 const std::vector<std::uint8_t>& payload);

}  // namespace ps::ipc::internal
