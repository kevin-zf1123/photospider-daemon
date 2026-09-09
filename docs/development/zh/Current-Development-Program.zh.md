# 当前开发计划

- 快照日期：2026-09-09
- 已审计基线：已结算 S3 `main@8816f85`
- 当前重点：kernel 0.6 安装消费；新 IPC 功能继续按需启动

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

S0 的 #2/#3/#4 已结算：九方法 IPC v3、受限 Session/Job 生命周期、取消、释放、
shutdown/restart loss 和隔离安装消费继续作为基线。

## 已结算 S2 kernel 0.4 消费维护

[#15](https://github.com/kevin-zf1123/photospider-daemon/issues/15) 在 53ec2ca 实现，
位于 kernel S2 #263/#264/#210/#211/#265/#266 之后。Daemon/Client 针对安装的 kernel
0.4 重建；schema 2 使用 tagged node output，执行显式提供空 bindings。保留 daemon
package 0.2 和 IPC v3。Codec 拒绝不支持的 declaration/reference、Float32 和非零
storage origin；借用字节复制到有界 payload。不增加逐 Job bindings 或 bulk/流式结果协议。

Static kernel 验证通过全部 14 项 runtime 加修复后的安装消费者；shared kernel 通过
15/15。覆盖 codec 拒绝、Session/Job 生命周期、取消、socket 所有权、进程信号、异常
边界、loader path，以及独立的 kernel/daemon minor-version probe。两仓独立审查在
kernel 修复后未发现剩余 blocker/required。受保护 matching-branch CI、Codex bot、
合并和 Issue/Project 结算记录在 #15 及其 PR；本地验证不单独构成交付 gate。

## S3 kernel 0.5 消费

[Issue #17](https://github.com/kevin-zf1123/photospider-daemon/issues/17) 更新安装依赖和版本检查到 kernel 0.5；现有 IPC v3 子集和 daemon 0.2 包保持。S3 快照、缓存及应用预览策略不进入 IPC。受保护合并依赖 kernel S3 #268，实际验证与结算记录在 Issue。

静态和共享的 kernel 0.5 安装消费验证均通过 15/15 CTest。
[PR #18](https://github.com/kevin-zf1123/photospider-daemon/pull/18) 记录当前提交的
CI、Codex bot 审查和 package probe 文档修正。

## 保留的 S1 功能范围

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

任务状态、授权终点和决策/实现完成条件见[任务协作](Task-Collaboration.zh.md)。

## 更新规则

Audited baseline、当前 milestone、critical path 或 blocked reason 变化时更新本快照。
普通 implementation detail 保留在所属 Issue 与 test 中。每项 status claim 必须引用已
完成 code 与 test；unchecked item 不定义当前行为。

## 已接受的排期方向，2026-09-05

维护者已接受以嵌入式图像计算为主线，本仓库新增功能按实际需求启动。
#9 至 #12 的任务及技术依赖保留；#11 仍无开始依赖。排期等待不计为技术阻塞
或完成。上文 S1 继续描述保留的功能范围，当前不自动启动这些新增功能。

Kernel ADR 0016 提供已接受的 binding/image 契约，ADR 0017 提供 S2 区域执行与存储。
当前 S4 维护消费安装的 0.6。#10/#11/#12 继续由独立决策和实现范围推进，不因兼容维护而关闭。

## S4 kernel 0.6 消费

[Issue #19](https://github.com/kevin-zf1123/photospider-daemon/issues/19) 更新安装依赖与
版本探针到 0.6，并将现有 allow_gpu 映射到显式 MetalFp32/CpuExact 规划。
IPC v3 和 daemon 0.2 保持。kernel S4 #279 合并后才能结算；验证与合并状态以 Issue 为准。
