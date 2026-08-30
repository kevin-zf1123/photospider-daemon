# Repository and Package Boundary

## Decision

`kevin-zf1123/photospider-daemon` exclusively owns local IPC v2 source,
packaging, protocol documentation, and verification. The Photospider kernel
repository exclusively owns the embedded Host, operation runtime, Job/worker,
policy, trust, isolation, evidence, and kernel implementation.

The daemon repository depends in one direction on an installed Photospider
package:

```text
PhotospiderDaemon::client
  -> Photospider::operation_runtime + Threads::Threads

photospiderd
  -> private daemon server/client
  -> Photospider::photospider (installed embedded Host)
```

No daemon export contains a source-tree or `src/lib` path. Private protocol
targets are never installed. This is a complete ownership migration, so the
old `Photospider::photospider_ipc_client` target is not retained as an alias.

The post-split repository is in IPC v2 compatible-maintenance. It owns fixes,
package compatibility, lifecycle hardening, and verification for the frozen
surface, but not IPC v3 expansion or kernel compiler artifacts. Kernel Host,
Job/worker, policy, trust, isolation, and evidence remain kernel authority.

## Version and dependency boundary

| Axis | K0 value | Compatibility boundary |
| --- | --- | --- |
| Photospider package | 0.1.0 | daemon producer and installed client require `EXACT`; generated package compatibility is same minor |
| PhotospiderDaemon package | 0.1.0 | generated package compatibility is same minor |
| IPC protocol | v2 | exact frozen 60-method wire contract |

Production daemon configuration requires only Photospider `embedded` and
`operation_runtime`. `operation_plugin_sdk` and `policy_sdk` are requested only
when `BUILD_TESTING=ON` because they build test fixtures. Pull-request CI pins
the supported post-split kernel revision
`c656ac58046c1d7fdb40372ae575728f526c0f01`; a weekly compatible-main job is a
downstream drift signal and does not gate every kernel pull request.

## Frozen extraction identity

- Source repository: `kevin-zf1123/photospider`
- Source commit: `f9fc3aefce45072c6fc6a856da11f20ff16ba00a`
- Annotated tag: `full-stack-archive-2026-08-30`
- Tag object: `bb876cbf76882bc0a9029956d6acc9ee3fbaae0e`
- Extracted-history main head: `de017c1cc004ef6c8497a3cddafa25d99d4da6f3`

The extracted commit id differs because path filtering rewrites tree and
parent identities. File history remains traceable; the original tag object is
not copied or retargeted in this repository.

## Deferred work

This boundary does not define protocol v3, wire cancellation or shutdown,
remote/multi-user service behavior, Host capability-facade migration, typed
compiler compatibility, or deeper Job/trust/isolation/policy/evidence work.
Next-protocol design remains blocked on a stable kernel Compiler MVP. Internal
WorkflowDocument/IR/planner artifacts never become daemon authority merely
because the daemon maps a future stable Host facade.
