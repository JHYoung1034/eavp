# EAVP Linux Runtime 与 V4L2 Capture 0.3a 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付平台统一管理的 Linux 事件 Reactor、既有 ALSA readiness 适配，以及通过 `/dev/video10` 验收的 V4L2 单平面 MMAP CPU VideoFrame 采集纵切面。

**Architecture:** `LinuxPlatformRuntime` 在平台统一拥有的 Reactor 线程内使用 level-triggered `epoll + eventfd` 串行驱动 Pipeline；Node 保留有界、非阻塞 `tick()`。V4L2 采用 `V4L2Api -> V4L2System -> V4L2SourceNode::Impl` 分层，驱动 MMAP Buffer 被逐行复制到 EAVP 多平面 CPU Buffer 后立即归还；ALSA 通过完整 poll descriptor 集合接入同一 readiness 边界。

**Tech Stack:** C++11、POSIX、Linux UAPI、epoll、eventfd、V4L2、可选 libasound 1.2.11、CMake 3.21、Ninja、GoogleTest 1.12.1、CTest、ASan/UBSan、ThreadSanitizer。

**Spec:** `docs/superpowers/specs/2026-08-25-eavp-linux-platform-runtime-design.md`、`docs/superpowers/specs/2026-08-21-eavp-linux-v4l2-capture-design.md`、`docs/superpowers/specs/2026-08-22-eavp-linux-alsa-capture-design.md`

## Global Constraints

- 所有新增或修改的项目文档、说明性注释和提交摘要优先使用简体中文；代码、标识符、命令、路径和标准名称除外。
- 生产核心保持 C++11 与 POSIX 基线；公共头文件不得使用 C++14 或更高版本特性。
- 公共 API 使用 `Status`/`Result<T>`，异常不得跨模块边界。
- 依赖方向保持 `platform -> control -> media -> base` 和 `management -> base`；`media` 不包含 V4L2、ALSA、epoll 或 eventfd 头文件。
- Node 不创建私有线程；Runtime 统一创建、停止并 join Reactor 线程。同一 Pipeline 的生命周期和 tick 始终在一个 Reactor 线程串行执行。
- Linux Runtime 使用 level-triggered epoll；V4L2/ALSA Node 的每次 tick 最多推进一个有界工作单元。
- Runtime 只借用 Pipeline/WaitSource；调用方必须保证 Runtime 完成 stop/析构后才销毁它们。
- V4L2 仅实现 `VIDEO_CAPTURE`、single-planar、MMAP、非阻塞 I/O，以及 YUV420P、NV12、YUYV422。
- MMAP Buffer 不进入下游；目标 CPU Buffer padding 清零，只复制有效像素，成功 DQBUF 后必须尝试 QBUF。
- live 视频直接下游默认使用 `kDropOldest`；0.3a 单执行域音频使用 `kBlock`，音频感知跨域丢弃与 discontinuity 传播进入 0.3c SPSC Queue。
- 不引入 libv4l2、FFmpeg、libx264、libx265、MPP、RGA 或 `HI_MPI` 生产依赖。
- `EAVP_ENABLE_LINUX_RUNTIME=ON`、`EAVP_ENABLE_V4L2=ON` 在 Linux 默认开启；真实设备测试默认关闭。
- `linux-tsan` 与 ASan/UBSan 互斥；三套 ARM preset 编译 Runtime/V4L2，但不运行测试。
- 不下载或安装生产工具/开发包；若现有环境缺少必要头文件、编译器或设备能力，停止执行并请求用户处理。
- 每项行为先写失败测试、确认红灯原因，再实现最小代码；每个任务形成可构建、可测试、可独立审查的 Conventional Commit。
- 只暂存任务列出的文件；不得修改或提交 `virtrual-v4l2-test.md`、`virtrual-input-test.md` 等个人学习记录。
- 项目版本保持 `0.2.0`，0.3a 不修改稳定包版本。

---

## File Structure

### 新增公共接口

- `include/eavp/platform/linux/wait_source.hpp`：Linux poll descriptor readiness 契约。
- `include/eavp/platform/linux/platform_runtime.hpp`：Runtime 配置、状态、注册、start/stop 和故障快照。
- `include/eavp/platform/linux/v4l2_capture.hpp`：无 Linux UAPI 类型的 V4L2 配置和 Source Node factory。

### 新增 Runtime 实现

- `src/platform/linux/linux_runtime_api.hpp`：私有 epoll/eventfd/read/write/clock 系统调用边界。
- `src/platform/linux/linux_runtime_api.cpp`：Linux 系统调用直接实现。
- `src/platform/linux/linux_event_loop.hpp`、`linux_event_loop.cpp`：descriptor 注册、事件合并和一次 Reactor 轮转。
- `src/platform/linux/platform_runtime_internal.hpp`：测试 peer、Runtime observer 和依赖注入边界。
- `src/platform/linux/platform_runtime.cpp`：线程状态机、Pipeline 生命周期、stop 唤醒、超时排空和 Metrics。

### 新增 V4L2 实现

- `src/platform/linux/v4l2_capture_config.cpp`：公开配置校验。
- `src/platform/linux/v4l2_api.hpp`：私有 open/ioctl/mmap/munmap/close/clock 边界。
- `src/platform/linux/v4l2_linux_api.cpp`：Linux/POSIX 直接实现。
- `src/platform/linux/v4l2_system.hpp`、`v4l2_system.cpp`：设备会话、格式协商、MMAP、streaming 和 dequeue/requeue。
- `src/platform/linux/v4l2_capture_internal.hpp`：测试 peer 和 V4L2 observer。
- `src/platform/linux/v4l2_capture.cpp`：Node、CPU copy、PTS、背压与 Metrics。

### 新增测试

- `tests/support/fake_linux_runtime_api.hpp`、`runtime_test_utils.hpp`。
- `tests/support/fake_v4l2_api.hpp`、`v4l2_test_utils.hpp`。
- `tests/unit/platform_runtime_test.cpp`。
- `tests/integration/alsa_capture_runtime_test.cpp`。
- `tests/unit/v4l2_capture_config_test.cpp`、`v4l2_system_test.cpp`、`v4l2_capture_test.cpp`。
- `tests/integration/v4l2_capture_pipeline_test.cpp`。
- `tests/integration/v4l2_capture_device_test.cpp`。

---

### Task 1：扩展多平面 CPU Buffer 与 YUYV422 Core 契约

**Files:**
- Modify: `include/eavp/media/buffer.hpp`
- Modify: `src/media/buffer.cpp`
- Modify: `include/eavp/media/video_format.hpp`
- Modify: `src/media/video_format.cpp`
- Modify: `src/media/capability.cpp`
- Modify: `include/eavp/media/port.hpp`
- Modify: `tests/unit/media_object_test.cpp`
- Modify: `tests/unit/media_backend_test.cpp`
- Modify: `tests/unit/media_runtime_test.cpp`

**Interfaces:**
- Produces: `Buffer::allocate(std::size_t, const std::vector<PlaneLayout>&)`。
- Produces: `PixelFormat::kYuyv422` 和稳定名称 `"yuyv422"`。
- Produces: `InputPort<T>::dropped_count()` 只读 Queue 丢弃计数。
- Consumes: 既有 `Buffer::create`、`VideoFormat::create`、`PlaneLayout`。

- [ ] **Step 1: 写多平面分配和 YUYV422 失败测试**

在 `media_object_test.cpp` 增加以下核心断言，并补充溢出、重叠 plane、容量不足和奇数 YUYV 宽度 table cases：

