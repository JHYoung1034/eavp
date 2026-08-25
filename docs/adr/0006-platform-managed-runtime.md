# ADR-0006：平台统一管理事件 Reactor 与计算线程池

> 状态：已接受
> 日期：2026-08-25

## 背景

确定性 Executor 适合 Core 测试，但不负责等待真实 V4L2/ALSA 输入。固定周期轮询可能空转或因调度抖动耗尽设备 Buffer；每个 Node 创建私有采集线程又会破坏统一生命周期、背压和可测试性。

## 决策

平台 Runtime 统一拥有执行线程。Linux I/O 由 `epoll + eventfd` Reactor 驱动，Node 保留有界、非阻塞的 `tick()`，不创建私有线程。同一 Pipeline 固定绑定一个 Reactor 并始终串行执行。

CPU 密集任务在后续里程碑进入 Runtime 管理的 ComputeWorkerPool；跨执行域使用线程安全的有界 Queue，并以 serial lane 保证同一 Node 不并发执行。厂商 SDK 只有在无法提供非阻塞 fd/事件集成时，才可使用 Runtime 显式管理的专用 worker lane。

本 ADR 细化 ADR-0004，不替代其结论。

## 结果

- 真实输入通过设备就绪事件及时调度，不依赖忙循环或固定 sleep；
- 线程数量、停止顺序和故障传播由平台统一控制；
- 同一 Pipeline 保留确定性顺序；
- Runtime 必须处理跨线程 stop、join 和状态快照，并接受 ThreadSanitizer 验收；
- 实时输入无法被无限反压，过载必须采用有界丢弃和 discontinuity/告警语义。
