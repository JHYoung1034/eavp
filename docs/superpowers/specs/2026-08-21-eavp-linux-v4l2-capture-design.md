# EAVP Linux V4L2 Capture 0.3a 设计规格

> 状态：已批准
>
> 适用版本：`0.3.0` 开发阶段，稳定版本仍为 `0.2.0`
>
> 规范性范围：Linux V4L2 单平面 MMAP 视频采集、CPU Frame 输出、事件驱动调度、生命周期、错误模型、可观测性与验收边界

## 1. 背景与决策

EAVP 0.2 已建立 Buffer、VideoFrame、类型化 Port、Pipeline、Capability、Backend Registry 和 Reference Backend。开发机上的 Intel GPU 只用于验证架构和数据流，不是嵌入式 ARM 产品的兼容目标。为让每个交付都可独立验收，0.3 Linux Native 拆分为：

- **0.3a**：V4L2 单平面 MMAP 采集，输出平台拥有的 CPU VideoFrame；
- **0.3b**：ALSA 非阻塞 PCM 采集，输出平台拥有的 CPU AudioFrame；
- **0.3c**：开发机可选 FFmpeg 软件编码 Backend，以 libx264、libx265、FFmpeg AAC encoder 和 libopus 验证 H.264、H.265、AAC 与 Opus 数据流；
- **0.3d**：V4L2 与 ALSA 同时采集，测量共同单调时钟域下的偏差和漂移。

0.3c 不要求兼容旧 Intel GPU，也不把 oneVPL、VA-API 或 FFmpeg 带入 ARM 生产依赖。开发机已具备 FFmpeg 6.1.1、`libavcodec 60.31.102` 和 `libavutil 58.29.100`；系统 libavcodec 已启用 libx264 与 libx265，后端可统一通过 libavcodec API 选择软件编码器。

## 2. 目标

0.3a 必须交付：

1. Linux V4L2 `VIDEO_CAPTURE` 单平面、MMAP、非阻塞采集；
2. YUV420P、NV12 与 YUYV422 到 EAVP VideoFormat/PlaneLayout 的严格映射；
3. 驱动 Buffer 到 EAVP CPU Buffer 的有界复制与明确所有权；
4. 与现有 MediaNode、OutputPort、MediaPipeline 及 Linux Platform Runtime 一致的事件驱动调度语义；
5. enriched Status、Metrics 与上层 Health 发布所需的完整错误上下文；
6. Fake System 的确定性测试，以及 `/dev/video10` 上连续 300 帧的可选真实设备验收；
7. Linux x86_64 原生矩阵和三套 ARM 交叉构建兼容性。

## 3. 非目标

本阶段不实现：

- V4L2 multi-planar、read I/O、USERPTR 或 DMABUF；
- V4L2 M2M、ISP 控制、媒体控制器拓扑、热插拔自动重连；
- ALSA、音视频同步、FFmpeg 编码 Backend、oneVPL、VA-API、MPP/RGA 或 `HI_MPI`；
- 编码、容器、FileSink、网络协议或 Service Mode；
- 外部视频源进程的启动、停止或监控；
- 对 `virtrual-v4l2-test.md` 个人学习记录的引用或修改。

## 4. 模块与依赖

### 4.1 公开平台接口

新增：

- `include/eavp/platform/linux/v4l2_capture.hpp`
- `src/platform/linux/v4l2_capture.cpp`

公开值对象与 Node 采用以下稳定名称：

```cpp
namespace eavp {

struct V4L2CaptureConfig {
    static Result<V4L2CaptureConfig> create(
        const std::string& device_path,
        PixelFormat pixel_format,
        int width,
        int height,
        int frame_rate_numerator,
        int frame_rate_denominator,
        std::size_t buffer_count);

    std::string device_path;
    PixelFormat pixel_format;
    int width;
    int height;
    int frame_rate_numerator;
    int frame_rate_denominator;
    std::size_t buffer_count;
};

class V4L2SourceNode : public MediaNode {
public:
    static Result<std::unique_ptr<V4L2SourceNode> > create(
        const std::string& id,
        const V4L2CaptureConfig& config,
        MetricRegistry* metrics);
    ~V4L2SourceNode() noexcept;

    OutputPort<VideoFrame>& output();
    Result<VideoFormat> actual_format() const;
};

}  // namespace eavp
```

