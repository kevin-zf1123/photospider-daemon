#!/usr/bin/env python3
"""Build old/new public clients and run the frozen IPC v2 four-cell gate."""

from __future__ import annotations

import argparse
import json
import platform
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path

from loader_environment import clean_loader_environment


def run_checked(
    command: list[str], cwd: Path, environment: dict[str, str]
) -> None:
    """@brief Run one visible child with an explicit clean loader environment.

    @param command Executable and arguments passed directly without a shell.
    @param cwd Existing child working directory.
    @param environment Explicit environment produced by
      ``clean_loader_environment``.
    @return None after a zero child exit status.
    @throws OSError If process creation fails.
    @throws RuntimeError If the child exits with a nonzero status.
    @note Probe configure, build, and run calls cannot inherit developer loader
      overrides through this required parameter.
    """

    print("$ " + " ".join(command), flush=True)
    completed = subprocess.run(command, cwd=cwd, env=environment, check=False)
    if completed.returncode != 0:
        raise RuntimeError(
            f"command exited with {completed.returncode}: {' '.join(command)}"
        )


def remove_work_tree(path: Path, allowed_parent: Path) -> None:
    """Remove one validated non-symlink strict descendant of an allowed root."""

    unresolved = path.absolute()
    if unresolved.is_symlink():
        raise RuntimeError(f"refusing symlink work tree: {unresolved}")
    parent = allowed_parent.resolve(strict=True)
    resolved = unresolved.resolve(strict=False)
    if resolved == parent or parent not in resolved.parents:
        raise RuntimeError(f"unsafe compatibility work tree: {resolved}")
    if resolved.exists():
        shutil.rmtree(resolved)


def build_probe(
    *,
    kind: str,
    source: Path,
    build: Path,
    cmake: str,
    generator: str,
    config: str,
    osx_architectures: str,
    photospider_dir: Path,
    daemon_dir: Path | None,
    environment: dict[str, str],
) -> Path:
    """@brief Configure and build one probe against an installed client package.

    @param kind Stable ``old`` or ``new`` package label.
    @param source Canonical daemon repository containing the common probe.
    @param build Isolated probe build directory.
    @param cmake CMake executable selected by the caller.
    @param generator Optional CMake generator name.
    @param config Optional build configuration.
    @param osx_architectures Optional Darwin architecture list.
    @param photospider_dir Installed frozen Photospider package directory.
    @param daemon_dir Installed standalone daemon package directory for the new
      client, or None for the frozen old client.
    @param environment Explicit sanitized loader environment.
    @return Canonical built compatibility probe executable.
    @throws RuntimeError If configure/build fails or no executable is produced.
    @note Both configure and build use the same explicit environment.
    """

    configure = [
        cmake,
        "-S",
        str(source / "tests" / "interop"),
        "-B",
        str(build),
        f"-DPHOTOSPIDER_DAEMON_INTEROP_CLIENT={kind}",
        f"-DPhotospider_DIR={photospider_dir}",
    ]
    if daemon_dir is not None:
        configure.append(f"-DPhotospiderDaemon_DIR={daemon_dir}")
    if generator:
        configure.extend(["-G", generator])
    if platform.system() == "Darwin" and osx_architectures:
        configure.append(f"-DCMAKE_OSX_ARCHITECTURES={osx_architectures}")
    if config and "Multi-Config" not in generator and generator != "Xcode":
        configure.append(f"-DCMAKE_BUILD_TYPE={config}")
    run_checked(configure, source, environment)
    build_command = [cmake, "--build", str(build), "--target", "ipc_compat_probe"]
    if config:
        build_command.extend(["--config", config])
    run_checked(build_command, source, environment)

    candidates = [build / "ipc_compat_probe"]
    if config:
        candidates.insert(0, build / config / "ipc_compat_probe")
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise RuntimeError(f"{kind} compatibility probe executable is absent")


