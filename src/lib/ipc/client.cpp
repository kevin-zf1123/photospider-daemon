#include "photospider/ipc/client.hpp"

#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "ipc/codec.hpp"
#include "ipc/frame.hpp"
#include "ipc/unix_socket.hpp"

namespace ps::ipc {

/** @brief Descriptor, request sequence, and single-call serialization. */
struct Client::Impl final {
  /**
   * @brief Executes one correlated request and invalidates transport on error.
   * @param request Method and arguments; request id is assigned here.
   * @return Complete decoded response or local codec/transport failure.
   * @throws std::bad_alloc If codec/result storage allocation fails.
   * @note The mutex covers the entire request/response exchange so correlation
   * state and stream framing cannot interleave.
   */
  Result<internal::Response> call(internal::Request request) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!descriptor.valid()) {
      return Result<internal::Response>(Status::failure(
          ErrorCode::InvalidArgument, "client is not connected"));
    }
    if (next_request_id == 0U) {
      descriptor.reset();
      return Result<internal::Response>(Status::failure(
          ErrorCode::ResourceExhausted, "client request id exhausted"));
    }
    request.request_id = next_request_id++;
    auto encoded = internal::encode_request(request);
    if (!encoded.ok()) {
      return Result<internal::Response>(encoded.status());
    }
    Status write_status =
        internal::write_frame(descriptor.get(), encoded.value());
    if (!write_status.ok()) {
      descriptor.reset();
      return Result<internal::Response>(std::move(write_status));
    }
    auto frame = internal::read_frame(descriptor.get());
    if (!frame.ok()) {
      descriptor.reset();
      return Result<internal::Response>(frame.status());
    }
    auto response = internal::decode_response(frame.value(), request.method,
                                              request.request_id);
    if (!response.ok()) {
      descriptor.reset();
    }
    return response;
  }

  /** @brief Serializes connection mutation and complete calls. */
  std::mutex mutex;
  /** @brief Owned local stream descriptor. */
  internal::UniqueDescriptor descriptor;
  /** @brief Next nonzero correlation id. */
  std::uint64_t next_request_id = 1U;
};

/**
 * @brief Implements creation of one disconnected client.
 * @copydetails Client::Client
 */
Client::Client() : impl_(std::make_unique<Impl>()) {}

/**
 * @brief Implements exact local transport teardown.
 * @copydetails Client::~Client
 */
Client::~Client() noexcept = default;

/**
 * @brief Implements move construction of client ownership.
 * @copydetails Client::Client(Client&&)
 */
Client::Client(Client&& other) noexcept = default;

/**
 * @brief Implements move replacement of client ownership.
 * @copydetails Client::operator=(Client&&)
 */
Client& Client::operator=(Client&& other) noexcept = default;

/**
 * @brief Implements connection to one explicit local socket.
 * @copydetails Client::connect
 */
Status Client::connect(const std::string& socket_path) {
  if (!impl_) {
    impl_ = std::make_unique<Impl>();
  }
  auto connected = internal::connect_unix_socket(socket_path);
  if (!connected.ok()) {
    return connected.status();
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->descriptor = connected.take_value();
  impl_->next_request_id = 1U;
  return Status::success();
}

/**
 * @brief Implements idempotent transport disconnection.
 * @copydetails Client::disconnect
 */
void Client::disconnect() noexcept {
  if (impl_) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->descriptor.reset();
  }
}

/**
 * @brief Implements descriptor-ownership observation.
 * @copydetails Client::connected
 */
