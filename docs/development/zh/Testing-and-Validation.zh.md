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
  request correlation 与 protocol-error sentinel。
- 真实 Unix-socket test 覆盖 oversized `0xffffffff`、truncated、duplicate、unknown-enum、
  invalid-UTF-8 与 trailing-byte request。完整十一字节 header 后发生 body EOF 时保留
  correlation；incomplete header 只使用 sentinel。Server 返回一个 typed failure 后关闭，
  且不产生 state mutation 或 declared-length allocation。
- Session test 覆盖正值 `maximum_sessions`、无 allocation/无 id 的 backpressure、
  close/reuse、精确 cleanup 与 concurrent admission。
- Server test 覆盖正值 active-handler bound、typed backpressure、顺序运行期 reaping、
  exception fencing、shutdown join，以及带 descriptor/node cleanup 与 same-path rebind 的
  deterministic post-bind construction failure。
- Installed-consumer test 证明 `bin/photospiderd` 已安装，同时 CMake export set 只含
  `PhotospiderDaemon::client`。专用 `BUILD_SHARED_LIBS=ON` package gate 还证明该
  target 保持为可用 static archive，且不创建 shared client ABI。

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

CI workflow 保持独立 ASAN/TSAN job。平台或 runtime limitation 必须记录为 limitation，
绝不能冒充 successful sanitizer result。

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
