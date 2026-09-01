#pragma once

#include <memory>
#include <string>

#include "orchestration/service.hpp"

namespace ps::ipc::internal {

/** @brief Fixed local server transport and orchestration configuration. */
struct ServerConfig final {
  /** @brief Explicit Unix-domain socket filesystem path. */
  std::string socket_path;
  /** @brief Process-local namespace/execution resources. */
  ServiceConfig service;
  /** @brief Positive pending local connection bound. */
  int listen_backlog = 32;
};

/**
 * @brief Same-user Unix-domain IPC server for the exact v3 method surface.
 *
 * @note Construction binds the socket. `run` blocks in accept until shutdown;
 * destruction closes connections, joins handlers, and removes the socket node.
 */
class Server final {
 public:
  /**
   * @brief Creates service state and binds a restricted local listener.
   * @param config Exact socket and positive resource bounds.
   * @throws std::invalid_argument For invalid configuration.
   * @throws std::runtime_error If the local listener cannot be created.
   * @throws std::bad_alloc If service or transport state allocation fails.
   * @throws std::system_error If bounded worker creation fails.
   * @note Any failure after bind removes the socket node before propagating.
   */
  explicit Server(ServerConfig config);

  /**
   * @brief Requests stop, joins handlers, and removes the socket node.
   * @throws Nothing.
   * @note Destruction cancels all remaining ephemeral service state.
   */
  ~Server() noexcept;

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  /**
   * @brief Accepts same-user connections until shutdown.
   * @return Success after requested shutdown or typed unexpected accept
   * failure.
   * @throws std::bad_alloc If handler ownership allocation fails.
   * @throws std::system_error If a connection handler cannot be started.
   * @note Call exactly once from one thread.
   */
  [[nodiscard]] Status run();

  /**
   * @brief Idempotently interrupts listener and every active connection.
   * @throws Nothing.
   * @note Running kernel executions receive cancellation during service
   * teardown.
   */
  void request_stop() noexcept;

  /**
   * @brief Returns the exact bound socket path.
   * @return Immutable borrowed path owned by this server.
   * @throws Nothing.
   * @note The reference remains valid for the server object's lifetime.
   */
  [[nodiscard]] const std::string& socket_path() const noexcept;

 private:
  /** @brief Opaque listener, connection handlers, and service. */
  struct Impl;
  /** @brief Unique server state. */
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps::ipc::internal