```cpp
TEST(BufferTest, AllocatesCpuStorageWithExplicitPlaneLayout) {
    const std::vector<eavp::PlaneLayout> planes{
        eavp::PlaneLayout(0U, 128U, 16U),
        eavp::PlaneLayout(128U, 64U, 16U)};
    eavp::Result<eavp::Buffer> result =
        eavp::Buffer::allocate(192U, planes);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(eavp::MemoryDomain::kCpu, result.value().memory_domain());
    EXPECT_EQ(2U, result.value().plane_count());
    EXPECT_EQ(128U, result.value().plane_layout(1U).value().offset);
}

TEST(VideoFormatTest, DescribesPackedYuyv422Exactly) {
    const std::vector<eavp::PlaneLayout> planes{
        eavp::PlaneLayout(0U, 256U, 32U)};
    const eavp::Result<eavp::VideoFormat> format =
        eavp::VideoFormat::create(eavp::PixelFormat::kYuyv422,
                                  16, 8, eavp::MemoryDomain::kCpu, planes);
    ASSERT_TRUE(format.ok());
    EXPECT_EQ("yuyv422",
              eavp::pixel_format_name(eavp::PixelFormat::kYuyv422).value());
}
```

- [ ] **Step 2: 运行目标测试并确认编译红灯**

Run:

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug --target eavp_media_object_tests
```

Expected: 编译失败，指出 `Buffer::allocate(size, planes)` 或 `kYuyv422` 尚不存在；不得接受无关错误作为红灯。

- [ ] **Step 3: 实现多平面分配与格式校验**

在 `buffer.cpp` 让旧 overload 委托新 overload；新 overload 先验证 size，再构造 `CpuBufferStorage` 并复用 `Buffer::create`：

```cpp
Result<Buffer> Buffer::allocate(
    std::size_t size, const std::vector<PlaneLayout>& planes) {
    if (size == 0U) {
        return Result<Buffer>(Status(StatusCode::kInvalidArgument,
                                     "buffer size must be positive"));
    }
    try {
        std::shared_ptr<BufferStorage> storage(new CpuBufferStorage(size));
        return create(storage, planes);
    } catch (const std::bad_alloc&) {
        return Result<Buffer>(Status(StatusCode::kResourceExhausted));
    } catch (...) {
        return Result<Buffer>(Status(StatusCode::kInternal));
    }
}
```

在 `video_format.cpp` 的完整 switch 中加入 YUYV422：恰有一个 plane、宽度为偶数、`width * 2` 乘法不溢出、stride 至少为 `width * 2`、size 至少覆盖 `stride * height`。同步更新 capability 的稳定枚举 switch，但 Reference Backend 不宣称新增能力。

在 `InputPort` 增加 `std::size_t dropped_count() const`，未连接时返回 0，已连接时委托 `BoundedQueue::dropped_count()`；不得暴露可写 Queue 或改变既有 send/receive 语义。增加容量 1、`kDropOldest` 的回归，确认第二次 push 后值为 1。

- [ ] **Step 4: 运行 Media Core 全量测试**

Run:

```bash
cmake --build --preset linux-debug --target eavp_media_object_tests eavp_media_backend_tests eavp_media_runtime_tests
ctest --test-dir build/linux-debug -R 'BufferTest|VideoFormatTest|Capability|QueueTest|PortTest' --output-on-failure
```

Expected: 新旧相关测试全部通过。

- [ ] **Step 5: 提交 Core 媒体对象扩展**

```bash
git add include/eavp/media/buffer.hpp include/eavp/media/video_format.hpp \
  src/media/buffer.cpp src/media/video_format.cpp src/media/capability.cpp \
  include/eavp/media/port.hpp tests/unit/media_object_test.cpp \
  tests/unit/media_backend_test.cpp tests/unit/media_runtime_test.cpp
git commit -m "feat(media): 支持多平面 CPU Buffer 与 YUYV422"
```

---

### Task 2：建立 Linux readiness 与 Runtime 公共配置

**Files:**
- Create: `include/eavp/platform/linux/wait_source.hpp`
- Create: `include/eavp/platform/linux/platform_runtime.hpp`
- Create: `src/platform/linux/platform_runtime.cpp`
- Create: `tests/unit/platform_runtime_test.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces: `LinuxWaitSource::poll_descriptors()`、`evaluate_poll_events()`。
- Produces: `LinuxPlatformRuntimeConfig::create(int,int)`、`reactor_count()`、`stop_timeout_ms()`。
- Produces: `PlatformRuntimeState` 和 `LinuxPlatformRuntime` 声明。
- Consumes: `MediaPipeline`、`MetricRegistry`、POSIX `pollfd`。

- [ ] **Step 1: 写配置和公共边界失败测试**

```cpp
TEST(LinuxPlatformRuntimeConfigTest, AcceptsSingleReactorAndPositiveStopTimeout) {
    eavp::Result<eavp::LinuxPlatformRuntimeConfig> result =
        eavp::LinuxPlatformRuntimeConfig::create(1, 2000);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(1, result.value().reactor_count());
    EXPECT_EQ(2000, result.value().stop_timeout_ms());
}

TEST(LinuxPlatformRuntimeConfigTest, RejectsInvalidOrUnsupportedValues) {
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::LinuxPlatformRuntimeConfig::create(0, 2000).status().code());
    EXPECT_EQ(eavp::StatusCode::kUnsupported,
              eavp::LinuxPlatformRuntimeConfig::create(2, 2000).status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::LinuxPlatformRuntimeConfig::create(1, 0).status().code());
}
```

增加编译期 Fake WaitSource，确认完整 descriptor 数组可往返且公共头不包含 ALSA/V4L2 类型。

- [ ] **Step 2: 运行测试确认缺少公共接口**

```bash
cmake --build --preset linux-debug --target eavp_platform_runtime_tests
```

Expected: 编译失败，指出 Runtime 类型和头文件不存在。

- [ ] **Step 3: 实现最小公共类型和配置 factory**

`wait_source.hpp` 只包含 `<poll.h>`、`<vector>` 和 Result；`platform_runtime.hpp` 使用 PImpl，不暴露 mutex、thread、epoll 或 eventfd。配置 factory 捕获 `std::bad_alloc`/未知异常并分别返回 `kResourceExhausted`/`kInternal`。

Runtime 方法在本任务先返回明确的 `kInvalidState`，但 `create()` 必须能构造 Created 状态对象；不得创建线程或系统 fd。

`MetricRegistry*` 允许为空；为空时关闭 Runtime Metrics，但不影响调度和错误传播。

- [ ] **Step 4: 运行配置测试与公共头扫描**

```bash
cmake --build --preset linux-debug --target eavp_platform_runtime_tests
ctest --test-dir build/linux-debug -R 'LinuxPlatformRuntimeConfigTest' --output-on-failure
rg -n 'alsa/|videodev2|sys/epoll|sys/eventfd' include/eavp/platform/linux/platform_runtime.hpp
```

Expected: 测试通过，扫描无输出。

- [ ] **Step 5: 提交 readiness 公共契约**

```bash
git add include/eavp/platform/linux/wait_source.hpp \
  include/eavp/platform/linux/platform_runtime.hpp \
  src/platform/linux/platform_runtime.cpp src/CMakeLists.txt \
  tests/unit/platform_runtime_test.cpp tests/unit/CMakeLists.txt
git commit -m "feat(platform): 定义 Linux readiness 与 Runtime 契约"
```

---

### Task 3：实现可测试的 epoll/eventfd 事件循环

**Files:**
- Create: `src/platform/linux/linux_runtime_api.hpp`
- Create: `src/platform/linux/linux_runtime_api.cpp`
- Create: `src/platform/linux/linux_event_loop.hpp`
- Create: `src/platform/linux/linux_event_loop.cpp`
- Create: `tests/support/fake_linux_runtime_api.hpp`
- Create: `tests/support/runtime_test_utils.hpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/platform_runtime_test.cpp`

**Interfaces:**
- Produces: 私有 `LinuxRuntimeApi` 系统调用边界和 `create_linux_runtime_api()`。
- Produces: 私有 `LinuxEventLoop::initialize/register_source/wait_once/wake/close` 和 `LinuxEventLoopTurn`。
- Consumes: `LinuxWaitSource`、Pipeline 注册记录、`struct epoll_event`。

