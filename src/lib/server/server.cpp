#include "server/server.hpp"

#include <unistd.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ipc/frame.hpp"
#include "ipc/unix_socket.hpp"

#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
#include "support/exception_fence_faults.hpp"
#endif

namespace ps::ipc::internal {
namespace {

/**
 * @brief Assigns one non-success status without allocating a diagnostic.
 * @param status Status storage to overwrite in place.
 * @param code Intended non-success category; `Ok` is normalized to `Internal`.
 * @throws Nothing.
 * @note Existing diagnostic storage is cleared in place and may retain
 * capacity; no diagnostic text is required by the wire failure contract.
 */
void set_failure_without_allocation(Status& status, ErrorCode code) noexcept {
  status.code = code == ErrorCode::Ok ? ErrorCode::Internal : code;
  status.message.clear();
}

/**
 * @brief Sends one best-effort typed pre-routing failure response.
 * @param descriptor Connected local stream descriptor.
 * @param status Required non-success protocol/backpressure status.
 * @param request_payload Optional malformed payload for correlation recovery.
 * @param progress Optional fixed prefix retained by an incomplete frame read.
 * @throws Nothing.
 * @note Full payload takes precedence over progress. Prefix materialization is
 * capped at eleven bytes; encoding/writing failure leaves teardown to owner.
 */
void write_protocol_failure(
    int descriptor, const Status& status,
    const std::vector<std::uint8_t>* request_payload = nullptr,
    const FrameReadProgress* progress = nullptr) noexcept {
  try {
    std::vector<std::uint8_t> prefix;
    if (!request_payload && progress && progress->payload_prefix_size != 0U) {
      prefix.assign(
          progress->payload_prefix.begin(),
          progress->payload_prefix.begin() +
              static_cast<std::ptrdiff_t>(progress->payload_prefix_size));
      request_payload = &prefix;
    }
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
    test::hit_exception_fence_fault(
        test::ExceptionFenceFaultPoint::ProtocolFailureEncode);
#endif
    auto encoded = encode_protocol_error(status, request_payload);
    if (encoded.ok()) {
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
      test::hit_exception_fence_fault(
          test::ExceptionFenceFaultPoint::ProtocolFailureWrite);
#endif
      static_cast<void>(write_frame(descriptor, encoded.value()));
    }
  } catch (...) {
  }
}

/**
 * @brief Attempts one typed no-diagnostic protocol failure.
 * @param descriptor Connected local stream descriptor.
 * @param code Intended non-success category.
 * @throws Nothing.
 * @note The response uses recovered correlation only when payload/progress is
 * supplied by the caller; with neither, encoding uses the documented
 * `request_id=0`, `method=daemon.info` sentinel. Encoding or writing may fail,
 * but the complete attempt stays inside an exception fence.
 */
void write_protocol_failure_code(int descriptor, ErrorCode code) noexcept {
  Status status;
  set_failure_without_allocation(status, code);
  write_protocol_failure(descriptor, status);
}

/**
 * @brief Fences diagnostic construction for one handler exception response.
 * @param descriptor Connected local stream descriptor.
 * @param code Typed catch category preserved even if diagnostics cannot be
 * allocated.
 * @param diagnostic Optional human-readable text copied on the primary path.
 * @param progress Fixed correlation prefix retained by the failed frame read.
 * @throws Nothing.
 * @note A secondary exception leaves an empty diagnostic but never changes the
 * failure to success. Encoding and writing remain separately best-effort.
 */
void write_handler_failure(int descriptor, ErrorCode code,
                           const char* diagnostic,
                           const FrameReadProgress* progress) noexcept {
  Status status;
  set_failure_without_allocation(status, code);
  try {
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
    test::hit_exception_fence_fault(
        test::ExceptionFenceFaultPoint::HandlerFailureStatus);
#endif
    status = Status::failure(code, diagnostic ? diagnostic : "");
  } catch (...) {
    set_failure_without_allocation(status, code);
  }
  write_protocol_failure(descriptor, status, nullptr, progress);
}

/**
 * @brief One joinable connection thread plus monotonic completion flag.
 * @note The accept owner joins this thread before erasing the record; handler
 * code never detaches or owns the record container.
 */
struct HandlerRecord final {
  /** @brief Owned joinable handler thread. */
  std::thread thread;
  /** @brief True after handler cleanup finishes. */
  std::shared_ptr<std::atomic<bool>> finished;
};

/**
 * @brief Exactly-once active-count/completion publication for one handler.
 *
 * @note Destruction runs on the handler thread after connection cleanup.
 */
class HandlerCompletion final {
 public:
  /**
   * @brief Binds active count and shared completion flag.
   * @param active Nonnull server-owned active handler counter.
   * @param finished Nonnull flag retained by the join owner.
   * @throws Nothing.
   */
  HandlerCompletion(std::atomic<std::uint32_t>* active,
                    std::shared_ptr<std::atomic<bool>> finished) noexcept
      : active_(active), finished_(std::move(finished)) {}

