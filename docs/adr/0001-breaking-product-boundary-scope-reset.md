# ADR 0001: Make the Daemon an Ephemeral Local Orchestration Layer

- Status: Accepted
- Date: 2026-09-01
- Decision type: Breaking 0.x product-boundary reset
- Archive tag: `pre-breaking-scope-reset-2026-09-01`
- Archived daemon commit: `1080548d6bb11d771c89032b7df956c9e2af3674`
- Companion kernel decision: `photospider/docs/adr/0015`

## Context

The extracted daemon originally maintained local IPC v2 as a frozen 60-method
facade over the old embedded Host. It also retained a four-cell compatibility
gate, graph-route mirroring, process-global policy/plugin controls, stable
collection snapshots, and protected output artifacts. That surface preserved
the old product split instead of providing the small orchestration layer needed
by the typed compiler/executor kernel.

This is a deliberate breaking 0.x reset. IPC v2 and its four-cell contract are
archive-only. Their source remains available through Git history and the
annotated tag above; no adapter, disabled target, alternate protocol, or
archived source copy remains in the active tree.

## Decision

### Product role and dependency direction

`photospider-daemon` is a same-user, local, non-persistent orchestration layer.
It depends only on an isolated installation of the public Photospider package:

```text
photospiderd and PhotospiderDaemon::client
  -> installed Photospider public compile/execute/value API
```

The daemon does not include private kernel headers, link a source-tree target,
copy compiler/optimizer/planner code, serialize internal IR, or become a
dependency of the kernel.

### Local Sessions

`SessionId` names one daemon-owned logical namespace in the current daemon
process. Multiple Sessions share the same user, process, operation set, and
trust domain. A Session is not a tenant, principal, authorization scope,
sandbox, or resource-isolation boundary.

`session.create` creates a fresh kernel `GraphContext` through the installed
public API. `session.close` rejects new submission, cancels unfinished Jobs,
removes matching records still owned by the global queue, waits only for
matching work already popped by a worker, releases all temporary results, and
destroys the context. Queue removal synchronously completes the existing
`Queued -> Running -> Cancelled` lifecycle; an unrelated Session's Running Job
does not participate in the wait. Daemon restart clears every Session.

Session retention has one positive process-global `maximum_sessions` bound.
Admission briefly holds `lifecycle_mutex -> sessions_mutex` to reserve capacity
before constructing a graph/compiler or consuming an identifier. Retained
Sessions, including closing records, plus pending creates cannot exceed the
bound. GraphContext construction and compilation run outside both locks, so an
unrelated retained Session can submit or close while a create is pending. A
full registry returns `ResourceExhausted` immediately. Compilation, construction,
and map-publication failures release the reservation and consume no identifier;
only successful insertion advances the monotonic id. Pending creates are not
published Sessions and do not appear in `daemon.info.active_sessions`. Close
releases exactly one retained slot after its own records settle, so later
creation can reuse capacity without reusing an identifier or waiting for
unrelated execution.

### Ephemeral Jobs

Every accepted submit receives a fresh opaque `JobId`. The exact state machine
is:

```text
Queued -> Running -> Succeeded | Failed | Cancelled
```

A terminal state is immutable. Cancellation is cooperative and best-effort;
the daemon forwards it to the kernel cancellation source and rejects any stale
completion or result publication after cancellation or Session close. Calling
`job.release` removes the terminal record and temporary result. Ordinary
process-global concurrency limits and backpressure are allowed.

The worker exception fence accepts a nullable borrowed diagnostic pointer and
constructs every owned string and `Status` inside its protected block. A null
standard-exception diagnostic becomes an empty message. If primary or
failure-status construction throws, the fallback still publishes a terminal
failure, wakes close waiters, clears result ownership, and preserves
cancellation or the primary error category.

There is no `JobAttemptId`, automatic retry, checkpoint, recovery journal,
durable Job specification, artifact id, output commit, receipt, per-tenant
quota, or retained state across restart. A caller retry is a new submit and a
new `JobId`.

### Local IPC v3

The protocol exposes exactly nine methods:

1. `session.create`
2. `session.close`
3. `job.submit`
4. `job.status`
5. `job.cancel`
6. `job.result`
7. `job.release`
8. `daemon.info`
9. `daemon.shutdown`

Supported POSIX builds use a Unix-domain stream socket. A supported Windows
port may implement the same frame/protocol contract over a local named-pipe
abstraction. The product has no TCP, HTTP, gRPC, TLS, remote address, remote
worker, v2 compatibility adapter, or dual protocol.

Socket startup is deliberately recovery-free. Every existing pathname,
including a live or stale socket, regular file, or symlink, is rejected without
unlinking or replacement. When the path is absent, concurrent bind attempts are
arbitrated atomically by the operating system. All allocation-backed path,
leaf, and guard state is prepared before bind. After a successful bind, only
non-allocating system observations capture the parent and socket
`st_dev`/`st_ino` generations through a fixed parent directory descriptor
before the guard is armed. An inconclusive capture abandons cleanup and
preserves the pathname; once armed, cleanup unlinks only a currently matching
socket and preserves every mismatch. Portable POSIX `fstatat` and `unlinkat`
remain separate operations, so this is fail-closed generation hygiene rather
than a claim of atomic compare-and-unlink against a hostile same-uid writer.
Automatic stale-node reclamation is outside this ephemeral reset; an operator
removes a crash residue explicitly.

