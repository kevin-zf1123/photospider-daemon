#!/usr/bin/env python3
"""Install PhotospiderDaemon and validate its public package boundary."""

from __future__ import annotations

import argparse
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
"""Canonical source root used only to import maintained test helpers."""

sys.path.insert(0, str(REPOSITORY_ROOT / "tools"))

from loader_environment import clean_loader_environment  # noqa: E402


EXPECTED_PUBLIC_HEADERS = (
    "include/photospider/ipc/client.hpp",
    "include/photospider/ipc/host.hpp",
    "include/photospider/ipc/protocol.hpp",
)
"""Exact daemon-owned public-header inventory installed by the package."""

EXPECTED_DIRECT_INCLUDES = {
    "include/photospider/ipc/client.hpp": (
        "cstddef",
        "cstdint",
        "map",
        "memory",
        "optional",
        "string",
        "vector",
        "photospider/core/export.hpp",
        "photospider/host/graph_session.hpp",
        "photospider/host/host.hpp",
        "photospider/ipc/protocol.hpp",
    ),
    "include/photospider/ipc/host.hpp": (
        "memory",
        "string",
        "photospider/core/export.hpp",
        "photospider/host/host.hpp",
    ),
    "include/photospider/ipc/protocol.hpp": (
        "cstddef",
        "cstdint",
        "optional",
        "string",
        "utility",
        "vector",
        "photospider/core/result_types.hpp",
        "photospider/data/value_artifact.hpp",
        "photospider/host/compute_request.hpp",
    ),
}
"""Positive direct-include boundary for each installed daemon header."""

FORBIDDEN_PUBLIC_DECLARATION_PATTERNS = {
    "backend implementation type": (
        r"\b(?:Kernel|GraphModel|InteractionService|ComputeService|"
        r"PluginManager|PolicyRegistry)\b"
    ),
    "nlohmann JSON type or header": r"nlohmann(?:::|/)",
    "raw Unix socket type": r"\b(?:sockaddr|sockaddr_un|sa_family_t)\b",
    "raw descriptor declaration": r"\b(?:int|long)\s+(?:[A-Za-z_]\w*_)?fd\b",
    "raw file identity type": r"\b(?:dev_t|ino_t|mode_t|off_t)\b",
    "raw file mapping symbol": (
        r"\b(?:mmap|munmap|MAP_FAILED|MAP_PRIVATE|PROT_READ)\b"
    ),
}
"""Raw transport, JSON, file, and backend declarations forbidden publicly."""

EXPECTED_CLIENT_METHODS = (
    "ping",
    "version",
    "load_graph",
    "close_graph",
    "list_graphs",
    "inspect_graph",
    "inspect_node",
    "inspect_dependency_tree",
    "reload_graph",
    "save_graph",
    "clear_graph",
    "get_node_yaml",
    "set_node_yaml",
    "list_node_ids",
    "ending_nodes",
    "traversal_orders",
    "traversal_details",
    "trees_containing_node",
    "project_roi",
    "project_roi_backward",
    "dirty_region_snapshot",
    "compute_planning_snapshot",
    "recent_compute_planning_snapshots",
    "submit_compute",
    "compute_status",
    "compute_result",
    "release_compute",
    "timing",
    "last_io_time",
    "last_error",
    "begin_dirty_source",
    "update_dirty_source",
    "end_dirty_source",
    "drain_compute_events",
    "clear_cache",
    "clear_drive_cache",
    "clear_memory_cache",
    "cache_all_nodes",
    "free_transient_memory",
    "synchronize_disk_cache",
    "plugins_load_report",
    "plugins_unload_all",
    "seed_builtin_ops",
    "ops_sources",
    "ops_combined_keys",
    "ops_combined_sources",
    "policy_available_types",
    "policy_description",
    "policy_scan",
    "policy_load",
    "policy_loaded_plugins",
    "configure_policy_defaults",
    "policy_info",
    "replace_policy",
    "execution_available_types",
    "execution_description",
    "configure_execution_defaults",
    "execution_info",
    "replace_execution",
    "execution_trace",
)
"""Normative exact and unique typed direct-client symbol inventory."""

EXPECTED_HOST_METHODS = (
    "load_graph",
    "close_graph",
    "list_graphs",
    "reload_graph",
    "save_graph",
    "clear_graph",
    "compute",
    "compute_async",
    "compute_and_get_values",
    "timing",
    "last_io_time",
    "last_error",
    "list_node_ids",
    "ending_nodes",
    "get_node_yaml",
    "set_node_yaml",
    "inspect_node",
    "inspect_graph",
    "dependency_tree",
    "traversal_orders",
    "traversal_details",
    "trees_containing_node",
    "project_roi",
    "project_roi_backward",
    "dirty_region_snapshot",
    "compute_planning_snapshot",
    "recent_compute_planning_snapshots",
    "begin_dirty_source",
    "update_dirty_source",
    "end_dirty_source",
    "drain_compute_events",
    "execution_trace",
    "clear_cache",
    "clear_drive_cache",
    "clear_memory_cache",
    "cache_all_nodes",
    "free_transient_memory",
    "synchronize_disk_cache",
    "plugins_load_report",
    "plugins_load",
    "plugins_unload_all",
    "seed_builtin_ops",
    "ops_sources",
    "ops_combined_keys",
    "ops_combined_sources",
    "policy_available_types",
    "policy_description",
    "policy_scan",
    "policy_load",
    "policy_loaded_plugins",
    "configure_policy_defaults",
    "policy_info",
    "replace_policy",
    "execution_available_types",
    "execution_description",
    "configure_execution_defaults",
    "execution_info",
    "replace_execution",
)
"""Normative exact and unique non-destructor Host virtual inventory."""


