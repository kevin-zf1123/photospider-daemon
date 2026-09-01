#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "photospider/ipc/client.hpp"
#include "support/test_support.hpp"

#ifndef PS_PHOTOSPIDERD_PATH
#error "PS_PHOTOSPIDERD_PATH must name the real daemon executable"
#endif

namespace {

/** @brief Poll interval for child, socket, and public Job observations. */
constexpr std::chrono::milliseconds kPollInterval{5};

/** @brief Deadline for a daemon to accept a correlated public request. */
constexpr std::chrono::seconds kStartupTimeout{4};

/** @brief Deadline for observing the delayed Job's Running state. */
constexpr std::chrono::seconds kRunningTimeout{4};

/** @brief Hard upper bound for every graceful child shutdown. */
constexpr std::chrono::seconds kShutdownTimeout{3};

/** @brief Delay long enough to distinguish cancellation from natural drain. */
constexpr std::int64_t kDelayedJobMilliseconds = 5000;

/** @brief Maximum accepted signal-shutdown time for the delayed Job. */
constexpr std::chrono::seconds kDelayedShutdownBound{2};

/**
 * @brief Exact ownership and bounded cleanup for one fork/exec daemon child.
 *
 * @note Destruction kills and reaps only a child not already observed exiting,
 * then removes test-owned crash residue so failures remain isolated.
 */
class ChildProcess final {
 public:
  /**
   * @brief Takes ownership of one positive child pid and test socket path.
   * @param pid Child process identifier.
   * @param socket_path Exact child listener path.
   * @throws std::bad_alloc If path ownership allocation fails.
   * @note The constructor performs no wait or signal operation.
   */
  ChildProcess(pid_t pid, std::string socket_path)
      : pid_(pid), socket_path_(std::move(socket_path)) {
    if (pid_ <= 0) {
      throw std::invalid_argument("child pid must be positive");
    }
  }

  /**
   * @brief Reaps or forcefully settles the owned child exactly once.
   * @throws Nothing.
   * @note `SIGKILL` is test-failure cleanup only and never the behavior under
   * assertion.
   */
  ~ChildProcess() noexcept { settle_noexcept(); }

  /**
   * @brief Forbids duplicate child ownership.
   * @param other Source owner that cannot be copied.
   * @throws Nothing; the operation is deleted.
   */
  ChildProcess(const ChildProcess& other) = delete;
  /**
   * @brief Forbids assigning duplicate child ownership.
   * @param other Source owner that cannot be assigned.
   * @return No value; the operation is deleted.
   * @throws Nothing; the operation is deleted.
   */
  ChildProcess& operator=(const ChildProcess& other) = delete;
  /**
   * @brief Transfers child cleanup ownership.
   * @param other Source left without a live child.
   * @throws Nothing.
   */
  ChildProcess(ChildProcess&& other) noexcept
      : pid_(std::exchange(other.pid_, -1)),
        socket_path_(std::move(other.socket_path_)),
        wait_status_(other.wait_status_),
        has_wait_status_(other.has_wait_status_) {
    other.has_wait_status_ = false;
  }
  /**
   * @brief Replaces ownership after settling this child.
   * @param other Source left without a live child.
   * @return This owner.
   * @throws Nothing.
   */
  ChildProcess& operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
      settle_noexcept();
      pid_ = std::exchange(other.pid_, -1);
      socket_path_ = std::move(other.socket_path_);
      wait_status_ = other.wait_status_;
      has_wait_status_ = other.has_wait_status_;
      other.has_wait_status_ = false;
    }
    return *this;
  }

  /**
   * @brief Sends one POSIX signal to the live child.
   * @param signal_number Signal number, normally `SIGINT` or `SIGTERM`.
   * @return True when `kill` accepted the signal.
   * @throws Nothing.
   */
  bool send_signal(int signal_number) noexcept {
    return pid_ > 0 && ::kill(pid_, signal_number) == 0;
  }

  /**
   * @brief Polls boundedly until the child is reaped.
   * @param timeout Maximum steady-clock duration.
   * @return True when an exit status was captured before the deadline.
   * @throws Nothing.
   * @note The method never sends a signal and may be called repeatedly.
   */
  bool wait_for_exit(std::chrono::milliseconds timeout) noexcept {
    if (pid_ <= 0) {
      return has_wait_status_;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
      int status = 0;
      const pid_t result = ::waitpid(pid_, &status, WNOHANG);
      if (result == pid_) {
        wait_status_ = status;
        has_wait_status_ = true;
        pid_ = -1;
        return true;
      }
      if (result < 0 && errno != EINTR) {
        return false;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        break;
      }
      std::this_thread::sleep_for(kPollInterval);
    } while (true);
    return false;
  }

  /**
   * @brief Reports whether the captured child status is normal success.
   * @return True only for `exit(0)`.
   * @throws Nothing.
   */
  bool exited_successfully() const noexcept {
    return has_wait_status_ && WIFEXITED(wait_status_) &&
           WEXITSTATUS(wait_status_) == 0;
  }

  /**
   * @brief Returns whether the child has already been reaped.
   * @return True after one wait captured status.
   * @throws Nothing.
   */
  bool exited() const noexcept { return has_wait_status_; }

  /**
   * @brief Returns the exact test socket path.
   * @return Immutable path reference.
   * @throws Nothing.
   */
  const std::string& socket_path() const noexcept { return socket_path_; }

 private:
  /**
   * @brief Forcefully settles only an unobserved child during test teardown.
   * @throws Nothing.
   * @note Wait interruptions are retried and the test-owned path is unlinked
   * after the child can no longer use it.
   */
  void settle_noexcept() noexcept {
    if (pid_ > 0) {
      static_cast<void>(::kill(pid_, SIGKILL));
      int status = 0;
      while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
      }
      pid_ = -1;
    }
    if (!socket_path_.empty()) {
      static_cast<void>(::unlink(socket_path_.c_str()));
    }
  }

  /** @brief Positive live child pid, or -1 after reaping/transfer. */
  pid_t pid_ = -1;
  /** @brief Exact test-owned socket path. */
  std::string socket_path_;
  /** @brief Raw status captured by waitpid. */
  int wait_status_ = 0;
  /** @brief Whether `wait_status_` is valid. */
  bool has_wait_status_ = false;
};

