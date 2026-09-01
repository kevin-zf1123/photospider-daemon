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
  trailing bytes, request correlation, and the protocol-error sentinel.
- Real Unix-socket tests cover oversized `0xffffffff`, truncated, duplicate,
  unknown-enum, invalid-UTF-8, and trailing-byte requests. The server returns
  one typed failure and closes without state mutation or large allocation.
- Session tests cover positive `maximum_sessions`, no-allocation/no-id
  backpressure, close/reuse, exact cleanup, and concurrent admission.
- Server tests cover the positive active-handler bound, typed backpressure,
  sequential runtime reaping, exception fencing, and shutdown join.
- Installed-consumer tests prove that `bin/photospiderd` is installed while
  only `PhotospiderDaemon::client` appears in the CMake export set.

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

The CI workflow keeps independent ASAN and TSAN jobs. Record a platform or
runtime limitation as a limitation, never as a successful sanitizer result.

## Manual frame/codec fuzzing

`photospider_daemon_frame_codec_fuzz` is a long-lived manual libFuzzer target.
It exercises bounded request/protocol-error decoding and the real stream-frame
reader. It is `EXCLUDE_FROM_ALL`, is never registered with CTest, and requires
Clang. It may be combined with the daemon ASAN mode when that platform's
sanitizer runtime supports the combination:

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

The maintained seed corpus is `tests/fuzz/corpus/frame_codec/`. Caller-selected
crash and artifact directories remain untracked. Fuzzing supplements, but does
not replace, the deterministic malformed real-socket regression cases. Use a
Clang distribution that actually ships its libFuzzer runtime; the temporary
working corpus keeps generated mutations out of maintained seeds.

## CTest ownership

CTest/CI entries are reserved for correctness, concurrency, error handling,
package consumption, compilation, and runtime lifecycle. Stale-term searches,
source-layout audits, migration checklists, Doxygen audits, Issue replay, and
evidence/provenance orchestration remain manual and outside CTest/CI.
