#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

#include "ipc/codec.hpp"
#include "ipc/frame.hpp"
#include "ipc/unix_socket.hpp"

namespace {

/**
 * @brief Decodes one hexadecimal digit for maintained text corpus seeds.
 * @param value Candidate ASCII byte.
 * @return Nibble value, or -1 for a non-hexadecimal byte.
 * @throws Nothing.
 */
int hex_nibble(std::uint8_t value) noexcept {
  if (value >= static_cast<std::uint8_t>('0') &&
      value <= static_cast<std::uint8_t>('9')) {
    return value - static_cast<std::uint8_t>('0');
  }
  if (value >= static_cast<std::uint8_t>('a') &&
      value <= static_cast<std::uint8_t>('f')) {
    return value - static_cast<std::uint8_t>('a') + 10;
  }
  if (value >= static_cast<std::uint8_t>('A') &&
      value <= static_cast<std::uint8_t>('F')) {
    return value - static_cast<std::uint8_t>('A') + 10;
  }
  return -1;
}

/**
 * @brief Materializes raw fuzz bytes or one maintained `hex:` wire seed.
 * @param data Arbitrary libFuzzer bytes.
 * @param size Exact input byte count.
 * @return Bounded payload presented to every codec decoder.
 * @throws std::bad_alloc If bounded scratch allocation fails.
 * @note A malformed hexadecimal marker falls back to the original raw bytes;
 * generated mutations therefore remain meaningful arbitrary wire inputs.
 */
std::vector<std::uint8_t> materialize_payload(const std::uint8_t* data,
                                              std::size_t size) {
  const std::size_t bounded_size =
      std::min(size, ps::ipc::kMaximumFramePayloadBytes + std::size_t{1U});
  constexpr std::uint8_t kHexPrefix[] = {'h', 'e', 'x', ':'};
  if (bounded_size >= sizeof(kHexPrefix) &&
      std::equal(std::begin(kHexPrefix), std::end(kHexPrefix), data)) {
    std::vector<std::uint8_t> decoded;
    decoded.reserve((bounded_size - sizeof(kHexPrefix)) / 2U);
    std::size_t offset = sizeof(kHexPrefix);
    while (offset < bounded_size && data[offset] != '\n' &&
           data[offset] != '\r') {
      if (offset + 1U >= bounded_size) {
        decoded.clear();
        break;
      }
      const int high = hex_nibble(data[offset]);
      const int low = hex_nibble(data[offset + 1U]);
      if (high < 0 || low < 0) {
        decoded.clear();
        break;
      }
      decoded.push_back(static_cast<std::uint8_t>((high << 4U) | low));
      offset += 2U;
    }
    if (!decoded.empty()) {
      return decoded;
    }
  }

  std::vector<std::uint8_t> payload;
  if (bounded_size != 0U) {
    payload.assign(data, data + bounded_size);
  }
  return payload;
}

}  // namespace

/**
 * @brief Fuzzes requests, response families, and stream frame classification.
 * @param data Arbitrary libFuzzer bytes.
 * @param size Exact byte count.
 * @return Always zero after bounded decode/read attempts.
 * @throws Nothing across the libFuzzer C boundary.
 * @note JobResult and DaemonInfo response decoding exercise named Values,
 * diagnostics, timings, and method counts. This manual target is never
 * registered with CTest and retains no crash artifact or corpus outside the
 * explicit caller-selected directories.
 */
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) noexcept {
  try {
    std::vector<std::uint8_t> payload = materialize_payload(data, size);
    static_cast<void>(ps::ipc::internal::decode_request(payload));
    static_cast<void>(ps::ipc::internal::decode_protocol_error(payload));
    static_cast<void>(ps::ipc::internal::decode_response(
        payload, ps::ipc::internal::Method::JobResult, 1U));
    static_cast<void>(ps::ipc::internal::decode_response(
        payload, ps::ipc::internal::Method::DaemonInfo, 1U));

    int descriptors[2]{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
      return 0;
    }
    ps::ipc::internal::UniqueDescriptor writer(descriptors[0]);
    ps::ipc::internal::UniqueDescriptor reader(descriptors[1]);
    const std::size_t stream_size = std::min(size, std::size_t{4096U});
    std::size_t offset = 0U;
    while (offset < stream_size) {
      const ssize_t written =
          ::write(writer.get(), data + offset, stream_size - offset);
      if (written <= 0) {
        break;
      }
      offset += static_cast<std::size_t>(written);
    }
    ::shutdown(writer.get(), SHUT_WR);
    static_cast<void>(ps::ipc::internal::read_frame(reader.get()));
  } catch (...) {
    return 0;
  }
  return 0;
}