/**
 * @brief Returns one process-unique uncreated child socket path.
 * @return Bounded path under `/tmp`.
 * @throws std::bad_alloc If path construction fails.
 */
std::string socket_path() {
  static std::atomic<std::uint32_t> sequence{0U};
  return "/tmp/psd-signal-" + std::to_string(::getpid()) + "-" +
         std::to_string(sequence.fetch_add(1U)) + ".sock";
}

/**
 * @brief Reports exact pathname absence.
 * @param path Path to inspect without following symlinks.
 * @return True only when `lstat` reports `ENOENT`.
 * @throws Nothing.
 */
bool path_absent(const std::string& path) noexcept {
  struct stat state{};
  errno = 0;
  return ::lstat(path.c_str(), &state) != 0 && errno == ENOENT;
}

/**
 * @brief Forks and execs the real daemon with bounded local configuration.
 * @return Exact child cleanup ownership.
 * @throws std::system_error If `fork` fails.
 * @throws std::bad_alloc If path ownership allocation fails.
 * @note The child performs only `execl` and `_exit` after fork.
 */
ChildProcess spawn_daemon() {
  const std::string path = socket_path();
  const pid_t pid = ::fork();
  if (pid < 0) {
    throw std::system_error(errno, std::generic_category(), "fork failed");
  }
  if (pid == 0) {
    ::execl(PS_PHOTOSPIDERD_PATH, PS_PHOTOSPIDERD_PATH, "--socket",
            path.c_str(), "--max-concurrency", "1", "--max-jobs", "16",
            "--max-sessions", "4", "--max-connections", "8",
            static_cast<char*>(nullptr));
    ::_exit(127);
  }
  return ChildProcess(pid, path);
}

/**
 * @brief Connects and verifies the child public service before a deadline.
 * @param child Live child used for early-exit observation.
 * @param client Disconnected client populated on success.
 * @return True after one correlated `daemon.info` succeeds.
 * @throws std::bad_alloc If transport/status allocation fails.
 */