  /**
   * @brief Publishes handler completion and releases one active slot.
   * @throws Nothing.
   * @note The flag uses release ordering before the accept loop joins.
   */
  ~HandlerCompletion() noexcept {
    if (finished_) {
      finished_->store(true, std::memory_order_release);
    }
    if (active_) {
      active_->fetch_sub(1U, std::memory_order_acq_rel);
    }
  }

  /**
   * @brief Forbids duplicating one handler completion publication.
   * @param other Source guard that cannot be copied.
   * @throws Nothing; the operation is deleted.
   * @note Exactly one thread releases the active slot.
   */
  HandlerCompletion(const HandlerCompletion& other) = delete;
  /**
   * @brief Forbids assigning exactly-once completion ownership.
   * @param other Source guard that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Completion flag and active count always settle together.
   */
  HandlerCompletion& operator=(const HandlerCompletion& other) = delete;

 private:
  /** @brief Server-owned active count. */
  std::atomic<std::uint32_t>* active_ = nullptr;
  /** @brief Join-owner completion observation. */
  std::shared_ptr<std::atomic<bool>> finished_;
};

}  // namespace

/**
 * @brief Complete private state for one local server lifetime.
 * @note Listener, handler, connection, and service ownership all end together.
 */
struct Server::Impl final {
  /**
   * @brief Constructs service and takes ownership of a bound listener.
   * @param config Validated fixed configuration.
   * @param listener Bound descriptor and exact pathname-generation guard.
   * @throws std::bad_alloc If service/handler storage allocation or the
   * private test-runtime construction hook fails that way.
   * @throws std::system_error If service worker creation fails.
   * @throws Any other exception deliberately raised by the private
   * test-runtime hook.
   * @note The parameter retains RAII ownership through every throwing step;
   * only complete construction transfers the descriptor to `stop()`.
   */
  Impl(ServerConfig config, BoundUnixListener listener)
      : config(std::move(config)),
        service(this->config.service),
        socket_node(std::move(listener.socket_node)) {
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
    if (this->config.construction_hook) {
      this->config.construction_hook(
          ServerConstructionStage::BeforeHandlerStorage);
    }
#endif
    handlers.reserve(this->config.maximum_active_connections);
    listener_fd.store(listener.descriptor.release(), std::memory_order_release);
  }

  /**
   * @brief Closes all transport state and joins every handler.
   * @throws Nothing.
   * @note The socket node is removed only when its fixed-parent device/inode
   * generation still matches this instance.
   */
  ~Impl() noexcept {
    stop();
    join_handlers();
    socket_node.remove();
  }

  /**
   * @brief Serves one sequential connection until EOF, failure, or shutdown.
   * @param connection Owned same-user stream descriptor.
   * @throws Nothing across the handler-thread boundary.
   * @note Malformed input receives one typed error then closes this connection;
   * accepted shutdown is captured immediately after dispatch and interrupts
   * the listener and peer handlers from the common no-throw tail regardless
   * of response encoding, acknowledgement write, or catch-path failure.
   */
  void serve_connection(UniqueDescriptor connection) noexcept {
    const int descriptor = connection.get();
    FrameReadProgress frame_progress;
    bool registered = false;
    bool accepted_shutdown = false;
    try {
      std::lock_guard<std::mutex> lock(connections_mutex);
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
      test::hit_exception_fence_fault(
          test::ExceptionFenceFaultPoint::ConnectionRegistration);
#endif
      active_connections.emplace(descriptor, descriptor);
      registered = true;
    } catch (...) {
      write_protocol_failure_code(descriptor, ErrorCode::ResourceExhausted);
      return;
    }
    try {
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
      test::hit_exception_fence_fault(
          test::ExceptionFenceFaultPoint::HandlerPrimary);
#endif
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
      if (config.handler_entry_hook) {
        config.handler_entry_hook();
      }
#endif
      while (!stopping.load(std::memory_order_acquire)) {
        auto frame = read_frame(descriptor, &frame_progress);
        if (!frame.ok()) {
          if (frame.status().code != ErrorCode::NotFound) {
            write_protocol_failure(descriptor, frame.status(), nullptr,
                                   &frame_progress);
          }
          break;
        }
        auto request = decode_request(frame.value());
        if (!request.ok()) {
          write_protocol_failure(descriptor, request.status(), &frame.value());
          break;
        }
        Response response = service.dispatch(request.value());
        accepted_shutdown =
            response.status.ok() && response.shutdown_after_write;
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
        test::observe_shutdown_response_ready(accepted_shutdown);
        test::hit_exception_fence_fault(
            test::ExceptionFenceFaultPoint::ResponseEncode);
#endif
        auto encoded = encode_response(response);
        if (!encoded.ok()) {
          write_protocol_failure(descriptor, encoded.status(), &frame.value());
          break;
        }
        const Status write_status = write_frame(descriptor, encoded.value());
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
        test::observe_shutdown_response_write(accepted_shutdown, write_status);
#endif
        if (!write_status.ok()) {
          break;
        }
        if (accepted_shutdown) {
          break;
        }
      }
    } catch (const std::bad_alloc&) {
      write_handler_failure(descriptor, ErrorCode::ResourceExhausted,
                            "connection handler allocation failed",
                            &frame_progress);
    } catch (const std::exception& error) {
      write_handler_failure(descriptor, ErrorCode::Internal, error.what(),
                            &frame_progress);
    } catch (...) {
      write_handler_failure(descriptor, ErrorCode::Internal,
                            "connection handler raised an exception",
                            &frame_progress);
    }
    if (accepted_shutdown) {
      stop();
    }
    if (registered) {
      std::lock_guard<std::mutex> lock(connections_mutex);
      active_connections.erase(descriptor);
    }
  }

