# Photospider Daemon

`photospider-daemon` 是针对 installed Photospider compiler/executor 的同用户、本机、
非持久 orchestration layer。破坏性边界由
[ADR 0001](../adr/zh/0001-breaking-product-boundary-scope-reset.zh.md) 规定。

## 产品边界

- `PhotospiderDaemon::client` 是安装的 typed local-IPC client。
- `photospiderd` 拥有受保护 local socket、临时 Session/Job registry、bounded
  Session/Job/active handler、运行期 handler reaping、cancellation、temporary
  result 与 shutdown。
- Daemon 只依赖隔离安装的公开 Photospider package。
- 它没有 private kernel include、copied compiler/planner implementation、内部 IR
  wire encoding、plugin-path route 或反向依赖。

Session 是一个进程和一个用户 trust domain 内的逻辑 namespace。Job 是临时的，严格
使用：

```text
Queued -> Running -> Succeeded | Failed | Cancelled
```

Restart 清空全部 Session、Job 与 result。不存在自动 retry、attempt identity、
checkpoint、recovery、durable result、receipt、tenant 或 remote service。

## Local IPC v3

精确方法是：

1. `session.create`
2. `session.close`
3. `job.submit`
4. `job.status`
5. `job.cancel`
6. `job.result`
7. `job.release`
8. `daemon.info`
9. `daemon.shutdown`

Darwin 与 Linux 使用 Unix-domain stream socket。不存在 IPC v2 adapter、TCP、HTTP、
gRPC、TLS 或 remote endpoint。参见
[IPC Protocol v3](../codebase-structure/zh/IPC-Protocol-v3.zh.md)。

## Build

先把重置后的 Photospider kernel 安装到隔离 prefix，然后：

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=/absolute/photospider-prefix \
  -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build --prefix /absolute/daemon-prefix
```

若只能看到 source checkout 或 private kernel header，daemon configure 必须失败。
Test 使用 installed package 导出的公开 compile/execute/Value facade。

Executable 接受正值 `--max-sessions`、`--max-jobs`、`--max-concurrency` 与
`--max-connections` bound。容量拒绝返回 typed `ResourceExhausted`，且不创建
Session、Job 或 handler state。

## Installed client

```cmake
find_package(PhotospiderDaemon 0.2 CONFIG REQUIRED COMPONENTS client)
target_link_libraries(app PRIVATE PhotospiderDaemon::client)
```

Package 只 export typed client target。它也把 `photospiderd` runtime 安装到 `bin/`，
但不 export executable/server CMake target。Codec、router、registry、transport 与
server implementation 保持 private。

## Correctness 边界

Daemon 验证 bounded binary frame、精确 integer range、UTF-8、request correlation、
instance-scoped opaque id、Job transition、公开 kernel result
type/shape/Region/layout、cancellation publication、descriptor/thread cleanup 与 socket
lifecycle。Unix peer-uid 与 mode-0600 check 建立文档规定的 same-user local boundary；
它们不会形成 tenant 或 remote-service product。

Malformed 或 oversized frame 会收到一个 typed protocol-error response，随后 connection
关闭。若存在可恢复的合法 v3 header，则保留其 request id 与 method；否则使用文档化
correlation sentinel：request id 为零且 method 为 `daemon.info`。Sanitizer 与手动 fuzz
命令见[测试与验证](../development/zh/Testing-and-Validation.zh.md)。

重置前 IPC v2 source 只能从 Git 历史和 `pre-breaking-scope-reset-2026-09-01`
取得；它不是 active compatibility contract。
