# Repository 与 Package 边界

## 决策

`photospider-daemon` 独占 same-user local IPC v3、临时 Session/Job orchestration、
`photospiderd`、typed client package 与相关 test。`photospider` 独占
WorkflowDocument、typed/optimized IR、operation trait、compiler/optimizer/planner、
local CPU/GPU execution、Value/Region/layout/memory 与 operation/provider runtime。

```text
PhotospiderDaemon::client -> installed Photospider::kernel + private IPC implementation
photospiderd -> PhotospiderDaemon::client + private server/orchestration code
```

Daemon 不链接 sibling source target，也不 include private header；不复制
compiler/planner logic 或序列化内部 IR。Kernel 不依赖 daemon。

## Public package

Daemon package 只安装 typed client header 与 `PhotospiderDaemon::client`。Frame、
codec、Unix socket、router、Session/Job registry 与 server 保持 private。不 export
raw binary/protocol escape hatch 或 server SDK。

Producer 通过 `find_package(Photospider 0.2 CONFIG REQUIRED COMPONENTS kernel)` 发现
精确支持的 Photospider 0.x package。Package version update 是有意的 breaking-
compatibility work，必须通过隔离 consumer gate。

## 显式缺失

仓库没有 IPC v2/four-cell compatibility、graph Host mirror、policy route、plugin
loader route、durable output store、stable collection cursor、remote transport、
authentication、tenant、process worker、recovery 或 durable state product。这些能力
已删除或不在范围内，不是 disabled option。

## Archive identity

重置前 daemon commit `1080548d6bb11d771c89032b7df956c9e2af3674` 由 annotated
tag `pre-breaking-scope-reset-2026-09-01` 保存。它只是历史 source，不约束 v3
package 或 wire。
