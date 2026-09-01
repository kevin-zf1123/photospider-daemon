# ADR 0001：将 Daemon 定位为临时本地编排层

- 状态：已接受
- 日期：2026-09-01
- 决策类型：0.x 破坏性产品边界重置
- 归档标签：`pre-breaking-scope-reset-2026-09-01`
- 已归档 daemon commit：`1080548d6bb11d771c89032b7df956c9e2af3674`
- 配套 kernel 决策：`photospider/docs/adr/0015`

## 背景

抽取后的 daemon 最初把 local IPC v2 维护成旧 embedded Host 的冻结 60-method
facade。它也保留 four-cell compatibility gate、graph route 镜像、进程全局
policy/plugin control、稳定 collection snapshot 和受保护 output artifact。该
surface 固化了旧产品拆分，而不是为 typed compiler/executor kernel 提供所需的精简
orchestration layer。

这是有意进行的 0.x 破坏性重置。IPC v2 及其 four-cell contract 只属于 archive。
源码通过 Git 历史和上述 annotated tag 保留；active tree 不保留 adapter、关闭的
target、备用协议或归档源码副本。

## 决策

### 产品角色与依赖方向

`photospider-daemon` 是同一用户、本机、非持久 orchestration layer。它只依赖
隔离安装的公开 Photospider package：

```text
photospiderd 与 PhotospiderDaemon::client
  -> 已安装的 Photospider 公开 compile/execute/value API
```

Daemon 不 include kernel 私有 header、不链接 source-tree target、不复制
compiler/optimizer/planner code、不序列化内部 IR，也不会成为 kernel 的依赖。

### Local Session

`SessionId` 命名当前 daemon 进程拥有的一个逻辑命名空间。多个 Session 共享同一
用户、进程、operation set 和 trust domain。Session 不是 tenant、principal、
authorization scope、sandbox 或 resource-isolation boundary。

`session.create` 通过已安装公开 API 创建新的 kernel `GraphContext`。
`session.close` 会拒绝新 submit、取消未完成 Job、等待其有界清理、释放全部临时
result 并销毁 context。Daemon restart 清空所有 Session。

Session retention 具有一个正值、进程全局的 `maximum_sessions` bound。Admission 在构造
graph/compiler 或消耗 identifier 前检查容量；registry 满时返回 `ResourceExhausted`。
Close 精确释放一个 slot，因此后续 create 可复用容量，但不会复用 identifier。

### 临时 Job

每次被接收的 submit 都得到全新的不透明 `JobId`。状态机严格为：

```text
Queued -> Running -> Succeeded | Failed | Cancelled
```

终态不可变。取消是 cooperative best-effort；daemon 将其转发给 kernel
cancellation source，并拒绝取消或 Session close 之后的 stale completion/result
publication。调用 `job.release` 会移除终态记录和临时 result。允许普通进程全局
concurrency limit 与 backpressure。

不存在 `JobAttemptId`、自动 retry、checkpoint、recovery journal、持久 Job
specification、artifact id、output commit、receipt、按 tenant quota 或 restart 后
保留状态。调用者 retry 是新 submit，并产生新 `JobId`。

### Local IPC v3

协议严格暴露九个方法：

1. `session.create`
2. `session.close`
3. `job.submit`
4. `job.status`
5. `job.cancel`
6. `job.result`
7. `job.release`
8. `daemon.info`
9. `daemon.shutdown`

受支持 POSIX build 使用 Unix-domain stream socket。受支持 Windows port 可在本机
named-pipe abstraction 上实现相同 frame/protocol contract。产品没有 TCP、HTTP、
gRPC、TLS、remote address、remote worker、v2 compatibility adapter 或双协议。

Socket startup 明确不提供 recovery。任何已有 pathname（包括 live/stale socket、
regular file 或 symlink）都直接拒绝，不 unlink 也不 replace。Path 不存在时，并发
bind 由 operating system 原子裁决。Bind 成功后通过固定 parent directory descriptor
记录 parent 与 socket 的 `st_dev`/`st_ino` generation。Cleanup 只 unlink 当前仍匹配
的 socket；任何 mismatch 或无法确认的观察都保留。Portable POSIX 的 `fstatat` 与
`unlinkat` 仍是两条独立指令，因此这里提供 fail-closed generation hygiene，而不声称
能对 hostile same-uid writer 实现原子 compare-and-unlink。Automatic stale-node
reclaim 不属于本次 ephemeral reset；crash residue 由 operator 显式移除。

