#include <pthread.h>
#include <signal.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include "server/server.hpp"

namespace {

/**
 * @brief Complete parsed local daemon command-line configuration.
 * @note Every numeric field is positive before server construction; bounds are
 * process-global local controls, not tenant policy.
 */
struct Options final {
  /** @brief Explicit Unix-domain socket path. */
  std::string socket_path;
  /** @brief Fixed global worker count. */
  std::uint32_t maximum_concurrency = 1U;
  /** @brief Fixed retained execution bound. */
  std::uint32_t maximum_jobs = 1024U;
  /** @brief Fixed retained logical namespace bound. */
  std::uint32_t maximum_sessions = 128U;
  /** @brief Fixed active local connection-handler bound. */
  std::uint32_t maximum_connections = 64U;
  /** @brief Whether the optional local GPU lane is configured. */
  bool gpu_enabled = false;
  /** @brief Whether help was requested. */
  bool help = false;
};

/**
 * @brief Prints the exact local-only daemon command surface.
 * @throws std::ios_base::failure If configured stream exceptions are enabled.
 * @note No network, policy, plugin-path, or persistence option is advertised.
 */
void print_help() {
  std::cout
      << "Usage: photospiderd --socket PATH [options]\n"
      << "\n"
      << "Options:\n"
      << "  --socket PATH          Unix-domain socket path (required)\n"
      << "  --max-concurrency N    Global execution worker count (default 1)\n"
      << "  --max-jobs N           Retained ephemeral execution bound (default "
         "1024)\n"
      << "  --max-sessions N       Retained Session bound (default 128)\n"
      << "  --max-connections N    Active connection bound (default 64)\n"
      << "  --gpu                   Enable the optional local GPU lane\n"
      << "  --help                  Show this help\n";
}

/**
 * @brief Parses one positive uint32 command argument.
 * @param text Candidate decimal text.
 * @param option Option name used in diagnostics.
 * @return Positive uint32 value.
 * @throws std::invalid_argument If syntax/range is invalid.
 * @throws std::out_of_range If decimal conversion exceeds uint64.
 * @throws std::bad_alloc If conversion/diagnostic allocation fails.
 * @note Zero is rejected for every maintained resource bound.
 */
std::uint32_t positive_uint32(const std::string& text,
                              const std::string& option) {
  std::size_t consumed = 0U;
  const std::uint64_t value = std::stoull(text, &consumed, 10);
  if (consumed != text.size() || value == 0U ||
      value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument(option + " requires a positive uint32");
  }
  return static_cast<std::uint32_t>(value);
}

/**
 * @brief Parses the complete bounded local daemon command line.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Complete options.
 * @throws std::invalid_argument For unknown, missing, or malformed arguments.
 * @throws std::out_of_range If one numeric argument exceeds uint64.
 * @throws std::bad_alloc If argument storage allocation fails.
 * @note Parsing has no filesystem or daemon side effect.
 */
Options parse_options(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help") {
      options.help = true;
    } else if (argument == "--gpu") {
      options.gpu_enabled = true;
    } else if (argument == "--socket" || argument == "--max-concurrency" ||
               argument == "--max-jobs" || argument == "--max-sessions" ||
               argument == "--max-connections") {
      if (index + 1 >= argc) {
        throw std::invalid_argument(argument + " requires a value");
      }
      const std::string value = argv[++index];
      if (argument == "--socket") {
        options.socket_path = value;
      } else if (argument == "--max-concurrency") {
        options.maximum_concurrency = positive_uint32(value, argument);
      } else if (argument == "--max-jobs") {
        options.maximum_jobs = positive_uint32(value, argument);
      } else if (argument == "--max-sessions") {
        options.maximum_sessions = positive_uint32(value, argument);
      } else {
        options.maximum_connections = positive_uint32(value, argument);
      }
    } else {
      throw std::invalid_argument("unknown option: " + argument);
    }
  }
  if (!options.help && options.socket_path.empty()) {
    throw std::invalid_argument("--socket PATH is required");
  }
  return options;
}

/**
 * @brief Blocks daemon-stop signals and restores the caller's prior mask.
 *
 * @note Construct before Server so every worker/handler thread inherits
 * blocked `SIGINT`/`SIGTERM` plus the waiter-only `SIGUSR1` wake signal;
 * restore only after all such threads are gone.
 */
