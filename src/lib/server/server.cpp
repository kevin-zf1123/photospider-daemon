#include "server/server.hpp"

#include <unistd.h>

#include <atomic>
#include <cerrno>
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

namespace ps::ipc::internal {
namespace {

/**
 * @brief Sends one best-effort typed pre-routing failure response.
 * @param descriptor Connected local stream descriptor.
 * @param status Required non-success protocol/backpressure status.
 * @param request_payload Optional malformed payload for correlation recovery.
 * @throws Nothing.
 * @note Encoding/writing failure simply leaves connection teardown to owner.
 */
void write_protocol_failure(
    int descriptor, const Status& status,
    const std::vector<std::uint8_t>* request_payload = nullptr) noexcept {
  try {
    auto encoded = encode_protocol_error(status, request_payload);
    if (encoded.ok()) {
      static_cast<void>(write_frame(descriptor, encoded.value()));
    }
  } catch (...) {
  }
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
   * @param listener Bound restricted listener descriptor.
   * @throws std::bad_alloc If service or configuration allocation fails.
   * @throws std::system_error If service worker creation fails.
   * @note The raw listener descriptor is thereafter closed only by `stop()`.
   */
  Impl(ServerConfig config, UniqueDescriptor listener)
      : config(std::move(config)),
        service(this->config.service),
        listener_fd(listener.release()) {
    handlers.reserve(this->config.maximum_active_connections);
  }

  /**
   * @brief Closes all transport state and joins every handler.
   * @throws Nothing.
   * @note The socket node is removed only when it is still a socket.
   */
  ~Impl() noexcept {
    stop();
    join_handlers();
    remove_socket_node(config.socket_path);
  }

  /**
   * @brief Serves one sequential connection until EOF, failure, or shutdown.
   * @param connection Owned same-user stream descriptor.
   * @throws Nothing across the handler-thread boundary.
   * @note Malformed input receives one typed error then closes this connection;
   * a successful shutdown request also interrupts listener and peer handlers.
   */
  void serve_connection(UniqueDescriptor connection) noexcept {
    const int descriptor = connection.get();
    bool registered = false;
    try {
      std::lock_guard<std::mutex> lock(connections_mutex);
      active_connections.emplace(descriptor, descriptor);
      registered = true;
    } catch (...) {
      return;
    }
    try {
      if (config.handler_entry_hook) {
        config.handler_entry_hook();
      }
      while (!stopping.load(std::memory_order_acquire)) {
        auto frame = read_frame(descriptor);
        if (!frame.ok()) {
          if (frame.status().code != ErrorCode::NotFound) {
            write_protocol_failure(descriptor, frame.status());
          }
          break;
        }
        auto request = decode_request(frame.value());
        if (!request.ok()) {
          write_protocol_failure(descriptor, request.status(), &frame.value());
          break;
        }
        Response response = service.dispatch(request.value());
        auto encoded = encode_response(response);
        if (!encoded.ok()) {
          write_protocol_failure(descriptor, encoded.status(), &frame.value());
          break;
        }
        if (!write_frame(descriptor, encoded.value()).ok()) {
          break;
        }
        if (response.shutdown_after_write) {
          stop();
          break;
        }
      }
    } catch (const std::bad_alloc&) {
      write_protocol_failure(
          descriptor, Status::failure(ErrorCode::ResourceExhausted,
                                      "connection handler allocation failed"));
    } catch (const std::exception& error) {
      write_protocol_failure(
          descriptor, Status::failure(ErrorCode::Internal, error.what()));
    } catch (...) {
      write_protocol_failure(
          descriptor,
          Status::failure(ErrorCode::Internal,
                          "connection handler raised an exception"));
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
 * @brief Implements construction and restricted local listener binding.
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
  auto listener =
      create_unix_listener(config.socket_path, config.listen_backlog);
  if (!listener.ok()) {
    throw std::runtime_error(listener.status().message);
  }
  const std::string socket_path = config.socket_path;
  try {
    impl_ = std::make_unique<Impl>(std::move(config), listener.take_value());
  } catch (...) {
    remove_socket_node(socket_path);
    throw;
  }
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
  if (!impl_ || impl_->stopping.load(std::memory_order_acquire)) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "local server is already stopped");
  }
  Status outcome = Status::success();
  while (!impl_->stopping.load(std::memory_order_acquire)) {
    const int listener = impl_->listener_fd.load(std::memory_order_acquire);
    if (listener < 0) {
      break;
    }
    auto connection = accept_same_user(listener);
    if (!connection.ok()) {
      if (impl_->stopping.load(std::memory_order_acquire) || errno == EBADF ||
          errno == EINVAL) {
        break;
      }
      outcome = connection.status();
      impl_->stop();
      break;
    }
    impl_->reap_finished_handlers();
    if (impl_->active_handlers.load(std::memory_order_acquire) >=
        impl_->config.maximum_active_connections) {
      write_protocol_failure(
          connection.value().get(),
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
                         owned = connection.take_value()]() mutable {
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
      remove_socket_node(impl_->config.socket_path);
      throw;
    }
  }
  impl_->stop();
  impl_->join_handlers();
  remove_socket_node(impl_->config.socket_path);
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

}  // namespace ps::ipc::internal
