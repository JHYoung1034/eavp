# EAVP Linux V4L2 Capture 0.3a 设计规格

> 状态：待审阅
>
> 适用版本：`0.3.0` 开发阶段，稳定版本仍为 `0.2.0`
>
> 规范性范围：Linux V4L2 单平面 MMAP 视频采集、CPU Frame 输出、生命周期、错误模型、可观测性与验收边界

## 1. 背景与决策

EAVP 0.2 已建立 Buffer、VideoFrame、类型化 Port、Pipeline、Capability、Backend Registry 和 Reference Backend。0.3 Linux Native 原计划同时引入 V4L2 与 oneVPL；为让每个交付都可独立验收，本阶段拆分为：

- **0.3a**：V4L2 单平面 MMAP 采集，输出平台拥有的 CPU VideoFrame；
- **0.3b**：oneVPL VPP、GPU Surface 与 H.264 硬件编码。

当前主机为 Intel Haswell。`libvpl-dev 2023.3.0`、`libmfx-gen1.2 23.2.3` 和 Intel 驱动均已安装，但 `vpl-inspect` 返回 `no implementations found`；VA-API 只能经旧 `i965` 驱动提供 H.264/VPP。因此 0.3b 保留已批准的架构方向，等待兼容 oneVPL 2.x 的 Intel GPU 环境，不在 0.3a 中临时替换为 VA-API。

## 2. 目标

0.3a 必须交付：

1. Linux V4L2 `VIDEO_CAPTURE` 单平面、MMAP、非阻塞采集；
2. YUV420P、NV12 与 YUYV422 到 EAVP VideoFormat/PlaneLayout 的严格映射；
3. 驱动 Buffer 到 EAVP CPU Buffer 的有界复制与明确所有权；
4. 与现有 MediaNode、OutputPort、MediaPipeline 和确定性 Executor 一致的调度语义；
5. enriched Status、Metrics 与上层 Health 发布所需的完整错误上下文；
6. Fake System 的确定性测试，以及 `/dev/video10` 上连续 300 帧的可选真实设备验收；
7. Linux x86_64 原生矩阵和三套 ARM 交叉构建兼容性。

## 3. 非目标

本阶段不实现：

- V4L2 multi-planar、read I/O、USERPTR 或 DMABUF；
- V4L2 M2M、ISP 控制、媒体控制器拓扑、热插拔自动重连；
- ALSA、音视频同步、oneVPL、VA-API、MPP/RGA 或 `HI_MPI`；
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
    V4L2SourceNode(const std::string& id,
                   const V4L2CaptureConfig& config,
                   MetricRegistry* metrics);
    ~V4L2SourceNode();

    OutputPort<VideoFrame>& output();
    Result<VideoFormat> actual_format() const;
};

}  // namespace eavp
```

构造函数不访问设备；所有可能失败的资源操作进入 `prepare()`。`MetricRegistry*` 可为空；非空时 Node 发布采集指标。Node 不持有 HealthManager 或 StateStore，避免把管理状态策略固化在设备适配中；组合层根据 enriched Status 发布 Actual/Health。

### 4.2 私有系统边界

新增私有文件：

- `src/platform/linux/v4l2_system.hpp`
- `src/platform/linux/v4l2_system.cpp`

`V4L2System` 封装 `open`、`ioctl`、`mmap`、`munmap`、`close` 与 monotonic clock。生产实现只调用 POSIX/Linux UAPI；测试实现按脚本返回结果。该接口不安装、不导出，不成为平台 ABI。

fd、MMAP Region 和已申请内核 Buffer 分别由 move-only RAII 对象持有。prepare 中任一步失败时，已取得资源必须按 `STREAMOFF（若已启动）→ munmap → REQBUFS(count=0) → close` 顺序清理。

### 4.3 Target 与依赖方向

V4L2 实现编入 `EAVP::platform`：

```text
EAVP::platform
  ├── EAVP::control
  ├── EAVP::management
  └── EAVP::media
        └── EAVP::base