- [ ] **Step 1: 写 descriptor 注册和事件合并失败测试**

Fake API 记录 `epoll_ctl`，并脚本化一次等待中同一 Pipeline 的两个 fd 同时 ready：

```cpp
TEST(LinuxEventLoopTest, CoalescesReadySourcesForTheSamePipeline) {
    RuntimeFixture fixture;
    fixture.source_a.set_descriptors(std::vector<pollfd>(1, readable_fd(10)));
    fixture.source_b.set_descriptors(std::vector<pollfd>(1, readable_fd(11)));
    fixture.api->queue_ready_fds(std::vector<int>{10, 11});

    ASSERT_TRUE(fixture.loop->initialize().ok());
    ASSERT_TRUE(fixture.loop->register_source(
        &fixture.pipeline, &fixture.source_a).ok());
    ASSERT_TRUE(fixture.loop->register_source(
        &fixture.pipeline, &fixture.source_b).ok());
    const eavp::Result<eavp::detail::LinuxEventLoopTurn> ready =
        fixture.loop->wait_once();
    ASSERT_TRUE(ready.ok());
    ASSERT_EQ(1U, ready.value().ready_pipelines.size());
    EXPECT_EQ(&fixture.pipeline, ready.value().ready_pipelines[0]);
    EXPECT_EQ(1U, ready.value().wakeup_count);
}
```

同时覆盖重复 fd、空 descriptors、source 返回 false、`EPOLLERR/HUP`、spurious event 和 eventfd wake token。

- [ ] **Step 2: 运行 Runtime 测试确认私有事件循环缺失**

```bash
cmake --build --preset linux-debug --target eavp_platform_runtime_tests
```

Expected: 编译失败，指出 `LinuxEventLoop`/Fake API 尚不存在。

- [ ] **Step 3: 实现系统调用边界与 level-triggered 注册**

`LinuxRuntimeApi` 的失败统一返回 `-1` 并由 `last_error()` 提供保存的 errno；生产实现直接委托：

```cpp
virtual int epoll_create() = 0;
virtual int epoll_add(int epoll_fd, int fd, std::uint32_t events,
                      std::uint64_t token) = 0;
virtual int epoll_wait_events(int epoll_fd, struct epoll_event* events,
                              int capacity, int timeout_ms) = 0;
virtual int create_event_fd() = 0;
virtual int read_event_fd(int fd, std::uint64_t* value) = 0;
virtual int write_event_fd(int fd, std::uint64_t value) = 0;
virtual int close_fd(int fd) = 0;
virtual int monotonic_now(struct timespec* value) = 0;
virtual int last_error() const = 0;
```

`LinuxEventLoopTurn` 固定包含 `ready_pipelines`、本轮 `wakeup_count`、`interrupted_count` 和 `control_wakeup`；Runtime 只能从该结果发布 poll Metrics，不读取 EventLoop 私有计数。

生产 flags 固定为 `EPOLL_CLOEXEC` 和 `EFD_NONBLOCK | EFD_CLOEXEC`。poll interests 映射为 EPOLLIN/OUT/PRI，不设置 `EPOLLET` 或 `EPOLLONESHOT`。

- [ ] **Step 4: 实现事件解释、EINTR 上界和错误上下文**

`wait_once()` 清零每个 source 的原始 pollfd `revents`，按 token 回填事件；同一 source 只调用一次 `evaluate_poll_events`，同一 Pipeline 只输出一次。连续 64 次 EINTR 后返回：

```cpp
Status(StatusCode::kIoError,
       "Linux Reactor 等待被连续信号中断",
       "linux_runtime", "epoll_wait", EINTR)
```

eventfd token 只消费唤醒计数，不把 Pipeline 标记 ready。所有 fd 在 close/destructor 中恰好关闭一次。

- [ ] **Step 5: 运行事件循环测试**

```bash
cmake --build --preset linux-debug --target eavp_platform_runtime_tests
ctest --test-dir build/linux-debug -R 'LinuxEventLoopTest' --output-on-failure
```

Expected: descriptor、事件合并、EINTR、wake 和资源释放测试全部通过。

- [ ] **Step 6: 提交 Linux 事件循环**

```bash
git add src/platform/linux/linux_runtime_api.hpp \
  src/platform/linux/linux_runtime_api.cpp \
  src/platform/linux/linux_event_loop.hpp \
  src/platform/linux/linux_event_loop.cpp src/CMakeLists.txt \
  tests/support/fake_linux_runtime_api.hpp \
  tests/support/runtime_test_utils.hpp tests/unit/platform_runtime_test.cpp
git commit -m "feat(platform): 实现 Linux epoll 事件循环"
```

---

### Task 4：实现平台统一 Runtime 线程与 Pipeline 生命周期

**Files:**
- Create: `src/platform/linux/platform_runtime_internal.hpp`
- Modify: `src/platform/linux/platform_runtime.cpp`
- Modify: `tests/unit/platform_runtime_test.cpp`

**Interfaces:**
- Produces: 可用的 `LinuxPlatformRuntime::register_pipeline/start/stop/state/last_failure`。
- Produces: 私有 `LinuxPlatformRuntimeTestPeer::create(...)` 和 `RuntimeObserver`。
- Consumes: Task 3 `LinuxEventLoop` 与 Runtime API。

- [ ] **Step 1: 写线程亲和、启动回滚和 stop 唤醒失败测试**

测试 Node 记录 prepare/start/tick/stop/reset 的 `std::thread::id`：

```cpp
TEST(LinuxPlatformRuntimeTest, RunsAllPipelineOperationsOnOneReactorThread) {
    RuntimeThreadFixture fixture;
    ASSERT_TRUE(fixture.runtime->register_pipeline(
        fixture.pipeline.get(), fixture.wait_sources()).ok());
    ASSERT_TRUE(fixture.runtime->start().ok());
    fixture.signal_ready();
    ASSERT_TRUE(fixture.wait_for_ticks(1));
    ASSERT_TRUE(fixture.runtime->stop().ok());

    EXPECT_TRUE(fixture.node->all_calls_share_one_thread());
    EXPECT_NE(std::this_thread::get_id(), fixture.node->execution_thread());
    EXPECT_FALSE(fixture.node->concurrent_tick_observed());
}
```

增加：第二个 Pipeline 启动失败时逆序清理第一个、start 失败 join、阻塞 epoll 时 stop 唤醒、重复 stop、Pipeline fatal failure、drain 超时后 cancel、Runtime 析构无遗留线程。

- [ ] **Step 2: 运行测试确认 Runtime 仍为 stub**

```bash
cmake --build --preset linux-debug --target eavp_platform_runtime_tests
ctest --test-dir build/linux-debug -R 'LinuxPlatformRuntimeTest' --output-on-failure
```

Expected: 新测试因 `start()` 返回 `kInvalidState` 或无 Reactor 线程而失败。

- [ ] **Step 3: 实现线程状态机和同步启动**

`Impl` 使用 mutex/condition_variable 保护 state、last_failure、启动完成和退出标志。`start()` 创建 `std::thread` 后等待 `kRunning`、`kError` 或线程退出；线程内按注册顺序执行 `pipeline->start()`，获取 descriptors 并初始化 EventLoop。

禁止控制线程直接调用 Pipeline 生命周期。部分失败时在线程内逆注册顺序调用 `cancel()`，保存第一媒体根因，关闭 loop 后通知等待方。

- [ ] **Step 4: 实现事件驱动 tick、同步 stop 和超时 drain**

Reactor 主循环只对 `wait_once()` 返回的 ready pipelines 各调用一次 `tick()`。Node 的 `kWouldBlock/kNotFound/kEndOfStream` 已由 Pipeline 处理；其他失败记录为 Runtime fatal。

