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
        listener_fd(listener.release()) {}

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
   * @note Malformed input closes only this connection; a successful shutdown
   * request also interrupts the listener and peer handlers.
   */
  void serve_connection(UniqueDescriptor connection) noexcept {
    const int descriptor = connection.get();
    try {
      std::lock_guard<std::mutex> lock(connections_mutex);
      active_connections.emplace(descriptor, descriptor);
    } catch (...) {
      return;
    }
    while (!stopping.load(std::memory_order_acquire)) {
      auto frame = read_frame(descriptor);
      if (!frame.ok()) {
        break;
      }
      auto request = decode_request(frame.value());
      if (!request.ok()) {
        break;
      }
      Response response = service.dispatch(request.value());
      auto encoded = encode_response(response);
      if (!encoded.ok() || !write_frame(descriptor, encoded.value()).ok()) {
        break;
      }
      if (response.shutdown_after_write) {
        stop();
        break;
      }
    }
    std::lock_guard<std::mutex> lock(connections_mutex);
    active_connections.erase(descriptor);
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
    for (std::thread& handler : handlers) {
      if (handler.joinable()) {
        handler.join();
      }
    }
    handlers.clear();
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
  std::vector<std::thread> handlers;
};

/**
 * @brief Implements construction and restricted local listener binding.
 * @copydetails Server::Server
 */
Server::Server(ServerConfig config) {
  if (config.socket_path.empty() || config.listen_backlog <= 0 ||
      config.service.maximum_concurrency == 0U ||
      config.service.maximum_jobs == 0U) {
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
    try {
      impl_->handlers.emplace_back(
          [state = impl_.get(), owned = connection.take_value()]() mutable {
            state->serve_connection(std::move(owned));
          });
    } catch (...) {
      impl_->stop();
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

}  // namespace ps::ipc::internal
