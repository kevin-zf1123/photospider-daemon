# Testing and Validation

This document defines maintained daemon validation after the breaking scope
reset. Tests cover the long-lived local protocol, bounded orchestration, thread
lifecycle, package boundary, and malformed-input behavior. They do not prove
migration completion or restore any removed product domain.

## Development and final loops

During implementation, format and lint changed C++, build affected targets,
and run focused tests. After source and documentation freeze, run at most one
native clean configure, one full build, and one complete CTest/JUnit pass. The
daemon must always configure against a fresh installed Photospider package,
never a sibling source target or private kernel include path.

## Maintained behavior coverage

- Binary codec tests cover exact enums, ranges, UTF-8, duplicate fields,
  trailing bytes, request correlation, and the protocol-error sentinel. A
  one-shot fake Unix server proves the public Client returns a completely read
  sentinel's typed status and resets its descriptor, preserves a complete
  ordinary business `NotFound`, and maps clean EOF after a fully decoded
  request but before any response to disconnected `Internal` with unknown
  request outcome. A table-driven boundary matrix covers parameters, nodes,
  inputs, outputs, Value axes/facets, backend maps, named Values, fallback
  reasons, operation timings, and daemon methods with maximum-count/empty,
  one-byte-short, one-minimum-entry, and semantic-maximum-plus-one cases. A
  noninstalled count observer proves the malformed node and timing cases reject
  before their large reserve points.
- Real Unix-socket tests cover oversized `0xffffffff`, truncated, duplicate,
  unknown-enum, invalid-UTF-8, and trailing-byte requests. A complete eleven-
  byte header followed by body EOF preserves correlation; an incomplete header
  uses only the sentinel. The server returns one typed failure and closes
  without state mutation or declared-length allocation.
- Session tests cover positive `maximum_sessions`, no-allocation/no-id
  backpressure, close/reuse, exact cleanup, and concurrent admission. A
  noninstalled post-Running gate holds Session A before its non-preemptible
  compiler boundary while close A waits. With A still retained as closing, a
  second worker and independent real-socket handlers prove Session B can
  create, submit, succeed, and close within explicit deadlines; A submit and
  repeated close return `NotFound`; capacity is not released early and is
  reused with a fresh id after A settles; no Job/result/handler remains. A
  separate pre-mutation snapshot `bad_alloc` proves closing rolls back and the
  same Session can submit and close on retry. A
  single-worker regression holds Session A Running while Session B remains
  Queued, then proves closing B completes within a short deadline, leaves A
  Running, removes B's Job, and immediately reuses Session capacity. Its
  timeout path cancels A before collecting the close future so failures clean
  up promptly; subsequent daemon shutdown cannot retain a close handler that
  waits for A's natural delay.
- Server tests cover the positive active-handler bound, typed backpressure,
  sequential runtime reaping, exception fencing, shutdown join, and
  deterministic post-bind construction failures with descriptor/node cleanup
  plus same-path rebinding. Every asynchronous Server test uses a fail-safe
  run guard that requests stop and joins on assertion return or exception.
  Handler-count assertions first wait for active zero and drive one explicit
  accept-loop reap rather than relying on elapsed time alone. Test-runtime-only
  response observers hold an already accepted shutdown before encoding, close
  the real peer, and prove one and four concurrent acknowledgement writes fail
  while the server still stops, removes its socket, and settles every handler
  and descriptor. A one-shot response-encode `bad_alloc` proves the same stop
  guarantee, while a pre-commit dispatch fault proves ordinary admission stays
  open until a later shutdown is accepted.
- Transport tests prove a peer-uid rejection remains connection-local even
  with unrelated ambient `errno`, a normal `request_stop` succeeds, and a
  non-stop fatal accept failure remains typed. They check close-on-exec on the
  client, listener, accepted stream, and fixed parent descriptor plus
  fork/exec non-inheritance; only a test-owned duplicate is made inheritable
  for the separate SIGPIPE self-exec scenario.
- Pathname tests reject embedded-NUL suffix and post-NUL slash variants before
  connect/listen effects and preserve every shorter prefix.
- Real `photospiderd` subprocess tests send `SIGINT`, send `SIGTERM` after a
  five-second cooperative Job reaches Running, and invoke `daemon.shutdown`.
  They require bounded `exit(0)`, generation-checked socket removal, and
  cancellation/join well before the delayed operation could drain naturally.
- Installed-consumer tests prove that portable `bin/photospiderd` is installed
  while only `PhotospiderDaemon::client` appears in the CMake export set. They
  clear Linux and Darwin loader-path environment variables and execute the
  installed runtime's `--help`, rather than treating file existence as runtime
  evidence. Local package checks cover static/shared kernels in same/separate
  prefixes; CI runs the separate-prefix static/shared matrix on Ubuntu and
  macOS. Shared-kernel inspection requires Linux RPATH/RUNPATH or Darwin
  `LC_RPATH` to include the loader-relative daemon libdir plus the actual
  non-system imported kernel directory. The client remains a static archive
  independent of `BUILD_SHARED_LIBS` and creates no shared client ABI.

