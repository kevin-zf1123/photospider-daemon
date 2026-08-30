# Photospider Daemon

`photospider-daemon` 是 Photospider local IPC version 2 的唯一仓库：它拥有 typed public
client、private framing/router/server implementation、foreground `photospiderd` sidecar、
authoritative protocol document 与长期 tests。

source 通过保留历史的方式从 Photospider commit
`f9fc3aefce45072c6fc6a856da11f20ff16ba00a` 提取；原仓 annotated tag
`full-stack-archive-2026-08-30`（tag object
`bb876cbf76882bc0a9029956d6acc9ee3fbaae0e`）归档该基线。这个 identity 只用于本次迁移和
四格 compatibility baseline，不是对任意历史版本的普遍兼容承诺。

## 产品边界

- `PhotospiderDaemon::client` 是唯一 installed library target。它 public link
  `Photospider::operation_runtime` 与 `Threads::Threads`，不链接完整 embedded kernel。
- `photospiderd` 私有消费 installed Photospider package 的 `Photospider::photospider`，并把
  恰好一个 public `ps::Host` 借给 private server/router。
- codec、frame、socket、registry、output-store、router、server targets 都是 private；不存在
  public raw-protocol implementation package。
- 本仓没有 Photospider submodule、copied kernel implementation、sibling-checkout include 或
  跨仓 private header dependency。

当前产品只支持 Darwin/Linux，是 foreground、same-user、local Unix-domain sidecar；不是
system service、multi-user service、remote endpoint 或 TCP server。

本仓现处于 IPC v2 compatible-maintenance：接受既有 surface 的 bug fix、package/CI compatibility
与 lifecycle hardening；不扩展 IPC v3、不序列化 kernel compiler IR，也不接管 kernel Job、policy、
trust、isolation、worker 或 evidence code。Next-protocol design 在 stable kernel Compiler MVP 前保持 blocked。

## 版本支持

| 版本轴 | 当前值 | 维护规则 |
| --- | --- | --- |
| Photospider package | 0.1.0 | producer/client exact dependency；package compatibility 为 same minor |
| PhotospiderDaemon package | 0.1.0 | package compatibility 为 same minor |
| IPC protocol | v2 | 精确冻结 60-method surface；不扩展 v3 |

daemon PR CI 固定 supported post-split kernel revision
`c656ac58046c1d7fdb40372ae575728f526c0f01`。上述 frozen full-stack commit 只用于 four-cell
旧侧。参见[版本与 CI 兼容契约](Version-and-CI-Compatibility.zh.md)。

## 构建

先安装 supported post-split Photospider kernel package。当前 0.1 line 使用 pinned supported revision：

```bash
git clone https://github.com/kevin-zf1123/photospider.git /tmp/photospider-kernel
git -C /tmp/photospider-kernel checkout --detach \
  c656ac58046c1d7fdb40372ae575728f526c0f01
cmake -S /tmp/photospider-kernel -B /tmp/photospider-kernel-build \
  -DBUILD_TESTING=OFF -DPHOTOSPIDER_BUILD_GRAPH_CLI=OFF \
  -DPHOTOSPIDER_BUILD_SINGLE_TENANT_JOB=OFF \
  -DCMAKE_INSTALL_PREFIX=/tmp/photospider-prefix
cmake --build /tmp/photospider-kernel-build --target photospider -j
cmake --install /tmp/photospider-kernel-build
```

随后只针对该 installation 配置 standalone daemon 仓：

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=/tmp/photospider-prefix \
  -DPHOTOSPIDER_DAEMON_DEPENDENCY_LIBDIR=/tmp/photospider-prefix/lib \
  -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build --prefix /tmp/photospider-daemon-prefix
```

`PHOTOSPIDER_DAEMON_DEPENDENCY_LIBDIR` 把 exact installed kernel runtime dir 写入
`photospiderd` install RPATH。Package discovery 同时要求 `Photospider 0.1.0 EXACT`；改变 supported
revision/version tuple 是显式 compatibility 工作，不是隐式 main-branch upgrade。

## Installed client

```cmake
find_package(PhotospiderDaemon CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE PhotospiderDaemon::client)
```

package 只 import public client，并传递查找 exact Photospider 0.1.0 `operation_runtime` component
与 Threads。

## Protocol 与 shutdown 不变量

wire 仍为 protocol version 2 与精确排序 60-method inventory。`compute.cancel` 与
`daemon.shutdown` 不是 method，返回 `method_not_found`；accepted job 报告
`cancellable: false`。`SIGINT`/`SIGTERM` 继续使用 self-pipe：先 stop admission，再 drain/join
accepted work，清理已证明 socket identity，并成功退出。

参见 [IPC-Protocol-v2.md](../codebase-structure/IPC-Protocol-v2.md) 与
[中文镜像](../codebase-structure/zh/IPC-Protocol-v2.zh.md)。

## 冻结四格门禁

migration-only runner 针对 archived client package 与本 package 分别构建同一 public probe，
再以独立 socket 执行 old-old、old-new、new-old、new-new：

```bash
python3 tools/run_frozen_compatibility_gate.py \
  --source "$PWD" \
  --old-photospider-dir /tmp/old-prefix/lib/cmake/Photospider \
  --new-photospider-dir /tmp/photospider-prefix/lib/cmake/Photospider \
  --new-photospider-daemon-dir /tmp/new-prefix/lib/cmake/PhotospiderDaemon \
  --old-daemon /tmp/old-prefix/bin/photospiderd \
  --new-daemon /tmp/new-prefix/bin/photospiderd \
  --work /tmp/photospider-daemon-four-cell
```

runner 故意不注册到 CTest：它是有界 migration evidence；ordinary unit/integration/package
consumer/daemon lifecycle tests 才是长期 product gate。

PR 与维护分支 push 会在 Ubuntu/macOS 运行该 pinned product gate。每周 Ubuntu-only
`compatible-main` job 单独检查 current kernel `main`；它是本仓 downstream drift signal，不是每个
kernel PR 的 required check。
