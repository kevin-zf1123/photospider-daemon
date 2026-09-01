# Photospider Local IPC Protocol Version 3

本文是 local wire contract 的英文权威文档忠实中文镜像。ADR 0001 定义 product
boundary。

## 范围与方法

IPC v3 是 same-user、same-machine、non-persistent orchestration protocol。Darwin
与 Linux 使用 mode-0600 Unix-domain stream socket，并验证 peer effective uid。
精确 sorted method inventory 为：

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
并返回 typed transport failure。进程级 `SIGPIPE` disposition 不会被修改。zero、
oversized、truncated 或 trailing bytes 会使完整 message 被拒绝。Server 在受控 close
前发送一个 bounded typed failure，且绝不根据攻击者声明的 frame length 分配内存。
Reader 最多保留固定十一字节 request header 用于 correlation，并只随实际到达的 byte
增长完整 payload storage。

Payload 内 integer 使用 little-endian。Text/byte vector 使用 little-endian uint32
byte count，随后是精确 bytes。Text 必须是 canonical UTF-8，并遵守 field-specific
limit。Boolean 只能是 zero/one。Floating parameter 保留精确 IEEE-754 binary64 bits。
allocation/iteration 前验证 count。

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
`maximum_sessions` bound 会在 graph/compiler allocation、publication 或 id consumption
前检查。Registry 满时返回 `ResourceExhausted`，且不创建任何状态。

### `session.close`

Request 包含 `SessionId`。Close 原子地与新 submit 竞争，移除所有 matching Job
record/result、请求 cancellation、等待 terminal settlement，再移除 Session 与 kernel
context。后续 close 返回 `NotFound`。

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
但仍先迁移到 `Running` 再到 `Cancelled`。Running callback 可以 drain，但 accepted
cancellation 阻止 result publication。

### `job.result`

只有 `Succeeded` 返回 copied temporary in-memory result。Queued/Running 返回
`InvalidArgument`；Failed/Cancelled 返回 terminal failure。

### `job.release`

只有 terminal Job 可 release。成功会删除完整 record 与 temporary result。后续
status/result/release 返回 `NotFound`。

Caller 只能通过 `job.submit` retry，并获得 distinct identifier。

## Daemon information 与 shutdown

`daemon.info` 返回 protocol version、non-security process instance token、package
version、`unix-domain` transport、exact sorted method list、live Session/Job count 与
fixed global concurrency，以及 fixed retained-Session bound。

`daemon.shutdown` 先 ack，再停止 listener、interrupt active client connection，并让
`photospiderd` 销毁 service。Service destruction 请求 cancellation、join fixed worker，
并释放全部 Session、Job、result、GraphContext、kernel execution resource、descriptor
与 socket node。下一个 process 从 empty registry 开始。

Server 还强制执行一个正值、全局 active-connection/handler bound。Excess connection
收到 sentinel `ResourceExhausted` protocol error，并在不启动 thread 的情况下关闭。
Accepted handler thread 保持 joinable；accept loop 在正常运行期 reap completed thread，
shutdown join 每个剩余 thread。不存在 detached handler。

## Correctness invariant

- state mutation 前完成 frame、version、enum、UTF-8、count、length、overflow 与
  trailing-data validation。
- Session creation 在 registry publication 前 compile。
- Session close 与 submit 原子竞争，并拒绝后续 result publication。
- Job state 单调，cancellation 后不能发布 success。
- Worker/callback exception 被 fenced 到一个 Job failure。
- descriptor、worker、GraphContext、result 与 queue ownership 恰好 settle 一次。
- Listener construction 在 bind 前准备 allocation-backed path/configuration state，并让
  descriptor/socket-node rollback guard 持有到完整 private-state publication；每个 injected
  construction failure 后 exact path 都可立即重新 bind。
- daemon production build/package consumer 只使用 isolated installed
  `Photospider::kernel` target。

这些是 local correctness property，不是 remote-service 或 persistent-state claim。