def run_checked(
    command: list[str],
    cwd: Path,
    environment: dict[str, str],
    *,
    echo_output: bool = True,
) -> subprocess.CompletedProcess[str]:
    """@brief Run one loader-isolated child and capture its diagnostics.

    @param command Executable and arguments passed directly without a shell.
    @param cwd Existing working directory for the child.
    @param environment Explicit sanitized loader environment.
    @param echo_output Whether captured output is copied to test diagnostics.
    @return Captured successful process result.
    @throws OSError If process creation fails.
    @throws RuntimeError If the child exits with a nonzero status.
    @note No call site may fall back to implicit environment inheritance.
    """

    print("$ " + " ".join(command), flush=True)
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=environment,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if echo_output:
        print(completed.stdout, end="", flush=True)
    if completed.returncode != 0:
        raise RuntimeError(
            f"command exited with {completed.returncode}: {' '.join(command)}"
        )
    return completed


def remove_transient_tree(path: Path, build_root: Path) -> None:
    """@brief Remove one validated transient tree below the producer build.

    The helper resolves the existing producer root, rejects symlink inputs and
    paths outside that root, then recursively removes the target when present.

    @param path Candidate tree; it must be a non-symlink strict descendant of
      ``build_root`` after canonical resolution.
    @param build_root Existing producer build tree that bounds deletion.
    @return None after the target is absent.
    @throws RuntimeError If ``path`` is a symlink, resolves to ``build_root``,
      or resolves outside ``build_root``.
    @throws OSError If root resolution, path inspection, or recursive removal
      fails.
    @note A missing validated target is a no-op. The producer root itself is
      never removed, and an existing target is deleted recursively.
    """

    resolved_root = build_root.resolve(strict=True)
    unresolved = path.absolute()
    if unresolved.is_symlink():
        raise RuntimeError(f"refusing symlink smoke root: {unresolved}")
    resolved = unresolved.resolve(strict=False)
    if resolved == resolved_root or resolved_root not in resolved.parents:
        raise RuntimeError(f"unsafe smoke root: {resolved}")
    if resolved.exists():
        shutil.rmtree(resolved)