`stop()` 设置 stopping 标志并写 eventfd；线程使用 `CLOCK_MONOTONIC` 计算 `stop_timeout_ms` deadline，重复调用 Pipeline `stop()`。若超时仍为 `kWouldBlock`，调用 `cancel()` 并把 `kTimeout` 作为 stop 结果；控制线程始终 join 后返回。

- [ ] **Step 5: 实现 Runtime observer 与 Metrics 优先级**

生产 observer 写入：

```text
runtime.poll.wakeups
runtime.poll.interrupted
runtime.pipeline.turns
runtime.pipeline.failures
runtime.reactor.running
```

TestPeer 可注入 observer failure。媒体错误优先于 Metrics；单独指标错误使 Runtime Error，但不得以线程异常退出。

- [ ] **Step 6: 运行 Runtime 全量与并发回归**

```bash
cmake --build --preset linux-debug --target eavp_platform_runtime_tests
ctest --test-dir build/linux-debug -R 'LinuxPlatformRuntime|LinuxEventLoop' --output-on-failure
```

Expected: 全部通过，无测试超时或残留线程。

- [ ] **Step 7: 提交平台 Runtime**

```bash
git add src/platform/linux/platform_runtime.cpp \
  src/platform/linux/platform_runtime_internal.hpp \
  tests/unit/platform_runtime_test.cpp
git commit -m "feat(platform): 实现统一 Linux Reactor Runtime"
```

---

### Task 5：把既有 ALSA Source 接入 readiness

**Files:**
- Modify: `include/eavp/platform/linux/alsa_capture.hpp`
- Modify: `src/platform/linux/alsa_api.hpp`
- Modify: `src/platform/linux/alsa_lib_api.cpp`
- Modify: `src/platform/linux/alsa_system.hpp`
- Modify: `src/platform/linux/alsa_system.cpp`
- Modify: `src/platform/linux/alsa_capture.cpp`
- Modify: `tests/support/fake_alsa_api.hpp`
- Modify: `tests/unit/alsa_system_test.cpp`
- Modify: `tests/unit/alsa_capture_test.cpp`
- Create: `tests/integration/alsa_capture_runtime_test.cpp`
- Modify: `tests/integration/CMakeLists.txt`

**Interfaces:**
- Produces: `AlsaSourceNode : public MediaNode, public LinuxWaitSource`。
- Produces: AlsaApi/System 的 `poll_descriptors_count/poll_descriptors/poll_descriptors_revents`。
- Consumes: Task 2 `LinuxWaitSource`。

- [ ] **Step 1: 写 ALSA 多 descriptor 与 revents 解码失败测试**

```cpp
TEST(AlsaSystemTest, PreservesCompletePollDescriptorArrayAndDemanglesEvents) {
    ScriptedAlsa fixture;
    fixture.set_poll_descriptors(std::vector<pollfd>{
        readable_fd(20), writable_fd(21)});
    fixture.set_poll_revents(POLLIN);
    ASSERT_TRUE(fixture.system.prepare(fixture.config).ok());
    ASSERT_TRUE(fixture.system.start().ok());

    eavp::Result<std::vector<pollfd> > descriptors =
        fixture.system.poll_descriptors();
    ASSERT_TRUE(descriptors.ok());
    ASSERT_EQ(2U, descriptors.value().size());
    descriptors.value()[0].revents = POLLIN;
    EXPECT_TRUE(fixture.system.evaluate_poll_events(
        descriptors.value()).value());
    EXPECT_EQ(1, fixture.api.poll_revents_calls());
}
```

覆盖 descriptor count 0/负值、数组长度变化、libasound 错误、POLLERR、HUP/NVAL、未 prepare/running 调用和公共异常边界。

- [ ] **Step 2: 运行 ALSA 目标确认接口缺失**

```bash
cmake --build --preset linux-debug --target eavp_alsa_system_tests eavp_alsa_capture_tests
```

Expected: 编译失败，指出 poll API/WaitSource override 不存在。

- [ ] **Step 3: 扩展 AlsaApi、生产委托与 Fake**

加入三个稳定 libasound 调用：

```cpp
virtual int pcm_poll_descriptors_count(snd_pcm_t* pcm) = 0;
virtual int pcm_poll_descriptors(snd_pcm_t* pcm, struct pollfd* descriptors,
                                 unsigned int count) = 0;
virtual int pcm_poll_descriptors_revents(
    snd_pcm_t* pcm, struct pollfd* descriptors, unsigned int count,
    unsigned short* revents) = 0;
```

生产实现直接委托对应 `snd_pcm_*`；Fake 返回完整原始顺序并记录调用。

- [ ] **Step 4: 实现 System 和 Node readiness**

System 只在 Running 且 handle 有效时返回 descriptors；evaluate 要求长度与首次取得的数量一致，并使用 `snd_pcm_poll_descriptors_revents()`。`POLLNVAL/HUP` 返回 enriched `kDeviceLost`；POLLERR 或可读/可写 readiness 返回 true，由下一次非阻塞 tick 完成 XRUN/设备错误判定。

AlsaSourceNode 公共头只继承 LinuxWaitSource，不暴露 ALSA 类型；override 捕获异常并委托 System。

- [ ] **Step 5: 运行所有 ALSA 回归**

增加 `AlsaCaptureRuntimeTest`：Fake ALSA 使用真实 pipe/eventfd descriptor 产生 readiness，由 LinuxPlatformRuntime 驱动 `AlsaSourceNode -> AudioChecksumSink` 输出 300 帧；直接下游使用容量 4、`kBlock`，测试代码不得直接调用 `pipeline.tick()`。

```bash
cmake --build --preset linux-debug --target \
  eavp_alsa_system_tests eavp_alsa_capture_tests \
  eavp_alsa_capture_pipeline_tests eavp_alsa_capture_runtime_tests
ctest --test-dir build/linux-debug -R 'Alsa' --output-on-failure
```

Expected: readiness 新测试与既有 PTS/XRUN/suspend/Health 测试全部通过。

- [ ] **Step 6: 提交 ALSA readiness**

```bash
git add include/eavp/platform/linux/alsa_capture.hpp \
  src/platform/linux/alsa_api.hpp src/platform/linux/alsa_lib_api.cpp \
  src/platform/linux/alsa_system.hpp src/platform/linux/alsa_system.cpp \
  src/platform/linux/alsa_capture.cpp tests/support/fake_alsa_api.hpp \
  tests/unit/alsa_system_test.cpp tests/unit/alsa_capture_test.cpp \
  tests/integration/alsa_capture_runtime_test.cpp \
  tests/integration/CMakeLists.txt
git commit -m "feat(platform): 接入 ALSA 事件 readiness"
```

---

### Task 6：实现 V4L2 配置与精确格式布局

**Files:**
- Create: `include/eavp/platform/linux/v4l2_capture.hpp`
- Create: `src/platform/linux/v4l2_capture_config.cpp`
- Create: `tests/unit/v4l2_capture_config_test.cpp`
- Create: `tests/support/v4l2_test_utils.hpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces: `V4L2CaptureConfig::create(...)` 及全部 getter。
- Produces: `V4L2SourceNode` factory/output/actual_format/WaitSource 声明。
- Consumes: Task 1 PixelFormat、多平面 Buffer；Task 2 LinuxWaitSource。

- [ ] **Step 1: 写配置 table-driven 失败测试**

```cpp
TEST(V4L2CaptureConfigTest, AcceptsApprovedVideo10Shape) {
    eavp::Result<eavp::V4L2CaptureConfig> result =
        eavp::V4L2CaptureConfig::create(
            "/dev/video10", eavp::PixelFormat::kYuv420p,
            1920, 1080, 30, 1, 4U);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(30, result.value().frame_rate_numerator());
    EXPECT_EQ(1, result.value().frame_rate_denominator());
    EXPECT_EQ(4U, result.value().buffer_count());
}
```

拒绝：空路径、unknown/RGB24/非法 enum、非正宽高/fps、YUV420/NV12 奇数尺寸、YUYV 奇数宽度、`buffer_count < 2`、`buffer_count > UINT32_MAX` 以及所有可达乘加溢出。`buffer_count > UINT32_MAX` 用例只在 `size_t` 更宽的平台执行；32-bit 构建断言该值域不可构造而不伪造 skip 通过。

- [ ] **Step 2: 运行测试确认 V4L2 公共类型缺失**

```bash
cmake --build --preset linux-debug --target eavp_v4l2_capture_config_tests
```

Expected: 编译失败，指出 `v4l2_capture.hpp` 或类型不存在。

- [ ] **Step 3: 实现无 Linux UAPI 的配置 factory 和 Node 声明**

Config 保存 frame rate 为 fps `numerator/denominator`；30/1 在 V4L2 `timeperframe` 中转换为 1/30。factory 捕获异常。Node 使用 PImpl factory：

```cpp
static Result<std::unique_ptr<V4L2SourceNode> > create(
    const std::string& id,
    const V4L2CaptureConfig& config,
    MetricRegistry* metrics);
