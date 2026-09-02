# Photospider Local IPC Protocol Version 3

This document is the authoritative local wire contract. ADR 0001 defines the
product boundary.

## Scope and methods

IPC v3 is a same-user, same-machine, non-persistent orchestration protocol.
Darwin and Linux use a Unix-domain stream socket and verify the peer effective
uid. Socket-node mode is not an authentication boundary; callers select a
suitably private parent directory. A supported-platform uid mismatch closes
only that accepted stream and the listener continues; accept, stream
preparation, and credential syscall failures remain typed fatal server
failures. The exact sorted method inventory is:

1. `daemon.info`
2. `daemon.shutdown`
3. `job.cancel`
4. `job.release`
5. `job.result`
6. `job.status`
7. `job.submit`
8. `session.close`
9. `session.create`

There is no remote transport, plugin-path method, internal-IR payload, or v2
adapter. An unknown method code or malformed request closes or errors the
connection before any registry, kernel, or shutdown effect.

## Frame and scalar encoding

Every message is one frame:

```text
uint32 payload_size, big-endian
payload_size bytes of typed binary payload
```

`payload_size` is `1..4,194,304`. Reads and writes handle partial progress and
`EINTR`. Linux sends use `MSG_NOSIGNAL`. Darwin configures `SO_NOSIGPIPE` on
each client stream before connect and on each accepted stream before peer
validation; configuration failure closes the descriptor and returns a typed
transport failure. No process-global `SIGPIPE` disposition is changed. With
`SIGPIPE` explicitly restored to `SIG_DFL`, a peer-close write through either
product-prepared endpoint returns typed transport failure and the process exits
normally rather than by signal. Zero,
oversized, truncated, or trailing bytes reject the complete message. The
server sends one bounded typed failure before controlled close and never
allocates from the attacker-declared frame length. The reader retains at most
the fixed eleven-byte request header for correlation and grows complete
payload storage only as bytes actually arrive.

Client, listener, accepted-stream, and fixed parent-directory descriptors are
close-on-exec. Linux creates sockets with `SOCK_CLOEXEC` and accepts with
`accept4(..., SOCK_CLOEXEC)`. When those atomic facilities are explicitly
unsupported, checked `F_GETFD`/`F_SETFD(FD_CLOEXEC)` fallback is used; any
failure closes the descriptor and returns a typed failure. Darwin uses this
fallback, so a concurrent fork can observe the finite create/accept-to-fcntl
window; the implementation does not claim atomic close-on-exec there.

Inside the payload, integers are little-endian. Text and byte vectors use a
little-endian uint32 byte count followed by exact bytes. Text is canonical
UTF-8 and subject to its field-specific limit. Booleans are exactly zero or
one. Floating parameters preserve exact IEEE-754 binary64 bits. Counts are
validated before allocation, map insertion, or iteration. After the semantic
maximum, each count must satisfy
`count <= (remaining - required_suffix) / minimum_entry_bytes`; suffix
subtraction is checked first and no multiplication is used. This single rule
covers Workflow parameters/nodes/inputs/outputs, Value shape/Region/strides
and facets, selected-backend maps, named Values, fallback reasons, operation
timings, and daemon methods. Transfer count, transfer bytes, and peak live
bytes remain three fixed uint64 scalars rather than a collection count.

A count that cannot fit the unread bytes is `InvalidArgument`, even when it is
below its semantic maximum, and is rejected before any count-sized allocation.
The byte fence does not relabel a genuinely admissible allocation failure:
private codec allocation still throws `std::bad_alloc`, and the server handler
exception fence reports that exhaustion as `ResourceExhausted`.

## Request and response header

A request begins with:

```text
uint16 protocol_version = 3
uint64 request_id != 0
uint8  method_code in 1..9
method-specific fields
```

A response begins with the same version, request id, and method code, followed
by a uint8 public kernel `ErrorCode` and a uint32-length UTF-8 diagnostic of at
most 4,096 bytes. A success then carries method-specific fields. The client
requires exact version/method/id correlation and invalidates its connection on
transport or response-codec failure. It never automatically retries.

Protocol-error correlation is recovered only when the received payload begins
with a complete valid v3 version, nonzero request id, and known method. When it
cannot be recovered (including length-prefix rejection, truncation before the
complete header, or an unknown version/method), the failed response uses the
sole sentinel `request_id=0`, `method=daemon.info`. If the complete valid header
arrives and later method-body bytes are truncated, the failed response keeps
that header's request id and method. A normal request and a successful response
may never use the sentinel.

