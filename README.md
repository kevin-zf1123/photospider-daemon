# Photospider Daemon

`photospider-daemon` is a same-user, local, non-persistent orchestration layer
for the installed Photospider compiler/executor. The breaking boundary is
governed by [ADR 0001](docs/adr/0001-breaking-product-boundary-scope-reset.md).

## Product boundary

- `PhotospiderDaemon::client` is the installed typed local-IPC client.
- `photospiderd` owns a peer-checked local socket, ephemeral Session/Job
  registries, bounded Sessions/Jobs/active handlers, runtime handler reaping,
  cancellation, temporary results, and shutdown.
- The daemon depends only on an isolated installed public Photospider package.
- It has no private kernel include, copied compiler/planner implementation,
  internal-IR wire encoding, plugin-path route, or reverse dependency.

Sessions are logical namespaces in one process and one user trust domain. Jobs
are ephemeral and use exactly:

```text
Queued -> Running -> Succeeded | Failed | Cancelled
```

`session.close` removes matching work still owned by the global queue and
completes its existing state transitions locally. It cooperatively cancels and
waits only for matching work already popped by a worker; an unrelated
Session's Running Job never participates. Once close wins, the Session remains
internally retained as closing: its submit and repeated close return
`NotFound`, it still consumes capacity, and the process-wide lifecycle lock is
released before its popped Jobs are awaited. Unrelated Session create, submit,
and close therefore continue; the closing Session releases capacity only after
its own work settles and the record is erased.

Restart clears every Session, Job, and result. There is no automatic retry,
attempt identity, checkpoint, recovery, durable result, receipt, tenant, or
remote service.

## Local IPC v3

The exact methods are:

1. `session.create`
2. `session.close`
3. `job.submit`
4. `job.status`
5. `job.cancel`
6. `job.result`
7. `job.release`
8. `daemon.info`
9. `daemon.shutdown`

Darwin and Linux use a Unix-domain stream socket. There is no IPC v2 adapter,
TCP, HTTP, gRPC, TLS, or remote endpoint. See
[IPC Protocol v3](docs/codebase-structure/IPC-Protocol-v3.md).

## Build

First install the reset Photospider kernel to an isolated prefix. Then:

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=/absolute/photospider-prefix \
  -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build --prefix /absolute/daemon-prefix
```

The daemon configure must fail if it can see only a source checkout or private
kernel headers. Tests use the public compile/execute/Value facade exported by
the installed package.

On Linux and Darwin, the installed `photospiderd` carries a portable
loader-relative search path to `../${CMAKE_INSTALL_LIBDIR}` and, when the
imported kernel is shared in a separate prefix, the actual non-system linked
kernel directory. Linux uses `$ORIGIN`; Darwin uses `@loader_path`. Windows
sets no RPATH. The installed-consumer gate clears `LD_LIBRARY_PATH`,
`DYLD_LIBRARY_PATH`, and `DYLD_FALLBACK_LIBRARY_PATH` before it executes the
installed runtime's `--help`, so a shared kernel cannot pass through ambient
developer loader state.

The executable accepts positive `--max-sessions`, `--max-jobs`,
`--max-concurrency`, and `--max-connections` bounds. Capacity rejection is a
typed `ResourceExhausted` response and creates no Session, Job, or handler
state.

## Installed client

```cmake
find_package(PhotospiderDaemon 0.2 CONFIG REQUIRED COMPONENTS client)
target_link_libraries(app PRIVATE PhotospiderDaemon::client)
```

The package exports only the typed client target. It also installs the
`photospiderd` runtime under `bin/`, but no executable/server CMake target is
exported. The client target is deliberately a static library even when the
producer is configured with `BUILD_SHARED_LIBS=ON`; the package promises no
shared client ABI. Codec, router, registry, transport, and server implementation
remain private.

## Correctness boundary

The daemon validates bounded binary frames, exact integer ranges, UTF-8,
request correlation, instance-scoped opaque ids, Job transitions, public kernel
result type/shape/Region/layout, cancellation publication,
descriptor/thread cleanup, and socket lifecycle. Unix peer credentials enforce
same-user connection acceptance. Socket-node mode is not an authentication
boundary: callers choose a suitably private parent directory, while the daemon
uses pathname generation only for fail-closed cleanup. These properties do not
create a tenant or remote-service product.

Malformed or oversized frames receive one typed protocol-error response and
then the connection closes. A recoverable valid v3 header keeps its request id
and method; otherwise the documented correlation sentinel is request id zero
with method `daemon.info`. A completely received failed sentinel returns its
typed status and disconnects the Client. Unix client/listener/accepted and
fixed-parent descriptors are close-on-exec; embedded-NUL path bytes are
rejected before any prefix can be used. See
[Testing and Validation](docs/development/Testing-and-Validation.md) for
sanitizer and manual fuzz commands.

## Delivery status

Public GitHub Issues maintain live delivery state.
[#15 daemon-local-orchestration](https://github.com/users/kevin-zf1123/projects/15)
is the maintainer operational view of those Issues. The checked-in
[Current Development Program](docs/development/Current-Development-Program.md)
records the audited baseline, current milestone, critical path, and active
leaf Issues without changing ADR 0001 or the IPC v3 contract.

The pre-reset IPC v2 source is available only through Git history and
`pre-breaking-scope-reset-2026-09-01`; it is not an active compatibility
contract.
