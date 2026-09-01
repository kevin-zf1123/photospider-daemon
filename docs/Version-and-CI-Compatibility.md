# Version and CI Compatibility Contract

## Independent axes

| Axis | Rule |
| --- | --- |
| Photospider package | explicit supported 0.x version; isolated installed dependency |
| PhotospiderDaemon package | explicit 0.x version; source/package breaks are documented |
| Local IPC | exact version 3 and nine-method inventory |

These axes are independent. Package compatibility does not imply wire
compatibility, and the wire never exposes internal compiler schema versions.
IPC v2 is archive-only and has no compatibility adapter.

## Pull-request gate

Daemon CI:

1. checks out the matching kernel feature branch when present, otherwise
   kernel main, in a separate directory;
2. configures, builds, and installs the kernel to a fresh prefix;
3. configures the daemon using only that prefix;
4. builds, runs CTest, installs, and runs an external typed-client consumer;
5. runs binary-codec, real-process Session/Job/cancellation/result/restart/
   shutdown, executable-help, and installed-client tests; and
6. runs separate Clang ASAN and TSAN configure/build/CTest jobs against the
   same installed-kernel boundary.

The installed-client gate also proves that `bin/photospiderd` exists while the
generated export set contains exactly `PhotospiderDaemon::client` and no
executable/server target. The long-lived frame/codec fuzz target is a manual
developer target, not a default build, CTest, or migration-residue gate.

No job builds an archived kernel or executes old/new wire combinations. There
is no frozen four-cell gate.

## Kernel update

Updating the supported kernel tuple requires:

- the exact kernel revision and package version;
- public API/package impact documented;
- clean isolated daemon configure/build/CTest/install/consumer results;
- reciprocal kernel/daemon documentation updates when the public facade
  changes; and
- no private include, source target, internal IR, or plugin path leakage.

The matching-branch rule supports a coordinated breaking cut. After merge and
feature-branch removal, daemon CI consumes kernel main.

## Test ownership

Kernel CI owns compiler/executor/Value/operation/package behavior. Daemon CI
owns local framing, typed client, Session/Job registries, process lifecycle,
and installed dependency use. Cross-repository validation is requested for an
installed API/package break or explicit release gate.
