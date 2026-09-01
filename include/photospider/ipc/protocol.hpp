#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/core/status.hpp"
#include "photospider/execution/execution.hpp"

namespace ps::ipc {

/** @brief Exact local IPC wire version implemented by this product. */
inline constexpr std::uint16_t kProtocolVersion = 3U;

/** @brief Maximum accepted binary frame payload, excluding length prefix. */
inline constexpr std::size_t kMaximumFramePayloadBytes = 4U * 1024U * 1024U;

/**
 * @brief Opaque process-local logical namespace identifier.
 *
 * @note Values are invalid after daemon restart and have no kernel meaning.
 */
struct SessionId final {
  /** @brief Nonzero daemon-instance token. */
  std::uint64_t instance = 0U;
  /** @brief Nonzero daemon-generated ephemeral value. */
  std::uint64_t value = 0U;
};

/**
 * @brief Opaque ephemeral execution identifier.
 *
 * @note Callers retry by submitting again and receiving a distinct value.
 */
struct JobId final {
  /** @brief Nonzero daemon-instance token. */
  std::uint64_t instance = 0U;
  /** @brief Nonzero daemon-generated ephemeral value. */
  std::uint64_t value = 0U;
};

/**
 * @brief Exact forward-only lifecycle of one ephemeral execution.
 *
 * @note Permitted transitions are exactly Queued to Running to one terminal
 * state. A queued cancellation is observed immediately after dispatch enters
 * Running.
 */
enum class JobState : std::uint8_t {
  Queued = 1U,
  Running = 2U,
  Succeeded = 3U,
  Failed = 4U,
  Cancelled = 5U,
};

/**
 * @brief Public submit controls mapped to kernel planning and execution.
 *
 * @note The request never carries native-library paths or internal IR.
 */
struct JobSubmitOptions final {
  /** @brief Whether the daemon-configured optional local GPU may be planned. */
  bool allow_gpu = false;
  /** @brief Per-execution local parallelism; zero uses kernel default. */
  std::uint32_t maximum_parallelism = 0U;
};

/**
 * @brief Non-destructive snapshot of one ephemeral execution.
 *
 * @note `outcome` is canonical success before terminal completion and carries
 * the final failure category for Failed or Cancelled.
 */
struct JobStatus final {
  /** @brief Exact ephemeral execution identifier. */
  JobId job_id;
  /** @brief Owning logical namespace identifier. */
  SessionId session_id;
  /** @brief Current forward-only lifecycle state. */
  JobState state = JobState::Queued;
  /** @brief Final status when terminal; otherwise canonical success. */
  Status outcome;
};

/**
 * @brief Read-only local daemon capabilities and live counts.
 *
 * @note Counts are observations and may change immediately after return.
 */
struct DaemonInfo final {
  /** @brief Exact wire protocol version, always three. */
  std::uint16_t protocol_version = kProtocolVersion;
  /** @brief Nonzero non-security identity unique to this process lifetime. */
  std::uint64_t instance_id = 0U;
  /** @brief Build-time daemon package version. */
  std::string service_version;
  /** @brief Local transport name, currently `unix-domain`. */
  std::string transport;
  /** @brief Exact sorted nine-method surface. */
  std::vector<std::string> methods;
  /** @brief Current logical namespace count. */
  std::uint64_t active_sessions = 0U;
  /** @brief Current retained execution count. */
  std::uint64_t active_jobs = 0U;
  /** @brief Fixed global running-execution bound. */
  std::uint32_t maximum_concurrency = 0U;
  /** @brief Fixed global retained-Session bound. */
  std::uint32_t maximum_sessions = 0U;
};

}  // namespace ps::ipc
