# Version and CI Compatibility Contract

## Authority

This document is the maintained package/version/CI contract for the standalone
daemon repository. The IPC wire behavior remains authoritative in
[IPC Protocol v2](codebase-structure/IPC-Protocol-v2.md), and current source
ownership remains authoritative in
[Repository and Package Boundary](Repository-Boundary.md).

The daemon is in compatible-maintenance. This contract governs reproducible
dependency updates and drift detection; it does not authorize protocol v3 or
kernel compiler-schema exposure.

## Independent version axes

| Axis | Current value | Automatic compatibility | Evidence owner |
| --- | --- | --- | --- |
| Photospider CMake package | 0.1.0 | exact dependency from daemon; Photospider package file accepts same minor | Photospider kernel repository |
| PhotospiderDaemon CMake package | 0.1.0 | package file accepts same minor | this repository |
| Local IPC wire | version 2 | exact 60-method protocol-v2 contract | this repository |

These axes are independent. A matching wire version does not prove that the
compiled client can link an arbitrary kernel package. A matching package
version does not authorize new wire fields. WorkflowDocument, IR, planner,
digest, plan-cache, and operation-trait versions are future kernel contracts
and are not daemon package or protocol versions.

During 0.x development, minor versions are potentially breaking. The generated
package files therefore use `SameMinorVersion`; patch releases within 0.1 may
be accepted by ordinary package consumers. This daemon's producer and exported
client remain narrower and call `find_package(Photospider 0.1.0 EXACT CONFIG
REQUIRED ...)` until a reviewed compatibility-range Issue changes the tuple.

## Component closure

Production configuration requests:

- `embedded`, used privately by `photospiderd`;
- `operation_runtime`, used publicly by `PhotospiderDaemon::client`.

With `BUILD_TESTING=ON`, configuration also requests
`operation_plugin_sdk` and `policy_sdk` for repository test fixtures. Those
components are not production requirements. The installed daemon package
finds exact Photospider 0.1.0 `operation_runtime` and Threads before importing
`PhotospiderDaemon::client`.

## Pinned pull-request gate

Pull requests and maintained branch pushes use two explicit kernel identities:

| Role | Revision | Purpose |
| --- | --- | --- |
| Supported post-split kernel | `c656ac58046c1d7fdb40372ae575728f526c0f01` | build daemon/client/server, CTest, install, consumer, layout/RPATH, and new client probe |
| Frozen full-stack archive | `f9fc3aefce45072c6fc6a856da11f20ff16ba00a` | build only the old client and old daemon sides of the four-cell gate |

CI checks out both revisions detached and verifies each exact `HEAD`. The
four-cell runner receives distinct old/new Photospider package directories,
then runs old-old, old-new, new-old, and new-new over separate sockets. The
frozen revision is immutable migration evidence, not the current producer
dependency.

The pinned gate also runs the maintained daemon/client/server tests, installed
consumer, install-layout/RPATH smoke, lifecycle behavior, and ownership/export
path audit. It does not depend on a sibling checkout or private kernel header.

## Compatible-main signal

A weekly Monday schedule and manual dispatch run one Ubuntu job against current
Photospider `main`. The job builds and installs the current kernel package,
then configures, builds, tests, and installs the daemon from that isolated
prefix. Failure signals that the supported tuple may need a deliberate update;
it does not change the pinned pull-request result and is not a required check
for every kernel pull request.

Kernel CI does not checkout this repository. A kernel change requests the
daemon downstream gate only when it changes an installed API/package boundary
or when a release gate explicitly requires it. Internal compiler-only changes
remain kernel-owned verification.

## Updating the supported tuple

An update must:

1. identify the exact candidate kernel revision and package version;
2. prove daemon configure/build/CTest/install/consumer/layout/lifecycle against
   an isolated installed prefix;
3. keep the old four-cell revision immutable and use the candidate only for
   the new side;
4. update the workflow constants, support matrix, README, boundary document,
   and Chinese mirrors together; and
5. record any breaking source, ABI, package, or protocol behavior explicitly.

No update may silently broaden to an arbitrary branch, same-major 0.x range,
or internal compiler representation.