bool Client::connected() const noexcept {
  if (!impl_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->descriptor.valid();
}

/**
 * @brief Implements the `session.create` RPC.
 * @copydetails Client::session_create
 */
Result<SessionId> Client::session_create(const WorkflowDocument& document) {
  if (!impl_) {
    return Result<SessionId>(
        Status::failure(ErrorCode::InvalidArgument, "client is moved from"));
  }
  internal::Request request;
  request.method = internal::Method::SessionCreate;
  request.document = document;
  auto response = impl_->call(std::move(request));
  if (!response.ok()) {
    return Result<SessionId>(response.status());
  }
  return response.value().status.ok()
             ? Result<SessionId>(response.value().session_id)
             : Result<SessionId>(response.value().status);
}

/**
 * @brief Implements the `session.close` RPC.
 * @copydetails Client::session_close
 */
Status Client::session_close(SessionId session_id) {
  if (!impl_) {
    return Status::failure(ErrorCode::InvalidArgument, "client is moved from");
  }
  internal::Request request;
  request.method = internal::Method::SessionClose;
  request.session_id = session_id;
  auto response = impl_->call(std::move(request));
  return response.ok() ? response.value().status : response.status();
}

/**
 * @brief Implements the `job.submit` RPC.
 * @copydetails Client::job_submit
 */
Result<JobId> Client::job_submit(SessionId session_id,
                                 const JobSubmitOptions& options) {
  if (!impl_) {
    return Result<JobId>(
        Status::failure(ErrorCode::InvalidArgument, "client is moved from"));
  }
  internal::Request request;
  request.method = internal::Method::JobSubmit;
  request.session_id = session_id;
  request.submit_options = options;
  auto response = impl_->call(std::move(request));
  if (!response.ok()) {
    return Result<JobId>(response.status());
  }
  return response.value().status.ok() ? Result<JobId>(response.value().job_id)
                                      : Result<JobId>(response.value().status);
}

/**
 * @brief Implements the `job.status` RPC.
 * @copydetails Client::job_status
 */
Result<JobStatus> Client::job_status(JobId job_id) {
  if (!impl_) {
    return Result<JobStatus>(
        Status::failure(ErrorCode::InvalidArgument, "client is moved from"));
  }
  internal::Request request;
  request.method = internal::Method::JobStatus;
  request.job_id = job_id;
  auto response = impl_->call(std::move(request));
  if (!response.ok()) {
    return Result<JobStatus>(response.status());
  }
  return response.value().status.ok()
             ? Result<JobStatus>(response.value().job_status)
             : Result<JobStatus>(response.value().status);
}

/**
 * @brief Implements the `job.cancel` RPC.
 * @copydetails Client::job_cancel
 */
Status Client::job_cancel(JobId job_id) {
  if (!impl_) {
    return Status::failure(ErrorCode::InvalidArgument, "client is moved from");
  }
  internal::Request request;
  request.method = internal::Method::JobCancel;
  request.job_id = job_id;
  auto response = impl_->call(std::move(request));
  return response.ok() ? response.value().status : response.status();
}

/**
 * @brief Implements the `job.result` RPC.
 * @copydetails Client::job_result
 */
Result<ExecutionResult> Client::job_result(JobId job_id) {
  if (!impl_) {
    return Result<ExecutionResult>(
        Status::failure(ErrorCode::InvalidArgument, "client is moved from"));
  }
  internal::Request request;
  request.method = internal::Method::JobResult;
  request.job_id = job_id;
  auto response = impl_->call(std::move(request));
  if (!response.ok()) {
    return Result<ExecutionResult>(response.status());
  }
  return response.value().status.ok()
             ? Result<ExecutionResult>(response.value().execution_result)
             : Result<ExecutionResult>(response.value().status);
}

/**
 * @brief Implements the `job.release` RPC.
 * @copydetails Client::job_release
 */
Status Client::job_release(JobId job_id) {
  if (!impl_) {
    return Status::failure(ErrorCode::InvalidArgument, "client is moved from");
  }
  internal::Request request;
  request.method = internal::Method::JobRelease;
  request.job_id = job_id;
  auto response = impl_->call(std::move(request));
  return response.ok() ? response.value().status : response.status();
}

/**
 * @brief Implements the `daemon.info` RPC.
 * @copydetails Client::daemon_info
 */
Result<DaemonInfo> Client::daemon_info() {
  if (!impl_) {
    return Result<DaemonInfo>(
        Status::failure(ErrorCode::InvalidArgument, "client is moved from"));
  }
  internal::Request request;
  request.method = internal::Method::DaemonInfo;
  auto response = impl_->call(std::move(request));
  if (!response.ok()) {
    return Result<DaemonInfo>(response.status());
  }
  return response.value().status.ok()
             ? Result<DaemonInfo>(response.value().daemon_info)
             : Result<DaemonInfo>(response.value().status);
}

/**
 * @brief Implements the `daemon.shutdown` RPC and connection teardown.
 * @copydetails Client::daemon_shutdown
 */
Status Client::daemon_shutdown() {
  if (!impl_) {
    return Status::failure(ErrorCode::InvalidArgument, "client is moved from");
  }
  internal::Request request;
  request.method = internal::Method::DaemonShutdown;
  auto response = impl_->call(std::move(request));
  const Status status =
      response.ok() ? response.value().status : response.status();
  disconnect();
  return status;
}

}  // namespace ps::ipc
