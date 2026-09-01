#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ipc/codec.hpp"
#include "ipc/frame.hpp"
#include "ipc/unix_socket.hpp"

/**
 * @brief Fuzzes bounded request decoding and real stream frame classification.
 * @param data Arbitrary libFuzzer bytes.
 * @param size Exact byte count.
 * @return Always zero after bounded decode/read attempts.
 * @throws Nothing across the libFuzzer C boundary.
 * @note This manual target is never registered with CTest and retains no crash
 * artifact or corpus outside the explicit caller-selected directories.
 */
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) noexcept {
  try {
    const std::size_t bounded_size =
        std::min(size, ps::ipc::kMaximumFramePayloadBytes + std::size_t{1U});
    std::vector<std::uint8_t> payload;
    if (bounded_size != 0U) {
      payload.assign(data, data + bounded_size);
    }
    static_cast<void>(ps::ipc::internal::decode_request(payload));
    static_cast<void>(ps::ipc::internal::decode_protocol_error(payload));

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
