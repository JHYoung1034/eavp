# 线程、背压与生命周期

> 状态：已接受\
> 适用版本：EAVP 0.2.0\
> 规范性范围：Node/Pipeline 与媒体后端的执行语义

0.2.0 不允许 Node 或后端实现创建 EAVP 不可见的控制线程。调用者通过确定性 Executor 按拓扑顺序执行 tick，使测试和故障恢复具有可重复顺序。厂商 SDK 的内部线程可以存在，但完成事件必须通过非阻塞 `submit`/`receive` 进入 EAVP；未来线程池只能替换 Executor，不得改变 Node 生命周期接口。

有界 Queue 支持 `BLOCK`、`DROP_OLDEST`、`DROP_NEWEST`。`BLOCK` 不执行线程阻塞，而是返回 `WOULD_BLOCK`；调度器停止推进对应上游。视频特有的 `DROP_NON_KEY` 和 `FLUSH_TO_KEYFRAME` 在真实编码 Packet 接入时实现。

Node 合法主路径是 `Created -> Prepared -> Running -> Stopped`。执行错误进入 `Error`，只有 `reset` 可以返回 `Created`。Pipeline 先验证图，再按拓扑 prepare/start；部分启动失败时逆序 stop 已启动节点。重复 start 和 stop 不重复产生副作用。

Processor 与 Encoder 的后端状态为 `Created -> Configured -> Running -> Draining -> Stopped`，错误进入 `Error`。一个后端实例首次配置时绑定 Executor 线程，之后在其他线程调用返回 `kInvalidState`。`receive()` 在尚无输出时返回 `kWouldBlock`，`begin_drain()` 后只允许接收剩余输出；`reset()` 是从错误或停止状态重建可配置实例的唯一入口。
