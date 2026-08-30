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