class SignalMaskGuard final {
 public:
  /**
   * @brief Blocks stop signals and the private synchronous-wait wake signal.
   * @throws std::system_error If signal-set construction or mask change fails.
   * @note The exact previous mask is retained for later restoration.
   */
  SignalMaskGuard() {
    if (sigemptyset(&signals_) != 0 || sigaddset(&signals_, SIGINT) != 0 ||
        sigaddset(&signals_, SIGTERM) != 0 ||
        sigaddset(&signals_, SIGUSR1) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "could not construct daemon signal set");
    }
    const int result = ::pthread_sigmask(SIG_BLOCK, &signals_, &previous_mask_);
    if (result != 0) {
      throw std::system_error(result, std::generic_category(),
                              "could not block daemon shutdown signals");
    }
    active_ = true;
  }

  /**
   * @brief Restores the exact previous mask on unhandled exit paths.
   * @throws Nothing.
   * @note Normal flow calls `restore()` so a restoration error is reported.
   */
  ~SignalMaskGuard() noexcept { restore_noexcept(); }

  /**
   * @brief Forbids duplicate ownership of one thread's prior signal mask.
   * @param other Source guard that cannot be copied.
   * @throws Nothing; the operation is deleted.
   */
  SignalMaskGuard(const SignalMaskGuard& other) = delete;
  /**
   * @brief Forbids assigning duplicate signal-mask restoration ownership.
   * @param other Source guard that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  SignalMaskGuard& operator=(const SignalMaskGuard& other) = delete;
  /**
   * @brief Forbids moving thread-affine signal-mask ownership.
   * @param other Source guard that cannot be moved.
   * @throws Nothing; the operation is deleted.
   */
  SignalMaskGuard(SignalMaskGuard&& other) = delete;
  /**
   * @brief Forbids move assignment of thread-affine signal-mask ownership.
   * @param other Source guard that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  SignalMaskGuard& operator=(SignalMaskGuard&& other) = delete;

  /**
   * @brief Returns the immutable blocked shutdown-signal set.
   * @return Borrowed `sigset_t` valid for this guard's lifetime.
   * @throws Nothing.
   */
  const sigset_t& signals() const noexcept { return signals_; }

  /**
   * @brief Drains pending managed signals and restores the previous mask.
   * @throws std::system_error If draining or mask restoration fails.
   * @note Call on the constructing thread after the waiter, Server handlers,
   * and service workers have joined. Repeated calls are no-ops.
   */
  void restore() {
    if (!active_) {
      return;
    }
    for (;;) {
      sigset_t pending{};
      if (sigpending(&pending) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "could not inspect pending daemon signals");
      }
      const int has_sigint = sigismember(&pending, SIGINT);
      const int has_sigterm = sigismember(&pending, SIGTERM);
      const int has_wake = sigismember(&pending, SIGUSR1);
      if (has_sigint < 0 || has_sigterm < 0 || has_wake < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "could not classify pending daemon signals");
      }
      if (has_sigint == 0 && has_sigterm == 0 && has_wake == 0) {
        break;
      }
      int signal_number = 0;
      const int result = sigwait(&signals_, &signal_number);
      if (result != 0) {
        throw std::system_error(result, std::generic_category(),
                                "could not drain daemon shutdown signals");
      }
    }
    const int result = ::pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr);
    if (result != 0) {
      throw std::system_error(result, std::generic_category(),
                              "could not restore process signal mask");
    }
    active_ = false;
  }

 private:
  /**
   * @brief Best-effort prior-mask restoration during stack unwinding.
   * @throws Nothing.
   * @note The POSIX error cannot be surfaced from a destructor; normal flow
   * uses `restore()` before leaving main.
   */
  void restore_noexcept() noexcept {
    if (active_) {
      static_cast<void>(
          ::pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr));
      active_ = false;
    }
  }

  /** @brief Blocked stop/internal-wake set shared with the waiter. */
  sigset_t signals_{};
  /** @brief Exact caller mask captured before blocking. */
  sigset_t previous_mask_{};
  /** @brief Whether prior-mask restoration remains owned. */
  bool active_ = false;
};

/**
 * @brief Synchronously consumes POSIX stop signals on one dedicated thread.
 *
 * @note The waiter never runs C++ cleanup in an asynchronous handler. It only
 * invokes thread-safe `Server::request_stop`; `SIGUSR1` is a directed normal-
 * completion wake. The main/server owners join and destroy resources through
 * ordinary control flow.
 */
class SignalStopWaiter final {
 public:
  /**
   * @brief Starts one bounded synchronous signal-wait loop.
   * @param server Nonnull Server that outlives this waiter.
   * @param signals Blocked set containing stop signals and `SIGUSR1` wake.
   * @throws std::invalid_argument If `server` is null.
   * @throws std::system_error If the waiter thread cannot start.
   * @note All process threads must already block the supplied signals.
   */
  SignalStopWaiter(ps::ipc::internal::Server* server, const sigset_t& signals)
      : server_(server), signals_(signals) {
    if (!server_) {
      throw std::invalid_argument("signal waiter requires a server");
    }
    thread_ = std::thread([this] { wait_loop(); });
  }