factory 只创建对象，不访问设备；所有设备资源操作进入 `prepare()`。factory 捕获分配失败和未知异常，公共边界不抛出异常。`MetricRegistry*` 可为空；非空时 Node 发布采集指标。Node 不持有 HealthManager 或 StateStore，避免把管理状态策略固化在设备适配中；组合层根据 enriched Status 发布 Actual/Health。

### 4.2 私有系统边界

新增私有文件：

- `src/platform/linux/v4l2_api.hpp`
- `src/platform/linux/v4l2_linux_api.cpp`
- `src/platform/linux/v4l2_system.hpp`
- `src/platform/linux/v4l2_system.cpp`
- `src/platform/linux/v4l2_capture_internal.hpp`

内部按 `V4L2Api -> V4L2System -> V4L2SourceNode::Impl` 分层。`V4L2Api` 仅封装 `open`、`ioctl`、`mmap`、`munmap`、`close` 与 monotonic clock；`V4L2System` 独占设备会话、MMAP regions、驱动队列状态和协商结果。生产实现只调用 POSIX/Linux UAPI；测试实现按脚本返回结果。私有接口不安装、不导出，不成为平台 ABI。

fd、MMAP Region 和已申请内核 Buffer 分别由 move-only RAII 对象持有。prepare 中任一步失败时，必须在返回前按 `munmap -> REQBUFS(count=0) -> close` 顺序完成回滚；已进入 streaming 的会话在释放前先 `STREAMOFF`。析构函数执行 noexcept 的兜底释放。

### 4.3 Target 与依赖方向

V4L2 实现编入 `EAVP::platform`：

```text
EAVP::platform
  ├── EAVP::control
  ├── EAVP::management
  └── EAVP::media
        └── EAVP::base
```

`media` 不包含 `<linux/videodev2.h>`，不依赖 platform。0.3c 的开发机验证实现使用独立可选 build-tree target `EAVP::backend_ffmpeg -> EAVP::media`，不反向进入 Core，不安装或导出。

V4L2 Source 实现 `LinuxWaitSource`，并由 `docs/superpowers/specs/2026-08-25-eavp-linux-platform-runtime-design.md` 定义的 Linux Platform Runtime 驱动。Runtime 属于 `platform`，不改变上述依赖方向。

## 5. 格式与内存契约

### 5.1 支持格式

| V4L2 FourCC | EAVP PixelFormat | Plane 数 | CPU Plane 布局 |
|---|---|---:|---|
| `V4L2_PIX_FMT_YUV420` | `kYuv420p` | 3 | Y、U、V |
| `V4L2_PIX_FMT_NV12` | `kNv12` | 2 | Y、UV |
| `V4L2_PIX_FMT_YUYV` | 新增 `kYuyv422` | 1 | packed YUYV |

`PixelFormat::kYuyv422` 必须同步加入稳定名称、VideoFormat factory、Frame/Buffer 一致性与 Capability 测试。宽高、stride 和每个 plane 的 `offset + size` 必须进行溢出检查；YUV420P/NV12 必须拒绝奇数宽高。

### 5.2 精确协商

Node 使用 `VIDIOC_S_FMT` 提交配置，再以驱动返回值作为唯一实际格式。以下任一变化均返回 `kCapabilityMismatch`，不静默接受：

- pixel format 改变；
- width 或 height 改变；
- 返回布局无法表示为受支持的 PlaneLayout；
- `sizeimage` 小于所有 plane 所需末端；
- 请求帧率不能由 `VIDIOC_S_PARM`/`VIDIOC_G_PARM` 确认。

实际 `bytesperline` 和 `sizeimage` 可大于可见数据需求；复制必须保留 stride，不把 padding 当作有效像素。

### 5.3 所有权

