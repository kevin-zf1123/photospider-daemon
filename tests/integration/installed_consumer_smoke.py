#!/usr/bin/env python3
"""Install PhotospiderDaemon and validate its public package boundary."""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
from pathlib import Path


def run_checked(command: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    """Run one child command and return its captured successful result."""

    print("$ " + " ".join(command), flush=True)
    completed = subprocess.run(
        command,
        cwd=cwd,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    print(completed.stdout, end="", flush=True)
    if completed.returncode != 0:
        raise RuntimeError(
            f"command exited with {completed.returncode}: {' '.join(command)}"
        )
    return completed


def remove_transient_tree(path: Path, build_root: Path) -> None:
    """Remove one non-symlink strict descendant of the producer build tree."""

    resolved_root = build_root.resolve(strict=True)
    unresolved = path.absolute()
    if unresolved.is_symlink():
        raise RuntimeError(f"refusing symlink smoke root: {unresolved}")
    resolved = unresolved.resolve(strict=False)
    if resolved == resolved_root or resolved_root not in resolved.parents:
        raise RuntimeError(f"unsafe smoke root: {resolved}")
    if resolved.exists():
        shutil.rmtree(resolved)


def write_consumer(source_dir: Path) -> None:
    """Write one tiny public-header and public-target package consumer."""

    source_dir.mkdir(parents=True)
    (source_dir / "CMakeLists.txt").write_text(
        """cmake_minimum_required(VERSION 3.20)
project(PhotospiderDaemonInstalledConsumer LANGUAGES CXX)
find_package(PhotospiderDaemon REQUIRED)
add_executable(daemon_consumer main.cpp)
target_compile_features(daemon_consumer PRIVATE cxx_std_17)
target_link_libraries(daemon_consumer PRIVATE PhotospiderDaemon::client)
""",
        encoding="utf-8",
    )
    (source_dir / "main.cpp").write_text(
        """#include <photospider/ipc/client.hpp>
#include <photospider/ipc/host.hpp>
#include <photospider/ipc/protocol.hpp>

int main() {
  ps::ipc::Client client;
  return client.connected() ? 1 : 0;
}
""",
        encoding="utf-8",
    )


def clear_loader_overrides() -> dict[str, str]:
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


def run_daemon_help(daemon: Path, cwd: Path) -> None:
    """Execute the installed daemon without inherited loader overrides."""

    completed = subprocess.run(
        [str(daemon), "--help"],
        cwd=cwd,
        env=clear_loader_overrides(),
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    print(completed.stdout, end="", flush=True)
    if completed.returncode != 0:
        raise RuntimeError("installed photospiderd --help failed")
    required = (
        "protocol version 2 local Unix socket daemon",
        "foreground same-user local Unix-domain sidecar",
    )
    for fragment in required:
        if fragment not in completed.stdout:
            raise RuntimeError(f"installed help omitted: {fragment}")


def main() -> int:
    """Run install, export scan, external build, and installed runtime checks."""

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
    if source == producer_build or source in producer_build.parents:
        raise RuntimeError("producer build must not contain the source tree")
    remove_transient_tree(work, producer_build)
    work.mkdir(parents=True)

    prefix = work / "prefix"
    consumer_source = work / "consumer-source"
    consumer_build = work / "consumer-build"
    try:
        install = [args.cmake, "--install", str(producer_build), "--prefix", str(prefix)]
        if args.config:
            install.extend(["--config", args.config])
        run_checked(install, work)

        installed_headers = sorted(
            path.relative_to(prefix).as_posix()
            for path in (prefix / "include" / "photospider" / "ipc").glob("*.hpp")
        )
        expected_headers = [
            "include/photospider/ipc/client.hpp",
            "include/photospider/ipc/host.hpp",
            "include/photospider/ipc/protocol.hpp",
        ]
        if installed_headers != expected_headers:
            raise RuntimeError(
                f"installed public header inventory mismatch: {installed_headers}"
            )

        target_files = list(prefix.glob("**/PhotospiderDaemonTargets*.cmake"))
        if not target_files:
            raise RuntimeError("installed target export is absent")
        exported_text = "\n".join(
            path.read_text(encoding="utf-8") for path in target_files
        )
        if "src/lib" in exported_text or str(source) in exported_text:
            raise RuntimeError("installed target export leaked source-private paths")
        if "PhotospiderDaemon::client" not in exported_text:
            raise RuntimeError("installed target export omitted the public client")
        if "Photospider::operation_runtime" not in exported_text or "Threads::Threads" not in exported_text:
            raise RuntimeError("public client export omitted its real link closure")

        write_consumer(consumer_source)
        configure = [
            args.cmake,
            "-S",
            str(consumer_source),
            "-B",
            str(consumer_build),
            f"-DPhotospiderDaemon_DIR={next(prefix.glob('**/cmake/PhotospiderDaemon'))}",
            f"-DPhotospider_DIR={Path(os.environ['PHOTOSPIDER_DAEMON_TEST_PHOTOSPIDER_DIR'])}",
        ]
        if args.generator:
            configure.extend(["-G", args.generator])
        if args.config and "Multi-Config" not in args.generator and args.generator != "Xcode":
            configure.append(f"-DCMAKE_BUILD_TYPE={args.config}")
        run_checked(configure, work)
        build = [args.cmake, "--build", str(consumer_build)]
        if args.config:
            build.extend(["--config", args.config])
        run_checked(build, work)

        executable = consumer_build / "daemon_consumer"
        if args.config and (consumer_build / args.config / "daemon_consumer").exists():
            executable = consumer_build / args.config / "daemon_consumer"
        run_checked([str(executable)], work)

        daemon = prefix / "bin" / "photospiderd"
        if not daemon.is_file():
            raise RuntimeError(f"installed daemon is absent: {daemon}")
        run_daemon_help(daemon, work)

        inspect_command = ["otool", "-L", str(daemon)]
        if platform.system() == "Linux":
            inspect_command = ["ldd", str(daemon)]
        inspected = run_checked(inspect_command, work).stdout
        if str(source) in inspected or str(producer_build) in inspected:
            raise RuntimeError("installed daemon retained a build/source loader path")
    finally:
        remove_transient_tree(work, producer_build)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
