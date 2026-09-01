# Photospider Local IPC Protocol Version 3

This document is the authoritative local wire contract. ADR 0001 defines the
product boundary.

## Scope and methods

IPC v3 is a same-user, same-machine, non-persistent orchestration protocol.
Darwin and Linux use a mode-0600 Unix-domain stream socket and verify the peer
effective uid. The exact sorted method inventory is:

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
transport failure. No process-global `SIGPIPE` disposition is changed. Zero,
oversized, truncated, or trailing bytes reject the complete message. The
server sends one bounded typed failure before controlled close and never
allocates from the attacker-declared frame length. The reader retains at most
the fixed eleven-byte request header for correlation and grows complete
payload storage only as bytes actually arrive.

Inside the payload, integers are little-endian. Text and byte vectors use a
little-endian uint32 byte count followed by exact bytes. Text is canonical
UTF-8 and subject to its field-specific limit. Booleans are exactly zero or
one. Floating parameters preserve exact IEEE-754 binary64 bits. Counts are
validated before allocation or iteration.

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

The request contains a `SessionId`. Close atomically wins against new submit,
removes all matching Job records/results, requests cancellation, waits for
their terminal settlement, then removes the Session and kernel context. A
later close returns `NotFound`.

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
`Cancelled`. A running callback may drain, but accepted cancellation prevents
result publication.

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

`daemon.shutdown` acknowledges, stops the listener, interrupts active client
connections, and lets `photospiderd` destroy the service. Service destruction
requests cancellation, joins fixed workers, and releases every Session, Job,
result, GraphContext, kernel execution resource, descriptor, and socket node.
The next process starts with empty registries.

The server also enforces one positive global active-connection/handler bound.
An excess connection receives a sentinel `ResourceExhausted` protocol error
and closes without starting a thread. Accepted handler threads remain
joinable; the accept loop reaps completed threads during normal operation and
shutdown joins every remainder. No handler is detached.

## Correctness invariants

- Frame, version, enum, UTF-8, count, length, overflow, and trailing-data
  validation occurs before state mutation.
- Session creation compiles before registry publication.
- Session close is atomic against submit and rejects later result publication.
- Job state is monotonic and cancellation cannot publish success afterward.
- Worker/callback exceptions are fenced into one Job failure.
- Descriptor, worker, GraphContext, result, and queue ownership settles exactly
  once.
- Listener construction prepares allocation-backed path/configuration state
  before bind and retains descriptor/socket-node rollback guards until complete
  private-state publication; every injected construction failure leaves the
  exact path immediately rebindable.
- Daemon production build and package consumer use only an isolated installed
  `Photospider::kernel` target.

These are local correctness properties, not remote-service or persistent-state
claims.
