#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/ipc/client.hpp"

namespace {

/** @brief Exact protocol-v2 method inventory frozen by the archive baseline. */
constexpr std::array<std::string_view, 60> kExpectedMethods = {
    "cache.cache_all_nodes",
    "cache.clear_all",
    "cache.clear_drive",
    "cache.clear_memory",
    "cache.free_transient",
    "cache.synchronize_disk",
    "compute.last_error",
    "compute.last_io_time",
    "compute.release",
    "compute.result",
    "compute.status",
    "compute.submit",
    "compute.timing",
    "daemon.ping",
    "daemon.version",
    "dirty.begin",
    "dirty.end",
    "dirty.update",
    "events.drain",
    "execution.configure_defaults",
    "execution.description",
    "execution.info",
    "execution.replace",
    "execution.trace",
    "execution.types",
    "graph.clear",
    "graph.close",
    "graph.list",
    "graph.load",
    "graph.node_yaml.get",
    "graph.node_yaml.set",
    "graph.reload",
    "graph.save",
    "inspect.compute_planning",
    "inspect.dependency_tree",
    "inspect.dirty_region",
    "inspect.ending_nodes",
    "inspect.graph",
    "inspect.node",
    "inspect.node_ids",
    "inspect.recent_compute_planning",
    "inspect.roi_backward",
    "inspect.roi_forward",
    "inspect.traversal_details",
    "inspect.traversal_orders",
    "inspect.trees_containing_node",
    "plugins.load_report",
    "plugins.ops_combined_keys",
    "plugins.ops_combined_sources",
    "plugins.ops_sources",
    "plugins.seed_builtins",
    "plugins.unload_all",
    "policy.configure_defaults",
    "policy.description",
    "policy.info",
    "policy.load",
    "policy.loaded_plugins",
    "policy.replace",
    "policy.scan",
    "policy.types",
};

/**
 * @brief Checks compile-time ascending order of the frozen method inventory.
 * @return True only when every adjacent method is strictly increasing.
 * @throws Nothing.
 */
constexpr bool expected_methods_are_sorted_and_unique() noexcept {
  for (std::size_t index = 1; index < kExpectedMethods.size(); ++index) {
    if (!(kExpectedMethods[index - 1] < kExpectedMethods[index])) {
      return false;
    }
  }
  return true;
}

static_assert(expected_methods_are_sorted_and_unique(),
              "frozen method inventory must remain sorted and unique");

/**
 * @brief Validates one direct-client connection against the frozen v2 surface.
 *
 * @param socket_path Absolute Unix-domain socket served by either baseline or
 *        extracted `photospiderd`.
 * @param expected_instance Optional instance id that reconnect must preserve.
 * @return Serving daemon instance id on success; empty string on failure.
 * @throws std::bad_alloc when client or copied metadata allocation fails.
 * @note The probe performs no mutation and never retries a failed RPC. A later
 *       invocation creates a fresh client to verify explicit reconnect use.
 */
std::string validate_connection(const std::string& socket_path,
                                const std::string& expected_instance) {
  ps::ipc::Client client;
  const ps::OperationStatus connected = client.connect(socket_path);
  if (!connected.ok) {
    std::cerr << "connect failed: " << connected.message << '\n';
    return {};
  }

  const ps::ipc::IpcResult<ps::ipc::DaemonPing> ping = client.ping();
  if (!ping.status.ok || !ping.value.pong ||
      ping.value.server_instance_id.empty()) {
    std::cerr << "ping validation failed: " << ping.status.message << '\n';
    return {};
  }
  if (!expected_instance.empty() &&
      ping.value.server_instance_id != expected_instance) {
    std::cerr << "reconnect reached another daemon instance\n";
    return {};
  }

  const ps::ipc::IpcResult<ps::ipc::DaemonVersion> version = client.version();
  if (!version.status.ok || version.value.protocol_version != 2 ||
      version.value.service_name != "photospiderd" ||
      version.value.transport != "unix" ||
      version.value.service_version.empty() ||
      version.value.server_instance_id != ping.value.server_instance_id ||
      version.value.methods.size() != kExpectedMethods.size()) {
    std::cerr << "version metadata validation failed: "
              << version.status.message << '\n';
    return {};
  }
  for (std::size_t index = 0; index < kExpectedMethods.size(); ++index) {
    if (version.value.methods[index] != kExpectedMethods[index]) {
      std::cerr << "method inventory mismatch at index " << index << '\n';
      return {};
    }
  }
  client.disconnect();
  return ping.value.server_instance_id;
}

/**
 * @brief Reports one failed typed operation with its stable status identity.
 * @param status Completed operation status.
 * @param operation Human-readable gate stage.
 * @return True for success; false after emitting one diagnostic for failure.
 * @throws Nothing unless the standard output stream is configured to throw.
 */
bool require_status(const ps::OperationStatus& status,
                    std::string_view operation) {
  if (status.ok) {
    return true;
  }
  std::cerr << operation << " failed: " << status.name << ": " << status.message
            << '\n';
  return false;
}

/**
 * @brief Tests whether an owned sorted string list contains one exact value.
 * @param values Candidate strings.
 * @param expected Exact value to find.
 * @return True when one element equals `expected`.
 * @throws Nothing.
 */
bool contains_string(const std::vector<std::string>& values,
                     std::string_view expected) noexcept {
  return std::any_of(
      values.begin(), values.end(),
      [expected](const std::string& value) { return value == expected; });
}

/**
 * @brief Waits for one accepted compute job to publish a terminal snapshot.
 * @param client Connected sequential client.
 * @param compute_id Opaque accepted compute identity.
 * @param terminal Receives the last terminal snapshot.
 * @return True after success/failure publication; false on RPC failure/timeout.
 * @throws std::bad_alloc when copied response storage cannot allocate.
 * @note The bounded polling loop issues only `compute.status` and never retries
 *       submission or mutates the job.
 */
bool wait_for_terminal(ps::ipc::Client& client,
                       const ps::ipc::ComputeRequestId& compute_id,
                       ps::ipc::ComputeJobSnapshot* terminal) {
  constexpr std::size_t kMaximumPolls = 1000;
  for (std::size_t attempt = 0; attempt < kMaximumPolls; ++attempt) {
    ps::ipc::IpcResult<ps::ipc::ComputeJobSnapshot> status =
        client.compute_status(compute_id);
    if (!require_status(status.status, "compute.status")) {
      return false;
    }
    if (status.value.cancellable) {
      std::cerr << "compute.status advertised cancellable=true\n";
      return false;
    }
    if (status.value.state == ps::ipc::ComputeJobState::Succeeded ||
        status.value.state == ps::ipc::ComputeJobState::Failed) {
      *terminal = std::move(status.value);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::cerr << "compute.status did not become terminal within ten seconds\n";
  return false;
}

/**
 * @brief Exercises submit/status/result/release for one frozen result mode.
 * @param client Connected sequential client.
 * @param session Active opaque graph session.
 * @param result_mode Status-only or named-Values selection.
 * @param expect_output Whether the terminal result must own one artifact.
 * @return True only when every lifecycle and artifact invariant holds.
 * @throws std::bad_alloc or filesystem exceptions on resource failure.
 * @note The request targets the deterministic built-in constant operation.
 */
bool validate_compute_lifecycle(ps::ipc::Client& client,
                                const ps::ipc::IpcSessionId& session,
                                ps::ipc::ComputeResultMode result_mode,
                                bool expect_output) {
  ps::ipc::ComputeSubmitRequest request;
  request.session_id = session;
  request.node = ps::NodeId{1};
  request.cache.precision = "float32";
  request.execution.quiet = true;
  request.telemetry.enable_timing = true;
  request.result_mode = result_mode;

  ps::ipc::IpcResult<ps::ipc::ComputeJobSnapshot> submitted =
      client.submit_compute(request);
  if (!require_status(submitted.status, "compute.submit") ||
      submitted.value.state != ps::ipc::ComputeJobState::Queued ||
      submitted.value.cancellable || submitted.value.status.has_value() ||
      submitted.value.output.has_value() ||
      submitted.value.session_id.value != session.value ||
      submitted.value.compute_id.value.empty()) {
    std::cerr << "compute.submit returned an invalid frozen job snapshot\n";
    return false;
  }

  ps::ipc::ComputeJobSnapshot terminal;
  if (!wait_for_terminal(client, submitted.value.compute_id, &terminal) ||
      terminal.state != ps::ipc::ComputeJobState::Succeeded ||
      !terminal.status.has_value() || !terminal.status->ok ||
      terminal.output.has_value()) {
    std::cerr
        << "compute.status did not publish successful output-free state\n";
    return false;
  }

  ps::ipc::IpcResult<ps::ipc::ComputeJobSnapshot> result =
      client.compute_result(submitted.value.compute_id);
  if (!require_status(result.status, "compute.result") ||
      result.value.state != ps::ipc::ComputeJobState::Succeeded ||
      !result.value.status.has_value() || !result.value.status->ok ||
      result.value.cancellable ||
      result.value.output.has_value() != expect_output) {
    std::cerr << "compute.result returned an invalid terminal snapshot\n";
    return false;
  }

  std::optional<ps::ipc::DeliveryLeaseId> delivery_id;
  std::optional<std::filesystem::path> artifact_path;
  if (result.value.output.has_value()) {
    const ps::ipc::OutputArtifactDelivery& output = *result.value.output;
    const ps::ipc::OutputArtifactMetadata& metadata = output.metadata;
    artifact_path = std::filesystem::path(metadata.path);
    if (!artifact_path->is_absolute() ||
        !std::filesystem::is_regular_file(*artifact_path) ||
        std::filesystem::file_size(*artifact_path) != metadata.byte_size ||
        metadata.archive_version != 1U || metadata.value_count != 1U ||
        metadata.output_id.value.empty() || output.delivery_id.value.empty()) {
      std::cerr << "named-Values metadata or protected artifact is invalid\n";
      return false;
    }
    delivery_id = output.delivery_id;
  }

  const ps::ipc::IpcResult<ps::ipc::ComputeReleaseResult> released =
      client.release_compute(submitted.value.compute_id, delivery_id);
  if (!require_status(released.status, "compute.release") ||
      !released.value.released ||
      released.value.compute_id.value != submitted.value.compute_id.value) {
    std::cerr << "compute.release returned an invalid acknowledgement\n";
    return false;
  }
  if (artifact_path.has_value() && std::filesystem::exists(*artifact_path)) {
    std::cerr << "compute.release retained a named-Values artifact path\n";
    return false;
  }
  return true;
}

/**
 * @brief Validates process-global plugin, policy, and execution control routes.
 * @param client Connected sequential client.
 * @return True only for the frozen built-in service inventories and controls.
 * @throws std::bad_alloc when copied service values cannot allocate.
 * @note Empty plugin scans exercise the public routes without introducing a
 *       platform-specific native DSO trust decision into the four-cell gate.
 */
bool validate_process_services(ps::ipc::Client& client) {
  const ps::VoidResult seeded = client.seed_builtin_ops();
  const ps::ipc::IpcResult<std::map<std::string, std::string>> sources =
      client.ops_sources();
  const ps::ipc::IpcResult<std::vector<std::string>> combined_keys =
      client.ops_combined_keys();
  const ps::ipc::IpcResult<std::map<std::string, std::string>>
      combined_sources = client.ops_combined_sources();
  if (!require_status(seeded.status, "plugins.seed_builtins") ||
      !require_status(sources.status, "plugins.ops_sources") ||
      !require_status(combined_keys.status, "plugins.ops_combined_keys") ||
      !require_status(combined_sources.status,
                      "plugins.ops_combined_sources") ||
      sources.value.count("image_generator:constant") != 1U ||
      sources.value.at("image_generator:constant") != "built-in" ||
      !contains_string(combined_keys.value, "image_generator:constant") ||
      combined_sources.value != sources.value) {
    std::cerr << "built-in operation inventory validation failed\n";
    return false;
  }

  const ps::ipc::IpcResult<ps::HostPluginLoadReport> empty_load =
      client.plugins_load_report({});
  if (!require_status(empty_load.status, "plugins.load_report") ||
      empty_load.value.attempted != 0 || empty_load.value.loaded != 0 ||
      !empty_load.value.errors.empty() ||
      !empty_load.value.new_op_keys.empty()) {
    std::cerr << "empty operation plugin load changed process state\n";
    return false;
  }

  const ps::ipc::IpcResult<std::vector<std::string>> policy_types =
      client.policy_available_types();
  const ps::ipc::IpcResult<std::string> interactive_description =
      client.policy_description("interactive");
  const ps::ipc::IpcResult<std::string> throughput_description =
      client.policy_description("throughput");
  const ps::ipc::IpcResult<std::size_t> policy_scan = client.policy_scan({});
  const ps::ipc::IpcResult<std::vector<std::string>> policy_plugins =
      client.policy_loaded_plugins();
  const ps::VoidResult policy_defaults =
      client.configure_policy_defaults(ps::HostPolicyConfig{});
  const ps::ipc::IpcResult<ps::PolicyInfoSnapshot> interactive_info =
      client.policy_info(ps::PolicyClass::Interactive);
  const ps::ipc::IpcResult<ps::PolicyInfoSnapshot> throughput_info =
      client.policy_info(ps::PolicyClass::Throughput);
  const ps::VoidResult policy_replaced =
      client.replace_policy(ps::PolicyClass::Interactive, "interactive");
  if (!require_status(policy_types.status, "policy.types") ||
      policy_types.value !=
          std::vector<std::string>({"interactive", "throughput"}) ||
      !require_status(interactive_description.status, "policy.description") ||
      interactive_description.value.empty() ||
      !require_status(throughput_description.status, "policy.description") ||
      throughput_description.value.empty() ||
      !require_status(policy_scan.status, "policy.scan") ||
      policy_scan.value != 0U ||
      !require_status(policy_plugins.status, "policy.loaded_plugins") ||
      !policy_plugins.value.empty() ||
      !require_status(policy_defaults.status, "policy.configure_defaults") ||
      !require_status(interactive_info.status, "policy.info") ||
      interactive_info.value.policy_type != "interactive" ||
      interactive_info.value.fault.has_value() ||
      !require_status(throughput_info.status, "policy.info") ||
      throughput_info.value.policy_type != "throughput" ||
      throughput_info.value.fault.has_value() ||
      !require_status(policy_replaced.status, "policy.replace")) {
    std::cerr << "built-in policy contract validation failed\n";
    return false;
  }

  const ps::ipc::IpcResult<std::vector<std::string>> execution_types =
      client.execution_available_types();
  const ps::ipc::IpcResult<std::string> cpu_description =
      client.execution_description("cpu");
  const ps::ipc::IpcResult<std::string> serial_description =
      client.execution_description("serial_debug");
  const ps::VoidResult execution_defaults =
      client.configure_execution_defaults(ps::HostExecutionConfig{});
  if (!require_status(execution_types.status, "execution.types") ||
      execution_types.value !=
          std::vector<std::string>({"cpu", "gpu_pipeline", "serial_debug"}) ||
      !require_status(cpu_description.status, "execution.description") ||
      cpu_description.value.empty() ||
      !require_status(serial_description.status, "execution.description") ||
      serial_description.value.empty() ||
      !require_status(execution_defaults.status,
                      "execution.configure_defaults")) {
    std::cerr << "built-in execution contract validation failed\n";
    return false;
  }
  return true;
}

/**
 * @brief Validates graph, compute, observation, and session behavior end to
 * end.
 * @param socket_path Absolute Unix socket for one baseline/extracted daemon.
 * @param graph_yaml Absolute deterministic built-in constant graph source.
 * @param work_root Isolated writable directory owned by this gate cell.
 * @return True only when the complete frozen behavior slice passes.
 * @throws std::bad_alloc or filesystem exceptions on resource failure.
 * @note This function mutates only its cell-local graph/cache roots and closes
 *       every graph/job before returning.
 */
bool validate_frozen_behavior(const std::string& socket_path,
                              const std::filesystem::path& graph_yaml,
                              const std::filesystem::path& work_root) {
  ps::ipc::Client client;
  if (!require_status(client.connect(socket_path), "connect") ||
      !validate_process_services(client)) {
    return false;
  }

  ps::GraphLoadRequest request;
  request.session = ps::GraphSessionId{"interop_v2"};
  request.root_dir = (work_root / "sessions").string();
  request.yaml_path = graph_yaml.string();
  request.cache_root_dir = (work_root / "cache").string();
  const ps::ipc::IpcResult<ps::ipc::GraphSessionSummary> loaded =
      client.load_graph(request);
  if (!require_status(loaded.status, "graph.load") ||
      loaded.value.session_name != request.session.value ||
      loaded.value.session_id.value.empty()) {
    return false;
  }
  const ps::ipc::IpcSessionId& session = loaded.value.session_id;

  const ps::ipc::IpcResult<std::vector<ps::ipc::GraphSessionSummary>> listed =
      client.list_graphs();
  const ps::ipc::IpcResult<ps::GraphInspectionView> graph =
      client.inspect_graph(session);
  const ps::ipc::IpcResult<ps::NodeInspectionView> node =
      client.inspect_node(session, ps::NodeId{1});
  const ps::ipc::IpcResult<std::vector<ps::NodeId>> node_ids =
      client.list_node_ids(session);
  const ps::ipc::IpcResult<std::vector<ps::NodeId>> endings =
      client.ending_nodes(session);
  const ps::ipc::IpcResult<ps::HostDependencyTreeSnapshot> dependencies =
      client.inspect_dependency_tree(session, ps::NodeId{1}, true);
  const ps::ipc::IpcResult<std::map<int, std::vector<ps::NodeId>>> orders =
      client.traversal_orders(session);
  const ps::ipc::IpcResult<
      std::map<int, std::vector<ps::HostTraversalNodeSnapshot>>>
      details = client.traversal_details(session);
  const ps::ipc::IpcResult<std::vector<ps::NodeId>> containing =
      client.trees_containing_node(session, ps::NodeId{1});
  const ps::ipc::IpcResult<std::string> node_yaml =
      client.get_node_yaml(session, ps::NodeId{1});
  const ps::ipc::IpcResult<ps::DirtyRegionInspectionSnapshot> dirty =
      client.dirty_region_snapshot(session);
  const ps::ipc::IpcResult<std::optional<ps::ComputePlanningInspectionSnapshot>>
      planning = client.compute_planning_snapshot(session);
  const ps::ipc::IpcResult<std::vector<ps::ComputePlanningInspectionSnapshot>>
      recent = client.recent_compute_planning_snapshots(session);
  if (!require_status(listed.status, "graph.list") ||
      listed.value.size() != 1U ||
      listed.value.front().session_id.value != session.value ||
      !require_status(graph.status, "inspect.graph") ||
      graph.value.session.value != session.value ||
      graph.value.nodes.size() != 1U ||
      graph.value.nodes.front().name != "interop_constant" ||
      !require_status(node.status, "inspect.node") ||
      node.value.id.value != 1 || node.value.name != "interop_constant" ||
      !require_status(node_ids.status, "inspect.node_ids") ||
      node_ids.value.size() != 1U || node_ids.value.front().value != 1 ||
      !require_status(endings.status, "inspect.ending_nodes") ||
      endings.value.size() != 1U || endings.value.front().value != 1 ||
      !require_status(dependencies.status, "inspect.dependency_tree") ||
      dependencies.value.entries.size() != 1U ||
      !require_status(orders.status, "inspect.traversal_orders") ||
      orders.value.size() != 1U ||
      !require_status(details.status, "inspect.traversal_details") ||
      details.value.size() != 1U ||
      !require_status(containing.status, "inspect.trees_containing_node") ||
      containing.value.size() != 1U || containing.value.front().value != 1 ||
      !require_status(node_yaml.status, "graph.node_yaml.get") ||
      node_yaml.value.find("id: 1") == std::string::npos ||
      !require_status(dirty.status, "inspect.dirty_region") ||
      !require_status(planning.status, "inspect.compute_planning") ||
      !require_status(recent.status, "inspect.recent_compute_planning")) {
    std::cerr << "graph or inspection behavior validation failed\n";
    return false;
  }

  const ps::ipc::IpcResult<ps::ExecutionInfoSnapshot> hp_info =
      client.execution_info(session, ps::ComputeIntent::GlobalHighPrecision);
  const ps::ipc::IpcResult<ps::ExecutionInfoSnapshot> rt_info =
      client.execution_info(session, ps::ComputeIntent::RealTimeUpdate);
  const ps::VoidResult replaced = client.replace_execution(
      session, ps::ComputeIntent::GlobalHighPrecision, "serial_debug");
  const ps::ipc::IpcResult<ps::ExecutionInfoSnapshot> serial_info =
      client.execution_info(session, ps::ComputeIntent::GlobalHighPrecision);
  const ps::VoidResult restored = client.replace_execution(
      session, ps::ComputeIntent::GlobalHighPrecision, "cpu");
  if (!require_status(hp_info.status, "execution.info") ||
      hp_info.value.execution_type != "cpu" ||
      !require_status(rt_info.status, "execution.info") ||
      rt_info.value.execution_type != "cpu" ||
      !require_status(replaced.status, "execution.replace") ||
      !require_status(serial_info.status, "execution.info") ||
      serial_info.value.execution_type != "serial_debug" ||
      !require_status(restored.status, "execution.replace")) {
    std::cerr << "per-session execution route validation failed\n";
    return false;
  }

  if (!validate_compute_lifecycle(client, session,
                                  ps::ipc::ComputeResultMode::Status, false) ||
      !validate_compute_lifecycle(client, session,
                                  ps::ipc::ComputeResultMode::Values, true)) {
    return false;
  }

  const ps::ipc::IpcResult<ps::ComputeEventBatch> events =
      client.drain_compute_events(session, ps::kComputeEventDrainMaxLimit);
  const ps::ipc::IpcResult<ps::ExecutionTracePage> trace =
      client.execution_trace(session, 0, ps::kExecutionTraceMaxLimit);
  const ps::ipc::IpcResult<ps::TimingSnapshot> timing = client.timing(session);
  const ps::ipc::IpcResult<double> io_time = client.last_io_time(session);
  const ps::ipc::IpcResult<ps::OperationStatus> last_error =
      client.last_error(session);
  if (!require_status(events.status, "events.drain") ||
      events.value.events.empty() ||
      !require_status(trace.status, "execution.trace") ||
      trace.value.session.value != session.value ||
      !require_status(timing.status, "compute.timing") ||
      !require_status(io_time.status, "compute.last_io_time") ||
      !require_status(last_error.status, "compute.last_error") ||
      !last_error.value.ok) {
    std::cerr << "event, trace, or compute observation validation failed\n";
    return false;
  }

  if (!require_status(client.close_graph(session).status, "graph.close")) {
    return false;
  }
  const ps::ipc::IpcResult<std::vector<ps::ipc::GraphSessionSummary>> empty =
      client.list_graphs();
  const ps::ipc::IpcResult<int> unloaded = client.plugins_unload_all();
  const ps::VoidResult reseeded = client.seed_builtin_ops();
  if (!require_status(empty.status, "graph.list") || !empty.value.empty() ||
      !require_status(unloaded.status, "plugins.unload_all") ||
      unloaded.value < 0 ||
      !require_status(reseeded.status, "plugins.seed_builtins")) {
    std::cerr << "session close or plugin lifecycle validation failed\n";
    return false;
  }
  client.disconnect();
  return true;
}

}  // namespace

/**
 * @brief Runs the public frozen-baseline client compatibility probe.
 *
 * @param argc Process argument count; socket, graph, and work paths required.
 * @param argv Process argument vector.
 * @return Zero after two fresh clients validate one daemon; nonzero on usage,
 *         transport, metadata, inventory, or reconnect failure.
 * @throws Nothing; unexpected exceptions are diagnosed and converted to exit
 *         status 1.
 * @note Build this same source once against the archived Photospider IPC client
 *       and once against `PhotospiderDaemon::client` for the four-cell gate.
 */
int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: ipc_compat_probe ABSOLUTE_SOCKET_PATH "
                 "ABSOLUTE_GRAPH_YAML ABSOLUTE_WORK_ROOT\n";
    return 2;
  }
  try {
    const std::string first = validate_connection(argv[1], {});
    if (first.empty()) {
      return 1;
    }
    const std::string second = validate_connection(argv[1], first);
    if (second.empty()) {
      return 1;
    }
    const std::filesystem::path graph_yaml =
        std::filesystem::canonical(argv[2]);
    const std::filesystem::path work_root = std::filesystem::canonical(argv[3]);
    if (!graph_yaml.is_absolute() || !work_root.is_absolute() ||
        !validate_frozen_behavior(argv[1], graph_yaml, work_root)) {
      return 1;
    }
    std::cout << "IPC v2 compatibility probe passed for instance " << second
              << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "compatibility probe failed: " << error.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "compatibility probe failed with an unknown exception\n";
    return 1;
  }
}
