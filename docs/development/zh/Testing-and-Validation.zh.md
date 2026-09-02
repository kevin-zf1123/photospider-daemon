# 测试与验证

本文定义 breaking scope reset 后维护中的 daemon validation。Test 覆盖长期 local
protocol、bounded orchestration、thread lifecycle、package boundary 与 malformed-input
behavior；不证明 migration completion，也不恢复任何被删除的产品领域。

## 开发与最终循环

实现期间格式化并 lint changed C++、build affected target、运行 focused test。Source 与
documentation 冻结后，最多运行一次 native clean configure、一次 full build 与一次完整
CTest/JUnit。Daemon 必须始终针对 fresh installed Photospider package 配置，绝不能使用
sibling source target 或 private kernel include path。

## 维护中的行为覆盖

- Binary codec test 覆盖精确 enum、range、UTF-8、duplicate field、trailing byte、
  request correlation 与 protocol-error sentinel。One-shot fake Unix server 证明 public
  Client 返回完整读取的 sentinel typed status，并 reset descriptor。
- 真实 Unix-socket test 覆盖 oversized `0xffffffff`、truncated、duplicate、unknown-enum、
  invalid-UTF-8 与 trailing-byte request。完整十一字节 header 后发生 body EOF 时保留
  correlation；incomplete header 只使用 sentinel。Server 返回一个 typed failure 后关闭，
  且不产生 state mutation 或 declared-length allocation。
- Session test 覆盖正值 `maximum_sessions`、无 allocation/无 id 的 backpressure、
  close/reuse、精确 cleanup 与 concurrent admission。
- Server test 覆盖正值 active-handler bound、typed backpressure、顺序运行期 reaping、
  exception fencing、shutdown join，以及带 descriptor/node cleanup 与 same-path rebind 的
  deterministic post-bind construction failure。每个 asynchronous Server test 都使用
  fail-safe run guard，在 assertion return 或 exception 时先 request stop 再 join。
  Handler-count assertion 会先等待 active zero，再显式驱动一次 accept-loop reap，而不是
  只依赖经过的时间。
- Transport test 证明即使 ambient `errno` 无关地残留，peer-uid rejection 仍只影响当前
  connection；正常 `request_stop` 成功，非 stop 的 fatal accept failure 保持 typed。
  它们检查 client、listener、accepted stream 与固定 parent descriptor 的
  close-on-exec 及 fork/exec 后不继承；只有独立 SIGPIPE self-exec 场景使用的 test-owned
  duplicate 会被显式设为可继承。
- Pathname test 在 connect/listen effect 前拒绝 embedded-NUL suffix 与 NUL 后 slash
  variant，并保留每个较短 prefix。
- 真实 `photospiderd` subprocess test 会发送 `SIGINT`、在五秒 cooperative Job 到达
  Running 后发送 `SIGTERM`，并调用 `daemon.shutdown`。它们要求有界 `exit(0)`、
  generation-checked socket removal，以及在 delayed operation 自然结束前完成
  cancellation/join。
- Installed-consumer test 证明 `bin/photospiderd` 已安装，同时 CMake export set 只含
  `PhotospiderDaemon::client`。专用 `BUILD_SHARED_LIBS=ON` package gate 还证明该
  target 保持为可用 static archive，且不创建 shared client ABI。

## 产品与 test runtime 分离

Installed `PhotospiderDaemon::client` 与正常 `photospiderd` executable 始终使用不含
test control 的 production object。启用 `BUILD_TESTING=ON` 时，
`photospider_daemon_test_runtime` 在 `PHOTOSPIDER_DAEMON_TEST_RUNTIME` 下独立编译完整
runtime source 列表，并加入 fixed exception controller。`test_local_daemon` 与
`test_exception_fences` 只链接这一不安装的 static variant；正常 binary 与 package
consumer 只链接 production target。任何 executable 都不会同时链接两个 variant。

修改 private lifecycle seam 后，须对 testing-on 与 testing-off build 的 production
archive 手动执行 `ar -t`、demangled `nm` 与 `strings` 检查。产品中不得存在 test
controller、construction stage、fault callback、handler-entry callback、lifecycle
count observer、test macro 或 test-support object。不安装的 test archive 是正向对照，
必须保留预期 seam。该 source/package audit 保持为手动检查，不注册为 CTest entry。

## Sanitizer

ASAN 与 TSAN 是独立 scoped CMake mode，不能同时启用。每个 daemon build 必须消费
使用相同 sanitizer 构建的 kernel package；instrumented daemon 与 uninstrumented static
kernel 混用既不能提供完整 boundary coverage，也可能使 standard-library container
annotation 失效：

```bash
cmake -S . -B <asan-build> -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH=<installed-kernel-prefix> -DBUILD_TESTING=ON \
  -DPHOTOSPIDER_DAEMON_ENABLE_ASAN=ON
cmake --build <asan-build> -j
ctest --test-dir <asan-build> --output-on-failure

cmake -S . -B <tsan-build> -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH=<installed-kernel-prefix> -DBUILD_TESTING=ON \
  -DPHOTOSPIDER_DAEMON_ENABLE_TSAN=ON
cmake --build <tsan-build> -j
ctest --test-dir <tsan-build> --output-on-failure
```

CI workflow 保持独立 ASAN/TSAN job，并设置十分钟 job bound。`test_local_daemon` 还有
120 秒 CTest timeout，远小于 hosted runner watchdog，同时给 TSAN 留出相称余量。Timeout
只是最终 fail-fast boundary；deterministic lifecycle settlement 与 RAII teardown 才是
根因修复。平台或 runtime limitation 必须记录为 limitation，绝不能冒充 successful
sanitizer result。

## 手动 frame/codec fuzz

`photospider_daemon_frame_codec_fuzz` 是长期手动 libFuzzer target，覆盖 bounded
request/protocol-error decoding 与真实 stream-frame reader。它使用
`EXCLUDE_FROM_ALL`，绝不注册到 CTest，并要求 Clang。若平台 sanitizer runtime
支持，也可与 daemon ASAN mode 组合：

```bash
cmake -S . -B <fuzz-build> -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH=<installed-kernel-prefix> -DBUILD_TESTING=OFF \
  -DPHOTOSPIDER_DAEMON_BUILD_MANUAL_FUZZ_TARGETS=ON
cmake --build <fuzz-build> --target photospider_daemon_frame_codec_fuzz -j
ps_daemon_fuzz_corpus=$(mktemp -d)
cp -R tests/fuzz/corpus/frame_codec/. "$ps_daemon_fuzz_corpus"/
<fuzz-build>/photospider_daemon_frame_codec_fuzz \
  "$ps_daemon_fuzz_corpus" -runs=1000 -max_len=4096
```

维护中的 seed corpus 是 `tests/fuzz/corpus/frame_codec/`。调用者选择的 crash/artifact
directory 保持 untracked。Fuzzing 补充 deterministic malformed real-socket regression，
但不能替代后者。使用确实提供 libFuzzer runtime 的 Clang distribution；temporary
working corpus 可避免 generated mutation 进入 maintained seed。

## CTest 所有权

CTest/CI entry 只用于 correctness、concurrency、error handling、package consumption、
compilation 与 runtime lifecycle。Stale-term search、source-layout audit、migration
checklist、Doxygen audit、Issue replay 与 evidence/provenance orchestration 保持手动，
不进入 CTest/CI。
