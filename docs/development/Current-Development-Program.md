# Current Development Program

- Snapshot date: 2026-09-05
- Audited implementation baseline: `main@602e89ab6ec63350d504bb7ae538294ce237e023`
- Current focus: installed-package compatibility; retained S1 features are demand-driven

## Role and authority

This file records the public delivery baseline, current milestone, active leaf
Issues, dependencies, and execution order. It cannot change the daemon product
boundary in ADR 0001 or the wire and lifecycle contract in IPC Protocol v3.

Public GitHub Issues are the live delivery-status authority. If this snapshot
differs from an Issue, the Issue prevails and this file must be reconciled.
[#15 daemon-local-orchestration](https://github.com/users/kevin-zf1123/projects/15)
is a maintainer operational view that mirrors Issues and cannot override them.
Private maintainer notes, including any OpenSpec files outside this public
repository, have no daemon architecture or delivery authority and do not gate
completion.

## S0 settlement baseline

The audited implementation already supplies the #2 and #4 scopes. The #3
package policy is implemented, and this settlement adds its missing direct
same-minor probes. The #3 row becomes delivered only after the target commit's
`daemon-ci` succeeds and its live Issue and Project item are closed; until
then, their open state governs.

| Issue | Current evidence and settlement condition |
| --- | --- |
| [#2](https://github.com/kevin-zf1123/photospider-daemon/issues/2) | Delivered: installed-kernel mapping, exact nine-method IPC v3, bounded Session/Job lifecycle, cancellation, result release, shutdown, and restart loss |
| [#3](https://github.com/kevin-zf1123/photospider-daemon/issues/3) | Existing contract: `Photospider 0.2` kernel component, same-minor compatibility, and public-only dependency. Settlement condition: target `daemon-ci` verifies the exact-minor and two cross-minor probes under `tests/version_probe/`, then the Issue and Project item close. |
| [#4](https://github.com/kevin-zf1123/photospider-daemon/issues/4) | Delivered: isolated kernel install, Linux/macOS static/shared matrix, installed daemon client, lifecycle tests, ASAN, and TSAN |

The latest baseline CI was
[`daemon-ci` run 38](https://github.com/kevin-zf1123/photospider-daemon/actions/runs/33720331110).
It passed on Linux and macOS against static and shared installed kernels, plus
the focused ASAN and TSAN jobs.

## Retained S1 feature backlog

S1 projects caller-owned runtime Values into Jobs and returns ordinary image or
tensor results without introducing persistence, artifact identity, recovery,
or remote transport.

### Critical path

1. [#10](https://github.com/kevin-zf1123/photospider-daemon/issues/10)
   freezes the per-Job binding projection, Session mutability decision,
   validation, ownership, cancellation, and release rules after the kernel
   binding contract is accepted.
2. [#11](https://github.com/kevin-zf1123/photospider-daemon/issues/11)
   freezes an ephemeral local bulk-result transport while retaining the
   4,194,304-byte control-frame limit.
3. [#12](https://github.com/kevin-zf1123/photospider-daemon/issues/12)
   implements one input-to-result IPC vertical after the kernel execution,
   binding projection, and bulk-transport contracts are complete.

The bulk-result decision can proceed in parallel with the kernel input
contract. Binding projection begins after the kernel contract is accepted.

## S1 decision scope

This snapshot does not choose bulk storage, result-generation, descriptor,
cleanup, or Session-update semantics. #10 decides the binding projection and
Session mutability. #11 decides the ephemeral bulk transport, platform
support, ownership, lifetime, release, and failure rules. Both decisions remain
subject to daemon ADR 0001, including same-user local scope, restart loss, and
the absence of artifact, recovery, remote-service, or tenant authority.

## Issue execution contract

An executable leaf Issue records its audited baseline commit, remaining delta,
governing public document, public/API/schema impact, start dependency,
integration dependency, completion gate, named fixture or vertical, exact
tests and oracle, non-goals, and expected completion evidence. Parent Issues
are indexes and closure aggregators and do not carry `ready-for-agent`.

For task status, authorization endpoints and decision/implementation completion,
see [Task Collaboration](Task-Collaboration.md).

## Update rule

Update this snapshot when the audited baseline, current milestone, critical
path, or blocked reason changes. Ordinary implementation details remain in the
owning Issue and tests. Every status claim must cite completed code and tests;
an unchecked item does not define current behavior.

## Accepted scheduling direction, 2026-09-05

The maintainer accepted embedded image computation as the main direction and
demand-driven new daemon features. #9 through #12 and their technical
dependencies remain; #11 still has no start dependency. Scheduling deferral
is neither technical blockage nor completion. The S1 above remains the retained
feature scope; it does not automatically start that feature work.

Kernel #256 now has revised Float32 image, ordinary-scalar and per-port
contracts; its concrete operation ABI v3 has been accepted. #10 remains
dependent on the kernel contract; Session/wire rules are unchanged. Kernel 0.3
requires minimal coordinated consumer/package maintenance or a separately
approved supported-version/CI-selection policy. Deferring new IPC features
does not waive that maintenance. No code, versions or CI changed. Decision
delivery is tracked in [kernel #256](https://github.com/kevin-zf1123/photospider/issues/256);
implementation and new daemon features remain separate tasks.
