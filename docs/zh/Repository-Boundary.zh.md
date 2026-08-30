# 仓库与 Package 边界

## 决策

`kevin-zf1123/photospider-daemon` 唯一拥有 local IPC v2 source、package、protocol docs 与
verification。Photospider kernel 主仓唯一拥有 embedded Host、operation runtime、Job/worker、
policy、trust、isolation、evidence 与 kernel implementation。

daemon 仓只沿一个方向依赖 installed Photospider package：

```text
PhotospiderDaemon::client
  -> Photospider::operation_runtime + Threads::Threads

photospiderd
  -> private daemon server/client
  -> Photospider::photospider（installed embedded Host）
```

daemon export 不含 source-tree/`src/lib` path；private protocol targets 绝不安装。本次是完整
ownership migration，因此不保留旧 `Photospider::photospider_ipc_client` alias。

post-split 仓现处于 IPC v2 compatible-maintenance。它拥有 frozen surface 的 fix、package
compatibility、lifecycle hardening 与 verification，但不拥有 IPC v3 expansion 或 kernel compiler
artifacts。Kernel Host、Job/worker、policy、trust、isolation、evidence 继续属于 kernel authority。

## 版本与 dependency 边界

| 版本轴 | K0 值 | Compatibility 边界 |
| --- | --- | --- |
| Photospider package | 0.1.0 | daemon producer/installed client 要求 `EXACT`；generated package compatibility 为 same minor |
| PhotospiderDaemon package | 0.1.0 | generated package compatibility 为 same minor |
| IPC protocol | v2 | 精确冻结 60-method wire contract |

Production daemon configure 只要求 Photospider `embedded` 与 `operation_runtime`。
`operation_plugin_sdk`、`policy_sdk` 只在 `BUILD_TESTING=ON` 时请求，因为它们用于 test fixtures。
PR CI 固定 supported post-split kernel revision
`c656ac58046c1d7fdb40372ae575728f526c0f01`；weekly compatible-main job 只是 downstream drift
signal，不阻塞每个 kernel PR。

## 冻结提取 identity

- Source repository：`kevin-zf1123/photospider`
- Source commit：`f9fc3aefce45072c6fc6a856da11f20ff16ba00a`
- Annotated tag：`full-stack-archive-2026-08-30`
- Tag object：`bb876cbf76882bc0a9029956d6acc9ee3fbaae0e`
- Extracted-history main head：`de017c1cc004ef6c8497a3cddafa25d99d4da6f3`

path filter 会重写 tree/parent identity，因此 extracted commit id 不同；file history 仍可追溯，
本仓不复制或重定向 original tag object。

## 延期工作

本边界不定义 protocol v3、wire cancel/shutdown、remote/multi-user service、Host capability-facade
migration、typed compiler compatibility，或更深 Job/trust/isolation/policy/evidence 工作。
Next-protocol design 在 stable kernel Compiler MVP 前保持 blocked。即使 daemon 将来映射 stable Host
facade，internal WorkflowDocument/IR/planner artifacts 也不会因此成为 daemon authority。