```

公共头不得包含 `<linux/videodev2.h>`。

- [ ] **Step 4: 运行配置测试和头文件扫描**

```bash
cmake --build --preset linux-debug --target eavp_v4l2_capture_config_tests
ctest --test-dir build/linux-debug -R 'V4L2CaptureConfigTest' --output-on-failure
rg -n 'videodev2|v4l2_' include/eavp/platform/linux/v4l2_capture.hpp
```

Expected: 测试通过；扫描只允许 EAVP 自身类型名，不允许 Linux UAPI include/type。

- [ ] **Step 5: 提交 V4L2 公共配置**

```bash
git add include/eavp/platform/linux/v4l2_capture.hpp \
  src/platform/linux/v4l2_capture_config.cpp src/CMakeLists.txt \
  tests/support/v4l2_test_utils.hpp tests/unit/v4l2_capture_config_test.cpp \
  tests/unit/CMakeLists.txt
git commit -m "feat(platform): 定义 V4L2 采集配置"
```

---

### Task 7：实现 V4L2 prepare、精确协商与资源回滚

**Files:**
- Create: `src/platform/linux/v4l2_api.hpp`
- Create: `src/platform/linux/v4l2_linux_api.cpp`
- Create: `src/platform/linux/v4l2_system.hpp`
- Create: `src/platform/linux/v4l2_system.cpp`
- Create: `tests/support/fake_v4l2_api.hpp`
- Create: `tests/unit/v4l2_system_test.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces: 私有 `V4L2Api`、`create_linux_v4l2_api()`。
- Produces: `V4L2System::prepare/start/stop/reset`、`negotiated()`、`poll_descriptors()`。
- Produces: `V4L2NegotiatedFormat`，包含 VideoFormat、总容量、visible row bytes 和 source offsets。
- Consumes: V4L2CaptureConfig、Linux UAPI、Task 1 VideoFormat。

- [ ] **Step 1: 写三种格式协商和逐步骤回滚失败测试**

Fake API 脚本化 QUERYCAP、S/G_FMT、S/G_PARM、REQBUFS、QUERYBUF、mmap。至少加入：

```cpp
TEST(V4L2SystemTest, NegotiatesPaddedYuv420pLayoutExactly) {
    ScriptedV4L2 fixture;
    fixture.set_format(V4L2_PIX_FMT_YUV420, 16, 8, 32, 384);
    ASSERT_TRUE(fixture.system.prepare(fixture.config()).ok());
    const eavp::VideoFormat& format = fixture.system.negotiated().format;
    ASSERT_EQ(3U, format.planes().size());
    EXPECT_EQ(32U, format.planes()[0].stride);
    EXPECT_EQ(16U, format.planes()[1].stride);
    EXPECT_EQ(16U, format.planes()[2].stride);
}
```

对每个 prepare 操作注入失败，断言已 mmap region 逆序 munmap、REQBUFS(0) 一次、close 一次。覆盖 capability bits、driver 改格式/尺寸/fps、sizeimage 太小、buffer 少于 2、index/offset/length 溢出和 mmap failure。

- [ ] **Step 2: 运行测试确认 System 缺失**

```bash
cmake --build --preset linux-debug --target eavp_v4l2_system_tests
```

Expected: 编译失败，指出 V4L2 私有 System/API 不存在。

- [ ] **Step 3: 实现原始 API 和 bounded EINTR helper**

V4L2Api 封装 open/ioctl/mmap/munmap/close/monotonic_now/last_error。open/ioctl 在 System 中最多重试 64 次 EINTR；close 不重试。所有失败转换为 `provider_id="v4l2"`、精确 operation 和 errno native code。

- [ ] **Step 4: 实现 prepare 精确协商与 layout 构造**

固定顺序：

```text
open(O_RDWR|O_NONBLOCK|O_CLOEXEC)
VIDIOC_QUERYCAP
VIDIOC_S_FMT / VIDIOC_G_FMT
VIDIOC_S_PARM / VIDIOC_G_PARM
VIDIOC_REQBUFS
VIDIOC_QUERYBUF + mmap（逐 buffer）
```

YUV420P 要求偶数 bytesperline，planes 为 `Y stride*h`、`U/V (stride/2)*(h/2)`；NV12 为 `Y stride*h`、`UV stride*(h/2)`；YUYV 为一个 `stride*h` plane。所有 offset/size 先检查溢出，且 `sizeimage` 覆盖最后 plane 末端。

- [ ] **Step 5: 实现 move-only 资源持有和幂等 reset**

System 独占 fd、regions 和内核 buffer allocation。prepare 失败在返回前清空全部资源；reset 顺序为 STREAMOFF（若 running）→ munmap reverse → REQBUFS(0) → close。析构 noexcept 调用同等兜底。

- [ ] **Step 6: 运行 V4L2 System prepare 测试**

```bash
cmake --build --preset linux-debug --target eavp_v4l2_system_tests
ctest --test-dir build/linux-debug -R 'V4L2SystemTest.*(Prepare|Negotiate|Rollback|Format)' --output-on-failure
```

Expected: 三格式协商及全部失败回滚测试通过。

- [ ] **Step 7: 提交 V4L2 会话协商**

```bash
git add src/platform/linux/v4l2_api.hpp \
  src/platform/linux/v4l2_linux_api.cpp \
  src/platform/linux/v4l2_system.hpp src/platform/linux/v4l2_system.cpp \
  src/CMakeLists.txt tests/support/fake_v4l2_api.hpp \
  tests/unit/v4l2_system_test.cpp tests/unit/CMakeLists.txt
git commit -m "feat(platform): 实现 V4L2 MMAP 会话协商"
```

---

### Task 8：实现 V4L2 streaming、dequeue/requeue 与 readiness

**Files:**
- Modify: `src/platform/linux/v4l2_system.hpp`
- Modify: `src/platform/linux/v4l2_system.cpp`
- Modify: `tests/support/fake_v4l2_api.hpp`
- Modify: `tests/unit/v4l2_system_test.cpp`

**Interfaces:**
- Produces: `V4L2DequeuedBuffer`。
- Produces: `V4L2System::dequeue()`、`requeue(std::uint32_t)`。
- Produces: `V4L2System::evaluate_poll_events(...)`。
- Consumes: Task 7 MMAP regions 和 session state。

- [ ] **Step 1: 写 start、DQ/Q 和错误映射失败测试**

```cpp
TEST(V4L2SystemTest, QueuesEveryBufferBeforeStreamOn) {
    ScriptedV4L2 fixture;
    ASSERT_TRUE(fixture.system.prepare(fixture.config()).ok());
    ASSERT_TRUE(fixture.system.start().ok());
    EXPECT_EQ(std::vector<std::string>({
        "VIDIOC_QBUF:0", "VIDIOC_QBUF:1", "VIDIOC_QBUF:2",
        "VIDIOC_QBUF:3", "VIDIOC_STREAMON"}),
        fixture.api.streaming_calls());
}
```

