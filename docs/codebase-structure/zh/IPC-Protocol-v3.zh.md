# Photospider Local IPC Protocol Version 3

本文是 local wire contract 的英文权威文档忠实中文镜像。ADR 0001 定义 product
boundary。

## 范围与方法

IPC v3 是 same-user、same-machine、non-persistent orchestration protocol。Darwin
与 Linux 使用 Unix-domain stream socket，并验证 peer effective uid。Socket-node
mode 不是 authentication boundary；调用者选择适当私有的 parent directory。受支持
平台上的 uid mismatch 只关闭该 accepted stream，listener 继续运行；accept、stream
preparation 与 credential syscall failure 仍是 typed fatal server failure。精确 sorted
method inventory 为：

1. `daemon.info`
2. `daemon.shutdown`
3. `job.cancel`
4. `job.release`
5. `job.result`
6. `job.status`
7. `job.submit`
8. `session.close`
9. `session.create`

不存在 remote transport、plugin-path method、internal-IR payload 或 v2 adapter。
unknown method code 或 malformed request 会在产生 registry、kernel、shutdown effect
前关闭连接或返回错误。

## Frame 与 scalar encoding

每条 message 是一个 frame：

```text
uint32 payload_size，big-endian
payload_size bytes typed binary payload
```

`payload_size` 范围为 `1..4,194,304`。read/write 处理 partial progress 与 `EINTR`。
Linux send 使用 `MSG_NOSIGNAL`。Darwin 在 connect 前为每个 client stream、在 peer
validation 前为每个 accepted stream 配置 `SO_NOSIGPIPE`；配置失败会关闭 descriptor
并返回 typed transport failure。进程级 `SIGPIPE` disposition 不会被修改。即使显式把
`SIGPIPE` 恢复为 `SIG_DFL`，通过任一 product-prepared endpoint 在 peer close 后 write
也会返回 typed transport failure，进程正常退出而不是被 signal 终止。zero、
oversized、truncated 或 trailing bytes 会使完整 message 被拒绝。Server 在受控 close
前发送一个 bounded typed failure，且绝不根据攻击者声明的 frame length 分配内存。
Reader 最多保留固定十一字节 request header 用于 correlation，并只随实际到达的 byte
增长完整 payload storage。

Client、listener、accepted stream 与固定 parent-directory descriptor 都设置
close-on-exec。Linux 使用 `SOCK_CLOEXEC` 创建 socket，并使用
`accept4(..., SOCK_CLOEXEC)` accept。若这些 atomic facility 明确不受支持，则使用经过
检查的 `F_GETFD`/`F_SETFD(FD_CLOEXEC)` fallback；任何失败都会关闭 descriptor 并返回
typed failure。Darwin 使用该 fallback，因此 concurrent fork 可能观察到有限的
create/accept-to-fcntl window；实现不声称该平台具有 atomic close-on-exec。

Payload 内 integer 使用 little-endian。Text/byte vector 使用 little-endian uint32
byte count，随后是精确 bytes。Text 必须是 canonical UTF-8，并遵守 field-specific
limit。Boolean 只能是 zero/one。Floating parameter 保留精确 IEEE-754 binary64 bits。
allocation、map insertion 或 iteration 前验证 count。通过 semantic maximum 后，每个
count 都必须满足
`count <= (remaining - required_suffix) / minimum_entry_bytes`；实现先检查 suffix
subtraction，且不执行 multiplication。这一条统一规则覆盖 Workflow 的
parameter/node/input/output、Value 的 shape/Region/stride 与 facet、selected-backend
map、named Value、fallback reason、operation timing 和 daemon method。Transfer count、
transfer bytes 与 peak live bytes 仍是三个固定 uint64 scalar，并不是 collection count。

即使 count 没有超过 semantic maximum，只要它无法装入 unread bytes，就会以
`InvalidArgument` 在任何 count-sized allocation 前拒绝。byte fence 不会重分类真正
可接纳输入的 allocation failure：private codec allocation 仍抛出 `std::bad_alloc`，
server handler exception fence 会将该耗尽报告为 `ResourceExhausted`。

## Request 与 response header

Request 起始结构：

```text
uint16 protocol_version = 3
uint64 request_id != 0
uint8  method_code in 1..9
method-specific fields
```

Response 使用相同 version、request id、method code，随后是 uint8 public kernel
`ErrorCode` 与最多 4,096 bytes 的 uint32-length UTF-8 diagnostic。成功时再携带
method-specific field。Client 要求 exact version/method/id correlation，并在
transport/response-codec failure 后失效 connection；它绝不自动 retry。

