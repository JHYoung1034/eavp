# EAVP Core 0.1 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**目标：** 交付可编译、可测试、可安装的 EAVP Core 0.1 和模拟端到端纵切面。

**架构：** 五个模块化库严格按单向依赖组合。媒体数据面由确定性调度器推进，控制面通过 Desired/Actual State 和 Reconciler 驱动 Pipeline，管理面输出指标与健康状态。

**技术栈：** C++11、POSIX、CMake 3.21、Ninja、CTest、GoogleTest 1.12.1。

**规格：** `docs/superpowers/specs/2026-08-18-eavp-core-baseline-design.md`

## 全局约束

- 新增或修改的项目文档优先使用简体中文。
- 生产核心仅依赖 C++11、标准库和 POSIX。
- 公共 API 使用 Status/Result，不允许异常跨模块边界。
- 首期只实现 Lite Mode 和纯软件模拟管线。

## 任务

- [ ] 建立规范、文档索引、构建预设、测试依赖和安装导出。
- [ ] 以失败测试驱动 Status/Result、ID、时间基准和 Buffer/Packet。
- [ ] 以失败测试驱动 Queue/Port、Graph、Node/Pipeline 状态机和失败回滚。
- [ ] 以失败测试驱动 StateStore、Command/Query 和 PipelineReconciler。
- [ ] 以失败测试驱动 Metrics、Health 和模拟平台纵切面。
- [ ] 完成 Debug、Release、Sanitizer、安装消费和 aarch64 交叉构建验证。
- [ ] 补齐架构、ADR、测试、ABI 和路线图文档并进行规格覆盖复核。