def ipc_consumer_source() -> str:
    """@brief Build the complete installed IPC-only consumer translation unit.

    @return C++17 source that runs safe disconnected lifecycle behavior and
      retains one non-executed branch referencing every typed Client and Host
      operation.
    @throws None The source is an immutable in-memory string.
    @note The ordinary no-argument run creates ``create_ipc_host(\"\")`` but
      never starts, discovers, or connects to a daemon. The argument-only
      branch exists solely to force complete external symbol resolution.
    """

    return """#include <memory>
#include <optional>
#include <photospider/ipc/client.hpp>
#include <photospider/ipc/host.hpp>
#include <photospider/ipc/protocol.hpp>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

/**
 * @brief Detects a public ``cancel_compute`` compatibility member.
 * @tparam T Installed Host or Client contract under inspection.
 * @tparam Probe Substitution-only member-address expression.
 * @throws Nothing.
 * @note Protocol v2 has no public wire-cancellation compatibility shim.
 */
template <typename T, typename Probe = void>
struct HasCancelCompute : std::false_type {};

/** @copydoc HasCancelCompute */
template <typename T>
struct HasCancelCompute<T, std::void_t<decltype(&T::cancel_compute)>>
    : std::true_type {};  // NOLINT(whitespace/indent_namespace)

static_assert(!HasCancelCompute<ps::Host>::value,
              "installed Host must expose no cancellation shim");
static_assert(!HasCancelCompute<ps::ipc::Client>::value,
              "installed Client must expose no cancellation shim");
static_assert(static_cast<int>(ps::ipc::ComputeJobState::Queued) == 0 &&
                  static_cast<int>(ps::ipc::ComputeJobState::Running) == 1 &&
                  static_cast<int>(ps::ipc::ComputeJobState::Succeeded) == 2 &&
                  static_cast<int>(ps::ipc::ComputeJobState::Failed) == 3,
              "IPC v2 compute state inventory must remain unchanged");
static_assert(static_cast<int>(ps::OperationErrorDomain::None) == 0 &&
                  static_cast<int>(ps::OperationErrorDomain::Transport) == 1 &&
                  static_cast<int>(ps::OperationErrorDomain::Protocol) == 2 &&
                  static_cast<int>(ps::OperationErrorDomain::Graph) == 3 &&
                  static_cast<int>(ps::OperationErrorDomain::Daemon) == 4,
              "public status-domain inventory must remain unchanged");

/**
 * @brief References every typed Client call and non-destructor Host virtual.
 * @param client Disconnected direct client used only for link references.
 * @param host Complete IPC Host used only for virtual dispatch references.
 * @return Zero after all calls; the package smoke never executes this branch.
 * @throws Whatever a referenced operation throws if a caller deliberately
 *         executes the reference-only branch.
 * @note Keeping calls in one runtime branch forces the external executable to
 *       resolve every current installed IPC client and adapter symbol.
 */
int reference_complete_surface(ps::ipc::Client& client, ps::Host& host) {
  const ps::ipc::IpcSessionId ipc_session{};
  const ps::ipc::ComputeRequestId compute_id{};
  const ps::GraphSessionId host_session{};
  const ps::GraphLoadRequest load_request{};
  const ps::ipc::ComputeSubmitRequest submit_request{};
  const ps::HostComputeRequest compute_request{};
  const ps::PixelRect roi{};
  const ps::HostPolicyConfig policy_config{};
  const ps::HostExecutionConfig execution_config{};
  const std::vector<std::string> paths{};
  const std::string text;
  constexpr ps::NodeId node{0};
  constexpr ps::ComputeIntent intent = ps::ComputeIntent::GlobalHighPrecision;
  constexpr ps::DirtyDomain dirty_domain = ps::DirtyDomain::HighPrecision;

  (void)client.ping();
  (void)client.version();
  (void)client.load_graph(load_request);
  (void)client.close_graph(ipc_session);
  (void)client.list_graphs();
  (void)client.inspect_graph(ipc_session);
  (void)client.inspect_node(ipc_session, node);
  (void)client.inspect_dependency_tree(ipc_session, std::nullopt, false);
  (void)client.reload_graph(ipc_session, text);
  (void)client.save_graph(ipc_session, text);
  (void)client.clear_graph(ipc_session);
  (void)client.get_node_yaml(ipc_session, node);
  (void)client.set_node_yaml(ipc_session, node, text);
  (void)client.list_node_ids(ipc_session);
  (void)client.ending_nodes(ipc_session);
  (void)client.traversal_orders(ipc_session);
  (void)client.traversal_details(ipc_session);
  (void)client.trees_containing_node(ipc_session, node);
  (void)client.project_roi(ipc_session, node, roi, node);
  (void)client.project_roi_backward(ipc_session, node, roi, node);
  (void)client.dirty_region_snapshot(ipc_session);
  (void)client.compute_planning_snapshot(ipc_session);
  (void)client.recent_compute_planning_snapshots(ipc_session);
  (void)client.submit_compute(submit_request);
  (void)client.compute_status(compute_id);
  (void)client.compute_result(compute_id);
  (void)client.release_compute(compute_id);
  (void)client.timing(ipc_session);
  (void)client.last_io_time(ipc_session);
  (void)client.last_error(ipc_session);
  (void)client.begin_dirty_source(ipc_session, node, dirty_domain, roi);
  (void)client.update_dirty_source(ipc_session, node, dirty_domain, roi);
  (void)client.end_dirty_source(ipc_session, node, dirty_domain);
  (void)client.drain_compute_events(ipc_session, 1);
  (void)client.clear_cache(ipc_session);
  (void)client.clear_drive_cache(ipc_session);
  (void)client.clear_memory_cache(ipc_session);
  (void)client.cache_all_nodes(ipc_session, text);
  (void)client.free_transient_memory(ipc_session);
  (void)client.synchronize_disk_cache(ipc_session, text);
  (void)client.plugins_load_report(paths);
  (void)client.plugins_unload_all();
  (void)client.seed_builtin_ops();
  (void)client.ops_sources();
  (void)client.ops_combined_keys();
  (void)client.ops_combined_sources();
  (void)client.policy_available_types();
  (void)client.policy_description(text);
  (void)client.policy_scan(paths);
  (void)client.policy_load(text);
  (void)client.policy_loaded_plugins();
  (void)client.configure_policy_defaults(policy_config);
  (void)client.policy_info(ps::PolicyClass::Interactive);
  (void)client.replace_policy(ps::PolicyClass::Interactive, text);
  (void)client.execution_available_types();
  (void)client.execution_description(text);
  (void)client.configure_execution_defaults(execution_config);
  (void)client.execution_info(ipc_session, intent);
  (void)client.replace_execution(ipc_session, intent, text);
  (void)client.execution_trace(ipc_session, 0, 1);

  (void)host.load_graph(load_request);
  (void)host.close_graph(host_session);
  (void)host.list_graphs();
  (void)host.reload_graph(host_session, text);
  (void)host.save_graph(host_session, text);
  (void)host.clear_graph(host_session);
  (void)host.compute(compute_request);
  (void)host.compute_async(compute_request);
  (void)host.compute_and_get_values(compute_request);
  (void)host.timing(host_session);
  (void)host.last_io_time(host_session);
  (void)host.last_error(host_session);
  (void)host.list_node_ids(host_session);
  (void)host.ending_nodes(host_session);
  (void)host.get_node_yaml(host_session, node);
  (void)host.set_node_yaml(host_session, node, text);
  (void)host.inspect_node(host_session, node);
  (void)host.inspect_graph(host_session);
  (void)host.dependency_tree(host_session, std::nullopt, false);
  (void)host.traversal_orders(host_session);
  (void)host.traversal_details(host_session);
  (void)host.trees_containing_node(host_session, node);
  (void)host.project_roi(host_session, node, roi, node);
  (void)host.project_roi_backward(host_session, node, roi, node);
  (void)host.dirty_region_snapshot(host_session);
  (void)host.compute_planning_snapshot(host_session);
  (void)host.recent_compute_planning_snapshots(host_session);
  (void)host.begin_dirty_source(host_session, node, dirty_domain, roi);
  (void)host.update_dirty_source(host_session, node, dirty_domain, roi);
  (void)host.end_dirty_source(host_session, node, dirty_domain);
  (void)host.drain_compute_events(host_session, 1);
  (void)host.execution_trace(host_session, 0, 1);
  (void)host.clear_cache(host_session);
  (void)host.clear_drive_cache(host_session);
  (void)host.clear_memory_cache(host_session);
  (void)host.cache_all_nodes(host_session, text);
  (void)host.free_transient_memory(host_session);
  (void)host.synchronize_disk_cache(host_session, text);
  (void)host.plugins_load_report(paths);
  (void)host.plugins_load(paths);
  (void)host.plugins_unload_all();
  (void)host.seed_builtin_ops();
  (void)host.ops_sources();
  (void)host.ops_combined_keys();
  (void)host.ops_combined_sources();
  (void)host.policy_available_types();
  (void)host.policy_description(text);
  (void)host.policy_scan(paths);
  (void)host.policy_load(text);
  (void)host.policy_loaded_plugins();
  (void)host.configure_policy_defaults(policy_config);
  (void)host.policy_info(ps::PolicyClass::Interactive);
  (void)host.replace_policy(ps::PolicyClass::Interactive, text);
  (void)host.execution_available_types();
  (void)host.execution_description(text);
  (void)host.configure_execution_defaults(execution_config);
  (void)host.execution_info(host_session, intent);
  (void)host.replace_execution(host_session, intent, text);
  return 0;
}

}  // namespace

/**
 * @brief Exercises safe no-daemon lifecycle behavior for the installed target.
 * @param argc Process argument count; extra arguments select reference-only
 *        calls and are never supplied by the smoke test.
 * @param argv Process argument vector, unused by the no-argument smoke path.
 * @return Zero only when lifecycle and factory behavior match the contract.
 * @throws std::bad_alloc if Client or IPC Host construction exhausts memory.
 * @throws std::system_error if Host polling synchronization initialization
 *         fails.
 */
int main(int argc, char** argv) {
  (void)argv;
  ps::ipc::Client initial;
  ps::ipc::Client moved(std::move(initial));
  ps::ipc::Client client;
  client = std::move(moved);
  const ps::OperationStatus status = client.connect("");
  client.disconnect();
  client.disconnect();
  const bool disconnected = !client.connected();

  std::unique_ptr<ps::Host> host = ps::ipc::create_ipc_host("");
  if (!host) {
    return 2;
  }
  const ps::ipc::ComputeJobSnapshot default_job;
  if (default_job.cancellable) {
    return 3;
  }
  if (argc > 1) {
    return reference_complete_surface(client, *host);
  }
  return !status.ok && status.domain == ps::OperationErrorDomain::Transport &&
                 disconnected
             ? 0
             : 1;
}
"""


