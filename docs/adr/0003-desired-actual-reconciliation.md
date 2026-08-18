# ADR-0003：控制采用 Desired/Actual 状态收敛

> 状态：已接受  
> 日期：2026-08-18

## 决策

写入口只修改通过校验的 Desired State，Reconciler 驱动运行对象并更新 Actual State。执行失败保留 Desired 并报告 Actual Error。

## 原因

该模型把用户意图与瞬时硬件状态分开，使查询、重试、故障恢复和审计具有稳定语义，并避免外部入口直接操作 Pipeline。