When the public Client completely reads a legal failed sentinel, it returns the
sentinel's typed status and invalidates the connection; ordinary responses
still require exact nonzero request-id/method correlation. The sentinel is a
best-effort server response: if transport failure prevents a complete read,
the Client returns that transport failure instead.

After a complete request write, clean EOF before any response bytes is a public
`Internal` transport failure. The Client resets its descriptor and reports that
the peer closed before a response and the request outcome is unknown; it does
not expose the private frame reader's `NotFound`, because the server may already
have applied request effects. The private reader keeps `NotFound` for a server
observing an ordinary client close, partial response EOF remains
`InvalidArgument`, and a complete ordinary business `NotFound` response or
failed sentinel retains its exact typed status.

## Ephemeral identifiers

`SessionId` and `JobId` each contain two opaque uint64 values:

```text
daemon_instance != 0
monotonic_sequence != 0
```

The process creates one non-security instance token at startup. Registry lookup
requires both fields, so a sequence reused after restart cannot match a prior
identifier. IDs are not paths, kernel handles, authorization values, retry
keys, or persistent identities.

## Workflow and result values

`session.create` carries one bounded public `WorkflowDocument`: schema version,
nodes, operation keys, ordered input edges, tagged scalar parameters, and named
outputs. The daemon decodes source only, creates `GraphContext`, and validates
it through the installed public `Compiler`. It never serializes semantic IR,
optimized IR, physical plan fields, callback/native handles, or DSO paths.

`job.result` encodes sorted named public `Value` objects. Each Value includes
element type, rank/shape, Region intervals, byte offset/strides, zero to 64
versioned facets (bounded key/version/payload), and bounded immutable bytes.
The response also carries raw execution timing, selected backends, transfer
counts/bytes, peak live bytes, fallback reasons, operation timings, and
non-security plan/result digest text. Decode republishes each Value through
public bounds/type/shape/Region/layout/facet validation; no partial result is
visible after failure.

## Sessions

### `session.create`

The request contains one complete WorkflowDocument. Success returns a fresh
`SessionId`. A Session is only a logical namespace retaining one immutable
kernel `GraphContext`. The positive process-global `maximum_sessions` bound is
checked before graph/compiler allocation, publication, or id consumption. A
full registry returns `ResourceExhausted` and creates nothing.

### `session.close`

The request contains a `SessionId`. Close atomically marks an open record as
closing against new submit. Submit and repeated close then return `NotFound`,
while the retained record continues consuming Session capacity. The
process-wide lifecycle lock is released before the Job registry removes
matching queue-owned records and synchronously completes their existing
`Queued -> Running -> Cancelled` transitions. Matching records already popped
by a worker retain cooperative cancellation, stale-publication fences, and a
terminal wait. No unrelated Session Job participates in that wait, and
unrelated Session create, submit, and close remain responsive. A snapshot
allocation failure precedes every Job mutation and clears closing so the caller
may retry. After successful settlement, close reacquires lifecycle ownership,
removes the Session/kernel context, and releases capacity.

## Jobs

The only lifecycle is:

```text
Queued -> Running -> Succeeded | Failed | Cancelled
```

### `job.submit`

The request contains a live `SessionId`, `allow_gpu`, and
`maximum_parallelism`. It does not repeat the workflow. A global retained-record
bound provides ordinary backpressure; rejection creates no `JobId`. Success
returns a fresh `JobId` in `Queued` state.

### `job.status`

The request contains `JobId`. Success returns JobId, SessionId, current state,
and terminal Status. Observation is non-destructive.

### `job.cancel`

Cancellation is idempotent for a terminal Job. A queued Job records the
cooperative request and still transitions through `Running` before
`Cancelled`. Session close may perform the same transition synchronously after
exactly removing queued ownership. A running callback may drain, but accepted
cancellation prevents result publication.

### `job.result`

Only `Succeeded` returns the copied temporary in-memory result. Queued/Running
returns `InvalidArgument`; Failed/Cancelled returns the terminal failure.

### `job.release`

Only a terminal Job can be released. Success erases its complete record and
temporary result. Later status/result/release calls return `NotFound`.

A caller retries only by `job.submit`, which creates a distinct identifier.

## Daemon information and shutdown

`daemon.info` returns protocol version, non-security process instance token,
package version, `unix-domain` transport, exact sorted method list, live
Session/Job counts, fixed global concurrency, and the fixed retained-Session
bound.