def complete_surface_inventory(source: str) -> dict[str, list[str]]:
    """@brief Extract independent Client and Host calls from the C++ harness.

    @param source Generated external IPC consumer translation unit.
    @return Ordered ``client`` and ``host`` method-name observations.
    @throws RuntimeError If the reference-only function markers are absent.
    @note Extraction does not consult the normative tuples, so omission,
      replacement, or duplication changes the observation independently.
    """

    marker = "int reference_complete_surface"
    end_marker = "\n}\n\n}  // namespace"
    if marker not in source or end_marker not in source:
        raise RuntimeError("complete IPC surface function markers are missing")
    body = source.split(marker, 1)[1].split(end_marker, 1)[0]
    return {
        name: re.findall(rf"\b{name}\.([A-Za-z_]\w*)\s*\(", body)
        for name in ("client", "host")
    }


def validate_complete_surface_inventory(source: str) -> dict[str, list[str]]:
    """@brief Enforce exact durable Client/Host symbol-reference inventories.

    @param source Generated external IPC consumer translation unit.
    @return Extracted inventories after count, set, and uniqueness checks.
    @throws RuntimeError If any expected call is omitted, duplicated, replaced,
      or if the normative tuples are not exactly 60 and 58 unique names.
    @note Successful C++ linking separately proves each observed name remains
      available from the installed package rather than a source checkout.
    """

    expected = {
        "client": (60, EXPECTED_CLIENT_METHODS),
        "host": (58, EXPECTED_HOST_METHODS),
    }
    observed = complete_surface_inventory(source)
    failures: list[str] = []
    for label, (count, names) in expected.items():
        if len(names) != count or len(set(names)) != count:
            failures.append(f"invalid normative {label} inventory")
        if len(observed[label]) != count:
            failures.append(f"{label} call count {len(observed[label])} != {count}")
        if len(set(observed[label])) != len(observed[label]):
            failures.append(f"duplicate {label} call reference")
        if set(observed[label]) != set(names):
            missing = sorted(set(names) - set(observed[label]))
            extra = sorted(set(observed[label]) - set(names))
            failures.append(
                f"{label} call set mismatch: missing={missing}, extra={extra}"
            )
    if failures:
        raise RuntimeError("; ".join(failures))
    return observed