Wire 传输 `WorkflowDocument` input 和公开 execution option/result。绝不传输
semantic IR、optimized IR、execution-plan internal、plugin path、DSO handle、
device handle、cache internal 或 kernel object pointer。

每个 malformed 或 oversized frame 在受控关闭 connection 前最多收到一个 typed error
response。若能恢复完整合法的 v3 header，response 保留其 request id 与 method；否则使用
显式 sentinel `request_id=0`、`method=daemon.info`。该 sentinel 只对失败的
protocol-error response 合法。

### Result 生命周期

成功 Job 持有内存中的公开 kernel result，直到 `job.release`、Session close、
有界终态逐出或 daemon shutdown。`job.result` 返回 typed public value encoding。
Result 没有持久 artifact identity、filesystem publication protocol、lease、
receipt、retention promise 或 recovery behavior。

### Shutdown 与 restart

`daemon.shutdown` 停止 admission、取消 queued/running Job、唤醒被阻塞的本地
connection、join 自有线程、释放 result 与 Session、只移除经过验证的 socket path，
然后退出。Signal shutdown 使用同一 cleanup path。进程 restart 从空 registry
开始。

Connection handling 具有一个正值、进程全局的 active-handler bound。超过该 bound 的
admission 返回 `ResourceExhausted`，并在不启动 thread 的情况下关闭。Handler thread
保持 joinable；accept loop 在 server 运行时 join 并删除 completed record，shutdown
join 所有剩余 thread。不存在 detached handler。

### 保留 correctness validation

Daemon 保留 defensive validation，但不宣称 security product：

- bounded frame length、精确 integer range、有效 UTF-8、唯一 object key 和各方法
  required field；
- malformed frame、correlation、opaque-id、state-transition 与 result-shape
  rejection；
- kernel 公开 type/shape/`Region`/layout/facet error 保持为稳定 failure；
- stale handle、stale completion 和取消后 publication rejection；
- exception fencing、精确 descriptor/thread/result cleanup 与 bounded
  backpressure；
- 平台支持时的负向、并发、restart-loss、Session-close、cancel、result release、
  ASAN、TSAN 与 fuzz test。

Unix socket mode/generation 检查是本地 lifecycle correctness 和同用户 path hygiene。
Same-user 身份不允许旧实例删除 replacement inode；这些检查不是 authentication 或
tenant isolation。

## 精确非目标

- IPC v2 compatibility 或 frozen four-cell gate。
- 覆盖每个 kernel operation 的完整 remote facade。
- Authentication、authorization、Principal、Tenant、role、capability 或
  multi-tenant quota。
- Remote access 或 worker execution。
- Durable Job、attempt、retry、checkpoint、recovery、artifact、receipt、
  backup/restore、deploy 或 rollback。
- Policy plugin、plugin admission、cryptographic trust、process isolation 或
  sandboxing。
- 通过 IPC 加载 operation/provider plugin。

这些领域已删除或不在范围内，不是 deferred 或 default-disabled。

## 被取代的权威

本 ADR 是最高 active daemon 产品边界权威。它取代旧 repository boundary、
version/CI compatibility contract、local IPC v2 protocol、four-cell
compatibility tooling，以及任何把这些材料视为维护中行为的 active Issue 或
Project description。重置前 archive tag 只是历史证据，不得被链接为 active
authority。

## 后果

- 既有 v2 client 不能连接 v3，也不提供 compatibility layer。
- 安装后的 daemon client 和 executable 要求重置后的公开 kernel package。
- Test 和 CI 验证九方法本地产品及隔离 installed-package boundary，而不是迁移兼容。
- 重新引入任何已删产品领域，都需要一个明确取代本决策的新 breaking ADR。
