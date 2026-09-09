# Current Development Program

- Snapshot date: 2026-09-09
- Audited implementation baseline: `53ec2ca` (installed kernel 0.4), following `main@b2babab`
- Current focus: kernel 0.4 installed consumption; new IPC features remain demand-driven

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

S0 Issues #2, #3 and #4 are settled: nine-method IPC v3, bounded Session/Job
lifecycle, cancellation, release, shutdown/restart loss and isolated installed
consumption remain the baseline.

## Kernel 0.4 consumer maintenance

[#15](https://github.com/kevin-zf1123/photospider-daemon/issues/15) is implemented
in `53ec2ca`, following kernel S2 #263/#264/#210/#211/#265/#266. The daemon and
Client rebuild against installed kernel 0.4; source schema 2 uses tagged node
outputs and execution supplies explicit empty bindings. Daemon package 0.2 and
IPC v3 remain. Codec entry rejects unsupported declarations/references, Float32
and nonzero storage origins; borrowed bytes are copied into the bounded payload.
No per-Job binding or bulk/streamed result protocol is added.

Static-kernel validation passed all 14 runtime tests plus the corrected installed
consumer; shared-kernel validation passed 15/15. Coverage includes codec rejection,
Session/Job lifecycle, cancellation, socket ownership, process signals, exception
fences, loader paths and separate kernel/daemon minor-version probes. Independent
two-repository review found no outstanding blocker/required after kernel fixes.
Protected matching-branch CI, Codex bot review, merge and Issue/Project settlement
are recorded in #15 and its PR; local validation alone is not the delivery gate.

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

Kernel ADR 0016 supplies the accepted binding/image contract; ADR 0017 supplies
S2 regional execution and storage. This maintenance consumes installed 0.4.
#10/#11/#12 remain separately scoped decisions and implementation, and are not
closed by compatibility maintenance.

## S3 kernel 0.5 consumption

[Issue #17](https://github.com/kevin-zf1123/photospider-daemon/issues/17) updates the installed dependency and version probes to kernel 0.5. The existing IPC v3 subset and daemon 0.2 package remain. Kernel S3 snapshots, caches and application preview policy are not added to IPC. Protected merge follows kernel S3 #268; live validation and settlement belong to the Issue.