  /**
   * @brief Marks normal completion and joins the bounded wait loop.
   * @throws Nothing under the invariant that destruction is not on the waiter.
   * @note Main normally calls `finish()` first so wait errors are reported.
   */
  ~SignalStopWaiter() noexcept { finish_noexcept(); }

  /**
   * @brief Forbids duplicate waiter/thread ownership.
   * @param other Source waiter that cannot be copied.
   * @throws Nothing; the operation is deleted.
   */
  SignalStopWaiter(const SignalStopWaiter& other) = delete;
  /**
   * @brief Forbids assigning duplicate waiter/thread ownership.
   * @param other Source waiter that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  SignalStopWaiter& operator=(const SignalStopWaiter& other) = delete;
  /**
   * @brief Forbids moving a waiter whose thread captures its address.
   * @param other Source waiter that cannot be moved.
   * @throws Nothing; the operation is deleted.
   */
  SignalStopWaiter(SignalStopWaiter&& other) = delete;
  /**
   * @brief Forbids move assignment of captured waiter state.
   * @param other Source waiter that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  SignalStopWaiter& operator=(SignalStopWaiter&& other) = delete;

  /**
   * @brief Signals normal completion, joins, and reports wait-loop failure.
   * @throws std::system_error If `sigwait` or directed wake failed.
   * @note Ordinary `daemon.shutdown` sets completion, directs `SIGUSR1` only
   * to this waiter, and joins without polling or a process-global handler.
   */
  void finish() {
    complete_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
      const int wake_result = ::pthread_kill(thread_.native_handle(), SIGUSR1);
      if (wake_result != 0 && wake_result != ESRCH) {
        error_.store(wake_result, std::memory_order_release);
      }
      thread_.join();
    }
    const int error = error_.load(std::memory_order_acquire);
    if (error != 0) {
      throw std::system_error(error, std::generic_category(),
                              "daemon signal wait failed");
    }
  }

 private:
  /**
   * @brief Waits synchronously for a managed signal or directed completion.
   * @throws Nothing across the dedicated thread boundary.
   * @note Unexpected wait failure requests stop so the main accept loop cannot
   * remain blocked while `finish()` awaits this thread.
   */
  void wait_loop() noexcept {
    for (;;) {
      int signal_number = 0;
      const int result = sigwait(&signals_, &signal_number);
      if (result != 0) {
        error_.store(result, std::memory_order_release);
        server_->request_stop();
        return;
      }
      if (signal_number == SIGINT || signal_number == SIGTERM) {
        server_->request_stop();
        return;
      }
      if (signal_number == SIGUSR1 &&
          complete_.load(std::memory_order_acquire)) {
        return;
      }
    }
  }

  /**
   * @brief Completes and joins during ordinary destruction/unwinding.
   * @throws Nothing under the non-self-join lifetime invariant.
   */
  void finish_noexcept() noexcept {
    complete_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
      static_cast<void>(::pthread_kill(thread_.native_handle(), SIGUSR1));
      thread_.join();
    }
  }

  /** @brief Main-owned server stopped but never destroyed by this waiter. */
  ps::ipc::internal::Server* server_ = nullptr;
  /** @brief Copied blocked shutdown-signal set. */
  sigset_t signals_{};
  /** @brief Normal completion flag observed between bounded waits. */
  std::atomic<bool> complete_{false};
  /** @brief Unexpected synchronous wait error, or zero. */
  std::atomic<int> error_{0};
  /** @brief Exactly one owned synchronous waiter thread. */
  std::thread thread_;
};

}  // namespace

/**
 * @brief Runs the non-persistent same-user local orchestration daemon.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Zero after graceful shutdown, two for usage errors, or one for
 * listener/runtime failure.
 * @throws Nothing for standard exceptions; they are reported and mapped to a
 * nonzero process result.
 * @note The daemon never backgrounds itself or persists process state.
 */
int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    if (options.help) {
      print_help();
      return 0;
    }
    SignalMaskGuard signal_mask;
    ps::Status status;
    {
      ps::ipc::internal::Server server(ps::ipc::internal::ServerConfig{
          options.socket_path,
          ps::ipc::internal::ServiceConfig{
              options.maximum_concurrency, options.maximum_jobs,
              options.maximum_sessions, options.gpu_enabled},
          32, options.maximum_connections});
      SignalStopWaiter signal_waiter(&server, signal_mask.signals());
      status = server.run();
      signal_waiter.finish();
    }
    signal_mask.restore();
    if (!status.ok()) {
      std::cerr << status.message << '\n';
      return 1;
    }
    return 0;
  } catch (const std::invalid_argument& error) {
    std::cerr << error.what() << '\n';
    print_help();
    return 2;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