The nested package consumer exposes a generator-aware
`run_photospider_daemon_consumer` target whose command uses the executable
target-file expression. The outer gate passes its exact generator,
platform/toolset when present, and active configuration, then builds that run
target. Single-config and multi-config layouts therefore require no guessed
build root, configuration directory, executable suffix, or bundle path. The
outer installed-runtime check likewise receives the configured bindir,
libdir, and executable suffix instead of hard-coding a host layout.

## Product and test runtime separation

The installed `PhotospiderDaemon::client` and normal `photospiderd` executable
always use test-control-free production objects. With `BUILD_TESTING=ON`,
`photospider_daemon_test_runtime` independently compiles the complete runtime
source list under `PHOTOSPIDER_DAEMON_TEST_RUNTIME` and adds the fixed exception
controller. `test_ipc_codec`, `test_local_daemon`, and
`test_exception_fences` link only that noninstalled static variant; normal
binaries and package consumers link only the production target. No executable
links both variants. The codec count observer and arithmetic probe exist only
under the private test-runtime macro.

After changing a private lifecycle seam, manually inspect the production
archive from both testing-on and testing-off builds with `ar -t`, demangled
`nm`, and `strings`. The product must contain no test controller, construction
stage, fault callback, handler-entry callback, lifecycle count observer, test
macro, or test-support object. The noninstalled test archive is the positive
control and must retain the expected seams. This source/package audit remains
manual and is not a CTest entry.

## Sanitizers

ASAN and TSAN are separate scoped CMake modes and cannot be enabled together.
Each daemon build must consume a kernel package built with the matching
sanitizer; mixing an instrumented daemon with an uninstrumented static kernel
does not provide complete boundary coverage and can invalidate standard-library
container annotations:

```bash
cmake -S . -B <asan-build> -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH=<installed-kernel-prefix> -DBUILD_TESTING=ON \
  -DPHOTOSPIDER_DAEMON_ENABLE_ASAN=ON
cmake --build <asan-build> -j
ctest --test-dir <asan-build> --output-on-failure

cmake -S . -B <tsan-build> -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH=<installed-kernel-prefix> -DBUILD_TESTING=ON \
  -DPHOTOSPIDER_DAEMON_ENABLE_TSAN=ON
cmake --build <tsan-build> -j
ctest --test-dir <tsan-build> --output-on-failure
```

The CI workflow keeps independent ASAN and TSAN jobs with a ten-minute job
bound. `test_local_daemon` also has a 120-second CTest timeout, far below the
hosted runner watchdog while leaving proportional TSAN headroom. A timeout is
only a final fail-fast boundary; deterministic lifecycle settlement and RAII
teardown remain the root fix. Record a platform or runtime limitation as a
limitation, never as a successful sanitizer result.

## Manual frame/codec fuzzing

`photospider_daemon_frame_codec_fuzz` is a long-lived manual libFuzzer target.
It exercises bounded request/protocol-error decoding, `job.result` and
`daemon.info` response decoding, and the real stream-frame reader. The
response paths include named Values, Value rank/facets, backend maps, fallback
reasons, operation timings, and daemon method counts. It is
`EXCLUDE_FROM_ALL`, is never registered with CTest, and requires Clang. It may
be combined with the daemon ASAN mode when that platform's sanitizer runtime
supports the combination:

```bash
cmake -S . -B <fuzz-build> -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH=<installed-kernel-prefix> -DBUILD_TESTING=OFF \
  -DPHOTOSPIDER_DAEMON_BUILD_MANUAL_FUZZ_TARGETS=ON
cmake --build <fuzz-build> --target photospider_daemon_frame_codec_fuzz -j
ps_daemon_fuzz_corpus=$(mktemp -d)
cp -R tests/fuzz/corpus/frame_codec/. "$ps_daemon_fuzz_corpus"/
<fuzz-build>/photospider_daemon_frame_codec_fuzz \
  "$ps_daemon_fuzz_corpus" -runs=1000 -max_len=4096
```

The maintained seed corpus is `tests/fuzz/corpus/frame_codec/`. Its `hex:`
seeds materialize directed `job.result` and `daemon.info` count payloads while
ordinary and mutated inputs remain raw fuzz bytes. Caller-selected crash and
artifact directories remain untracked. Fuzzing supplements, but does not
replace, the deterministic malformed real-socket regression cases. Use a
Clang distribution that actually ships its libFuzzer runtime; the temporary
working corpus keeps generated mutations out of maintained seeds.

## CTest ownership

CTest/CI entries are reserved for correctness, concurrency, error handling,
package consumption, compilation, and runtime lifecycle. Stale-term searches,
source-layout audits, migration checklists, Doxygen audits, Issue replay, and
evidence/provenance orchestration remain manual and outside CTest/CI.