覆盖 DQBUF EAGAIN、EINTR 64 次、ENODEV/ENXIO/EIO、非法 index、bytesused 超 mmap length、`V4L2_BUF_FLAG_ERROR`、QBUF failure priority、STREAMOFF 一次和 readiness ERR/HUP。

- [ ] **Step 2: 运行测试确认 streaming 接口缺失**

```bash
cmake --build --preset linux-debug --target eavp_v4l2_system_tests
ctest --test-dir build/linux-debug -R 'V4L2SystemTest.*(Stream|Dequeue|Requeue|Poll)' --output-on-failure
```

Expected: 新测试编译或行为失败。

- [ ] **Step 3: 实现 start/dequeue/requeue/stop**

`start()` QBUF 全部 buffers 后 STREAMON；`dequeue()` 每次只调用一次成功 DQBUF，EAGAIN 返回 `kWouldBlock`。返回对象包含 index、只读 data、mapped length、bytesused、flags、sequence 和 timeval。`requeue()` 验证 index 后执行 QBUF。

DQBUF 后的调用者无论后续结果如何都必须显式 requeue；Task 9 Node 测试同时验证媒体失败与 QBUF 失败时返回 QBUF 根因。

- [ ] **Step 4: 实现 WaitSource 事件解释**

V4L2 descriptor events 固定为 `POLLIN | POLLRDNORM | POLLPRI`。HUP/NVAL 返回 enriched `kDeviceLost`，POLLERR 返回 enriched `kIoError`；readable/priority 返回 true，无相关事件返回 false。

- [ ] **Step 5: 运行全部 V4L2 System 测试**

```bash
cmake --build --preset linux-debug --target eavp_v4l2_system_tests
ctest --test-dir build/linux-debug -R 'V4L2SystemTest' --output-on-failure
```

Expected: prepare、streaming、readiness、错误映射及资源释放全部通过。

- [ ] **Step 6: 提交 V4L2 streaming**

```bash
git add src/platform/linux/v4l2_system.hpp \
  src/platform/linux/v4l2_system.cpp tests/support/fake_v4l2_api.hpp \
  tests/unit/v4l2_system_test.cpp
git commit -m "feat(platform): 实现 V4L2 非阻塞 streaming"
```

---

### Task 9：实现 V4L2 Source Node、CPU copy、PTS 与 Metrics

**Files:**
- Create: `src/platform/linux/v4l2_capture_internal.hpp`
- Create: `src/platform/linux/v4l2_capture.cpp`
- Create: `tests/unit/v4l2_capture_test.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces: 完整 `V4L2SourceNode` factory、lifecycle、output、actual_format 和 LinuxWaitSource overrides。
- Produces: 私有 `V4L2Observer` 与 `V4L2SourceNodeTestPeer`。
- Produces: 私有 `V4L2FrameAllocator`，生产实现委托 `Buffer::allocate(size, planes)`，测试可注入 allocation/map failure。
- Consumes: Task 1 多平面 Buffer、Task 8 V4L2System。

- [ ] **Step 1: 写 copy/padding/Frame 生命周期失败测试**

构造 padded YUV420P source，padding 填 0xEE、有效像素填确定值：

```cpp
TEST(V4L2SourceNodeTest, CopiesVisibleRowsAndZeroesDriverPadding) {
    V4L2NodeFixture fixture = make_padded_yuv420p_fixture();
    ASSERT_TRUE(fixture.start().ok());
    ASSERT_TRUE(fixture.node->tick().ok());
    const eavp::VideoFrame frame = fixture.sink.take_frame();
    expect_visible_pixels_equal_script(frame);
    expect_all_padding_bytes(frame, 0U);
    EXPECT_EQ(1, fixture.api.qbuf_after_dequeue_calls());
}
```

覆盖 NV12/YUYV、bytesused 最后有效字节不足、Buffer 分配失败、map/frame 构造失败、copy 后 source MMAP 改写但 Frame 不变，以及任何失败仍尝试 QBUF。

- [ ] **Step 2: 写 pending、timestamp、sequence 和 observer 失败测试**

覆盖：pending 时不 DQ、stop 丢 pending、驱动 monotonic timestamp、零/非法 timestamp fallback、int64 溢出、PTS 回退钳制、uint32 sequence wrap、missing frame count、captured/bytes/would_block/drop/gauge 指标，以及媒体错误优先 observer 错误。

同时覆盖 `actual_format()` 在 prepare 前/ reset 后返回 `kInvalidState`，prepare 成功后精确返回驱动协商的 VideoFormat。

- [ ] **Step 3: 运行测试确认 Node 实现缺失**

```bash
cmake --build --preset linux-debug --target eavp_v4l2_capture_tests
```

Expected: 链接或编译失败，指出 V4L2SourceNode 方法不存在。

- [ ] **Step 4: 实现 factory、lifecycle 和 WaitSource 委托**

factory 拒绝空 id，允许 `metrics == NULL`，捕获全部异常。prepare 委托 System 并缓存 actual format；start 清空 PTS/sequence/pending；stop 丢弃 pending 后 STREAMOFF；reset 释放 System 并清空实际格式。poll 方法只在 Running 委托 System。

TestPeer 接受 `std::unique_ptr<V4L2System>`、`std::unique_ptr<V4L2FrameAllocator>` 和可选 observer。生产 allocator 只负责创建多平面 CPU Buffer；Fake allocator 可以返回 `kResourceExhausted` 或带失败 map storage 的 Buffer，不使用全局 new hook 或真实超大分配制造红灯。

- [ ] **Step 5: 实现逐 plane、逐行有效像素 copy**

分配 `Buffer::allocate(total_size, planes)` 后，对每个目标 plane `memset(0)`，再按 source offset/stride 和目标 stride 逐行 `memcpy(visible_row_bytes)`。在任何 pointer 计算前验证 `offset + stride*(rows-1) + visible_bytes <= bytesused` 和 mmap length。

DQBUF 成功后始终调用 requeue；若 copy/frame 错误和 requeue 同时失败，返回 requeue Status。

- [ ] **Step 6: 实现 PTS、sequence 和 observer**

驱动 timestamp 仅在 `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC`、非零、`0 <= tv_usec < 1000000` 且换算不溢出时使用；否则取 DQ 后 `CLOCK_MONOTONIC`。PTS 使用 `TimeBase(1,1000000)` 并钳制到上一帧。

sequence 期望值使用 uint32 wrap；向前 delta 小于半区间时把缺失数量加入 `v4l2.sequence.gaps`，明显反向序列返回 `kCorruptData`。

- [ ] **Step 7: 运行 V4L2 Node 全量测试**

```bash
cmake --build --preset linux-debug --target eavp_v4l2_capture_tests
ctest --test-dir build/linux-debug -R 'V4L2SourceNodeTest' --output-on-failure
```

Expected: copy、padding、PTS、sequence、背压、Metrics、异常边界全部通过。

- [ ] **Step 8: 提交 V4L2 Source Node**

```bash
git add src/platform/linux/v4l2_capture_internal.hpp \
  src/platform/linux/v4l2_capture.cpp src/CMakeLists.txt \
  tests/unit/v4l2_capture_test.cpp tests/unit/CMakeLists.txt