The daemon does not apply a post-bind pathname `chmod` and does not use the
socket node's ambient mode as an authentication boundary. The embedding caller
selects a suitably private parent directory. Same-user connection acceptance
is enforced by peer credentials, and the host continues to reject a peer whose
uid does not match the daemon effective uid.

The wire carries `WorkflowDocument` input and public execution options/results.
It never carries semantic IR, optimized IR, execution-plan internals, plugin
paths, DSO handles, device handles, cache internals, or kernel object pointers.

Every malformed or oversized frame receives at most one typed error response
before controlled connection close. If a complete valid v3 header can be
recovered, the response preserves its request id and method. Otherwise it uses
the explicit sentinel `request_id=0`, `method=daemon.info`; this sentinel is
valid only for a failed protocol-error response.

### Result lifetime

A successful Job owns an in-memory public kernel result until `job.release`,
Session close, bounded terminal eviction, or daemon shutdown. `job.result`
returns a typed public value encoding. Results have no durable artifact
identity, filesystem publication protocol, lease, receipt, retention promise,
or recovery behavior.

### Shutdown and restart

`daemon.shutdown` stops admission, cancels queued and running Jobs, wakes
blocked local connections, joins owned threads, releases results and Sessions,
removes only the verified socket path, and exits. Signal shutdown uses the
same cleanup path. Process restart begins with empty registries.

Shutdown acceptance has one explicit Service linearization point. Dispatch
first stages the complete successful Response and finishes every operation
that may allocate, throw, or trigger a test fault. It then performs only the
no-throw `shutting_down=true` and `shutdown_after_write=true` commit. A dispatch
failure before that commit returns both flags false and leaves ordinary
admission open; concurrent shutdown requests may each be accepted and converge
on the same monotonic fence. The server captures accepted shutdown immediately
after dispatch. Once captured, its handler invokes idempotent `stop()` from a
common no-throw tail whether acknowledgement encoding fails, the peer makes the
real write fail, a catch path can send only a best-effort failure, or the
acknowledgement succeeds. Transport success therefore controls only what the
client knows, never whether an already accepted shutdown completes.

Before constructing Server or service workers, `photospiderd` blocks
`SIGINT`, `SIGTERM`, and one waiter-only `SIGUSR1` completion wake. A dedicated
thread synchronously consumes the blocked set with `sigwait`; external stop
signals invoke only thread-safe `Server::request_stop`. Normal RPC shutdown
sets a completion flag, directs `SIGUSR1` to the waiter, and joins it. Ordinary
control flow then destroys Server/service state, cancels and joins work,
performs generation-checked socket cleanup, drains pending managed signals,
and restores the main thread's original mask. No asynchronous handler runs C++
cleanup and no process-wide signal ignore is installed.

Connection handling has one positive process-global active-handler bound.
Admission beyond that bound returns `ResourceExhausted` and closes without
starting a thread. Handler threads remain joinable; the accept loop joins and
erases completed records while the server is running, and shutdown joins every
remainder. No handler is detached.

### Correctness validation retained

The daemon retains defensive validation without making security-product
claims:

- bounded frame length, exact integer ranges, valid UTF-8, unique object keys,
  and method-specific required fields;
- malformed frame, correlation, opaque-id, state-transition, and result-shape
  rejection;
- kernel public type/shape/`Region`/layout/facet errors preserved as stable
  failures;
- stale handle, stale completion, and post-cancellation publication rejection;
- exception fencing, exact descriptor/thread/result cleanup, and bounded
  backpressure;
- negative, concurrency, restart-loss, Session-close, cancellation, result
  release, ASAN, TSAN, and fuzz testing where supported.

Unix socket path/generation checks are local lifecycle correctness. Peer
credentials enforce same-user connection acceptance; socket-node mode does not
authenticate a peer. Same-user status does not permit an old instance to remove
or mutate a replacement inode. None of these checks is tenant isolation.

## Exact non-goals

- IPC v2 compatibility or the frozen four-cell gate.
- A complete remote facade over every kernel operation.
- Authentication, authorization, Principal, Tenant, role, capability, or
  multi-tenant quota.
- Remote access or worker execution.
- Durable Jobs, attempts, retries, checkpoints, recovery, artifacts, receipts,
  backup/restore, deployment, or rollback.
- Policy plugins, plugin admission, cryptographic trust, process isolation, or
  sandboxing.
- Loading operation/provider plugins through IPC.

These domains are removed or out of scope, not deferred or default-disabled.

## Superseded authorities

This ADR is the highest active daemon product-boundary authority. It supersedes
the previous repository boundary, version/CI compatibility contract, local IPC
v2 protocol, four-cell compatibility tooling, and any active Issue or Project
description that treats those materials as maintained behavior. The archived
pre-reset tag is historical evidence only and must not be linked as active
authority.

## Consequences

- Existing v2 clients do not connect to v3 and no compatibility layer is
  provided.
- The installed daemon client and executable require the reset public kernel
  package.
- Tests and CI validate the nine-method local product and isolated installed
  package boundary rather than migration compatibility.
- Reintroducing a removed product domain requires a new explicit breaking ADR
  that supersedes this decision.