def read_exact(connection: socket.socket, size: int) -> bytes:
    """Read exactly one bounded byte count from a connected Unix socket."""

    chunks: list[bytes] = []
    remaining = size
    while remaining:
        chunk = connection.recv(remaining)
        if not chunk:
            raise RuntimeError("daemon closed a compatibility frame early")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def raw_call(socket_path: Path, method: str, request_id: str) -> dict[str, object]:
    """Perform one bounded protocol-v2 JSON frame call without a client library."""

    request = json.dumps(
        {
            "protocol_version": 2,
            "id": request_id,
            "method": method,
            "params": {},
        },
        separators=(",", ":"),
    ).encode("utf-8")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(2.0)
        connection.connect(str(socket_path))
        connection.sendall(struct.pack("!I", len(request)) + request)
        length = struct.unpack("!I", read_exact(connection, 4))[0]
        if length == 0 or length > 16 * 1024 * 1024:
            raise RuntimeError(f"daemon returned invalid frame length {length}")
        response = json.loads(read_exact(connection, length).decode("utf-8"))
    if not isinstance(response, dict) or response.get("id") != request_id:
        raise RuntimeError(f"uncorrelated response for {method}: {response}")
    return response


def wait_ready(process: subprocess.Popen[str], socket_path: Path) -> None:
    """Wait for one daemon to answer a correlated raw protocol-v2 ping."""

    deadline = time.monotonic() + 10.0
    last_error = "socket not ready"
    while time.monotonic() < deadline:
        if process.poll() is not None:
            output = process.communicate()[0]
            raise RuntimeError(
                f"daemon exited before readiness ({process.returncode}):\n{output}"
            )
        try:
            response = raw_call(socket_path, "daemon.ping", "readiness")
            if response.get("result", {}).get("pong") is True:
                return
            last_error = f"unexpected ping response: {response}"
        except (OSError, RuntimeError, json.JSONDecodeError) as error:
            last_error = str(error)
        time.sleep(0.02)
    raise RuntimeError(f"daemon readiness timed out: {last_error}")


