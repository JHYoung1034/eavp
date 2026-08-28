# 线程、背压与生命周期

> 状态：已接受\
> 适用版本：稳定包 EAVP 0.2.0 与 0.3 Linux Native 开发能力\
> 规范性范围：Node/Pipeline、Linux Platform Runtime 与媒体后端的执行语义

Node 或后端实现不得创建 EAVP 不可见的控制线程。确定性 Executor 按拓扑顺序执行 tick，用于测试和模拟；0.3a Linux Platform Runtime 统一拥有生产 Reactor 线程，通过 `epoll + eventfd` 在设备 ready 时驱动 Pipeline。`tick()` 表示一次有界、非阻塞推进，不是周期计时器或线程入口。同一 Pipeline 固定绑定一个 Reactor 并始终串行执行。

首期 Runtime 只创建一个 I/O Reactor。未来多 Reactor 由产品配置并按完整 Pipeline 分片，运行期间不迁移；0.3c 的 CPU 密集工作进入 Runtime 管理的 ComputeWorkerPool，并通过 serial lane 保证同一 Node 不并发执行。厂商 SDK 的内部线程可以存在，但完成事件必须通过非阻塞 `submit`/`receive`、完成 fd 或 eventfd 进入 EAVP；只有无法非阻塞适配时才允许使用 Runtime 显式管理的专用 worker lane。

有界 Queue 支持 `BLOCK`、`DROP_OLDEST`、`DROP_NEWEST`。`BLOCK` 不执行线程阻塞，而是返回 `WOULD_BLOCK`；调度器停止推进对应上游。摄像头和麦克风是不可反压实时源：live 视频直接下游默认使用 `DROP_OLDEST`；0.3a 单执行域音频继续使用 `BLOCK`，依靠 Reactor 及时调度，避免现有 Queue 无感知丢弃后无法标记 discontinuity。0.3c 的音频感知 SPSC Queue 必须把跨域丢弃传播为下一 AudioFrame discontinuity。显式 `BLOCK` 只能提供短暂缓冲，持续过载最终仍会导致 V4L2 丢帧或 ALSA XRUN。视频特有的 `DROP_NON_KEY` 和 `FLUSH_TO_KEYFRAME` 在真实编码 Packet 接入时实现。

Node 合法主路径是 `Created -> Prepared -> Running -> Stopped`。执行错误进入 `Error`，只有 `reset` 可以返回 `Created`。Pipeline 先验证图，再按拓扑 prepare/start；部分启动失败时逆序 stop 已启动节点。重复 start 和 stop 不重复产生副作用。

Processor 与 Encoder 的后端状态为 `Created -> Configured -> Running -> Draining -> Stopped`，错误进入 `Error`。一个后端实例首次配置时绑定 Executor 线程，之后在其他线程调用返回 `kInvalidState`。`receive()` 在尚无输出时返回 `kWouldBlock`，`begin_drain()` 后只允许接收剩余输出；`reset()` 是从错误或停止状态重建可配置实例的唯一入口。

`BufferStorage` 实现必须声明自身是否允许同一 Storage 的并发或重入 `map()`；通用 `MappedRegion` 不替 Storage 串行化。无论采用何种策略，每次成功 `map()` 都必须由且仅由对应 `MappedRegion` 触发一次 `unmap()`，move 只转移这一次配对责任。只读映射不暴露可写指针。内置 CPU Storage 明确允许同一线程重入映射，也允许调用方自行同步后的并发映射；其 `unmap()` 不改变底层字节所有权，相关回归同时持有读写与只读映射并验证共享可见性。第三方 Storage 若不允许重入或并发，必须在自身 `map()` 中以明确 Status 拒绝，而不是阻塞 Executor。

Pipeline 的普通 `stop()` 按拓扑跨 Executor 调用逐步排空，`kWouldBlock` 保持 `Draining`。调用方需要放弃排空时可显式 `cancel()`，由 Pipeline 逆拓扑强制 reset；析构只尝试一次普通 stop，未收敛即执行同样的无抛异常清理，不以无界循环等待永久背压。

0.3b `AlsaSourceNode` 不创建线程，也不调用 `snd_pcm_wait`。每次 tick 至多进行一次非阻塞读取：短读仅累积，绝不补零或输出不足 `samples_per_frame` 的帧；`-EAGAIN` 或零读取返回 `kWouldBlock`。若存在 pending AudioFrame，必须优先重试下游发送；背压时不得继续读取 ALSA。XRUN/suspend 恢复有界地跨 tick 推进，恢复时丢弃 partial、重新锚定时间线，并仅在恢复后的第一个完整帧设置 `discontinuity`。`stop` 丢弃 partial/pending 并关闭设备，重复 stop/reset 保持幂等。

0.3a V4L2 与 ALSA Source 已实现 Linux readiness 接口并完成开发验收。V4L2 使用设备 fd，ALSA 保留 libasound 返回的完整 descriptor 数组并通过 libasound 解释 revents。Runtime 的 start 等待 Reactor 在线程内完成 Pipeline prepare/start；stop 使用 eventfd 唤醒，在同一 Reactor 线程 stop/reset Pipeline 后 join。不得 detach 线程，启动失败不得遗留运行线程。