只有 received payload 以完整合法的 v3 version、nonzero request id 与 known method
开头时，才能恢复 protocol-error correlation。无法恢复时（包括 length-prefix rejection、
complete header 前的 truncation 或 unknown version/method），failed response 使用唯一
sentinel `request_id=0`、`method=daemon.info`。如果完整合法 header 已到达、后续
method-body byte 才截断，failed response 保留该 header 的 request id 与 method。普通
request 与 successful response 绝不能使用 sentinel。

Public Client 完整读到合法 failed sentinel 时，会返回 sentinel 的 typed status 并使
connection 失效；普通 response 仍要求 exact nonzero request-id/method correlation。
Sentinel 是 best-effort server response：若 transport failure 阻止完整读取，Client
返回该 transport failure。

完整写出 request 后，若尚未收到任何 response byte 就发生 clean EOF，则 public
Client 返回 `Internal` transport failure。Client 会 reset descriptor，并说明 peer 在
response 前关闭、request outcome unknown；它不会暴露 private frame reader 的
`NotFound`，因为 server 可能已经执行 request effect。Private reader 继续用
`NotFound` 表示 server 观察到普通 client close；partial response EOF 仍为
`InvalidArgument`；完整普通业务 `NotFound` response 或 failed sentinel 继续保留精确
typed status。

## Ephemeral identifier

`SessionId` 与 `JobId` 各包含两个 opaque uint64：

```text
daemon_instance != 0
monotonic_sequence != 0
```

Process 启动时创建一个 non-security instance token。Registry lookup 同时匹配两个
field，所以重启后复用的 sequence 不能命中 prior identifier。ID 不是 path、kernel
handle、authorization value、retry key 或 persistent identity。

## Workflow 与 result Value

`session.create` 携带一个 bounded public `WorkflowDocument`：schema version、node、
operation key、ordered input edge、tagged scalar parameter 与 named output。Daemon
只 decode source，创建 `GraphContext`，并通过 installed public `Compiler` 验证。它
绝不序列化 semantic IR、optimized IR、physical plan field、callback/native handle 或
DSO path。

`job.result` 编码 sorted named public `Value`。每个 Value 包含 element type、
rank/shape、Region interval、byte offset/stride、0..64 个 versioned facet（bounded
key/version/payload）与 bounded immutable bytes。Response 还包含 raw execution
timing、selected backend、transfer count/bytes、peak live bytes、fallback reason、
operation timing 与 non-security plan/result digest text。Decode 会通过 public
bounds/type/shape/Region/layout/facet validation 重新发布每个 Value；失败后不会
暴露 partial result。

## Session

### `session.create`

Request 包含一个完整 WorkflowDocument。成功返回 fresh `SessionId`。Session 只是
保留一个 immutable kernel `GraphContext` 的 logical namespace。正值、进程全局的
`maximum_sessions` bound 统计 retained Session（包括 closing record）与未发布的 pending
create。Admission 会在短暂的 `lifecycle_mutex -> sessions_mutex` 临界区内预留一个 slot，
随后才进行 graph/compiler allocation、publication 或 id consumption。GraphContext
construction 与 compilation 在两个锁之外运行。Registry 满时立即返回
`ResourceExhausted`；compilation、construction 或 insertion failure 会回滚 reservation，
且不消耗 id。第二个短临界区只发布完整 record，随后推进 monotonic id。Pending create
不会出现在 `daemon.info.active_sessions` 中。

### `session.close`

Request 包含 `SessionId`。Close 会原子地把 open record 标记为 closing，并与新 submit
竞争；此后 submit 与 repeated close 返回 `NotFound`，retained record 则继续占用 Session
capacity。Process-wide lifecycle lock 会在 Job registry 移除 matching queue-owned record
并同步完成其既有 `Queued -> Running -> Cancelled` 迁移前释放。已被 worker pop 的
matching record 会逐个在其 JobRecord mutex 下仲裁。若 worker 已发布 terminal state，
close 不请求 cancellation，并保留 state、outcome 与 result；若 record 仍 nonterminal，
close 会在持有 worker 最终 cancellation check/result publication 所使用的同一 mutex 时
请求 cancellation。Worker 若先取得 mutex，可以在 close 前发布 `Succeeded`，且
already-found reader 继续拥有有效 terminal snapshot；close 若先取得 mutex，则会在 worker
finalize 前使 cancellation 可见，worker 只能发布不含 result 的 `Cancelled`。Close 在
独立 terminal wait 前释放每个 record mutex，且等待时不持有 Job-registry、Session-registry
或 lifecycle mutex。无关 Session Job 不参与该等待，无关 Session 的 create、submit 与
close 仍可响应。Snapshot allocation failure 发生在所有 Job mutation 前，并会清除
closing 以允许 caller retry。成功 settle 后，close 重新取得 lifecycle ownership，移除
Session/kernel context 并释放 capacity。删除 terminal Job 时只移除其 registry reference，
不会 reset 已发布的 state、outcome 或 result：已经保留 shared record 的 result handler
会完成一份 coherent snapshot，erase 后才开始的每个 lookup 都返回 `NotFound`。