git commit -m "feat(platform): 实现 V4L2 CPU Frame Source"
```

---

### Task 10：实现 Runtime 驱动的 300 帧 Fake 纵切面

**Files:**
- Create: `tests/integration/v4l2_capture_pipeline_test.cpp`
- Modify: `tests/integration/CMakeLists.txt`
- Modify: `tests/support/fake_linux_runtime_api.hpp`
- Modify: `tests/support/fake_v4l2_api.hpp`
- Modify: `tests/support/v4l2_test_utils.hpp`

**Interfaces:**
- Consumes: LinuxPlatformRuntime、V4L2SourceNode、OutputPort/BoundedQueue、MetricRegistry。
- Produces: 300 帧 Runtime 集成验收和 FrameChecksumSink。

- [ ] **Step 1: 写 Runtime 纵切面失败测试**

```cpp
TEST(V4L2CapturePipelineTest, CapturesExactlyThreeHundredFramesViaRuntime) {
    V4L2RuntimePipeline fixture(300U);
    ASSERT_TRUE(fixture.runtime.start().ok());
    ASSERT_TRUE(fixture.wait_until_sink_frames(300U, 5000));
    ASSERT_TRUE(fixture.runtime.stop().ok());

    EXPECT_EQ(300U, fixture.sink.frame_count());
    EXPECT_TRUE(fixture.sink.pts_are_monotonic());
    EXPECT_EQ(fixture.expected_checksum(), fixture.sink.checksum());
    EXPECT_EQ(300U,
              fixture.metrics.counter("v4l2.frames.captured").value());
    EXPECT_EQ(eavp::HealthStatus::kOk,
              fixture.health.component("v4l2.capture").value().status);
    EXPECT_EQ(0, fixture.open_handles());
}
```

Fake readiness 必须通过 eventfd/脚本 epoll 唤醒 Runtime，不允许测试线程直接循环 `pipeline.tick()`。

- [ ] **Step 2: 运行集成目标确认端到端红灯**

```bash
cmake --build --preset linux-debug --target eavp_v4l2_capture_pipeline_tests
ctest --test-dir build/linux-debug -R 'V4L2CapturePipelineTest' --output-on-failure
```

Expected: 首次因 Runtime/Fake readiness 组合缺陷或计数不符失败。

- [ ] **Step 3: 完善 Fake 脚本和 ChecksumSink 最小行为**

每次 ready 只提供一个 DQBUF；Frame payload 按 sequence 生成确定向量。Sink 每次 tick 最多消费一个 Frame，记录 plane 布局、PTS、checksum 和重复 sequence。连接使用容量 4、`kDropOldest`。

- [ ] **Step 4: 运行集成测试和全部 host Debug**

```bash
cmake --build --preset linux-debug
ctest --test-dir build/linux-debug -R 'V4L2CapturePipelineTest' --output-on-failure
ctest --preset linux-debug --output-on-failure
```

Expected: 300 帧集成测试与既有全量测试全部通过。

- [ ] **Step 5: 提交 Fake Runtime 纵切面**

```bash
git add tests/integration/v4l2_capture_pipeline_test.cpp \
  tests/integration/CMakeLists.txt tests/support/fake_linux_runtime_api.hpp \
  tests/support/fake_v4l2_api.hpp tests/support/v4l2_test_utils.hpp
git commit -m "test(platform): 验证 Runtime 驱动 V4L2 纵切面"
```

---

### Task 11：完成 CMake、TSan、安装消费和三套 ARM 边界

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `CMakePresets.json`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`
- Modify: `tests/integration/CMakeLists.txt`
- Modify: `tests/consumer/main.cpp`
- Modify: `tests/consumer/CMakeLists.txt`
- Modify: `cmake/EAVPConfig.cmake.in`

**Interfaces:**
- Produces: `EAVP_ENABLE_LINUX_RUNTIME`、`EAVP_ENABLE_V4L2`、`EAVP_ENABLE_V4L2_DEVICE_TESTS`、`EAVP_ENABLE_THREAD_SANITIZER`。
- Produces: `linux-tsan` configure/build/test preset。
- Consumes: 所有 Runtime/V4L2 sources 和公开安装头。

- [ ] **Step 1: 写配置约束和安装消费红灯**

在 consumer 中创建 RuntimeConfig、V4L2CaptureConfig，并在启用宏下链接 Node factory：

```cpp
eavp::Result<eavp::V4L2CaptureConfig> v4l2_config =
    eavp::V4L2CaptureConfig::create(
        "/dev/video10", eavp::PixelFormat::kYuv420p,
        1920, 1080, 30, 1, 4U);
if (!v4l2_config.ok()) return 1;
```

增加 CMake configure tests：设备测试未启用 V4L2/Runtime 时失败，TSan 与 ASan 同时开启时失败，非 Linux 强制关闭 Runtime/V4L2。

- [ ] **Step 2: 运行安装消费确认未导出新实现**

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --test-dir build/linux-debug -R '^eavp.install_consumer$' --output-on-failure
```

Expected: consumer 因新头/符号/宏尚未完整安装或链接而失败。

- [ ] **Step 3: 实现选项、条件 sources 和 package 配置**

Linux Runtime/V4L2 默认 ON；设备测试默认 OFF。使用 CMake header check 验证 `<linux/videodev2.h>`。Runtime 链接 Threads，不新增 find_package。安装整个公开 platform/linux 目录；`EAVPConfig.cmake.in` 导出 `EAVP_LINUX_RUNTIME_ENABLED`、`EAVP_V4L2_ENABLED` 和既有 `EAVP_ALSA_ENABLED`。consumer CMake 根据前两项分别定义 `EAVP_CONSUMER_LINUX_RUNTIME_ENABLED=1`、`EAVP_CONSUMER_V4L2_ENABLED=1`，只在对应能力存在时引用 factory 符号。

- [ ] **Step 4: 增加独立 TSan preset**

`linux-tsan` 使用 Debug、tests ON、ALSA/V4L2/Runtime ON、`-fsanitize=thread -fno-omit-frame-pointer`。CMake 在 `EAVP_ENABLE_THREAD_SANITIZER && EAVP_ENABLE_SANITIZERS` 时 FATAL_ERROR。

- [ ] **Step 5: 更新三套 ARM preset 并 clean build**

三个 preset 显式设置：

```json
"EAVP_ENABLE_LINUX_RUNTIME": "ON",
"EAVP_ENABLE_V4L2": "ON",
"EAVP_ENABLE_V4L2_DEVICE_TESTS": "OFF",
"EAVP_ENABLE_ALSA": "OFF"
```

Run:

```bash
cmake --preset aarch64-release
cmake --build --preset aarch64-release --target clean
cmake --build --preset aarch64-release
cmake --preset rockchip-armhf-release
cmake --build --preset rockchip-armhf-release --target clean
cmake --build --preset rockchip-armhf-release
cmake --preset hisiv600-release
cmake --build --preset hisiv600-release --target clean
cmake --build --preset hisiv600-release
```

Expected: 三套 clean build 成功；若工具链或 sysroot 缺失，停止并请求用户处理。

- [ ] **Step 6: 校验对象架构和安装消费**

```bash
readelf -h build/aarch64-release/src/CMakeFiles/eavp_platform.dir/platform/linux/v4l2_system.cpp.o
readelf -h build/rockchip-armhf-release/src/CMakeFiles/eavp_platform.dir/platform/linux/v4l2_system.cpp.o
readelf -h build/hisiv600-release/src/CMakeFiles/eavp_platform.dir/platform/linux/v4l2_system.cpp.o
ctest --test-dir build/linux-debug -R '^eavp.install_consumer$' --output-on-failure
```

Expected: 分别为 AArch64、ARM、ARM；consumer 通过 `find_package(EAVP)` 链接。

- [ ] **Step 7: 提交构建与安装边界**

```bash
git add CMakeLists.txt CMakePresets.json src/CMakeLists.txt \
  tests/unit/CMakeLists.txt tests/integration/CMakeLists.txt \
  tests/consumer/main.cpp tests/consumer/CMakeLists.txt \
  cmake/EAVPConfig.cmake.in
