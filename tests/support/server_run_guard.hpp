#pragma once

#include <future>
#include <stdexcept>
#include <utility>

#include "server/server.hpp"

namespace ps::ipc::test {

/**
 * @brief Owns one asynchronous Server::run call with fail-safe test teardown.
 *
 * @note Declare after the referenced Server. Any assertion return or exception
 * then destroys this guard first, requests stop, and joins before Server state
 * can be destroyed or an async future destructor can wait indefinitely.
 */
class ServerRunGuard final {
 public:
  /**
   * @brief Starts exactly one asynchronous blocking accept loop.
   * @param server Nonnull bound Server that outlives this guard.
   * @throws std::invalid_argument If `server` is null.
   * @throws std::bad_alloc If asynchronous state allocation fails.
   * @throws std::system_error If the test thread cannot start.
   * @note No automatic request_stop occurs until teardown or explicit join.
   */
  explicit ServerRunGuard(internal::Server* server) : server_(server) {
    if (!server_) {
      throw std::invalid_argument("server run guard requires a server");
    }
    future_ =
        std::async(std::launch::async, [server] { return server->run(); });
  }

  /**
   * @brief Requests stop and joins only when normal code did not already join.
   * @throws Nothing.
   * @note Exceptions from Server::run are swallowed only during assertion or
   * exception unwinding; normal code calls `join()` to observe them.
   */
  ~ServerRunGuard() noexcept { stop_and_join_noexcept(); }

  /**
   * @brief Forbids duplicate asynchronous run ownership.
   * @param other Source guard that cannot be copied.
   * @throws Nothing; the operation is deleted.
   */
  ServerRunGuard(const ServerRunGuard& other) = delete;
  /**
   * @brief Forbids assigning duplicate asynchronous run ownership.
   * @param other Source guard that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  ServerRunGuard& operator=(const ServerRunGuard& other) = delete;
  /**
   * @brief Forbids moving state captured by the asynchronous test thread.
   * @param other Source guard that cannot be moved.
   * @throws Nothing; the operation is deleted.
   */
  ServerRunGuard(ServerRunGuard&& other) = delete;
  /**
   * @brief Forbids move assignment of captured asynchronous state.
   * @param other Source guard that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  ServerRunGuard& operator=(ServerRunGuard&& other) = delete;

  /**
   * @brief Joins a Server already stopped by the behavior under test.
   * @return Immutable run status retained by this guard.
   * @throws Any exception propagated by Server::run or future retrieval.
   * @note This method deliberately does not request stop, so an RPC shutdown
   * regression cannot be masked by the test guard. Repeated calls reuse the
   * first outcome.
   */
  const Status& join() {
    if (!joined_) {
      try {
        outcome_ = future_.get();
      } catch (...) {
        joined_ = true;
        throw;
      }
      joined_ = true;
    }
    return outcome_;
  }

 private:
  /**
   * @brief Stops and joins an unfinished run during fail-safe teardown.
   * @throws Nothing.
   * @note Server::request_stop is idempotent and future exceptions are ignored
   * only because the original assertion/exception is already authoritative.
   */
  void stop_and_join_noexcept() noexcept {
    if (joined_) {
      return;
    }
    server_->request_stop();
    try {
      if (future_.valid()) {
        outcome_ = future_.get();
      }
    } catch (...) {
    }
    joined_ = true;
  }

  /** @brief Borrowed Server that outlives this guard. */
  internal::Server* server_ = nullptr;
  /** @brief Exactly one asynchronous Server::run result. */
  std::future<Status> future_;
  /** @brief Cached normal run outcome. */
  Status outcome_;
  /** @brief Whether the future has already been consumed. */
  bool joined_ = false;
};

}  // namespace ps::ipc::test
