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