bool wait_until_ready(ChildProcess* child, ps::ipc::Client* client) {
  const auto deadline = std::chrono::steady_clock::now() + kStartupTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (child->wait_for_exit(std::chrono::milliseconds{0})) {
      return false;
    }
    if (client->connect(child->socket_path()).ok()) {
      auto info = client->daemon_info();
      if (info.ok()) {
        return true;
      }
      client->disconnect();
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  return false;
}

/**
 * @brief Builds one cooperative delayed public workflow.
 * @return Workflow whose second node delays for five seconds unless cancelled.
 * @throws std::bad_alloc If source storage allocation fails.
 */
ps::WorkflowDocument delayed_document() {
  ps::WorkflowDocument document;
  document.nodes = {
      ps::WorkflowNode{1U, "core.constant", {}, {{"value", 7.0}}},
      ps::WorkflowNode{2U,
                       "core.delay",
                       {ps::WorkflowInput{1U, "value"}},
                       {{"milliseconds", kDelayedJobMilliseconds}}},
  };
  document.outputs = {ps::WorkflowOutput{"value", 2U, "value"}};
  return document;
}

/**
 * @brief Waits for one public Job to enter Running.
 * @param client Connected child client.
 * @param job Exact child-local Job id.
 * @return True when Running is observed before any terminal state/deadline.
 * @throws std::bad_alloc If status decoding allocation fails.
 */
bool wait_running(ps::ipc::Client* client, ps::ipc::JobId job) {
  const auto deadline = std::chrono::steady_clock::now() + kRunningTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto status = client->job_status(job);
    if (!status.ok()) {
      return false;
    }
    if (status.value().state == ps::ipc::JobState::Running) {
      return true;
    }
    if (status.value().state == ps::ipc::JobState::Succeeded ||
        status.value().state == ps::ipc::JobState::Failed ||
        status.value().state == ps::ipc::JobState::Cancelled) {
      return false;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  return false;
}

/**
 * @brief Exercises `SIGINT` graceful shutdown without submitted work.
 * @return True for bounded `exit(0)` and socket removal.
 * @throws std::bad_alloc If fixture/client storage allocation fails.
 * @throws std::system_error If child creation fails.
 */
bool sigint_case() {
  ChildProcess child = spawn_daemon();
  ps::ipc::Client client;
  if (!wait_until_ready(&child, &client) || !child.send_signal(SIGINT) ||
      !child.wait_for_exit(kShutdownTimeout)) {
    return false;
  }
  return child.exited_successfully() && path_absent(child.socket_path());
}

/**
 * @brief Exercises `SIGTERM` while one cooperative delayed Job is Running.
 * @return True for cancellation-bounded `exit(0)` and socket removal.
 * @throws std::bad_alloc If fixture/client storage allocation fails.
 * @throws std::system_error If child creation fails.
 * @note The accepted bound is well below the operation's natural delay.
 */
bool sigterm_delayed_case() {
  ChildProcess child = spawn_daemon();
  ps::ipc::Client client;
  if (!wait_until_ready(&child, &client)) {
    return false;
  }
  auto session = client.session_create(delayed_document());
  if (!session.ok()) {
    return false;
  }
  auto job = client.job_submit(session.value());
  if (!job.ok() || !wait_running(&client, job.value())) {
    return false;
  }
  const auto started = std::chrono::steady_clock::now();
  if (!child.send_signal(SIGTERM) || !child.wait_for_exit(kShutdownTimeout)) {
    return false;
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  return child.exited_successfully() && elapsed < kDelayedShutdownBound &&
         path_absent(child.socket_path());
}

/**
 * @brief Exercises ordinary RPC shutdown with the signal waiter idle.
 * @return True for acknowledged bounded `exit(0)` and socket removal.
 * @throws std::bad_alloc If fixture/client storage allocation fails.
 * @throws std::system_error If child creation fails.
 */
bool rpc_shutdown_case() {
  ChildProcess child = spawn_daemon();
  ps::ipc::Client client;
  if (!wait_until_ready(&child, &client) || !client.daemon_shutdown().ok() ||
      !child.wait_for_exit(kShutdownTimeout)) {
    return false;
  }
  return child.exited_successfully() && path_absent(child.socket_path());
}

}  // namespace

/**
 * @brief Selects one isolated real-photospiderd shutdown regression.
 * @param argc Exact mode argument count.
 * @param argv Mode selector from CTest.
 * @return Zero when the selected lifecycle is bounded and clean.
 * @throws Nothing; unexpected exceptions become a failed process result.
 */
int main(int argc, char** argv) {
  try {
    if (argc != 2) {
      return 2;
    }
    if (std::strcmp(argv[1], "sigint") == 0) {
      PS_IPC_CHECK(sigint_case());
      return 0;
    }
    if (std::strcmp(argv[1], "sigterm-delayed") == 0) {
      PS_IPC_CHECK(sigterm_delayed_case());
      return 0;
    }
    if (std::strcmp(argv[1], "rpc-shutdown") == 0) {
      PS_IPC_CHECK(rpc_shutdown_case());
      return 0;
    }
    return 2;
  } catch (...) {
    return 1;
  }
}
