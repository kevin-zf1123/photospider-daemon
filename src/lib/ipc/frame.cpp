#include "ipc/frame.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ps::ipc::internal {
namespace {

/**
 * @brief Reads an exact byte count with explicit clean-EOF classification.
 * @param descriptor Connected stream descriptor.
 * @param destination Writable byte range.
 * @param size Exact requested count.
 * @param clean_eof Set only when EOF occurs before reading any byte.
 * @return Success or transport/truncation failure.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note EINTR resumes the same read operation.
 */
Status read_exact(int descriptor, void* destination, std::size_t size,
                  bool* clean_eof) {
  auto* bytes = static_cast<std::uint8_t*>(destination);
  std::size_t offset = 0U;
  *clean_eof = false;
  while (offset < size) {
    const ssize_t count = ::recv(descriptor, bytes + offset, size - offset, 0);
    if (count == 0) {
      if (offset == 0U) {
        *clean_eof = true;
        return Status::failure(ErrorCode::NotFound,
                               "local peer closed the connection");
      }
      return Status::failure(ErrorCode::InvalidArgument,
                             "local peer closed during a frame");
    }
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::failure(
          ErrorCode::Internal,
          std::string("local frame read failed: ") + std::strerror(errno));
    }
    offset += static_cast<std::size_t>(count);
  }
  return Status::success();
}

/**
 * @brief Writes an exact byte range without SIGPIPE.
 * @param descriptor Connected stream descriptor.
 * @param source Immutable byte range.
 * @param size Exact count.
 * @return Success or transport failure.
 * @throws std::bad_alloc If a failure diagnostic allocation fails.
 * @note EINTR resumes the same write operation.
 */
Status write_exact(int descriptor, const void* source, std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(source);
  std::size_t offset = 0U;
  while (offset < size) {
#if defined(MSG_NOSIGNAL)
    const ssize_t count =
        ::send(descriptor, bytes + offset, size - offset, MSG_NOSIGNAL);
#else
    const ssize_t count = ::send(descriptor, bytes + offset, size - offset, 0);
#endif
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::failure(
          ErrorCode::Internal,
          std::string("local frame write failed: ") + std::strerror(errno));
    }
    if (count == 0) {
      return Status::failure(ErrorCode::Internal,
                             "local frame write made no progress");
    }
    offset += static_cast<std::size_t>(count);
  }
  return Status::success();
}

}  // namespace

/**
 * @brief Implements bounded local frame reading.
 * @copydetails read_frame
 */
Result<std::vector<std::uint8_t>> read_frame(int descriptor) {
  if (descriptor < 0) {
    return Result<std::vector<std::uint8_t>>(Status::failure(
        ErrorCode::InvalidArgument, "frame descriptor is invalid"));
  }
  std::uint8_t header[4]{};
  bool clean_eof = false;
  Status status = read_exact(descriptor, header, sizeof(header), &clean_eof);
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>(std::move(status));
  }
  const std::uint32_t size = (static_cast<std::uint32_t>(header[0]) << 24U) |
                             (static_cast<std::uint32_t>(header[1]) << 16U) |
                             (static_cast<std::uint32_t>(header[2]) << 8U) |
                             static_cast<std::uint32_t>(header[3]);
  if (size == 0U || size > kMaximumFramePayloadBytes) {
    return Result<std::vector<std::uint8_t>>(Status::failure(
        ErrorCode::InvalidArgument, "frame payload length is outside bounds"));
  }
  std::vector<std::uint8_t> payload(size);
  status = read_exact(descriptor, payload.data(), payload.size(), &clean_eof);
  if (!status.ok()) {
    return Result<std::vector<std::uint8_t>>(std::move(status));
  }
  return Result<std::vector<std::uint8_t>>(std::move(payload));
}

/**
 * @brief Implements bounded local frame writing.
 * @copydetails write_frame
 */
Status write_frame(int descriptor, const std::vector<std::uint8_t>& payload) {
  if (descriptor < 0 || payload.empty() ||
      payload.size() > kMaximumFramePayloadBytes ||
      payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "frame descriptor or payload is invalid");
  }
  const auto size = static_cast<std::uint32_t>(payload.size());
  const std::uint8_t header[4] = {
      static_cast<std::uint8_t>((size >> 24U) & 0xffU),
      static_cast<std::uint8_t>((size >> 16U) & 0xffU),
      static_cast<std::uint8_t>((size >> 8U) & 0xffU),
      static_cast<std::uint8_t>(size & 0xffU),
  };
  Status status = write_exact(descriptor, header, sizeof(header));
  return status.ok() ? write_exact(descriptor, payload.data(), payload.size())
                     : status;
}

}  // namespace ps::ipc::internal
