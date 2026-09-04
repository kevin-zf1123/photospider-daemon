# 当前开发计划

- 快照日期：2026-09-04
- 已审计实现 baseline：`main@602e89ab6ec63350d504bb7ae538294ce237e023`
- 当前 milestone：S1 per-Job binding 与实用 ephemeral result

## 角色与权威

本文件记录公开 delivery baseline、当前 milestone、active leaf Issue、dependency 与
执行顺序。它不能修改 ADR 0001 的 daemon 产品边界，也不能修改 IPC Protocol v3 的
wire 与 lifecycle contract。

公开 GitHub Issue 是 live delivery-status authority。本快照与 Issue 不一致时，以 Issue
为准，并同步修订本文件。
[#15 daemon-local-orchestration](https://github.com/users/kevin-zf1123/projects/15)
是 maintainer operational view，只同步 Issue 状态，不能覆盖 Issue。私有 maintainer
note 包括公开仓库以外的 OpenSpec 文件，它们没有 daemon architecture 或 delivery
authority，也不构成 completion gate。

## S0 结算 baseline

已审计实现已经具备 #2 与 #4 的 scope。#3 的 package policy 已实现，本次结算补充其缺少的
直接 same-minor probe。只有目标 commit 的 `daemon-ci` 成功，且 live Issue 与 Project
item 均关闭后，#3 才在本表中成为 delivered；在此之前，以它们的 open 状态为准。

| Issue | 当前证据与结算条件 |
| --- | --- |
| [#2](https://github.com/kevin-zf1123/photospider-daemon/issues/2) | 已交付：installed-kernel mapping、精确九方法 IPC v3、bounded Session/Job lifecycle、cancellation、result release、shutdown 与 restart loss |
| [#3](https://github.com/kevin-zf1123/photospider-daemon/issues/3) | 既有 contract：`Photospider 0.2` kernel component、same-minor compatibility 与 public-only dependency。结算条件：目标 `daemon-ci` 验证 `tests/version_probe/` 中的 exact-minor 与两个 cross-minor probe，随后关闭 Issue 与 Project item。 |
| [#4](https://github.com/kevin-zf1123/photospider-daemon/issues/4) | 已交付：隔离 kernel install、Linux/macOS static/shared matrix、installed daemon client、lifecycle test、ASAN 与 TSAN |

最新 baseline CI 是
[`daemon-ci` run 38](https://github.com/kevin-zf1123/photospider-daemon/actions/runs/33720331110)。
它在 Linux 与 macOS 上通过 static/shared installed kernel，以及 focused ASAN 与 TSAN
job。

## 当前 milestone

S1 把 caller-owned runtime Value 投影到 Job，并返回普通 image 或 tensor result；不引入
persistence、artifact identity、recovery 或 remote transport。

### Critical path

1. [#10](https://github.com/kevin-zf1123/photospider-daemon/issues/10)
   在 kernel binding contract 被接受后，冻结 per-Job binding projection、Session
   mutability decision、validation、ownership、cancellation 与 release rule。
2. [#11](https://github.com/kevin-zf1123/photospider-daemon/issues/11)
   在保留 4,194,304-byte control-frame limit 的条件下，冻结 ephemeral local
   bulk-result transport。
3. [#12](https://github.com/kevin-zf1123/photospider-daemon/issues/12)
   在 kernel execution、binding projection 与 bulk-transport contract 完成后，实现一条
   input-to-result IPC vertical。

Bulk-result decision 可以与 kernel input contract 并行。Binding projection 在 kernel
contract 被接受后开始。

## S1 决策范围

本快照不选择 bulk storage、result-generation、descriptor、cleanup 或 Session-update
语义。#10 决定 binding projection 与 Session mutability；#11 决定 ephemeral bulk
transport、platform support、ownership、lifetime、release 与 failure rule。两项决策仍受
daemon ADR 0001 约束，包括 same-user local scope、restart loss，以及不引入 artifact、
recovery、remote-service 或 tenant authority。

## Issue 执行契约

可执行 leaf Issue 记录 audited baseline commit、remaining delta、governing public
document、public/API/schema impact、start dependency、integration dependency、
completion gate、named fixture 或 vertical、精确 test 与 oracle、non-goal，以及预期
completion evidence。Parent Issue 只作为 index 与 closure aggregator，不携带
`ready-for-agent`。

## 更新规则

Audited baseline、当前 milestone、critical path 或 blocked reason 变化时更新本快照。
普通 implementation detail 保留在所属 Issue 与 test 中。每项 status claim 必须引用已
完成 code 与 test；unchecked item 不定义当前行为。