MMAP 区域始终由驱动队列拥有，不包装成跨 tick 生存的 VideoFrame。每次成功 DQBUF 后：

1. 校验 buffer index、flags、bytesused 和 plane 范围；
2. 使用新增的 `Buffer::allocate(size, planes)` 分配具有协商布局的 EAVP 多平面 CPU Buffer；
3. 目标存储先清零，再逐 plane、逐行复制有效像素字节；
4. 构造共享只读 VideoFrame；
5. 立即 QBUF 归还驱动 Buffer。

CPU Buffer 保留协商得到的 stride 和 plane offset，但不复制驱动 padding 中的未定义数据；`bytesused` 必须覆盖最后一个有效像素，不要求尾部 padding 被标记为有效媒体数据。因此下游队列、背压和 Frame 生命周期不延长 V4L2 MMAP Buffer 的占用，校验统计也不受未定义 padding 影响。0.3a 不宣称零拷贝。

单平面格式的布局约束为：

- YUV420P：宽高和 luma stride 为偶数，Y stride 为 `bytesperline`，U/V stride 各为其一半；
- NV12：宽高为偶数，Y/UV stride 均为 `bytesperline`；
- YUYV422：单 plane packed，stride 不小于 `width * 2`。

所有 stride、offset、plane size、总容量和最后有效字节位置均在乘加前检查溢出。

## 6. 调度与生命周期

Node 不创建私有线程，设备以 `O_RDWR | O_NONBLOCK | O_CLOEXEC` 打开，并向 Runtime 暴露一个 V4L2 poll descriptor。Runtime 使用 level-triggered epoll，在设备 readable 时执行一次完整 Pipeline tick。每次 `tick()` 最多产生一帧：

1. 若存在 `pending_frame`，先尝试发送；仍背压则返回 `kWouldBlock`；
2. 执行一次非阻塞 DQBUF；`EAGAIN` 返回 `kWouldBlock`；
3. 完成校验、复制和 QBUF；
4. 尝试发送新 Frame；下游背压时保存为 `pending_frame`。

设备仍有已完成 Buffer 时，level-triggered epoll 会再次报告 ready；因此积压通过多次有界 Pipeline turn 消化，不在一次 tick 内无界循环。`DeterministicExecutor` 只保留给 Fake 和单元测试，真实设备验收必须通过 Linux Platform Runtime 驱动。

生命周期行为：

| Node 操作 | V4L2 行为 | 结果 |
|---|---|---|
| `prepare` | open、QUERYCAP、S/G_FMT、S/G_PARM、REQBUFS、QUERYBUF、mmap | Prepared |
| `start` | 全部 QBUF 后 STREAMON | Running |
| `tick` | 至多一次 DQBUF/copy/QBUF/send | Running 或 Error |
| `stop` | 立即 STREAMOFF，丢弃内部 pending | Stopped |
| `reset` | munmap、REQBUFS(0)、close | Created |

stop 不再采集新 Frame；已经进入端口队列的 Frame 由现有 Pipeline 继续排空。尚未进入队列的 pending Frame 在 stop 时丢弃，并计入 `v4l2.frames.dropped_on_stop`。重复 stop/reset 必须幂等。

所有 `open`/`ioctl` 的 `EINTR` 最多重试 64 次；达到上限后返回 enriched I/O 错误，避免单线程 Executor 永久停留在一次 tick 中。DQBUF 成功后，无论复制或 Frame 构造是否成功都必须尝试 QBUF；若原媒体操作和 QBUF 同时失败，优先返回会破坏后续驱动队列可用性的 QBUF 错误。

## 7. 时间戳与序列

VideoFrame 统一使用 `TimeBase(1, 1000000)`：

- 驱动设置 `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC` 时，将 `v4l2_buffer.timestamp` 转为微秒；
- 仅接受明确标记为 monotonic 且字段合法、非零的驱动时间戳；其他情况使用 DQBUF 后读取的 `CLOCK_MONOTONIC`；
- 乘加转换必须检查 `int64_t` 溢出；
- 对外 PTS 必须单调不减，回退时不得小于上一帧 PTS；
- `v4l2_buffer.sequence` 不连续不使 Pipeline 失败，`v4l2.sequence.gaps` 累加缺失帧数量，并正确处理 `uint32_t` 回绕。