  /**
   * @brief Joins and erases every handler whose connection has finished.
   * @throws Nothing.
   * @note Called by the accept owner before each new admission.
   */
  void reap_finished_handlers() noexcept {
    std::lock_guard<std::mutex> lock(handlers_mutex);
    for (auto iterator = handlers.begin(); iterator != handlers.end();) {
      if (!iterator->finished->load(std::memory_order_acquire)) {
        ++iterator;
        continue;
      }
      if (iterator->thread.joinable()) {
        iterator->thread.join();
      }
      iterator = handlers.erase(iterator);
    }
  }

  /**
   * @brief Idempotently closes listener and interrupts active connections.
   * @throws Nothing.
   * @note Connection handlers retain close ownership of their descriptors.
   */
  void stop() noexcept {
    if (stopping.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    const int listener = listener_fd.exchange(-1, std::memory_order_acq_rel);
    if (listener >= 0) {
      shutdown_descriptor(listener);
      ::close(listener);
    }
    {
      std::lock_guard<std::mutex> lock(connections_mutex);
      for (const auto& entry : active_connections) {
        shutdown_descriptor(entry.first);
      }
    }
  }

  /**
   * @brief Joins every handler after their descriptors are interrupted.
   * @throws Nothing under the invariant that the caller is not a handler.
   * @note `run()` and destruction are the only callers.
   */
  void join_handlers() noexcept {
    std::vector<HandlerRecord> owned;
    {
      std::lock_guard<std::mutex> lock(handlers_mutex);
      owned.swap(handlers);
    }
    for (HandlerRecord& handler : owned) {
      if (handler.thread.joinable()) {
        handler.thread.join();
      }
    }
  }

  /** @brief Fixed server configuration and socket path. */
  ServerConfig config;
  /** @brief Empty-at-start in-memory orchestration service. */
  Service service;
  /** @brief Exact bound socket pathname-generation cleanup ownership. */
  SocketNodeGuard socket_node;
  /** @brief Listener descriptor atomically closed by stop. */
  std::atomic<int> listener_fd{-1};
  /** @brief Monotonic shutdown flag. */
  std::atomic<bool> stopping{false};
  /** @brief Serializes active descriptor inventory. */
  std::mutex connections_mutex;
  /** @brief Active descriptors used only for shutdown interruption. */
  std::map<int, int> active_connections;
  /** @brief Joinable per-connection handlers owned by the run thread. */
  std::vector<HandlerRecord> handlers;
  /** @brief Serializes handler record reaping/join observation. */
  mutable std::mutex handlers_mutex;
  /** @brief Current globally admitted connection-handler count. */
  std::atomic<std::uint32_t> active_handlers{0U};
};

/**
 * @brief Implements construction and generation-owned local listener binding.
 * @copydetails Server::Server
 */
Server::Server(ServerConfig config) {
  if (config.socket_path.empty() || config.listen_backlog <= 0 ||
      config.service.maximum_concurrency == 0U ||
      config.service.maximum_jobs == 0U ||
      config.service.maximum_sessions == 0U ||
      config.maximum_active_connections == 0U ||
      config.maximum_active_connections > 4096U) {
    throw std::invalid_argument("local server configuration is invalid");
  }
  const Status path_status = validate_unix_socket_path(config.socket_path);
  if (!path_status.ok()) {
    throw std::invalid_argument(path_status.message);
  }
  auto listener =
      create_unix_listener(config.socket_path, config.listen_backlog);
  if (!listener.ok()) {
    throw std::runtime_error(listener.status().message);
  }
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
  if (config.construction_hook) {
    config.construction_hook(ServerConstructionStage::AfterListenerBind);
  }
#endif
  impl_ = std::make_unique<Impl>(std::move(config), listener.take_value());
}

/**
 * @brief Implements complete local server teardown.
 * @copydetails Server::~Server
 */
Server::~Server() noexcept = default;

/**
 * @brief Implements the blocking same-user accept loop.
 * @copydetails Server::run
 */
Status Server::run() {
  if (!impl_) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "local server has no runtime state");
  }
  if (impl_->stopping.load(std::memory_order_acquire)) {
    return Status::success();
  }
  Status outcome = Status::success();
  while (!impl_->stopping.load(std::memory_order_acquire)) {
    const int listener = impl_->listener_fd.load(std::memory_order_acquire);
    if (listener < 0) {
      break;
    }
    auto connection = accept_same_user(listener);
    if (impl_->stopping.load(std::memory_order_acquire)) {
      break;
    }
    if (connection.disposition == AcceptDisposition::PeerRejected) {
      continue;
    }
    if (connection.disposition == AcceptDisposition::FatalFailure) {
      outcome = std::move(connection.status);
      impl_->stop();
      break;
    }
    impl_->reap_finished_handlers();
    if (impl_->active_handlers.load(std::memory_order_acquire) >=
        impl_->config.maximum_active_connections) {
      write_protocol_failure(
          connection.descriptor.get(),
          Status::failure(ErrorCode::ResourceExhausted,
                          "active connection capacity is exhausted"));
      continue;
    }
    bool slot_reserved = false;
    try {
      auto finished = std::make_shared<std::atomic<bool>>(false);
      impl_->active_handlers.fetch_add(1U, std::memory_order_acq_rel);
      slot_reserved = true;
      std::lock_guard<std::mutex> lock(impl_->handlers_mutex);
      impl_->handlers.push_back(
          HandlerRecord{std::thread(), std::move(finished)});
      try {
        HandlerRecord& record = impl_->handlers.back();
        record.thread =
            std::thread([state = impl_.get(), finished = record.finished,
                         owned = std::move(connection.descriptor)]() mutable {
              HandlerCompletion completion(&state->active_handlers, finished);
              state->serve_connection(std::move(owned));
            });
      } catch (...) {
        impl_->handlers.pop_back();
        throw;
      }
    } catch (...) {
      if (slot_reserved) {
        impl_->active_handlers.fetch_sub(1U, std::memory_order_acq_rel);
      }
      impl_->stop();
      impl_->join_handlers();
      impl_->socket_node.remove();
      throw;
    }
  }
  impl_->stop();
  impl_->join_handlers();
  impl_->socket_node.remove();
  return outcome;
}

