#pragma once

#include <cstddef>
#include <cstdint>
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
#include <functional>
#endif
#include <memory>
#include <string>

#include "orchestration/service.hpp"

namespace ps::ipc::internal {

#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
/**
 * @brief Deterministic private construction stages exposed to lifecycle tests.
 *
 * @note Hooks observe only bound-listener publication boundaries and do not
 * become a public daemon configuration surface.
 */
enum class ServerConstructionStage : std::uint32_t {
  /** @brief Listener bind succeeded but private state is not allocated. */
  AfterListenerBind = 1U,
  /** @brief Service exists but handler storage/listener transfer is pending. */
  BeforeHandlerStorage = 2U,
};
#endif

/**
 * @brief Fixed local server transport and orchestration configuration.
 * @note Connection and service bounds are positive process-global local
 * controls, not tenant quotas or remote admission policy.
 */
struct ServerConfig final {
  /** @brief Explicit Unix-domain socket filesystem path. */
  std::string socket_path;
  /** @brief Process-local namespace/execution resources. */
  ServiceConfig service;
  /** @brief Positive pending local connection bound. */
  int listen_backlog = 32;
  /** @brief Positive global active connection/handler bound. */
  std::uint32_t maximum_active_connections = 64U;
#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
  /**
   * @brief Optional private handler-entry observer/fault-injection callback.
   *
   * The callback runs after descriptor registration and before frame reads.
   * It must be thread-safe when multiple handlers enter concurrently.
   * Exceptions are fenced into one typed connection failure and cleanup.
   * This field is visible only in the noninstalled test runtime and remains
   * empty when a test does not inject a handler-entry observation or fault.
   */
  std::function<void()> handler_entry_hook;
  /**
   * @brief Optional private construction observer/fault-injection callback.
   *
   * The callback runs synchronously on the constructing thread at a named
   * post-bind stage. Exceptions must leave no descriptor or socket node.
   * This field is visible only in the noninstalled test runtime and remains
   * empty when a test does not inject a construction observation or fault.
   */
  std::function<void(ServerConstructionStage)> construction_hook;
#endif
};

/**
 * @brief Same-user Unix-domain IPC server for the exact v3 method surface.
 *
 * @note Construction rejects every existing pathname and binds only an absent
 * entry. `run` blocks in accept until shutdown; destruction closes
 * connections, joins handlers, and removes the node only while its captured
 * device/inode generation still matches.
 */
class Server final {
 public:
  /**
   * @brief Creates service state and binds a generation-owned local listener.
   * @param config Exact socket and positive resource bounds.
   * @throws std::invalid_argument For invalid configuration.
   * @throws std::runtime_error If the local listener cannot be created.
   * @throws std::bad_alloc If service or transport state allocation fails.
   * @throws std::system_error If bounded worker creation fails.
   * @throws Any other exception raised by a noninstalled test-runtime
   * construction hook when that private variant is enabled.
   * @note Any failure after bind conditionally removes only the captured
   * socket generation before propagating. A pre-existing entry is never
   * reclaimed, including a stale socket.
   */
  explicit Server(ServerConfig config);

  /**
   * @brief Requests stop, joins handlers, and removes the socket node.
   * @throws Nothing.
   * @note Destruction cancels all remaining ephemeral service state.
   */
  ~Server() noexcept;

  /**
   * @brief Forbids copying listener, handler, and service ownership.
   * @param other Source server that cannot be copied.
   * @throws Nothing; the operation is deleted.
   * @note One bound socket path has exactly one owning server.
   */
  Server(const Server& other) = delete;
  /**
   * @brief Forbids assigning active listener/handler ownership.
   * @param other Source server that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   * @note Shutdown and join remain bound to the constructing object.
   */
  Server& operator=(const Server& other) = delete;

  /**
   * @brief Accepts same-user connections until shutdown.
   * @return Success after requested shutdown or typed unexpected accept
   * failure.
   * @throws std::bad_alloc If handler ownership allocation fails.
   * @throws std::system_error If a connection handler cannot be started.
   * @note Call exactly once from one thread. A peer uid mismatch closes only
   * that accepted stream and admission continues. Fatal accept, stream
   * preparation, or credential syscall failure stops admission unless
   * `request_stop()` already established normal shutdown. Handler-creation
   * failure joins every already-owned handler, removes the socket node, and
   * then propagates.
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

#if defined(PHOTOSPIDER_DAEMON_TEST_RUNTIME)
  /**
   * @brief Returns the current active connection-handler count.
   * @return Exact atomic count at the observation instant.
   * @throws Nothing.
   * @note The value may change immediately after return and exists for
   * lifecycle diagnostics/tests, not public package transport control.
   */
  [[nodiscard]] std::uint32_t active_handler_count() const noexcept;

  /**
   * @brief Returns handler thread records still owned before runtime reaping.
   * @return Current bounded retained record count.
   * @throws Nothing.
   * @note Finished records are joined and erased by the accept loop.
   */
  [[nodiscard]] std::size_t retained_handler_count() const noexcept;

  /**
   * @brief Returns the private active-descriptor registry size for tests.
   * @return Exact registered descriptor count, or maximum size on lock
   * failure.
   * @throws Nothing.
   * @note This method exists only in the noninstalled test-runtime variant.
   */
  [[nodiscard]] std::size_t active_connection_count_for_test() const noexcept;
#endif

 private:
  /** @brief Opaque listener, connection handlers, and service. */
  struct Impl;
  /** @brief Unique server state. */
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps::ipc::internal
