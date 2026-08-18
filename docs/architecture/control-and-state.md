# 控制与状态收敛

> 状态：已接受  
> 适用版本：EAVP Core 0.1  
> 规范性范围：Command、Query、StateStore 和 Reconciler

写操作使用显式类型的 Command，首期支持启动和停止模拟 Pipeline。Command 只表达意图，不暴露 Node。读操作使用 Query 返回 StateStore 快照，不直接读取运行对象。

Desired Store 与 Actual Store 是不同实例。每次有效写入生成新的单调递增版本；相同值写入不增加版本。快照不可变，可在锁外读取。

PipelineReconciler 比较目标状态与实际状态：相同则无操作；Running 目标驱动 start；Stopped 目标驱动 stop。执行成功更新 Actual，失败保留 Desired、写入 Error 和错误信息并更新 Health。Core 0.1 只提供显式 `reconcile_once`，后台重试属于 Recovery Policy 子项目。

