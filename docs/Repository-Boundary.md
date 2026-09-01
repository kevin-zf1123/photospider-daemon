# Repository and Package Boundary

## Decision

`photospider-daemon` exclusively owns same-user local IPC v3, ephemeral
Session/Job orchestration, `photospiderd`, the typed client package, and their
tests. `photospider` exclusively owns WorkflowDocument, typed/optimized IR,
operation traits, compiler/optimizer/planner, local CPU/GPU execution,
Value/Region/layout/memory, and operation/provider runtime.

```text
PhotospiderDaemon::client -> installed Photospider::kernel + private IPC implementation
photospiderd -> PhotospiderDaemon::client + private server/orchestration code
```

The daemon does not link a sibling source target or include a private header.
It does not copy compiler/planner logic or serialize internal IR. The kernel
does not depend on the daemon.

## Public package

The daemon package exports only typed client headers and
`PhotospiderDaemon::client`. It separately installs `bin/photospiderd` for
runtime use; the executable is intentionally absent from
`PhotospiderDaemonTargets.cmake`. Frame, codec, Unix socket, router,
Session/Job registries, and server are private. No raw binary/protocol escape
hatch or server SDK is exported.

The producer discovers the exact supported Photospider 0.x package through
`find_package(Photospider 0.2 CONFIG REQUIRED COMPONENTS kernel)`. Package
version updates are deliberate breaking-compatibility work and must pass the
isolated consumer gate.

## Explicit absence

The repository contains no IPC v2/four-cell compatibility, graph Host mirror,
policy route, plugin loader route, durable output store, stable collection
cursor, remote transport, authentication, tenant, process worker, recovery,
or durable state product. Those capabilities are removed or out of scope, not
disabled options.

## Archive identity

The pre-reset daemon commit
`1080548d6bb11d771c89032b7df956c9e2af3674` is preserved by annotated tag
`pre-breaking-scope-reset-2026-09-01`. It is historical source only and does
not constrain the v3 package or wire.
