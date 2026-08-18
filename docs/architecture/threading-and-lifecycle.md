# 线程、背压与生命周期

> 状态：已接受  
> 适用版本：EAVP Core 0.1  
> 规范性范围：Node/Pipeline 执行语义

Core 0.1 不允许 Node 创建私有线程。调用者通过确定性 Executor 按拓扑顺序执行 tick，使测试和故障恢复具有可重复顺序。未来线程池只能替换 Executor，不得改变 Node 生命周期接口。

有界 Queue 支持 `BLOCK`、`DROP_OLDEST`、`DROP_NEWEST`。`BLOCK` 不执行线程阻塞，而是返回 `WOULD_BLOCK`；调度器停止推进对应上游。视频特有的 `DROP_NON_KEY` 和 `FLUSH_TO_KEYFRAME` 在真实编码 Packet 接入时实现。

Node 合法主路径是 `Created -> Prepared -> Running -> Stopped`。执行错误进入 `Error`，只有 `reset` 可以返回 `Created`。Pipeline 先验证图，再按拓扑 prepare/start；部分启动失败时逆序 stop 已启动节点。重复 start 和 stop 不重复产生副作用。