0.3a 不生成 DTS，也不做跨设备时钟同步。

## 8. 错误模型

所有 ioctl 遇到 `EINTR` 必须重试。状态映射如下：

| 条件 | StatusCode |
|---|---|
| DQBUF `EAGAIN`、端口背压 | `kWouldBlock` |
| `ENODEV`、`ENXIO`、`EIO`、设备失效/HUP | `kDeviceLost` |
| 非 V4L2、无 capture/streaming、格式不匹配、Buffer 数不足 | `kCapabilityMismatch` |
| 配置非法、index/bytesused/plane 越界 | `kInvalidArgument` 或 `kCorruptData` |
| mmap、容器或 CPU Buffer 分配失败 | `kResourceExhausted` |
| 其他 open/ioctl/munmap/close 失败 | `kIoError` 或 `kInternal` |

系统错误 Status 必须携带：

- `provider_id = "v4l2"`；
- 精确 operation，例如 `VIDIOC_DQBUF`；
- 简体中文可诊断消息；
- errno native code。

外部异常不得越过 Node 公共边界；`std::bad_alloc` 映射为无消息的 `kResourceExhausted`，其他异常映射为无消息的 `kInternal`。设备故障进入 Pipeline Error；0.3a 不自动重连，恢复由上层显式 reset/reconcile。

## 9. 可观测性

Node 在 MetricRegistry 非空时发布：

- Counter `v4l2.frames.captured`；
- Counter `v4l2.frames.dropped_on_stop`；
- Counter `v4l2.sequence.gaps`；
- Counter `v4l2.dequeue.would_block`；
- Counter `v4l2.bytes.copied`；
- Gauge `v4l2.pending_frame`，取值 0 或 1。

私有 `V4L2Observer` 隔离 Node 事件与具体 MetricRegistry 写入，并允许 Fake 注入确定性指标失败。指标写入失败不得静默忽略；单独发生时向上传播，当媒体操作和指标写入同时失败时优先返回媒体操作失败。验收组合层使用 Status 的 provider/operation/message/native code 发布 Actual Error，并将组件 `v4l2.capture` 的 Health 置为 Error；成功显式恢复后清理旧错误并恢复 Health。0.3a 不把 HealthManager 注入设备 Node，也不重复实现组合层健康策略。

## 10. 构建与可移植性

新增 CMake 选项：

```text
EAVP_ENABLE_V4L2=ON
EAVP_ENABLE_V4L2_DEVICE_TESTS=OFF
```

- Linux 上 V4L2 默认启用；缺少 `<linux/videodev2.h>` 时配置失败并给出安装提示；
- V4L2 真实调度要求 `EAVP_ENABLE_LINUX_RUNTIME=ON`；Runtime 只依赖 Threads、POSIX 与 Linux UAPI；
- 非 Linux 平台强制关闭并不编译公开 V4L2 头；
- 生产代码只依赖 C++11、POSIX 与 Linux UAPI；
- FFmpeg、v4l2-ctl 和 v4l2loopback 不进入生产 target，也不是普通测试依赖；
- Rockchip ARMHF、HiSilicon v600 ARM32 与 aarch64 preset 编译 V4L2 代码，但关闭并不运行设备测试；
- FFmpeg Backend 不在 0.3a target 或 CMake 选项中实现。

0.3a 仍处于 0.3.0 开发阶段，不修改 CMake package 版本或 README 中的当前稳定版本。发布版本变更由 0.3b 或后续独立发布决策统一处理。

## 11. 测试策略

### 11.1 单元测试

Fake V4L2System 必须覆盖：

