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
`session.close` 会拒绝新 submit、取消未完成 Job、移除仍由 global queue 拥有的
matching record，只等待已被 worker pop 的 matching work，释放全部临时 result 并销毁
context。Queue removal 会同步完成既有 `Queued -> Running -> Cancelled` lifecycle；
无关 Session 的 Running Job 不参与该等待。Daemon restart 清空所有 Session。

Session retention 具有一个正值、进程全局的 `maximum_sessions` bound。Admission 在构造
graph/compiler 或消耗 identifier 前检查容量；registry 满时返回 `ResourceExhausted`。
Close 在自身 record settle 后精确释放一个 slot，因此后续 create 可复用容量，但不会
复用 identifier，也不会等待无关 execution。

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
bind 由 operating system 原子裁决。所有 allocation-backed path、leaf 与 guard state
都在 bind 前准备完毕。Bind 成功后，只用不分配的 system observation 通过固定 parent
directory descriptor 捕获 parent 与 socket 的 `st_dev`/`st_ino` generation，然后才
arm guard。若 capture 无法确认，则放弃 cleanup 并保留 pathname；guard 一旦 armed，
cleanup 只 unlink 当前仍匹配的 socket，并保留所有 mismatch。Portable POSIX 的
`fstatat` 与 `unlinkat` 仍是两条独立指令，因此这里提供 fail-closed generation
hygiene，而不声称能对 hostile same-uid writer 实现原子 compare-and-unlink。
Automatic stale-node reclaim 不属于本次 ephemeral reset；crash residue 由 operator
显式移除。

Daemon 不执行 bind 后的 pathname `chmod`，也不把 socket node 的 ambient mode 当作
authentication boundary。Embedding caller 选择适当私有的 parent directory。
Same-user connection acceptance 由 peer credential 强制执行；host 继续拒绝 uid 与
daemon effective uid 不匹配的 peer。

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

Shutdown acceptance 具有一个显式 Service linearization point。Dispatch 会先 stage 完整
successful Response，并完成所有可能 allocate、throw 或触发 test fault 的 operation；然后
只执行 no-throw 的 `shutting_down=true` 与 `shutdown_after_write=true` commit。该 commit
前的 dispatch failure 会让两个 flag 都保持 false，并继续开放普通 admission；并发 shutdown
request 可分别被接受，并收敛到同一个 monotonic fence。Server 会在 dispatch 后立即保存
accepted shutdown。一旦保存，无论 acknowledgement encoding 失败、peer 让 real write
失败、catch path 只能发送 best-effort failure，还是 acknowledgement 成功，handler 都会从
共同 no-throw tail 调用幂等 `stop()`。因此，transport success 只决定 client 知道什么，
绝不决定已经接受的 shutdown 是否完成。

`photospiderd` 在构造 Server/service worker 前阻塞 `SIGINT`、`SIGTERM` 与一个仅供
waiter 完成唤醒的 `SIGUSR1`。专用 thread 使用 `sigwait` 同步消费 blocked set；外部
stop signal 只调用 thread-safe `Server::request_stop`。普通 RPC shutdown 设置
completion flag、定向发送 `SIGUSR1` 唤醒 waiter 并 join。随后 ordinary control flow
销毁 Server/service state、取消并 join work、执行 generation-checked socket cleanup、
drain pending managed signal，并恢复 main thread 原始 mask。没有 asynchronous handler
执行 C++ cleanup，也不安装 process-wide signal ignore。

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

Unix socket path/generation 检查属于本地 lifecycle correctness。Peer credential
强制执行 same-user connection acceptance；socket-node mode 不认证 peer。Same-user
身份不允许旧实例删除或修改 replacement inode。这些检查都不是 tenant isolation。

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
