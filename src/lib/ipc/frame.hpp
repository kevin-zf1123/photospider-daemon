#pragma once

#include <cstdint>
#include <vector>

#include "photospider/core/status.hpp"
#include "photospider/ipc/protocol.hpp"

namespace ps::ipc::internal {

/**
 * @brief Reads one four-byte big-endian length-prefixed binary payload.
 * @param descriptor Connected local stream descriptor.
 * @return Complete bounded payload or typed EOF/malformed/transport failure.
 * @throws std::bad_alloc If payload allocation fails.
 * @note EOF before a new header returns `NotFound`; mid-frame EOF is invalid.
 */
[[nodiscard]] Result<std::vector<std::uint8_t>> read_frame(int descriptor);

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
