# EAVP Linux Platform Runtime 0.3a 设计规格

> 状态：已批准
>
> 适用版本：`0.3a`（`0.3.0` 开发阶段），稳定版本仍为 `0.2.0`
>
> 规范性范围：Linux 事件驱动 I/O 调度、平台统一线程管理、Pipeline 串行执行、实时采集背压与 0.3c 计算线程池边界

## 1. 背景与决策

Core 0.1 的 `DeterministicExecutor` 通过调用 `MediaPipeline::tick()` 提供了可重复的测试调度，但它没有设备就绪等待、长期运行循环或调度时限，不能直接作为产品运行时。固定周期轮询会在空闲时浪费 CPU，并可能因周期或抖动过大导致 ALSA XRUN、V4L2 Buffer 耗尽和帧序列缺口。

EAVP 保留 `MediaNode::tick()`，但把它严格定义为“由 Executor 在适当时机调用的一次有界、非阻塞推进”，而不是计时器或线程入口。0.3a 新增平台统一管理的 Linux Runtime：I/O Reactor 使用 `epoll` 等待 V4L2、ALSA、timerfd 和 eventfd；Node 不创建私有线程。同一 Pipeline 的全部生命周期和 tick 始终串行执行。

本决策细化 ADR-0004，不替代其“Node 不拥有线程、Runtime/Executor 统一管理调度”的结论。

## 2. 目标

0.3a Runtime 必须交付：

1. Linux `epoll + eventfd` 的事件驱动 Reactor；
2. V4L2 单 fd 与 ALSA 多 poll descriptor 的统一 readiness 边界；
3. 平台统一创建、停止和 join Reactor 线程，Node 不创建线程；
4. 同一 Pipeline 永不并发执行，并固定绑定同一 Reactor；
5. 设备就绪后执行一次完整拓扑轮转，积压设备通过 level-triggered readiness 再次被调度；
6. 可从其他控制线程有界唤醒和停止 Reactor；
7. 启动失败、设备丢失、poll 错误和 Pipeline 错误的可观测传播；
8. Fake API、真实 V4L2/ALSA 回归、ThreadSanitizer 和三套 ARM 交叉编译验收。

## 3. 非目标

本阶段不实现：

- 通用任务窃取线程池或任意 Node 的并发 tick；
- 运行中动态增删、迁移 Pipeline 或修改 descriptor 集合；
- CPU affinity、`SCHED_FIFO`、自动线程数探测或 NUMA 策略；
- 热插拔自动重连、Service Mode 主循环或跨进程调度；
- 0.3c 的 ComputeWorkerPool、跨执行域 SPSC Queue 或 FFmpeg 编码；
- 无界输入缓存，或用增加 Buffer 掩盖持续吞吐不足。

## 4. 调度分层

```text
PlatformRuntime
  -> IoReactorGroup
       -> LinuxEventLoopExecutor
            -> epoll/eventfd
            -> MediaPipeline::tick()
                 -> MediaNode::tick()

0.3c 以后：
PlatformRuntime
  -> IoReactorGroup
  -> ComputeWorkerPool
       -> serial lane / strand
       -> bounded SPSC Queue
```

`DeterministicExecutor` 保留在 `media`，继续用于单元、Fake 和模拟测试。Linux Runtime 位于 `platform`，不改变 `media -> base` 的依赖方向。首期 `IoReactorGroup` 只创建一个 Reactor；未来允许由产品配置增加 Reactor 数量，但一个 Pipeline 在运行期间不得迁移。

## 5. Readiness 接口

新增 Linux 专用公开接口：

- `include/eavp/platform/linux/wait_source.hpp`
- `include/eavp/platform/linux/platform_runtime.hpp`

概念接口如下：

```cpp
namespace eavp {

class LinuxWaitSource {
public:
    virtual ~LinuxWaitSource() {}

    virtual Result<std::vector<struct pollfd> > poll_descriptors() = 0;
    virtual Result<bool> evaluate_poll_events(
        const std::vector<struct pollfd>& descriptors) = 0;
};

class LinuxPlatformRuntimeConfig {
public:
    static Result<LinuxPlatformRuntimeConfig> create(
        int reactor_count,
        int stop_timeout_ms);

    int reactor_count() const;
    int stop_timeout_ms() const;
};

enum class PlatformRuntimeState {
    kCreated,
    kStarting,
    kRunning,
    kStopping,
    kStopped,
    kError,
};

class LinuxPlatformRuntime {
public:
    static Result<std::unique_ptr<LinuxPlatformRuntime> > create(
        const LinuxPlatformRuntimeConfig& config,
        MetricRegistry* metrics);
    ~LinuxPlatformRuntime() noexcept;

    Status register_pipeline(
        MediaPipeline* pipeline,
        const std::vector<LinuxWaitSource*>& wait_sources);
    Status start();
    Status stop();
    PlatformRuntimeState state() const;
    Status last_failure() const;
};

}  // namespace eavp
```