`daemon.shutdown` is accepted at a no-throw Service commit after the complete
success response and every fault-capable operation have been staged. The commit
atomically closes later ordinary admission and marks that response as accepted;
a pre-commit dispatch failure does neither. Concurrent shutdown requests may
all be accepted and converge on the same monotonic state. The server captures
acceptance before response encoding and then stops the listener from the
handler's common no-throw tail after every acknowledgement outcome. Encoding
failure, real write failure, or failure of a best-effort catch response cannot
undo or strand accepted shutdown. A client that does not receive the success
acknowledgement must treat the request outcome as unknown even though the server
still completes an already accepted stop. Service destruction requests
cancellation, joins fixed workers, and releases every Session, Job, result,
GraphContext, kernel execution resource, descriptor, and socket node. The next
process starts with empty registries.

The POSIX executable blocks `SIGINT`/`SIGTERM` before constructing the Server
or any worker and consumes them synchronously on a dedicated `sigwait` thread.
That thread only requests the same stop path. An additionally blocked
waiter-only `SIGUSR1` wakes normal `daemon.shutdown` completion so the waiter
joins without polling; after Server/workers are destroyed, the original main-
thread signal mask is restored. Signal handling never runs C++ cleanup from an
asynchronous handler and never installs global `SIG_IGN`.

The server also enforces one positive global active-connection/handler bound.
An excess connection receives a sentinel `ResourceExhausted` protocol error
and closes without starting a thread. Accepted handler threads remain
joinable; the accept loop reaps completed threads during normal operation and
shutdown joins every remainder. No handler is detached.

## Socket pathname lifecycle

Startup accepts only an absent socket pathname. A live socket, stale socket,
regular file, symlink, or any other existing directory entry produces a typed
startup failure and remains unchanged. The daemon performs no stale-node probe,
automatic unlink, crash recovery, or lock-file recovery. Concurrent attempts
that both observe absence rely on the atomic Unix `bind` result to select one
owner.

An empty, over-bound, or embedded-NUL pathname is `InvalidArgument`. Validation
uses the complete `std::string` before split, parent open, socket creation,
bind, or connect, so POSIX NUL termination cannot redirect an operation to a
shorter prefix.

Before bind, the listener moves every allocation-backed parent path, leaf, and
fixed-directory capability into an inactive guard. After bind and before arm,
it performs only non-allocating system observations to capture the exact
parent/socket `st_dev` and `st_ino` values. The guard state is
`Empty -> Prepared -> Armed -> Consumed`: a capture failure abandons the
unverified `Prepared` state and preserves the current path; a successful
capture arms exact-generation cleanup. Construction rollback, normal shutdown,
signal shutdown, and destruction use this same move-only guard. Cleanup first
revalidates the fixed parent descriptor, the parent pathname, the socket type,
and both generations; mismatch or replacement preserves the current path. A
normal matching cleanup removes the node and permits a clean restart.

Listener setup performs no post-bind pathname `chmod`. The socket node's
ambient mode follows the caller's directory and process umask, is not an
authentication boundary, and is never used to authorize a peer. Peer
credentials remain the same-user acceptance check, and the host rejects a
non-owner peer. Portable POSIX has no conditional
compare-and-unlink primitive: the final `fstatat` and `unlinkat` calls are
separate, and the implementation does not claim protection from a hostile
same-uid writer racing exactly between them.

## Correctness invariants

- Frame, version, enum, UTF-8, count, length, overflow, and trailing-data
  validation occurs before state mutation.
- Session creation compiles before registry publication.
- Session close is atomic against submit, retains capacity while marked
  closing, releases global lifecycle serialization before waiting only for its
  own popped work, removes queued ownership exactly, and rejects later result
  publication. Pre-mutation snapshot failure reopens the Session; successful
  settlement erases it with a fresh-id-only capacity reuse path.
- Job state is monotonic and cancellation cannot publish success afterward.
- Worker/callback exceptions are fenced into one Job failure.
- Descriptor, worker, GraphContext, result, and queue ownership settles exactly
  once.
- Listener construction prepares allocation-backed path/leaf/guard state before
  bind, performs no allocating or throwing C++ operation between successful
  bind and generation arm, and retains descriptor/socket-node rollback guards
  until complete private-state publication. A pre-arm allocation failure creates
  no node and leaves the exact path immediately rebindable. Inconclusive capture
  preserves the path; exact-generation cleanup never removes or changes a
  replacement, including its mode.
- Daemon production build and package consumer use only an isolated installed
  `Photospider::kernel` target.

These are local correctness properties, not remote-service or persistent-state
claims.