def audit_installed_headers(prefix: Path, dependency_prefix: Path) -> tuple[str, ...]:
    """@brief Validate the exact installed public-header declaration boundary.

    @param prefix Isolated PhotospiderDaemon install prefix.
    @param dependency_prefix Isolated installed Photospider dependency prefix.
    @return Exact sorted daemon public-header paths after all checks pass.
    @throws OSError If an installed header cannot be read.
    @throws RuntimeError If inventory, direct includes, declarations, or public
      dependency-header ownership differs from the frozen boundary.
    @note Exact positive include inventories make private, JSON, socket, and
      backend includes fail without trying to enumerate every private spelling.
    """

    installed_headers = tuple(
        sorted(
            path.relative_to(prefix).as_posix()
            for path in (prefix / "include" / "photospider" / "ipc").glob(
                "*.hpp"
            )
        )
    )
    if installed_headers != EXPECTED_PUBLIC_HEADERS:
        raise RuntimeError(
            f"installed public header inventory mismatch: {installed_headers}"
        )

    combined_text: list[str] = []
    for header in installed_headers:
        text = (prefix / header).read_text(encoding="utf-8")
        combined_text.append(text)
        directives = re.findall(r"^\s*#\s*include\b[^\n]*", text, re.MULTILINE)
        includes = re.findall(
            r"^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]", text, re.MULTILINE
        )
        if len(directives) != len(includes):
            raise RuntimeError(f"unparsed include directive in {header}")
        if tuple(includes) != EXPECTED_DIRECT_INCLUDES[header]:
            raise RuntimeError(
                f"public direct include inventory mismatch in {header}: {includes}"
            )
        if len(includes) != len(set(includes)):
            raise RuntimeError(f"duplicate public include in {header}")
        for include in includes:
            if not include.startswith("photospider/"):
                continue
            owner = prefix if include.startswith("photospider/ipc/") else dependency_prefix
            installed_dependency = owner / "include" / include
            if not installed_dependency.is_file():
                raise RuntimeError(
                    f"public include is not installed by its owner: {include}"
                )

    all_header_text = "\n".join(combined_text)
    forbidden = sorted(
        label
        for label, pattern in FORBIDDEN_PUBLIC_DECLARATION_PATTERNS.items()
        if re.search(pattern, all_header_text, re.IGNORECASE)
    )
    if forbidden:
        raise RuntimeError(f"installed public headers expose private declarations: {forbidden}")
    return installed_headers


def write_consumer(source_dir: Path, headers: tuple[str, ...]) -> None:
    """@brief Write header-isolation and complete-surface external consumers.

    @param source_dir Empty transient external project source directory.
    @param headers Exact installed daemon public-header inventory.
    @return None after all CMake and C++ sources are present.
    @throws OSError If transient source creation fails.
    @throws RuntimeError If the generated complete-surface inventory is not
      exactly 60 Client and 58 Host names with no duplicates.
    @note Every translation unit links only ``PhotospiderDaemon::client``;
      neither the embedded kernel nor private daemon targets are imported.
    """

    source_dir.mkdir(parents=True)
    target_rows: list[str] = []
    for index, header in enumerate(headers):
        target = f"daemon_public_header_{index}"
        source_name = f"{target}.cpp"
        include = header.removeprefix("include/")
        (source_dir / source_name).write_text(
            f"""#include <{include}>

/**
 * @brief Supplies one standalone compile anchor for
 * ``{include}``.
 * @return Zero after the header has compiled independently.
 * @throws Nothing.
 * @note This function is never linked into a runtime product.
 */
int {target}_anchor() {{
  return 0;
}}
""",
            encoding="utf-8",
        )
        target_rows.extend(
            [
                f"add_library({target} OBJECT {source_name})",
                f"target_compile_features({target} PRIVATE cxx_std_17)",
                f"target_link_libraries({target} PRIVATE PhotospiderDaemon::client)",
                f"assert_client_only({target})",
            ]
        )

    (source_dir / "CMakeLists.txt").write_text(
        "\n".join(
            [
                "cmake_minimum_required(VERSION 3.20)",
                "project(PhotospiderDaemonInstalledConsumer LANGUAGES CXX)",
                "find_package(PhotospiderDaemon CONFIG REQUIRED)",
                "if(NOT TARGET PhotospiderDaemon::client)",
                '  message(FATAL_ERROR "public daemon client target is absent")',
                "endif()",
                "if(NOT TARGET Photospider::operation_runtime OR",
                "   NOT TARGET Threads::Threads)",
                '  message(FATAL_ERROR "public client closure is incomplete")',
                "endif()",
                "if(TARGET Photospider::photospider OR",
                "   TARGET nlohmann_json::nlohmann_json OR",
                "   TARGET yaml-cpp::yaml-cpp OR",
                "   TARGET photospider_daemon_client_objects OR",
                "   TARGET photospider_daemon_server_internal)",
                '  message(FATAL_ERROR "consumer discovered backend/private targets")',
                "endif()",
                "get_target_property(_client_links",
                "  PhotospiderDaemon::client INTERFACE_LINK_LIBRARIES)",
                "set(_expected_client_links",
                "  Photospider::operation_runtime Threads::Threads)",
                "set(_observed_client_links ${_client_links})",
                "list(SORT _expected_client_links)",
                "list(SORT _observed_client_links)",
                "if(NOT _observed_client_links STREQUAL _expected_client_links)",
                '  message(FATAL_ERROR "unexpected public client link closure: ${_client_links}")',
                "endif()",
                "function(assert_client_only target_name)",
                "  get_target_property(_links ${target_name} LINK_LIBRARIES)",
                '  if(NOT _links STREQUAL "PhotospiderDaemon::client")',
                '    message(FATAL_ERROR "${target_name} links unexpected targets: ${_links}")',
                "  endif()",
                "endfunction()",
                *target_rows,
                "add_executable(daemon_consumer main.cpp)",
                "target_compile_features(daemon_consumer PRIVATE cxx_std_17)",
                "target_link_libraries(daemon_consumer",
                "  PRIVATE PhotospiderDaemon::client)",
                "assert_client_only(daemon_consumer)",
                "",
            ]
        ),
        encoding="utf-8",
    )
    source = ipc_consumer_source()
    inventories = validate_complete_surface_inventory(source)
    print(
        "complete installed surface inventory: "
        f"Client={len(inventories['client'])}, Host={len(inventories['host'])}",
        flush=True,
    )
    (source_dir / "main.cpp").write_text(source, encoding="utf-8")