- 三种 FourCC 的精确格式和 plane 映射；
- 非法宽高、stride、sizeimage、bytesused、buffer index 与溢出；
- 每个 prepare 步骤失败后的逆序资源回收；
- EINTR 重试和 EAGAIN `kWouldBlock`；
- `ENODEV`/`EIO` enriched `kDeviceLost`；
- pending Frame 背压时不再次 DQBUF；
- copy 后立即 QBUF，Frame 离开 tick 后仍有效；
- V4L2 时间戳、monotonic fallback、溢出和 sequence gap；
- stop 丢弃 pending、STREAMOFF 一次、重复 stop/reset 幂等；
- 指标累积与指标失败优先级；
- 公开 API 无异常越界。

### 11.2 确定性集成测试

使用 Fake System 组合真实 `LinuxPlatformRuntime -> V4L2SourceNode -> bounded Queue -> FrameChecksumSink`，通过 Fake readiness 连续生成 300 帧。断言：

- Sink 正好接收 300 帧，无重复；
- 格式、plane、stride 与 TimeBase 正确；
- PTS 单调不减；
- 校验统计与测试向量一致；
- Pipeline、Metrics 和 Health 最终健康；
- stop 后所有 V4L2 资源释放。

该测试进入 Debug、Release、ASan/UBSan 和 ThreadSanitizer 普通 CTest。

### 11.3 可选真实设备验收

设备测试默认关闭，显式启用后直接消费现有数据源，不启动或管理 FFmpeg。配置来源：

- `EAVP_V4L2_DEVICE`，默认 `/dev/video10`；
- `EAVP_V4L2_FRAME_COUNT`，默认 `300`；
- `EAVP_V4L2_TIMEOUT_SECONDS`，默认 `20`。
- `EAVP_V4L2_PIXEL_FORMAT`，默认 `yuv420p`；
- `EAVP_V4L2_WIDTH`、`EAVP_V4L2_HEIGHT`，默认 `1920`、`1080`；
- `EAVP_V4L2_FPS_NUMERATOR`、`EAVP_V4L2_FPS_DENOMINATOR`，默认 `30`、`1`；
- `EAVP_V4L2_BUFFER_COUNT`，默认 `4`。

测试必须先检查设备存在、权限、capture/streaming 能力和实际格式；在超时内采集指定帧数，并验证格式、plane 布局、PTS、帧数、校验统计、Metrics 和 Health。设备存在但没有数据时返回明确的环境超时诊断，不冒充 DeviceLost 或测试崩溃。

## 12. 验收标准

0.3a 完成时必须同时满足：

1. YUV420P、NV12、YUYV422 的 factory、V4L2 映射和 Buffer/Frame 测试通过；
2. Fake System 单元与 300 帧确定性纵切面通过；
3. `/dev/video10` 在现有外部数据源下于超时内采集 300 帧；
4. Debug、Release、ASan/UBSan CTest 全部通过，包含安装消费工程；
5. Rockchip ARMHF 与 HiSilicon v600 生成 `ELF32 / ARM` 对象，aarch64 生成 `ELF64 / AArch64` 对象；
6. V4L2 公开头通过安装后的 `find_package(EAVP)` 消费；
7. 不新增生产第三方库，FFmpeg 不进入链接依赖；
8. 文档以简体中文为主，无未解释的占位标记；
9. 不宣称 DMABUF 零拷贝、FFmpeg 编码 Backend、oneVPL、VA-API、MPP/RGA 或 `HI_MPI` 已实现。
10. Linux Platform Runtime 的 eventfd stop、串行 Pipeline tick 和 ThreadSanitizer 验收通过。

## 13. 0.3c 开发机 FFmpeg 软件编码验证

0.3c 按独立规格实现，不属于 0.3a 交付范围。保留的数据流为：

```text
V4L2 YUV420P CPU Frame
  -> EAVP::backend_ffmpeg
  -> libavcodec
  -> libx264 H.264 或 libx265 H.265
  -> Annex-B MediaPacket

ALSA interleaved CPU AudioFrame
  -> frame aggregation / sample format conversion
  -> FFmpeg AAC encoder 或 libopus
  -> AAC 或 Opus MediaPacket
```

### 13.1 Target 与依赖