/**
 * @brief Implements idempotent server interruption.
 * @copydetails Server::request_stop
 */
void Server::request_stop() noexcept {
  if (impl_) {
    impl_->stop();
  }
}

/**
 * @brief Implements bound socket-path observation.
 * @copydetails Server::socket_path
 */
const std::string& Server::socket_path() const noexcept {
  return impl_->config.socket_path;
}

#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
/**
 * @brief Implements active connection-handler count observation.
 * @copydetails Server::active_handler_count
 */
std::uint32_t Server::active_handler_count() const noexcept {
  return impl_ ? impl_->active_handlers.load(std::memory_order_acquire) : 0U;
}

/**
 * @brief Implements retained joinable handler-record observation.
 * @copydetails Server::retained_handler_count
 */
std::size_t Server::retained_handler_count() const noexcept {
  if (!impl_) {
    return 0U;
  }
  std::lock_guard<std::mutex> lock(impl_->handlers_mutex);
  return impl_->handlers.size();
}

/**
 * @brief Implements private active-descriptor observation for fault tests.
 * @copydetails Server::active_connection_count_for_test
 */
std::size_t Server::active_connection_count_for_test() const noexcept {
  if (!impl_) {
    return 0U;
  }
  try {
    std::lock_guard<std::mutex> lock(impl_->connections_mutex);
    return impl_->active_connections.size();
  } catch (...) {
    return static_cast<std::size_t>(-1);
  }
}
#endif

}  // namespace ps::ipc::internal