def path_is_within(path: Path, root: Path) -> bool:
    """@brief Test canonical path ownership without string-prefix ambiguity.

    @param path Canonical dependency or executable path.
    @param root Canonical ownership root.
    @return True when ``path`` equals ``root`` or is its descendant.
    @throws None Canonicalization is completed by callers.
    @note Component ancestry prevents sibling names with common prefixes from
      being accepted as installed ownership.
    """

    return path == root or root in path.parents


def dependency_prefix_from_package_dir(package_dir: Path) -> Path:
    """@brief Derive one installed prefix from a Photospider config directory.

    @param package_dir Canonical ``<prefix>/<libdir>/cmake/Photospider`` path.
    @return Canonical installed Photospider prefix.
    @throws RuntimeError If the directory is not an installed package layout.
    @note The nearest ancestor owning the installed public Host header is the
      prefix, including multi-component GNUInstallDirs library directories.
      Source/build package layouts are rejected later by ownership roots.
    """

    if package_dir.name != "Photospider" or package_dir.parent.name != "cmake":
        raise RuntimeError(
            f"Photospider_DIR is not an installed package directory: {package_dir}"
        )
    prefix_candidates = [
        parent.resolve(strict=True)
        for parent in package_dir.parents
        if (parent / "include" / "photospider" / "host" / "host.hpp").is_file()
    ]
    if len(prefix_candidates) != 1:
        raise RuntimeError(
            "Photospider_DIR does not identify one installed public-header prefix: "
            f"{prefix_candidates}"
        )
    return prefix_candidates[0]


def expand_macho_loader_token(value: str, binary: Path) -> Path:
    """@brief Expand one Mach-O executable-relative loader path.

    @param value Absolute path or path beginning with ``@loader_path`` or
      ``@executable_path``.
    @param binary Canonical installed executable being inspected.
    @return Expanded absolute path without requiring the target to exist.
    @throws RuntimeError If an unsupported Mach-O token is supplied.
    @note ``@rpath`` is expanded separately against every recorded LC_RPATH.
    """

    for token in ("@loader_path", "@executable_path"):
        if value == token:
            return binary.parent
        prefix = token + "/"
        if value.startswith(prefix):
            return binary.parent / value.removeprefix(prefix)
    candidate = Path(value)
    if candidate.is_absolute():
        return candidate
    raise RuntimeError(f"unsupported Mach-O loader token: {value}")


def darwin_loader_paths(
    binary: Path, cwd: Path, environment: dict[str, str]
) -> list[Path]:
    """@brief Resolve all ``otool`` dependency records to real files.

    @param binary Canonical installed executable or external consumer.
    @param cwd Existing working directory for inspection commands.
    @param environment Explicit sanitized loader environment.
    @return Canonical existing dependency paths from ``otool -L``.
    @throws RuntimeError If metadata is malformed, a token is unsupported, or
      no recorded LC_RPATH resolves an ``@rpath`` dependency.
    @note The binary header is validated and excluded from dependency records.
    """

    linked = run_checked(["otool", "-L", str(binary)], cwd, environment).stdout
    records = linked.splitlines()
    if not records or records[0] != f"{binary}:":
        raise RuntimeError("otool output omitted the inspected binary header")
    load_commands = run_checked(
        ["otool", "-l", str(binary)],
        cwd,
        environment,
        echo_output=False,
    ).stdout
    rpaths = re.findall(
        r"^\s*path\s+(.+?)\s+\(offset\s+\d+\)\s*$",
        load_commands,
        re.MULTILINE,
    )

    resolved: list[Path] = []
    for record in records[1:]:
        match = re.match(r"^\s*(.+?)\s+\(compatibility version ", record)
        if match is None:
            raise RuntimeError(f"unparsed otool dependency record: {record}")
        install_name = match.group(1)
        if install_name.startswith(("/usr/lib/", "/System/Library/")):
            # Modern Darwin can retain these canonical install names only in
            # the dyld shared cache, so no standalone filesystem inode exists.
            resolved.append(Path(install_name))
            continue
        candidates: list[Path]
        if install_name.startswith("@rpath/"):
            suffix = install_name.removeprefix("@rpath/")
            candidates = [
                expand_macho_loader_token(rpath, binary) / suffix
                for rpath in rpaths
            ]
        else:
            candidates = [expand_macho_loader_token(install_name, binary)]
        existing = [candidate for candidate in candidates if candidate.is_file()]
        if not existing:
            raise RuntimeError(
                f"Mach-O dependency does not resolve from installed RPATH: {install_name}"
            )
        resolved.append(existing[0].resolve(strict=True))
    return resolved


