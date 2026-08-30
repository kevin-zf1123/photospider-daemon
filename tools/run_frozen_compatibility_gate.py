#!/usr/bin/env python3
"""Build old/new public clients and run the frozen IPC v2 four-cell gate."""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path


def run_checked(command: list[str], cwd: Path) -> None:
    """Run one visible child command and require a zero exit status."""

    print("$ " + " ".join(command), flush=True)
    completed = subprocess.run(command, cwd=cwd, check=False)
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
) -> Path:
    """Configure and build the common probe against one installed package."""

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
    run_checked(configure, source)
    build_command = [cmake, "--build", str(build), "--target", "ipc_compat_probe"]
    if config:
        build_command.extend(["--config", config])
    run_checked(build_command, source)

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


def clean_loader_environment() -> dict[str, str]:
    """Return an environment without developer loader override variables."""

    environment = dict(os.environ)
    for name in (
        "LD_LIBRARY_PATH",
        "LD_PRELOAD",
        "LIBPATH",
        "SHLIB_PATH",
        "DYLD_LIBRARY_PATH",
        "DYLD_FALLBACK_LIBRARY_PATH",
        "DYLD_FRAMEWORK_PATH",
        "DYLD_FALLBACK_FRAMEWORK_PATH",
        "DYLD_INSERT_LIBRARIES",
    ):
        environment.pop(name, None)
    return environment


def resolve_osx_architectures(requested: str, daemon: Path) -> str:
    """Resolve an explicit or installed-daemon Darwin architecture list."""

    if requested or platform.system() != "Darwin":
        return requested
    inspected = subprocess.run(
        ["lipo", "-archs", str(daemon)],
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
    label: str, probe: Path, daemon: Path, graph_yaml: Path
) -> None:
    """Run one isolated public-client to product-daemon compatibility cell."""

    cell_root = Path(tempfile.mkdtemp(prefix=f"ps-{label}-", dir="/tmp"))
    cell_root.chmod(0o700)
    socket_path = cell_root / "daemon.sock"
    process = subprocess.Popen(
        [str(daemon), "--socket", str(socket_path)],
        cwd=cell_root,
        env=clean_loader_environment(),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    try:
        wait_ready(process, socket_path)
        run_checked(
            [str(probe), str(socket_path), str(graph_yaml), str(cell_root)],
            cell_root,
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
    """Build two public probes and execute all four frozen-baseline cells."""

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
    allowed_parent = Path(args.work).absolute().parent.resolve(strict=True)
    work = Path(args.work).absolute()
    remove_work_tree(work, allowed_parent)
    work.mkdir(parents=True)
    try:
        old_daemon = Path(args.old_daemon).resolve(strict=True)
        new_daemon = Path(args.new_daemon).resolve(strict=True)
        osx_architectures = resolve_osx_architectures(
            args.osx_architectures, old_daemon
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
        )
        graph_yaml = (source / "tests/interop/frozen_v2_constant.yaml").resolve(
            strict=True
        )
        run_cell("old-old", old_probe, old_daemon, graph_yaml)
        run_cell("old-new", old_probe, new_daemon, graph_yaml)
        run_cell("new-old", new_probe, old_daemon, graph_yaml)
        run_cell("new-new", new_probe, new_daemon, graph_yaml)
        print("frozen IPC v2 four-cell compatibility gate: PASS", flush=True)
    finally:
        remove_work_tree(work, allowed_parent)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