接口只借用 Pipeline 和 WaitSource；它们必须在 Runtime 完成 stop 和析构前保持有效。平台组合对象必须通过成员声明和显式 stop 保证 `Runtime -> Pipeline -> Node` 的销毁顺序。空 Pipeline、空 wait source、重复指针、重复 fd 或一个 Pipeline 重复注册均返回 `kInvalidArgument`/`kAlreadyExists`。

0.3a 只接受 `reactor_count=1`；其他正值在多 Reactor 实现进入规格前返回 `kUnsupported`。`stop_timeout_ms` 必须为正，默认产品配置使用 2000 ms。

注册只允许在 Runtime 启动前进行。0.3a 的 descriptor 集合在 Pipeline start 后取得，并在 stop 前保持稳定。V4L2 Source 返回设备 fd；ALSA Source 返回 `snd_pcm_poll_descriptors_count()` 指定长度和原始顺序的完整数组，并通过 `snd_pcm_poll_descriptors_revents()` 解释结果，不能假设 capture 必然对应单一 `POLLIN` fd。

## 6. 线程与生命周期

`LinuxPlatformRuntime::start()` 创建平台拥有的 Reactor 线程，并等待该线程完成以下操作后才返回：

1. 按注册顺序 prepare/start Pipeline；
2. 获取并验证所有 descriptors；
3. 创建 epoll 和非阻塞、close-on-exec eventfd；
4. 注册描述符并进入 Running。

任一启动步骤失败时，已启动 Pipeline 按逆注册顺序 stop/reset，关闭 epoll/eventfd，join 线程，并把原始 enriched Status 返回给调用者。不得把异步启动失败伪装成 start 成功。

运行期间：

- Reactor 使用 level-triggered epoll；
- 同一次等待中属于同一 Pipeline 的多个 ready source 合并为一次 Pipeline tick；
- 每次 wakeup 对每个 ready Pipeline 最多执行一次完整拓扑轮转；
- 设备仍有积压时，level-triggered epoll 立即再次返回；
- 同一 Pipeline 不允许重入或并发 tick；
- `EPOLLERR`、`EPOLLHUP` 和 source 解释失败优先于普通 readable 事件；
- `epoll_wait(EINTR)` 最多连续重试 64 次，达到上限返回 enriched `kIoError`；
- Pipeline 致命失败使 Runtime 进入 Error，保存 `last_failure` 并停止正常调度。

`stop()` 可从控制线程调用，通过 eventfd 唤醒 Reactor。Pipeline 的 stop/reset 必须在其绑定 Reactor 线程执行；`kWouldBlock` 在 `stop_timeout_ms` 内继续有界排空，超过期限则执行 `cancel()` 并返回 `kTimeout`。之后 Reactor 关闭 descriptors 并退出，控制线程 join 后才返回。重复 stop 幂等，不使用 detached thread。析构函数执行 noexcept 的 cancel、唤醒和 join 兜底。Node 若违反“单次 tick/stop 有界”的基础契约，Runtime 无法安全抢占该调用，属于 Provider 缺陷。

## 7. 实时输入与背压

摄像头和麦克风属于不可反压的实时源。下游不消费并不会暂停硬件输入：V4L2 最终会耗尽 queued buffers，ALSA 最终会 XRUN。因此应用层 Queue 只能表达有限延迟预算，不能保证无损吸收持续过载。

0.3a 规则如下：

- 单执行域轻量管线沿用现有有界 Queue；
- live capture 的直接下游默认使用 `kDropOldest`；
- 显式选择 `kBlock` 时允许短暂 pending，但产品必须接受硬件层最终丢帧/XRUN；
- V4L2 丢帧通过 queue drop、sequence gap 和 overrun 指标报告；
- ALSA XRUN 或应用层音频丢弃后，下一完整 AudioFrame 必须标记 discontinuity；
- 持续处理吞吐低于输入速率时必须按策略丢弃并发布告警，不能无限扩容。

0.3c 引入 ComputeWorkerPool 时，I/O Reactor 与计算线程之间使用线程安全的有界 SPSC Queue。每个 Node 实例由 serial lane/strand 保证同一时刻最多执行一个任务；I/O 任务不得被软件编码等 CPU 密集任务长期阻塞。

## 8. 错误模型

Runtime 公共 API 不允许异常跨边界。`std::bad_alloc` 映射为无消息 `kResourceExhausted`，其他异常映射为无消息 `kInternal`。

系统错误使用 `provider_id="linux_runtime"`，operation 为具体 API，例如 `epoll_create1`、`epoll_ctl`、`epoll_wait`、`eventfd`、`read(eventfd)` 或 `write(eventfd)`，native code 保存 errno。错误优先级为：