git commit -m "build(platform): 接入 Runtime 与 V4L2 验证矩阵"
```

---

### Task 12：实现 `/dev/video10` 真实设备验收

**Files:**
- Create: `tests/integration/v4l2_capture_device_test.cpp`
- Modify: `tests/integration/CMakeLists.txt`

**Interfaces:**
- Consumes: LinuxPlatformRuntime、V4L2SourceNode、现有 `/dev/video10` producer。
- Produces: capability preflight、300 帧设备采集、timeout 诊断和 Runtime stop 验收。

- [ ] **Step 1: 写默认关闭的真实设备测试**

读取并严格解析：

```text
EAVP_V4L2_DEVICE=/dev/video10
EAVP_V4L2_FRAME_COUNT=300
EAVP_V4L2_TIMEOUT_SECONDS=20
EAVP_V4L2_PIXEL_FORMAT=yuv420p
EAVP_V4L2_WIDTH=1920
EAVP_V4L2_HEIGHT=1080
EAVP_V4L2_FPS_NUMERATOR=30
EAVP_V4L2_FPS_DENOMINATOR=1
EAVP_V4L2_BUFFER_COUNT=4
```

测试先检查 path、权限、VIDEO_CAPTURE/STREAMING 和精确格式，再启动 Runtime。没有数据时在 deadline 返回包含设备、期望帧数和已采帧数的 `kTimeout` 诊断；不得启动或管理 FFmpeg producer。

- [ ] **Step 2: 配置设备测试并确认真实红灯原因**

```bash
cmake -S . -B build/linux-v4l2-device -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DEAVP_BUILD_TESTS=ON \
  -DEAVP_ENABLE_LINUX_RUNTIME=ON -DEAVP_ENABLE_V4L2=ON \
  -DEAVP_ENABLE_V4L2_DEVICE_TESTS=ON -DEAVP_ENABLE_ALSA=ON \
  -DEAVP_ENABLE_ALSA_DEVICE_TESTS=ON
cmake --build build/linux-v4l2-device --target eavp_v4l2_capture_device_tests
ctest --test-dir build/linux-v4l2-device -R '^eavp\.v4l2_device\.' --output-on-failure
```

Expected: 首次只允许因尚未完成的设备测试行为、精确协商诊断或当前外部 producer 状态失败；若缺少设备/权限/数据，停止并请求用户修复环境。

- [ ] **Step 3: 完成设备 Sink、deadline 和资源断言**

Sink 记录 300 帧、格式、plane、PTS 单调性、checksum、Metrics 和组合层 Health；静态或重复画面合法，不要求 checksum 非零或变化。stop 后 Runtime thread、V4L2 fd、MMAP 和 queued buffer 资源全部释放。

- [ ] **Step 4: 运行真实 `/dev/video10` 300 帧验收**

```bash
EAVP_V4L2_DEVICE=/dev/video10 \
EAVP_V4L2_FRAME_COUNT=300 \
EAVP_V4L2_TIMEOUT_SECONDS=20 \
EAVP_V4L2_PIXEL_FORMAT=yuv420p \
EAVP_V4L2_WIDTH=1920 EAVP_V4L2_HEIGHT=1080 \
EAVP_V4L2_FPS_NUMERATOR=30 EAVP_V4L2_FPS_DENOMINATOR=1 \
EAVP_V4L2_BUFFER_COUNT=4 \
ctest --test-dir build/linux-v4l2-device \
  -R '^eavp\.v4l2_device\.' --output-on-failure
```

Expected: capability、格式/PTS 和 300 帧采集测试全部通过。

- [ ] **Step 5: 回归既有 ALSA Loopback**

```bash
EAVP_ALSA_DEVICE=hw:Loopback,1,0 \
EAVP_ALSA_SAMPLE_FORMAT=s16le EAVP_ALSA_SAMPLE_RATE=48000 \
EAVP_ALSA_CHANNELS=2 EAVP_ALSA_SAMPLES_PER_FRAME=480 \
EAVP_ALSA_FRAME_COUNT=300 EAVP_ALSA_TIMEOUT_SECONDS=10 \
ctest --test-dir build/linux-v4l2-device \
  -R '^eavp\.alsa_device\.' --output-on-failure
```

Expected: 既有 ALSA 设备验收保持通过。

- [ ] **Step 6: 提交真实设备验收**

```bash
git add tests/integration/v4l2_capture_device_test.cpp \
  tests/integration/CMakeLists.txt
git commit -m "test(platform): 验证真实 V4L2 采集"
```

---

### Task 13：完成全矩阵验证与实现状态文档

**Files:**
- Modify: `README.md`
- Modify: `docs/architecture/build-and-portability.md`
- Modify: `docs/architecture/testing-strategy.md`
- Modify: `docs/architecture/threading-and-lifecycle.md`
- Modify: `docs/roadmap.md`
- Modify: `docs/standards/third-party-dependencies.md`
- Modify: `docs/superpowers/specs/2026-08-25-eavp-linux-platform-runtime-design.md`
- Modify: `docs/superpowers/specs/2026-08-21-eavp-linux-v4l2-capture-design.md`
- Modify: `docs/superpowers/specs/2026-08-22-eavp-linux-alsa-capture-design.md`

**Interfaces:**
- Consumes: Tasks 1-12 的全部实现和实测证据。
- Produces: 0.3a 实现状态、准确验收矩阵和 0.3c 前置边界。

- [ ] **Step 1: clean 运行 host Debug、Release、ASan/UBSan、TSan**

```bash
for preset in linux-debug linux-release linux-asan linux-tsan; do
  cmake --preset "$preset"
  cmake --build --preset "$preset" --target clean
  cmake --build --preset "$preset"
  ctest --preset "$preset" --output-on-failure
done
```

Expected: 四套配置全部通过；ASan/UBSan/TSan 无报告。不得复用其他 preset 的对象作为通过证据。

- [ ] **Step 2: 重跑安装、真实设备和三套 ARM clean 证据**

重复 Task 11/12 的 consumer、V4L2、ALSA 和三套 ARM 命令，并记录测试总数、耗时、readelf machine 和任何设计性 skip。不得把未执行项目写成通过。

- [ ] **Step 3: 更新规范状态和架构文档**

只有所有强制验收通过后，才把 Runtime/V4L2 规格状态改为“已实现”并记录验收命令实际执行当日的 ISO 日期，把 ALSA readiness 改为已实现，并在路线图将 0.3a 标为完成开发验收。README 仍声明稳定包为 0.2.0。

依赖登记必须明确：Runtime/V4L2 无新增第三方生产库，ALSA 仍为可选系统依赖，ThreadSanitizer 只属于验证工具。

- [ ] **Step 4: 执行静态范围和占位扫描**

```bash
rg -n 'TO''DO|TB''D' README.md docs include src tests \
  CMakeLists.txt CMakePresets.json --glob '!docs/superpowers/plans/*'
rg -n 'libavcodec|libx264|libx265|libv4l2|rockchip|HI_MPI' \
  include/eavp/platform/linux src/platform/linux CMakeLists.txt src/CMakeLists.txt
rg -n '#include.*(videodev2|alsa|epoll|eventfd)' include/eavp/media include/eavp/base
git diff --check
```

Expected: 无未解释占位；0.3a target 无 FFmpeg/libv4l2/厂商 SDK；Core 头无平台头；diff 格式干净。

- [ ] **Step 5: 提交 0.3a 实现状态**

```bash
git add README.md docs/architecture/build-and-portability.md \
  docs/architecture/testing-strategy.md \
  docs/architecture/threading-and-lifecycle.md docs/roadmap.md \
  docs/standards/third-party-dependencies.md \
  docs/superpowers/specs/2026-08-25-eavp-linux-platform-runtime-design.md \
  docs/superpowers/specs/2026-08-21-eavp-linux-v4l2-capture-design.md \
  docs/superpowers/specs/2026-08-22-eavp-linux-alsa-capture-design.md
git commit -m "docs(platform): 记录 Linux Runtime 与 V4L2 验收"
```

- [ ] **Step 6: 最终提交与工作树审计**

```bash
git status --short
git log --oneline --decorate -15
git diff 80d199c..HEAD --check
```

Expected: 只剩用户个人学习文档的既有状态；所有 0.3a 交付均已提交，不包含 build 目录、日志、缓存或运行时产物。