def linux_loader_paths(
    binary: Path, cwd: Path, environment: dict[str, str]
) -> list[Path]:
    """@brief Resolve all absolute dependency records reported by ``ldd``.

    @param binary Canonical installed executable or external consumer.
    @param cwd Existing working directory for the inspection command.
    @param environment Explicit sanitized loader environment.
    @return Canonical existing absolute dependency paths.
    @throws RuntimeError If ``ldd`` reports an unresolved dependency or an
      absolute dependency path no longer exists.
    @note Synthetic records such as ``linux-vdso`` have no filesystem owner and
      are intentionally omitted.
    """

    output = run_checked(["ldd", str(binary)], cwd, environment).stdout
    resolved: list[Path] = []
    for record in output.splitlines():
        if "=> not found" in record:
            raise RuntimeError(f"ldd reported an unresolved dependency: {record}")
        candidate = ""
        if "=>" in record:
            candidate = record.split("=>", 1)[1].strip().split(" (", 1)[0]
        else:
            match = re.match(r"^\s*(/.*?)\s+\(0x[0-9a-fA-F]+\)\s*$", record)
            if match is not None:
                candidate = match.group(1)
        if candidate.startswith("/"):
            resolved.append(Path(candidate).resolve(strict=True))
    return resolved


def validate_resolved_loader_paths(
    *,
    binary_label: str,
    resolved_paths: list[Path],
    expected_dependency_prefix: Path,
    forbidden_roots: tuple[Path, ...],
    allowed_roots: tuple[Path, ...],
) -> None:
    """@brief Require real operation-runtime ownership by one installed prefix.

    @param binary_label Stable diagnostic name for the inspected executable.
    @param resolved_paths Canonical loader dependency records.
    @param expected_dependency_prefix Canonical installed Photospider prefix.
    @param forbidden_roots Source, build, and sibling checkout roots.
    @param allowed_roots Explicit installed prefixes that may live below a
      transient outer build root.
    @return None after exact runtime ownership and residue checks pass.
    @throws RuntimeError If the operation runtime is absent, duplicated,
      outside the expected prefix, or any dependency resolves from a forbidden
      non-installed root.
    @note Allowed installed prefixes take precedence over an enclosing
      transient producer-build root; arbitrary sibling/build paths do not.
    """

    runtimes = [
        path
        for path in resolved_paths
        if "photospider_operation_runtime" in path.name
    ]
    if len(runtimes) != 1:
        raise RuntimeError(
            f"{binary_label} operation runtime loader count is {len(runtimes)}, expected 1"
        )
    if not path_is_within(runtimes[0], expected_dependency_prefix):
        raise RuntimeError(
            f"{binary_label} operation runtime is outside the expected installed prefix: "
            f"{runtimes[0]}"
        )
    for path in resolved_paths:
        forbidden = any(path_is_within(path, root) for root in forbidden_roots)
        allowed = any(path_is_within(path, root) for root in allowed_roots)
        if forbidden and not allowed:
            raise RuntimeError(
                f"{binary_label} loader resolved a source/build/sibling path: {path}"
            )


def inspect_loader_resolution(
    *,
    binary: Path,
    label: str,
    cwd: Path,
    environment: dict[str, str],
    expected_dependency_prefix: Path,
    forbidden_roots: tuple[Path, ...],
    allowed_roots: tuple[Path, ...],
) -> None:
    """@brief Inspect and validate one real installed loader dependency graph.

    @param binary Canonical executable to inspect.
    @param label Stable diagnostic label.
    @param cwd Existing working directory for loader tools.
    @param environment Explicit sanitized loader environment.
    @param expected_dependency_prefix Installed Photospider owner prefix.
    @param forbidden_roots Source, build, and sibling roots that must not own
      any resolved dependency.
    @param allowed_roots Installed-prefix exceptions inside transient roots.
    @return None after platform inspection and ownership validation pass.
    @throws RuntimeError If the platform is unsupported or any loader invariant
      fails closed.
    @note Linux uses real ``ldd`` resolution; Darwin resolves each ``otool -L``
      install name through the executable's recorded LC_RPATH entries.
    """

    inspected_system = platform.system()
    if inspected_system == "Darwin":
        paths = darwin_loader_paths(binary, cwd, environment)
    elif inspected_system == "Linux":
        paths = linux_loader_paths(binary, cwd, environment)
    else:
        raise RuntimeError(f"loader inspection is unsupported on {inspected_system}")
    validate_resolved_loader_paths(
        binary_label=label,
        resolved_paths=paths,
        expected_dependency_prefix=expected_dependency_prefix,
        forbidden_roots=forbidden_roots,
        allowed_roots=allowed_roots,
    )
    print(f"{label} loader records resolve from installed prefixes", flush=True)


def run_daemon_help(
    daemon: Path, cwd: Path, environment: dict[str, str]
) -> None:
    """@brief Execute installed help under the sanitized loader boundary.

    @param daemon Canonical installed ``photospiderd`` executable.
    @param cwd Existing isolated working directory.
    @param environment Explicit sanitized loader environment.
    @return None after required capability text is observed.
    @throws RuntimeError If execution fails or required text is absent.
    @note Help starts no daemon lifecycle and creates no socket.
    """

    completed = run_checked([str(daemon), "--help"], cwd, environment)
    required = (
        "protocol version 2 local Unix socket daemon",
        "foreground same-user local Unix-domain sidecar",
    )
    for fragment in required:
        if fragment not in completed.stdout:
            raise RuntimeError(f"installed help omitted: {fragment}")