1. 设备/WaitSource 或 Pipeline 的媒体根因；
2. 破坏 Reactor 继续运行的 epoll/eventfd 错误；
3. Metrics 写入失败。

设备断开由具体 WaitSource 转换为 `kDeviceLost`；Runtime 不自动重连。`last_failure()` 返回线程安全快照；正常 stop 不覆盖已有致命根因。

## 9. 可观测性

首期 MetricRegistry 没有 labels，Runtime 使用稳定全局名称：

- Counter `runtime.poll.wakeups`；
- Counter `runtime.poll.interrupted`；
- Counter `runtime.pipeline.turns`；
- Counter `runtime.pipeline.failures`；
- Gauge `runtime.reactor.running`，取值 0 或 1。

V4L2 sequence gap、Queue drop 和 ALSA XRUN 继续由设备 Node 的既有指标统计，Runtime 不重复生成缺少设备语义的聚合 overrun。指标写入失败不得导致线程异常退出。当媒体错误与指标失败同时发生时保留媒体根因；仅指标失败时 Runtime 返回指标 Status。0.3a 不加入高基数动态标签或直方图。

## 10. 构建与可移植性

新增选项：

```text
EAVP_ENABLE_LINUX_RUNTIME=ON
EAVP_ENABLE_THREAD_SANITIZER=OFF
```

- Linux Runtime 默认开启，非 Linux 平台强制关闭；
- Runtime 只依赖 C++11、Threads、POSIX 和 Linux UAPI，不新增生产第三方库；
- ThreadSanitizer 与 ASan/UBSan 互斥，新增独立 `linux-tsan` preset；
- Debug、Release、ASan/UBSan 和 TSan 构建 Runtime 测试；
- aarch64、Rockchip ARMHF、HiSilicon v600 均编译 Runtime/V4L2，但不运行设备或线程测试；
- 如果工具链/sysroot 缺少 Linux UAPI 或编译器 sanitizer 支持，停止执行并请求用户补充环境，不自行安装。

## 11. 测试策略

### 11.1 单元测试

使用 Fake epoll/eventfd API 和 Fake WaitSource 覆盖：

- descriptor 注册、重复 fd 和非法事件；
- timeout、spurious wakeup、EINTR 64 次上界；
- ERR/HUP、WaitSource 解释失败和 Pipeline 失败；
- 同一等待批次对同一 Pipeline 的事件合并；
- 多 Pipeline 的注册顺序、启动回滚和停止逆序；
- start/stop 幂等、启动失败无遗留线程；
- 阻塞在 epoll 时 stop 可唤醒并 join；
- 所有 Pipeline 生命周期和 tick 位于同一 Reactor 线程；
- 同一 Pipeline 无重入或并发 tick；
- last_failure、Metrics 及异常边界。

### 11.2 集成与设备测试

- Fake V4L2 300 帧纵切面必须由 LinuxPlatformRuntime 驱动，不手工忙循环；
- `/dev/video10` 连续 300 帧必须由 Runtime 驱动；
- 既有 ALSA Fake 和 Loopback 验收继续通过，并增加完整 poll descriptor 适配回归；
- stop 在无设备事件时也必须通过 eventfd 于测试时限内完成；
- `linux-tsan` 全量 CTest 不得报告数据竞争、锁顺序问题或线程泄漏。

### 11.3 验收矩阵

0.3a 完成时必须通过 Debug、Release、ASan/UBSan、TSan、安装消费工程、真实 V4L2、既有真实 ALSA，以及 aarch64/Rockchip ARMHF/HiSilicon v600 clean build/readelf。ARM 只验证生成目标架构对象，不宣称目标板运行或厂商 SDK 集成。

## 12. 后续边界

0.3c 在 PlatformRuntime 下增加可配置 ComputeWorkerPool、每 Pipeline serial lane 和跨域 SPSC Queue，隔离 libx264/libx265/AAC/Opus 软件编码。Rockchip MPP、海思 `HI_MPI` 等异步接口优先通过完成 fd/eventfd 回到 Reactor；只有厂商 API 无法非阻塞集成时，Provider 才可使用 Runtime 显式管理的专用 worker lane，不得创建不可见线程。

0.3d 在同一 Reactor 时钟域内同时驱动 V4L2 与 ALSA，测量采集 PTS 偏差和漂移；首期只测量与告警，不自动校正。

## 13. 参考依据

- Linux V4L2 `poll()`：`https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/func-poll.html`
- Linux `epoll`：`https://man7.org/linux/man-pages/man7/epoll.7.html`
- Linux `eventfd`：`https://man7.org/linux/man-pages/man2/eventfd.2.html`
- ALSA PCM poll descriptors：`https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m.html`