def resolve_osx_architectures(
    requested: str, daemon: Path, environment: dict[str, str]
) -> str:
    """@brief Resolve a requested or installed-daemon Darwin architecture list.

    @param requested Explicit semicolon-separated architecture list, if any.
    @param daemon Canonical frozen daemon executable inspected when needed.
    @param environment Explicit sanitized loader environment for ``lipo``.
    @return Requested or inferred semicolon-separated architectures.
    @throws RuntimeError If Darwin metadata cannot be read.
    @note Non-Darwin callers return the requested string without a child.
    """

    if requested or platform.system() != "Darwin":
        return requested
    inspected = subprocess.run(
        ["lipo", "-archs", str(daemon)],
        env=environment,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    architectures = inspected.stdout.split()
    if inspected.returncode != 0 or not architectures:
        raise RuntimeError(
            "cannot infer the frozen daemon architecture; pass "
            "--osx-architectures explicitly:\n"
            + inspected.stdout
        )
    return ";".join(architectures)


def stop_daemon(process: subprocess.Popen[str], socket_path: Path) -> str:
    """Send SIGTERM, require graceful zero exit, and return captured output."""

    process.send_signal(signal.SIGTERM)
    try:
        output = process.communicate(timeout=15.0)[0]
    except subprocess.TimeoutExpired as error:
        process.kill()
        process.communicate(timeout=5.0)
        raise RuntimeError("daemon did not drain and join after SIGTERM") from error
    if process.returncode != 0:
        raise RuntimeError(
            f"daemon SIGTERM exit was {process.returncode}:\n{output}"
        )
    if socket_path.exists() or socket_path.is_symlink():
        raise RuntimeError("daemon left its socket pathname after SIGTERM")
    return output


def run_cell(
    label: str,
    probe: Path,
    daemon: Path,
    graph_yaml: Path,
    environment: dict[str, str],
) -> None:
    """@brief Run one isolated public-client/product-daemon compatibility cell.

    @param label Stable old/new cell label.
    @param probe Canonical installed-client probe executable.
    @param daemon Canonical installed product daemon executable.
    @param graph_yaml Canonical deterministic compatibility graph.
    @param environment Explicit sanitized loader environment used by both
      daemon and probe.
    @return None after correlated metadata, deep vertical behavior, negatives,
      and graceful signal shutdown pass.
    @throws RuntimeError If any cell invariant fails.
    @note Raw socket calls occur in-process and cannot redirect dynamic loading.
    """

    cell_root = Path(tempfile.mkdtemp(prefix=f"ps-{label}-", dir="/tmp"))
    cell_root.chmod(0o700)
    socket_path = cell_root / "daemon.sock"
    process = subprocess.Popen(
        [str(daemon), "--socket", str(socket_path)],
        cwd=cell_root,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    try:
        wait_ready(process, socket_path)
        run_checked(
            [str(probe), str(socket_path), str(graph_yaml), str(cell_root)],
            cell_root,
            environment,
        )
        for method in ("compute.cancel", "daemon.shutdown"):
            response = raw_call(socket_path, method, f"negative-{method}")
            error = response.get("error")
            if not isinstance(error, dict) or error.get("domain") != "protocol":
                raise RuntimeError(f"{method} did not return a protocol error")
            if error.get("code") != -32601 or error.get("name") != "method_not_found":
                raise RuntimeError(f"{method} was unexpectedly admitted: {response}")
        output = stop_daemon(process, socket_path)
        if output:
            print(output, end="", flush=True)
        print(f"{label}: PASS", flush=True)
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate(timeout=5.0)
        shutil.rmtree(cell_root)


def main() -> int:
    """@brief Build two probes and execute all four frozen-baseline cells.

    @return Zero after old-old, old-new, new-old, and new-new pass.
    @throws OSError If paths or child processes cannot be accessed.
    @throws RuntimeError If path safety, probe construction, metadata, deep
      behavior, negative routes, or graceful shutdown fails.
    @note One clean loader environment is captured before any child and passed
      to architecture inspection, both probe configure/build pairs, both
      product daemons, and every probe run.
    """

    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--old-photospider-dir", required=True)
    parser.add_argument("--new-photospider-daemon-dir", required=True)
    parser.add_argument("--old-daemon", required=True)
    parser.add_argument("--new-daemon", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--generator", default="")
    parser.add_argument("--config", default="")
    parser.add_argument(
        "--osx-architectures",
        default="",
    )
    args = parser.parse_args()

    source = Path(args.source).resolve(strict=True)
    environment = clean_loader_environment()
    allowed_parent = Path(args.work).absolute().parent.resolve(strict=True)
    work = Path(args.work).absolute()
    remove_work_tree(work, allowed_parent)
    work.mkdir(parents=True)
    try:
        old_daemon = Path(args.old_daemon).resolve(strict=True)
        new_daemon = Path(args.new_daemon).resolve(strict=True)
        osx_architectures = resolve_osx_architectures(
            args.osx_architectures, old_daemon, environment
        )
        old_probe = build_probe(
            kind="old",
            source=source,
            build=work / "old-client-build",
            cmake=args.cmake,
            generator=args.generator,
            config=args.config,
            osx_architectures=osx_architectures,
            photospider_dir=Path(args.old_photospider_dir).resolve(strict=True),
            daemon_dir=None,
            environment=environment,
        )
        new_probe = build_probe(
            kind="new",
            source=source,
            build=work / "new-client-build",
            cmake=args.cmake,
            generator=args.generator,
            config=args.config,
            osx_architectures=osx_architectures,
            photospider_dir=Path(args.old_photospider_dir).resolve(strict=True),
            daemon_dir=Path(args.new_photospider_daemon_dir).resolve(strict=True),
            environment=environment,
        )
        graph_yaml = (source / "tests/interop/frozen_v2_constant.yaml").resolve(
            strict=True
        )
        run_cell("old-old", old_probe, old_daemon, graph_yaml, environment)
        run_cell("old-new", old_probe, new_daemon, graph_yaml, environment)
        run_cell("new-old", new_probe, old_daemon, graph_yaml, environment)
        run_cell("new-new", new_probe, new_daemon, graph_yaml, environment)
        print("frozen IPC v2 four-cell compatibility gate: PASS", flush=True)
    finally:
        remove_work_tree(work, allowed_parent)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
