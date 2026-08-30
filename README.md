# Photospider Daemon

`photospider-daemon` is the sole repository for Photospider local IPC version
2: the typed public client, private framing/router/server implementation, the
foreground `photospiderd` sidecar, the authoritative protocol document, and
their maintained tests.

The source was history-preservingly extracted from Photospider commit
`f9fc3aefce45072c6fc6a856da11f20ff16ba00a`, archived in the original
repository by annotated tag `full-stack-archive-2026-08-30` (tag object
`bb876cbf76882bc0a9029956d6acc9ee3fbaae0e`). This identity is the migration
and four-cell compatibility baseline, not a general historical compatibility
promise.

## Product boundary

- `PhotospiderDaemon::client` is the only installed library target. It links
  `Photospider::operation_runtime` and `Threads::Threads` publicly and does not
  link the complete embedded kernel.
- `photospiderd` privately consumes `Photospider::photospider` from an installed
  Photospider package and lends exactly one public `ps::Host` to the private
  server/router.
- Codec, frame, socket, registry, output-store, router, and server targets are
  private. There is no public raw-protocol implementation package.
- The repository has no Photospider submodule, copied kernel implementation,
  sibling-checkout include, or cross-repository private header dependency.

The current product is a foreground, same-user, local Unix-domain sidecar for
Darwin and Linux. It is not a system service, multi-user service, remote
endpoint, or TCP server.

This repository is now in IPC v2 compatible-maintenance. It accepts fixes,
package/CI compatibility work, and lifecycle hardening for the existing
surface; it does not expand IPC v3, serialize kernel compiler IR, or take
ownership of kernel Job, policy, trust, isolation, worker, or evidence code.
Next-protocol design remains blocked on a stable kernel Compiler MVP.

## Version support

| Axis | Current value | Maintained rule |
| --- | --- | --- |
| Photospider package | 0.1.0 | exact producer/client dependency; package compatibility is same minor |
| PhotospiderDaemon package | 0.1.0 | package compatibility is same minor |
| IPC protocol | v2 | exact frozen 60-method surface; no v3 expansion |

Daemon pull-request CI pins the supported post-split kernel revision
`c656ac58046c1d7fdb40372ae575728f526c0f01`. The frozen full-stack commit above
is used only for the old side of the four-cell gate. See the
[version and CI compatibility contract](docs/Version-and-CI-Compatibility.md).

## Build

First install the supported post-split Photospider kernel package. For the
current 0.1 line, use the pinned supported revision, for example:

```bash
git clone https://github.com/kevin-zf1123/photospider.git /tmp/photospider-kernel
git -C /tmp/photospider-kernel checkout --detach \
  c656ac58046c1d7fdb40372ae575728f526c0f01
cmake -S /tmp/photospider-kernel -B /tmp/photospider-kernel-build \
  -DBUILD_TESTING=OFF -DPHOTOSPIDER_BUILD_GRAPH_CLI=OFF \
  -DPHOTOSPIDER_BUILD_SINGLE_TENANT_JOB=OFF \
  -DCMAKE_INSTALL_PREFIX=/tmp/photospider-prefix
cmake --build /tmp/photospider-kernel-build --target photospider -j
cmake --install /tmp/photospider-kernel-build
```

Then configure this standalone repository only against that installation:

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=/tmp/photospider-prefix \
  -DPHOTOSPIDER_DAEMON_DEPENDENCY_LIBDIR=/tmp/photospider-prefix/lib \
  -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build --prefix /tmp/photospider-daemon-prefix
```

`PHOTOSPIDER_DAEMON_DEPENDENCY_LIBDIR` records the exact installed kernel
runtime directory in `photospiderd`'s install RPATH. Package discovery also
requires `Photospider 0.1.0 EXACT`; changing the supported revision or version
tuple is deliberate compatibility work rather than an implicit main-branch
upgrade.

## Installed client

```cmake
find_package(PhotospiderDaemon CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE PhotospiderDaemon::client)
```

The package imports only the public client. It finds exact Photospider 0.1.0
`operation_runtime` and Threads transitively.

## Protocol and shutdown invariants

The wire remains protocol version 2 with the exact sorted 60-method inventory.
`compute.cancel` and `daemon.shutdown` are not methods and return
`method_not_found`; accepted jobs report `cancellable: false`. `SIGINT` and
`SIGTERM` use the established self-pipe path to stop admission, drain and join
accepted work, clean the proven socket identity, and exit successfully.

See [IPC-Protocol-v2.md](docs/codebase-structure/IPC-Protocol-v2.md) and its
[Chinese mirror](docs/codebase-structure/zh/IPC-Protocol-v2.zh.md).

## Frozen four-cell gate

The migration-only runner builds the same public probe against the archived
client package and this package, then executes old-old, old-new, new-old, and
new-new cells with separate sockets:

```bash
python3 tools/run_frozen_compatibility_gate.py \
  --source "$PWD" \
  --old-photospider-dir /tmp/old-prefix/lib/cmake/Photospider \
  --new-photospider-dir /tmp/photospider-prefix/lib/cmake/Photospider \
  --new-photospider-daemon-dir /tmp/new-prefix/lib/cmake/PhotospiderDaemon \
  --old-daemon /tmp/old-prefix/bin/photospiderd \
  --new-daemon /tmp/new-prefix/bin/photospiderd \
  --work /tmp/photospider-daemon-four-cell
```

The runner is intentionally not registered with CTest: it is bounded migration
evidence, while the ordinary unit, integration, package-consumer, and daemon
lifecycle tests remain the long-lived product gate.

Pull requests and maintained branch pushes run that pinned product gate on
Ubuntu and macOS. A weekly Ubuntu-only `compatible-main` job separately checks
current kernel `main`; it is a downstream drift signal in this repository and
is not a required check for every kernel pull request.
