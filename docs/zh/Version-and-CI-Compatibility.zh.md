# 版本与 CI 兼容契约

## 独立 axis

| Axis | 规则 |
| --- | --- |
| Photospider package | 显式支持的 0.x version；隔离 installed dependency |
| PhotospiderDaemon package | 显式 0.x version；记录 source/package break |
| Local IPC | 精确 version 3 与九方法 inventory |

这些 axis 相互独立。Package compatibility 不隐含 wire compatibility，wire 也绝不
暴露内部 compiler schema version。IPC v2 属于 archive-only，没有 compatibility
adapter。

## Pull-request gate

Daemon CI：

1. 存在 matching kernel feature branch 时 checkout 它，否则在独立目录 checkout
   kernel main；
2. 把 kernel 配置、构建并安装到 fresh prefix；
3. 只使用该 prefix 配置 daemon；
4. build、运行 CTest、install，并运行 external typed-client consumer；
5. 运行 binary-codec、socket ownership/SIGPIPE、real-process signal 与 RPC shutdown、
   Session/Job/cancellation/result/restart、executable-help 与 installed-client test；
6. 针对相同 installed-kernel boundary 分别运行 bounded Clang ASAN/TSAN
   configure/build/CTest job，并具备 deterministic test teardown 与 per-test timeout
   protection。

Installed-client gate 还证明 `bin/photospiderd` 存在，同时 generated export set 只包含
`PhotospiderDaemon::client`，不包含 executable/server target。长期 frame/codec fuzz
target 是手动 developer target，不属于 default build、CTest 或 migration-residue gate。

没有 job 构建 archived kernel 或执行 old/new wire combination。不存在 frozen
four-cell gate。

## Kernel update

更新 supported kernel tuple 要求：

- 精确 kernel revision 与 package version；
- 记录 public API/package impact；
- clean isolated daemon configure/build/CTest/install/consumer result；
- public facade 改变时同步 kernel/daemon 文档；
- 不泄漏 private include、source target、internal IR 或 plugin path。

Matching-branch rule 支持 coordinated breaking cut。Merge 并删除 feature branch 后，
daemon CI 消费 kernel main。

## Test 所有权

Kernel CI 拥有 compiler/executor/Value/operation/package behavior。Daemon CI 拥有
local framing、typed client、Session/Job registry、process lifecycle 与 installed
dependency use。Installed API/package break 或显式 release gate 才请求跨仓
validation。
