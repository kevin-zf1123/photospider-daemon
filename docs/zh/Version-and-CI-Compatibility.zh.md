# 版本与 CI 兼容契约

## 权威

本文是 standalone daemon 仓维护中的 package/version/CI contract。IPC wire behavior 由
[IPC Protocol v2](../codebase-structure/zh/IPC-Protocol-v2.zh.md) 掌握，当前 source ownership 由
[仓库与 Package 边界](Repository-Boundary.zh.md)掌握。

daemon 处于 compatible-maintenance。本文治理可复现 dependency update 与 drift detection；它不授权
protocol v3 或暴露 kernel compiler schema。

## 独立版本轴

| 版本轴 | 当前值 | 自动兼容 | Evidence owner |
| --- | --- | --- | --- |
| Photospider CMake package | 0.1.0 | daemon exact dependency；Photospider package file 接受 same minor | Photospider kernel 仓 |
| PhotospiderDaemon CMake package | 0.1.0 | package file 接受 same minor | 本仓 |
| Local IPC wire | version 2 | 精确 60-method protocol-v2 contract | 本仓 |

这些版本轴彼此独立。相同 wire version 不能证明 compiled client 可链接任意 kernel package；相同
package version 也不授权新 wire field。WorkflowDocument、IR、planner、digest、plan-cache 与
operation-trait version 是未来 kernel contracts，不是 daemon package/protocol version。

0.x 开发期间 minor version 可能 breaking，因此 generated package file 使用 `SameMinorVersion`；
同一 0.1 line 的 patch release 可被普通 package consumer 接受。本 daemon producer/exported client
仍更窄，在 reviewed compatibility-range Issue 修改 tuple 前调用
`find_package(Photospider 0.1.0 EXACT CONFIG REQUIRED ...)`。

## Component closure

Production configure 请求：

- `embedded`，由 `photospiderd` 私有使用；
- `operation_runtime`，由 `PhotospiderDaemon::client` public 使用。

`BUILD_TESTING=ON` 时，还为仓库 test fixtures 请求 `operation_plugin_sdk` 与 `policy_sdk`；它们不是
production requirements。Installed daemon package 在 import `PhotospiderDaemon::client` 前查找 exact
Photospider 0.1.0 `operation_runtime` 与 Threads。

## Pinned PR gate

PR 与维护分支 push 使用两个显式 kernel identities：

| 角色 | Revision | 用途 |
| --- | --- | --- |
| Supported post-split kernel | `c656ac58046c1d7fdb40372ae575728f526c0f01` | 构建 daemon/client/server、CTest、install、consumer、layout/RPATH 与 new client probe |
| Frozen full-stack archive | `f9fc3aefce45072c6fc6a856da11f20ff16ba00a` | 只构建 four-cell old client/old daemon 侧 |

CI detached checkout 两个 revision，并验证各自 exact `HEAD`。four-cell runner 接受不同 old/new
Photospider package directories，再以独立 socket 运行 old-old、old-new、new-old、new-new。Frozen
revision 是 immutable migration evidence，不是当前 producer dependency。

pinned gate 还运行 maintained daemon/client/server tests、installed consumer、install-layout/RPATH
smoke、lifecycle behavior 与 ownership/export path audit；不依赖 sibling checkout 或 private kernel header。

## Compatible-main signal

每周一 schedule 与 manual dispatch 在 Ubuntu 运行一个 current Photospider `main` job：先构建/安装
current kernel package，再从该 isolated prefix configure/build/test/install daemon。失败表示 supported
tuple 可能需要显式更新；它不改变 pinned PR result，也不是每个 kernel PR 的 required check。

Kernel CI 不 checkout 本仓。只有 installed API/package boundary 变化或显式 release gate 要求时，
kernel change 才请求 daemon downstream gate。Internal compiler-only change 保持 kernel-owned verification。

## 更新 supported tuple

更新必须：

1. 标识 exact candidate kernel revision 与 package version；
2. 从 isolated installed prefix 证明 daemon configure/build/CTest/install/consumer/layout/lifecycle；
3. 保持 old four-cell revision immutable，只把 candidate 用于 new 侧；
4. 一并更新 workflow constants、support matrix、README、boundary doc 与中文镜像；
5. 显式记录 breaking source、ABI、package 或 protocol behavior。

任何更新都不得静默扩展到任意 branch、same-major 0.x range 或 internal compiler representation。