## Job

唯一 lifecycle：

```text
Queued -> Running -> Succeeded | Failed | Cancelled
```

### `job.submit`

Request 包含 live `SessionId`、`allow_gpu` 与 `maximum_parallelism`；不重复 workflow。
Global retained-record bound 提供普通 backpressure；rejection 不创建 `JobId`。成功
返回处于 `Queued` 的 fresh `JobId`。

### `job.status`

Request 包含 `JobId`。成功返回 JobId、SessionId、current state 与 terminal Status。
Observation 是 non-destructive。

### `job.cancel`

对 terminal Job 的 cancellation 是 idempotent。Queued Job 记录 cooperative request，
但仍先迁移到 `Running` 再到 `Cancelled`。Session close 可以在精确移除 queued
ownership 后同步完成相同迁移。Running callback 可以 drain，但 accepted cancellation
阻止 result publication。

### `job.result`

只有 `Succeeded` 返回 copied temporary in-memory result。Queued/Running 返回
`InvalidArgument`；Failed/Cancelled 返回 terminal failure。成功 registry lookup 会在取得
record mutex 前保留 shared ownership。并发 release 或 Session close 可以删除 registry
reference 并释放 logical Job capacity，但不能清除该 retained terminal snapshot。旧 reader
完成 result deep-copy；physical record/result storage 会在最后一个 worker/handler/shared
owner 释放后 retire。正值 active-handler bound 同时限制这段宽限期内的 reader 数量。

### `job.release`

只有 terminal Job 可 release。成功会验证 terminal state，并在不修改 record 的情况下删除
registry reference。后续 status/result/release 返回 `NotFound`；已经完成 find 的 reader
仍可在自然析构前完成 immutable terminal snapshot。

Caller 只能通过 `job.submit` retry，并获得 distinct identifier。

## Daemon information 与 shutdown

`daemon.info` 返回 protocol version、non-security process instance token、package
version、`unix-domain` transport、exact sorted method list、live Session/Job count 与
fixed global concurrency，以及 fixed retained-Session bound。Active Session count
包含 retained closing record，但不包含未发布的 pending create。

`daemon.shutdown` 会在完整 success response 与所有 fault-capable operation 都完成 stage
之后，于 no-throw Service commit 被接受。该 commit 会 atomically 关闭后续普通 admission
并标记该 response 已接受；commit 前 dispatch failure 不会执行其中任何一项。并发 shutdown
request 可以全部被接受，并收敛到相同 monotonic state。Server 在 response encoding 前保存
acceptance，随后无论 acknowledgement 的结果如何，都从 handler 的共同 no-throw tail
停止 listener。Encoding failure、real write failure 或 catch response 的 best-effort failure
都不能撤销或搁置 accepted shutdown。未收到 success acknowledgement 的 client 必须把 request
outcome 视为 unknown，即使 server 仍会完成已经接受的 stop。Service destruction 请求
cancellation、join fixed worker，并释放全部 Session、Job、result、GraphContext、kernel
execution resource、descriptor 与 socket node。下一个 process 从 empty registry 开始。

POSIX executable 在构造 Server 或任何 worker 前阻塞 `SIGINT`/`SIGTERM`，并由专用
`sigwait` thread 同步消费；该 thread 只请求同一 stop path。另一个预先阻塞、仅供
waiter 使用的 `SIGUSR1` 会在普通 `daemon.shutdown` 完成时唤醒 waiter，使其无需 polling
即可 join；Server/worker 销毁后恢复 main thread 原始 signal mask。Signal handling
绝不在 asynchronous handler 中运行 C++ cleanup，也不安装 global `SIG_IGN`。

Server 还强制执行一个正值、全局 active-connection/handler bound。Excess connection
收到 sentinel `ResourceExhausted` protocol error，并在不启动 thread 的情况下关闭。
Accepted handler thread 保持 joinable；accept loop 在正常运行期 reap completed thread，
shutdown join 每个剩余 thread。不存在 detached handler。

## Socket pathname lifecycle