- 新增 `EAVP_ENABLE_FFMPEG_BACKEND=OFF`，默认关闭；
- 新增 build-tree alias `EAVP::backend_ffmpeg`，只依赖 `EAVP::media` 与所需的 libavcodec、libavutil；音频转换确有需要时可增加只属于该可选 Backend 的 libswresample；
- 不直接包含 x264/x265 头文件，通过 `avcodec_find_encoder_by_name("libx264")` 和 `avcodec_find_encoder_by_name("libx265")` 选择实现；
- 不依赖 libswscale，视频首期仅接收 CPU YUV420P/NV12；
- target 不安装、不导出，三套 ARM preset 强制关闭；
- Rockchip MPP/RGA 与海思 `HI_MPI` 继续使用各自独立 Provider 和硬件规格。

### 13.2 Encoder 契约

FFmpeg Provider 的视频路径实现现有 `MediaBackendProvider` 和 `VideoEncoder`；音频路径按 0.3c 独立规格增加平台无关 `AudioEncoder`、Capability 和 Registry selection：

- H.264 使用 libx264，H.265 使用 libx265；编码器不存在时 probe 不声明相应 Capability；
- AAC 使用 FFmpeg AAC encoder，Opus 使用 libopus；不可用时 probe 不声明相应 Capability；
- submit 将 EAVP CPU Frame 逐 plane、逐行复制到 `AVFrame`，不宣称零拷贝；
- `avcodec_send_frame` 的 `AVERROR(EAGAIN)` 映射为 `kWouldBlock`，输入不得被视为已消费；
- receive 使用 `avcodec_receive_packet`，EAGAIN 映射为 `kWouldBlock`，EOF 映射为稳定 `kEndOfStream`；
- begin_drain 在空 Frame 被接受前允许因 EAGAIN 返回 `kWouldBlock` 并在后续调用重试；一旦接受则标记 Draining，不再重复提交，随后持续 receive 直到 EOF；
- H.264/H.265 输出分别标记为 `CodecId::kH264/kH265` 与 `EncodedStreamFormat::kAnnexB`，保留 PTS/DTS；
- AAC/Opus Packet 的 stream format、codec config、frame aggregation、PTS/DTS/duration 由 0.3c 独立规格冻结；原始 AudioFrame 不增加 DTS；
- reset 释放 AVPacket、AVFrame、AVCodecContext，并回到 Created；
- 后端不创建 EAVP 私有线程；确定性测试把 FFmpeg codec thread count 固定为 1。

### 13.3 错误与验收

FFmpeg 错误转换为 enriched Status：`provider_id="ffmpeg"`、operation 为具体 libavcodec API、native code 为负 `AVERROR`，消息由 `av_strerror` 生成；异常不得跨公共边界。

0.3c 的确定性测试使用合成 YUV420P/NV12 Frame 和 PCM AudioFrame，分别编码 H.264/H.265/AAC/Opus，再通过 libavcodec decoder 在进程内验证码流可解码、帧数、格式和时间戳。真实纵切面复用 V4L2 与 ALSA Source；不引入正式 FileSink、容器模块或音视频复用。

FFmpeg Backend 只验证平台架构、Backend 契约和标准码流数据流，不代表 ARM 产品将携带 FFmpeg。嵌入式部署仍优先使用 SoC 厂商媒体接口。

## 14. 参考依据

- Linux Kernel V4L2 capture 示例：`https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/capture.c.html`
- Linux Kernel V4L2 MMAP streaming：`https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/mmap.html`
- Linux Kernel V4L2 poll：`https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/func-poll.html`
- EAVP Linux Platform Runtime：`docs/superpowers/specs/2026-08-25-eavp-linux-platform-runtime-design.md`
- FFmpeg libavcodec send/receive API：`https://ffmpeg.org/doxygen/6.1/group__lavc__encdec.html`
- 本机验证：Linux 7.0、`/dev/video10` 为 v4l2loopback YUV420P 1920×1080@30、FFmpeg 6.1.1 的 libavcodec 已启用 libx264/libx265。
