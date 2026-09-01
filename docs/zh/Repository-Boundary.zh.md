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

Daemon package 只 export typed client header 与 `PhotospiderDaemon::client`。它另行
安装供运行时使用的 `bin/photospiderd`；executable 有意不出现在
`PhotospiderDaemonTargets.cmake`。Client 始终是 static library，不受
`BUILD_SHARED_LIBS` 影响；package 不定义 shared client ABI。Frame、codec、Unix
socket、router、Session/Job registry 与 server 保持 private。不 export raw
binary/protocol escape hatch 或 server SDK。

`BUILD_TESTING` 绝不会改变链接到 installed client 或 `photospiderd` 的 object。
启用该选项时，同一组 runtime source 会在 private macro 下被单独编译为一个不安装的
static test variant。Lifecycle observer、construction/handler fault callback、
exception controller 与 cleanup count 只存在于该 variant。每个 test executable
只能链接 production archive 或 test variant，绝不同时链接二者，因此两个 private
class definition 不会产生 ODR 或 duplicate-symbol 歧义。

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