def main() -> int:
    """@brief Install and validate the standalone daemon package boundary.

    @return Zero after export, consumer, runtime, and loader checks pass.
    @throws OSError If filesystem access or a child process cannot start.
    @throws RuntimeError If any package, runtime, or loader invariant fails.
    @note Every child receives one explicit loader-sanitized environment. Real
      consumer and daemon dependency records must resolve the Photospider
      operation runtime from the expected installed dependency prefix.
    """

    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--producer-build", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--generator", default="")
    parser.add_argument("--config", default="")
    args = parser.parse_args()

    source = Path(args.source).resolve(strict=True)
    producer_build = Path(args.producer_build).resolve(strict=True)
    work = Path(args.work).absolute()
    environment = clean_loader_environment()
    if source == producer_build or source in producer_build.parents:
        raise RuntimeError("producer build must not contain the source tree")
    if "PHOTOSPIDER_DAEMON_TEST_PHOTOSPIDER_DIR" not in environment:
        raise RuntimeError("installed Photospider package directory is not configured")
    photospider_dir = Path(
        environment["PHOTOSPIDER_DAEMON_TEST_PHOTOSPIDER_DIR"]
    ).resolve(strict=True)
    dependency_prefix = dependency_prefix_from_package_dir(photospider_dir)
    sibling_source = source.parent / "photospider"
    dependency_forbidden = [source, producer_build]
    if sibling_source.exists():
        dependency_forbidden.append(sibling_source.resolve(strict=True))
    if any(
        path_is_within(dependency_prefix, root) for root in dependency_forbidden
    ):
        raise RuntimeError(
            "Photospider dependency prefix is inside a source/build/sibling tree: "
            f"{dependency_prefix}"
        )
    remove_transient_tree(work, producer_build)
    work.mkdir(parents=True)

    prefix = work / "prefix"
    consumer_source = work / "consumer-source"
    consumer_build = work / "consumer-build"
    try:
        install = [args.cmake, "--install", str(producer_build), "--prefix", str(prefix)]
        if args.config:
            install.extend(["--config", args.config])
        run_checked(install, work, environment)
        installed_prefix = prefix.resolve(strict=True)
        installed_headers = audit_installed_headers(
            installed_prefix, dependency_prefix
        )

        target_files = list(
            installed_prefix.glob("**/PhotospiderDaemonTargets*.cmake")
        )
        if not target_files:
            raise RuntimeError("installed target export is absent")
        exported_text = "\n".join(
            path.read_text(encoding="utf-8") for path in target_files
        )
        forbidden_export_fragments = (
            "src/lib",
            str(source),
            str(producer_build),
            "Photospider::photospider",
            "nlohmann_json",
            "photospider_daemon_client_objects",
            "photospider_daemon_server_internal",
        )
        if any(fragment in exported_text for fragment in forbidden_export_fragments):
            raise RuntimeError("installed target export leaked source-private paths")
        if "PhotospiderDaemon::client" not in exported_text:
            raise RuntimeError("installed target export omitted the public client")
        if (
            "Photospider::operation_runtime" not in exported_text
            or "Threads::Threads" not in exported_text
        ):
            raise RuntimeError("public client export omitted its real link closure")

        daemon_package_dirs = list(
            installed_prefix.glob("**/cmake/PhotospiderDaemon")
        )
        if len(daemon_package_dirs) != 1:
            raise RuntimeError(
                "installed daemon package directory inventory is not unique: "
                f"{daemon_package_dirs}"
            )
        write_consumer(consumer_source, installed_headers)
        configure = [
            args.cmake,
            "-S",
            str(consumer_source),
            "-B",
            str(consumer_build),
            f"-DPhotospiderDaemon_DIR={daemon_package_dirs[0]}",
            f"-DPhotospider_DIR={photospider_dir}",
        ]
        if args.generator:
            configure.extend(["-G", args.generator])
        if (
            args.config
            and "Multi-Config" not in args.generator
            and args.generator != "Xcode"
        ):
            configure.append(f"-DCMAKE_BUILD_TYPE={args.config}")
        run_checked(configure, work, environment)
        build = [args.cmake, "--build", str(consumer_build)]
        if args.config:
            build.extend(["--config", args.config])
        run_checked(build, work, environment)

        executable = consumer_build / "daemon_consumer"
        if args.config and (consumer_build / args.config / "daemon_consumer").exists():
            executable = consumer_build / args.config / "daemon_consumer"
        executable = executable.resolve(strict=True)
        run_checked([str(executable)], work, environment)

        daemon = installed_prefix / "bin" / "photospiderd"
        if not daemon.is_file():
            raise RuntimeError(f"installed daemon is absent: {daemon}")
        daemon = daemon.resolve(strict=True)
        run_daemon_help(daemon, work, environment)

        forbidden_roots = tuple(dependency_forbidden)
        allowed_roots = (installed_prefix, dependency_prefix)
        inspect_loader_resolution(
            binary=executable,
            label="installed external consumer",
            cwd=work,
            environment=environment,
            expected_dependency_prefix=dependency_prefix,
            forbidden_roots=forbidden_roots,
            allowed_roots=allowed_roots,
        )
        inspect_loader_resolution(
            binary=daemon,
            label="installed photospiderd",
            cwd=work,
            environment=environment,
            expected_dependency_prefix=dependency_prefix,
            forbidden_roots=forbidden_roots,
            allowed_roots=allowed_roots,
        )
    finally:
        remove_transient_tree(work, producer_build)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