Startup 只接受不存在的 socket pathname。Live socket、stale socket、regular file、
symlink 或任何其他已有 directory entry 都会产生 typed startup failure，并保持原样。
Daemon 不执行 stale-node probe、automatic unlink、crash recovery 或 lock-file recovery。
若并发 attempt 都观察到 path 不存在，则依靠 Unix 原子 `bind` result 选出唯一 owner。

Empty、over-bound 或包含 embedded NUL 的 pathname 返回 `InvalidArgument`。Validation
会在 split、parent open、socket creation、bind 或 connect 前检查完整 `std::string`，
因此 POSIX NUL termination 不能把操作重定向到较短 prefix。

Bind 前，listener 把所有 allocation-backed parent path、leaf 与 fixed-directory
capability 移入 inactive guard。Bind 后至 arm 前，只执行不分配的 system observation，
以捕获 parent/socket 的精确 `st_dev` 与 `st_ino`。Guard 状态为
`Empty -> Prepared -> Armed -> Consumed`：capture failure 会放弃未经验证的
`Prepared` state 并保留当前 path；capture 成功则 arm exact-generation cleanup。
Construction rollback、normal shutdown、signal shutdown 与 destruction 共享这个
move-only guard。Cleanup 先重新验证固定 parent descriptor、parent pathname、socket
type 与两组 generation；mismatch 或 replacement 会保留当前 path。正常匹配的 cleanup
移除 node 并允许 clean restart。

Listener setup 不执行 bind 后 pathname `chmod`。Socket node 的 ambient mode 由调用者
目录和 process umask 决定，不是 authentication boundary，也从不用于授权 peer。Peer
credential 仍是 same-user acceptance check，host 会拒绝 non-owner peer。Portable POSIX
没有 conditional compare-and-unlink primitive：最终 `fstatat` 与
`unlinkat` 是两条独立 call，实现不声称能防御 hostile same-uid writer 恰好在两者之间
发起的 race。

## Correctness invariant

- state mutation 前完成 frame、version、enum、UTF-8、count、length、overflow 与
  trailing-data validation。
- Session creation 在短暂的 lifecycle/Session 锁序下对 retained-plus-pending capacity
  reservation，在两个锁之外 compile，且只发布完整 record。每个 pre-publication failure
  都会精确回滚一个 reservation 且不消耗 id；pending create 不是 active Session。
- Session close 与 submit 原子竞争，在标记 closing 期间保持占用 capacity，等待自身已
  pop work 前释放 global lifecycle serialization，并精确移除 queued ownership。已 pop
  record 的 close cancellation 与 worker final publication 共享 record mutex：已由 worker
  发布的 terminal result 会在不 cancel 的情况下保留；close 若先对 nonterminal record
  作出决策，则会在 worker 最终检查前请求 cancellation，并禁止 result publication。
  Pre-mutation snapshot failure 会重新打开 Session；成功 settle 后 erase，并只通过 fresh
  id 复用 capacity。
- Job state 单调，cancellation 后不能发布 success。
- Release 与 Session close 删除 terminal registry reference 时不清除已发布的
  state/outcome/result。已经完成 lookup 的 reader 拥有 coherent deep-copy source；fresh
  lookup 返回 `NotFound`，logical capacity 在 erase 时释放，physical storage 随最后一个
  shared owner retire。
- Service dispatch exception 会保留 request correlation、清空所有 success-only payload
  与 `shutdown_after_write`，并产生一个 typed failure。Standard exception 的 diagnostic
  是 nullable borrowed pointer；null 会在受保护的 failure-Status construction 内规范化
  为空 message，secondary construction failure 则保留既有 allocation-free fallback。
- Worker/callback exception 被 fenced 到一个 Job failure。Worker catch boundary 只传递
  nullable diagnostic pointer；null 规范化为空 message，secondary construction failure
  仍会保留 cancellation 或 primary error category，并发布 terminal state 与 cleanup。
- descriptor、worker、GraphContext、result 与 queue ownership 恰好 settle 一次。
- Listener construction 在 bind 前准备 allocation-backed path/leaf/guard state，在成功
  bind 与 generation arm 之间不执行可能分配或抛异常的 C++ operation，并让
  descriptor/socket-node rollback guard 持有到完整 private-state publication。Pre-arm
  allocation failure 不创建 node，exact path 可立即重新 bind。无法确认的 capture 会保留
  path；exact-generation cleanup 从不删除或修改 replacement，包括其 mode。
- daemon production build/package consumer 只使用 isolated installed
  `Photospider::kernel` target。

这些是 local correctness property，不是 remote-service 或 persistent-state claim。