```

`media` 不包含 `<linux/videodev2.h>`，不依赖 platform。oneVPL 后续使用独立可选 target `EAVP::onevpl -> EAVP::media`，不反向进入 Core。

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
2. 分配 EAVP CPU Buffer；
3. 逐 plane 复制包含 stride 的有效区域；
4. 构造共享只读 VideoFrame；
5. 立即 QBUF 归还驱动 Buffer。

因此下游队列、背压和 Frame 生命周期不延长 V4L2 MMAP Buffer 的占用。0.3a 不宣称零拷贝。

## 6. 调度与生命周期

Node 不创建私有线程，设备以 `O_RDWR | O_NONBLOCK | O_CLOEXEC` 打开。每次 `tick()` 最多产生一帧：

1. 若存在 `pending_frame`，先尝试发送；仍背压则返回 `kWouldBlock`；
2. 执行一次非阻塞 DQBUF；`EAGAIN` 返回 `kWouldBlock`；
3. 完成校验、复制和 QBUF；
4. 尝试发送新 Frame；下游背压时保存为 `pending_frame`。

生命周期行为：

| Node 操作 | V4L2 行为 | 结果 |
|---|---|---|
| `prepare` | QUERYCAP、S/G_FMT、S/G_PARM、REQBUFS、QUERYBUF、mmap | Prepared |
| `start` | 全部 QBUF 后 STREAMON | Running |
| `tick` | 至多一次 DQBUF/copy/QBUF/send | Running 或 Error |
| `stop` | 立即 STREAMOFF，丢弃内部 pending | Stopped |
| `reset` | munmap、REQBUFS(0)、close | Created |

stop 不再采集新 Frame；已经进入端口队列的 Frame 由现有 Pipeline 继续排空。尚未进入队列的 pending Frame 在 stop 时丢弃，并计入 `v4l2.frames.dropped_on_stop`。重复 stop/reset 必须幂等。

## 7. 时间戳与序列

VideoFrame 统一使用 `TimeBase(1, 1000000)`：

- 驱动设置 `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC` 时，将 `v4l2_buffer.timestamp` 转为微秒；
- 驱动时间戳类型不可靠或为零时，使用 DQBUF 后读取的 `CLOCK_MONOTONIC`；
- 乘加转换必须检查 `int64_t` 溢出；
- 对外 PTS 必须单调不减，回退时不得小于上一帧 PTS；
- `v4l2_buffer.sequence` 不连续不使 Pipeline 失败，但递增 gap 指标。

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

指标写入失败不得静默忽略；当媒体操作和指标写入同时失败时，优先返回媒体操作失败。验收组合层使用 Status 的 provider/operation/message/native code 发布 Actual Error，并将组件 `v4l2.capture` 的 Health 置为 Error；成功显式恢复后清理旧错误并恢复 Health。

## 10. 构建与可移植性

新增 CMake 选项：

```text
EAVP_ENABLE_V4L2=ON
EAVP_ENABLE_V4L2_DEVICE_TESTS=OFF
```

- Linux 上 V4L2 默认启用；缺少 `<linux/videodev2.h>` 时配置失败并给出安装提示；
- 非 Linux 平台强制关闭并不编译公开 V4L2 头；
- 生产代码只依赖 C++11、POSIX 与 Linux UAPI；
- FFmpeg、v4l2-ctl 和 v4l2loopback 不进入生产 target，也不是普通测试依赖；
- Rockchip ARMHF、HiSilicon v600 ARM32 与 aarch64 preset 编译 V4L2 代码，但关闭并不运行设备测试；
- oneVPL 不在 0.3a target 或 CMake 选项中实现。

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

使用 Fake System 组合真实 `V4L2SourceNode → bounded Queue → FrameChecksumSink`，连续生成 300 帧。断言：

- Sink 正好接收 300 帧，无重复；
- 格式、plane、stride 与 TimeBase 正确；
- PTS 单调不减；
- 校验统计与测试向量一致；
- Pipeline、Metrics 和 Health 最终健康；
- stop 后所有 V4L2 资源释放。

该测试进入 Debug、Release 和 ASan/UBSan 普通 CTest。

### 11.3 可选真实设备验收

设备测试默认关闭，显式启用后直接消费现有数据源，不启动或管理 FFmpeg。配置来源：

- `EAVP_V4L2_DEVICE`，默认 `/dev/video10`；
- `EAVP_V4L2_FRAME_COUNT`，默认 `300`；
- `EAVP_V4L2_TIMEOUT_SECONDS`，默认 `20`。

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
9. 不宣称 DMABUF 零拷贝、oneVPL、VA-API、MPP/RGA 或 `HI_MPI` 已实现。

## 13. 0.3b 保留设计

兼容硬件环境具备后，0.3b 按独立规格实现：

```text
V4L2 YUV420P CPU Frame
  -> oneVPL VPP（上传并转 NV12）
  -> oneVPL DeviceOpaque Surface
  -> oneVPL H.264 Encoder
  -> 测试用 Annex-B Sink
```

oneVPL 使用独立可选 target `EAVP::onevpl`，通过 `MFXLoad` 选择硬件实现；异步操作以零超时 SyncOperation 向 Executor 传播 `kWouldBlock`；Surface 由 DeviceOpaque BufferStorage 的共享所有权管理。生产代码不依赖 FFmpeg，验收可使用外部工具验证 Annex-B 可解析和解码。

该节只冻结后续方向，不属于 0.3a 实现或验收范围。

## 14. 参考依据

- Linux Kernel V4L2 capture 示例：`https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/capture.c.html`
- Linux Kernel V4L2 MMAP streaming：`https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/mmap.html`
- Intel oneVPL 入门与 2.x Loader：`https://www.intel.com/content/www/us/en/developer/articles/guide/get-started-with-the-oneapi-video-processing-library.html`
- 本机验证：Linux 7.0、`/dev/video10` 为 v4l2loopback YUV420P 1920×1080@30、oneVPL API 2.9、Haswell 上无可枚举 oneVPL 实现。
