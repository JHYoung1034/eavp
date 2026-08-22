# EAVP Linux ALSA Capture 0.3b 设计规格

> 状态：已批准（2026-08-22）
>
> 适用版本：`0.3.0` 开发阶段，稳定版本仍为 `0.2.0`
>
> 规范性范围：Linux ALSA 非阻塞 PCM 采集、AudioFormat/AudioFrame 契约、时间戳、恢复语义、可观测性与验收边界

## 1. 背景与决策

EAVP 必须把音频和视频作为同等重要的媒体输入。0.2 已提供 Buffer、类型化 Port、Pipeline、Backend Registry 和初始 AudioFrame，但现有 AudioFrame 只保存松散的采样参数，不能严格表达 ALSA 格式、Buffer 大小、首采样点 PTS 或采集中断。

0.3 Linux Native 按可独立验收的纵切面拆分为：

- **0.3a**：V4L2 单平面 MMAP 视频采集，输出平台拥有的 CPU VideoFrame；
- **0.3b**：ALSA 非阻塞 PCM 采集，输出平台拥有的 CPU AudioFrame；
- **0.3c**：开发机可选 FFmpeg 软件编码 Backend，以 libx264、libx265、FFmpeg AAC encoder 和 libopus 验证 H.264、H.265、AAC 与 Opus 数据流；
- **0.3d**：V4L2 与 ALSA 同时采集，验证共同单调时钟域下的偏差、漂移、Metrics 和 Health。

0.3b 直接消费用户已准备的 ALSA Loopback 虚拟麦克风，不启动或管理外部音频生产进程。ALSA 是 Linux 平台适配依赖，不进入 `media` Core；FFmpeg 不属于 0.3b 生产或测试依赖。

## 2. 目标

0.3b 必须交付：

1. Linux ALSA `SND_PCM_STREAM_CAPTURE`、`RW_INTERLEAVED`、非阻塞 PCM 采集；
2. S16_LE、S24_LE（32-bit container）、S32_LE、FLOAT_LE 的严格 EAVP 格式模型；
3. mono、stereo 和精确采样率配置，首条真实设备验收固定为 48 kHz；
4. 默认每个 AudioFrame 包含每声道 480 个采样，即 48 kHz 下 10 ms；
5. 硬件 period 与 EAVP 输出帧解耦，短读累积为固定长度 AudioFrame；
6. 首采样点 PTS、XRUN/suspend 重锚定及显式 discontinuity；
7. 与 MediaNode、OutputPort、MediaPipeline 和单线程确定性 Executor 一致的背压语义；
8. Fake System 确定性测试，以及现有 ALSA Loopback 节点上的可选真实设备验收；
9. ALSA 原生错误上下文、Metrics 和 Health 汇总。

## 3. 非目标

0.3b 不实现：

- PCM playback、full-duplex、mixer、音量控制、音频设备枚举或热插拔自动重连；
- `MMAP_INTERLEAVED`、非交织或 planar 音频、S24_3LE packed、big-endian、超过双声道的布局；
- 采样率转换、声道混合、sample format 转换、回声消除、降噪、自动增益或重采样漂移校正；
- AAC/Opus 编码、封装、网络协议或音视频复用；
- 跨设备时钟同步、自动丢帧/补帧或音视频偏差修正；
- 外部 Loopback 数据生产进程的启动、停止或监控；
- 对个人学习记录文档的引用或修改。

AAC/Opus 编码与必要的帧聚合、sample format 转换进入 0.3c；音视频同时采集和偏差/漂移测量进入 0.3d。

## 4. Core 音频数据模型

### 4.1 AudioFormat

新增 `include/eavp/media/audio_format.hpp`，将音频格式从 AudioFrame 的松散参数中抽离：

```cpp
namespace eavp {

enum class SampleFormat {
    kUnknown,
    kSigned16LittleEndian,
    kSigned24In32LittleEndian,
    kSigned32LittleEndian,
    kFloat32LittleEndian,
};

enum class AudioSampleLayout {
    kInterleaved,
};

enum class AudioChannelLayout {
    kMono,
    kStereo,
};

class AudioFormat {
public:
    static Result<AudioFormat> create(
        SampleFormat sample_format,
        int sample_rate,
        AudioChannelLayout channel_layout,
        AudioSampleLayout sample_layout,
        MemoryDomain memory_domain);

    SampleFormat sample_format() const;
    int sample_rate() const;
    AudioChannelLayout channel_layout() const;
    int channels() const;
    AudioSampleLayout sample_layout() const;
    MemoryDomain memory_domain() const;
    std::size_t bytes_per_sample() const;
    std::size_t bytes_per_pcm_frame() const;
};

}  // namespace eavp
```

这里的“PCM frame”沿用 ALSA 定义，表示每个声道各一个采样点；`AudioFrame` 表示一段包含多个 PCM frames 的媒体对象。S24_LE 明确为 32-bit little-endian container、24 个有效位，每采样占 4 bytes；不得与 3-byte packed S24_3LE 混用。

`AudioFormat::create` 必须拒绝 unknown format、非正采样率、未知 channel/layout/memory domain，以及所有整数溢出。0.3b 的 ALSA Source 只接受 `kCpu + kInterleaved + mono/stereo`。

### 4.2 AudioFrame

现有 AudioFrame 调整为：

```cpp
class AudioFrame {
public:
    static Result<AudioFrame> create(
        const Buffer& buffer,
        const AudioFormat& format,
        int samples_per_channel,
        std::int64_t pts,
        const TimeBase& time_base,
        bool discontinuity);

    const Buffer& buffer() const;
    const AudioFormat& format() const;
    int samples_per_channel() const;
    std::int64_t pts() const;
    const TimeBase& time_base() const;
    bool discontinuity() const;
};
```

规范语义如下：

- `pts` 是本 AudioFrame **第一个 PCM 采样点**的呈现/播放时间；
- 原始 PCM Frame 没有解码调度，不携带 DTS；AAC/Opus 等编码 Packet 才分别携带 PTS/DTS；
- `samples_per_channel` 是每声道采样数，不是所有声道采样值之和；
- 帧持续时间由 `samples_per_channel / sample_rate` 唯一确定；
- `discontinuity` 表示该帧之前存在已知时间线中断；正常启动的首帧为 false，XRUN 或 suspend 恢复后的首帧为 true；
- Buffer 必须与 AudioFormat memory domain 一致，必须恰有一个 plane，且可用字节数必须精确等于 `samples_per_channel * bytes_per_pcm_frame()`；
- `samples_per_channel` 和 TimeBase 必须为正，所有大小与时间换算必须检查溢出。

现有 `SampleFormat` 枚举和 AudioFrame factory 属于 1.0 前实验性 API，0.3b 允许上述不兼容调整。不得保留语义含糊的 `samples()` 或无 endian/container 信息的枚举值作为第二套长期接口。

## 5. 公开平台接口

新增：

- `include/eavp/platform/linux/alsa_capture.hpp`
- `src/platform/linux/alsa_capture.cpp`

公开头文件不得暴露 `snd_pcm_t`、ALSA enum 或其他 libasound 类型。

```cpp
namespace eavp {

class AlsaCaptureConfig {
public:
    static Result<AlsaCaptureConfig> create(
        const std::string& device_name,
        const AudioFormat& format,
        int samples_per_frame,
        int period_size_hint,
        int buffer_periods);

    const std::string& device_name() const;
    const AudioFormat& format() const;
    int samples_per_frame() const;
    int period_size_hint() const;
    int buffer_periods() const;
};

class AlsaSourceNode : public MediaNode {
public:
    static Result<std::unique_ptr<AlsaSourceNode> > create(
        const std::string& id,
        const AlsaCaptureConfig& config,
        MetricRegistry* metrics,
        HealthManager* health);
    ~AlsaSourceNode();

    OutputPort<AudioFrame>& output();
};

}  // namespace eavp
```

配置 factory 必须拒绝空设备名、非 CPU/interleaved/mono/stereo 格式、非正 frame/period/buffer 值和计算溢出。Node factory 必须拒绝空 id 和空 Metrics/Health，并在内部捕获分配异常，任何异常不得越过公共 API。首个纵切面使用 `samples_per_frame=480`、`period_size_hint=480`、`buffer_periods=4`。

`period_size_hint` 是 ALSA 硬件协商提示，不是 AudioFrame 的长度契约。驱动返回的实际 period 必须记录并用于诊断，但输出仍严格按 `samples_per_frame` 聚合。

## 6. 模块与依赖

ALSA 适配实现进入 `EAVP::platform`：

```text
EAVP::platform
  -> EAVP::media
  -> EAVP::management
  -> ALSA::ALSA

EAVP::media
  -> EAVP::base
```

`media` 提供平台无关 AudioFormat/AudioFrame，不包含 ALSA 头文件。`platform` 负责 ALSA API、节点、Metrics 和 Health，依赖方向不变。

内部 `AlsaSystem` 封装 `snd_pcm_open/close`、HW/SW params、`snd_pcm_prepare/start/drop/resume/readi/htimestamp/avail_update`、错误文本和 monotonic clock。生产实现调用 libasound；Fake 实现按脚本返回结果。该接口位于 `src/platform/linux` 私有目录，不安装、不导出，也不成为 ABI。

## 7. ALSA 配置与协商

### 7.1 打开模式

使用：

```text
SND_PCM_STREAM_CAPTURE
SND_PCM_NONBLOCK
SND_PCM_NO_AUTO_RESAMPLE
SND_PCM_NO_AUTO_CHANNELS
SND_PCM_NO_AUTO_FORMAT
```

禁止 ALSA plugin 静默改变 rate、channels 或 sample format。0.3b 不调用 `snd_pcm_wait`，避免阻塞单线程 Executor。

### 7.2 HW params

按以下顺序配置：

1. `snd_pcm_hw_params_any`；
2. `SND_PCM_ACCESS_RW_INTERLEAVED`；
3. SampleFormat 到 ALSA format 的精确映射；
4. 精确 channels；
5. 使用 exact rate API 配置采样率，不接受 `_near` 返回的替代 rate；
6. 使用 `period_size_hint` 协商可接受的实际 period；
7. 使用 `buffer_periods` 协商 buffer size，且实际 buffer 必须至少容纳两个实际 period；
8. 提交 HW params，并读回实际 format、rate、channels、period 和 buffer 逐项验证。

映射固定为：

| EAVP SampleFormat | ALSA format | container bytes |
|---|---|---:|
| `kSigned16LittleEndian` | `SND_PCM_FORMAT_S16_LE` | 2 |
| `kSigned24In32LittleEndian` | `SND_PCM_FORMAT_S24_LE` | 4 |
| `kSigned32LittleEndian` | `SND_PCM_FORMAT_S32_LE` | 4 |
| `kFloat32LittleEndian` | `SND_PCM_FORMAT_FLOAT_LE` | 4 |

### 7.3 SW params

启用 ALSA timestamp，优先请求 `SND_PCM_TSTAMP_TYPE_MONOTONIC`，使其与 V4L2 的 `CLOCK_MONOTONIC` 微秒域一致。`avail_min` 使用实际 period；其他阈值使用已验证的 ALSA capture 默认值。若设备或 plugin 不支持 monotonic timestamp，采集仍可启动，但必须启用明确的 `CLOCK_MONOTONIC` fallback 并增加指标。

## 8. 数据流、所有权与背压

```text
ALSA capture ring
  -> 非阻塞 snd_pcm_readi
  -> EAVP CPU accumulation Buffer
  -> AudioFrame
  -> OutputPort<AudioFrame>
  -> bounded Queue
```

AlsaSourceNode 不创建线程。每次 `tick()`：

1. 若存在 pending AudioFrame，先重试发送；下游仍背压时返回 `kWouldBlock`，不得读取 ALSA；
2. 若处于 suspend 恢复流程，先推进恢复；未恢复时返回 `kWouldBlock`；
3. 最多读取当前输出帧尚缺少的 PCM frames；
4. 正数短读写入 accumulation Buffer，并在下一 tick 继续；
5. 累积到 `samples_per_frame` 后构造只读共享 AudioFrame；
6. 发送成功后才开始下一帧；发送失败则保留 pending Frame。

ALSA ring 中的数据必须复制到 EAVP 拥有的 CPU Buffer。AudioFrame 离开当前 tick 后仍有效，不持有 ALSA ring 指针。一次 tick 不循环抽干设备，避免音频节点独占 Executor。

`snd_pcm_readi` 返回 0 或 `-EAGAIN` 时返回 `kWouldBlock`；短读是正常行为，不补零、不丢弃，也不提前输出不足长度的 AudioFrame。

## 9. PTS 与 discontinuity

### 9.1 公共时钟域

ALSA 与 V4L2 均输出：

```text
TimeBase(1, 1000000)
clock domain = CLOCK_MONOTONIC
```

AudioFrame PTS 不能使用 `CLOCK_REALTIME`，也不能使用进程启动相对时间。0.3b 只建立单设备连续时间线；与视频的比较在 0.3d 验收。

### 9.2 首帧锚点

第一次成功读取前，优先使用 `snd_pcm_htimestamp` 返回的 monotonic status timestamp 和 available PCM frame 数，换算最早未读采样点的时间：

```text
anchor_pts_us = status_timestamp_us
              - rescale(available_before_read,
                        TimeBase(1, sample_rate),
                        TimeBase(1, 1000000))
```

若 ALSA timestamp 不可用或不可验证为 monotonic，则使用 `clock_gettime(CLOCK_MONOTONIC)` 与 `snd_pcm_avail_update` 做同样估算，并记录 fallback。负 available、无效 timespec、换算溢出或明显超出当前 monotonic 时间均视为 timestamp 失败，不得静默生成错误 PTS。

### 9.3 后续帧

锚定后不逐帧复制可能抖动的 status timestamp。每个完整 AudioFrame 的 PTS 由累计输出采样数确定：

```text
frame_pts_us = anchor_pts_us
             + rescale(samples_emitted_before_frame,
                       TimeBase(1, sample_rate),
                       TimeBase(1, 1000000))
```

因此连续帧间隔只由采样数决定。48 kHz、480 samples/channel 时，相邻 PTS 正好相差 10000 us。

### 9.4 重新锚定

发生 XRUN 或 suspend 后，旧时间线不得继续外推。恢复时丢弃未完成 accumulation Buffer，清空旧锚点；恢复后的第一次成功读取重新锚定，其首个完整 AudioFrame 设置 `discontinuity=true`。之后恢复为 false，直到下一次中断。

## 10. 错误与恢复

所有 ALSA 失败均转换为 enriched Status：`provider_id="alsa"`、`operation` 为具体 API 名、`native_code` 保存原始负 ALSA error，消息由 `snd_strerror` 生成。异常不得越过公开 Node 边界。

| 条件 | 行为与 Status |
|---|---|
| 配置值非法 | factory 返回 `kInvalidArgument`，不调用 ALSA |
| 设备不支持 access/format/rate/channels/period/buffer | `kCapabilityMismatch` |
| 设备名不存在 | `kNotFound` |
| `-EAGAIN` 或零长度读取 | `kWouldBlock` |
| `-EINTR` | 在当前操作内重试，不改变状态 |
| `-EPIPE` XRUN | `snd_pcm_prepare` 后重新 `snd_pcm_start`；成功则丢弃 partial、重新锚定并标记 discontinuity，失败返回 enriched fatal Status |
| `-ESTRPIPE` suspend | 非阻塞 `snd_pcm_resume`；`-EAGAIN` 时后续 tick 重试；其他 resume 失败回退 `snd_pcm_prepare + snd_pcm_start`；恢复后重新锚定并标记 discontinuity |
| `-ENODEV`、`-ENXIO` 或已确认设备断开 | `kDeviceLost`，Node/Pipeline 进入 Error，不无限自动重连 |
| 内存分配失败 | `kResourceExhausted` |
| 其他 ALSA I/O 失败 | `kIoError` |

XRUN/suspend 恢复成功属于可观测的非致命中断，不把 Pipeline 留在 Error。恢复操作必须有界，不能在一次 tick 内等待设备恢复。

## 11. 生命周期

| Node 操作 | ALSA 行为 | 结果 |
|---|---|---|
| `prepare` | open、HW/SW params、`snd_pcm_prepare`、分配 accumulation Buffer | 成功进入 Prepared；任一步失败逆序关闭 |
| `start` | `snd_pcm_start`，清空历史锚点和计数窗口 | 成功进入 Running |
| `tick` | 推进 pending、恢复或一次非阻塞 read | 不阻塞 Executor |
| `stop` | `snd_pcm_drop`、丢弃 partial/pending、关闭 handle | 释放设备，重复 stop 幂等 |
| `reset` | 执行同等幂等清理并清零会话状态 | 回到 Created |
| 析构 | noexcept 等价清理 | 不泄漏 ALSA handle 或 Buffer |

`stop` 不输出不足 `samples_per_frame` 的尾帧。采集 Source 没有 drain 输入；停止即丢弃尚未形成完整时间契约的 partial 数据。

## 12. Metrics 与 Health

由于 MetricRegistry 暂无 labels，每个节点使用稳定前缀 `alsa_capture.<node_id>.`：

- counters：`frames`、`samples`、`bytes`、`short_reads`、`would_block`、`xruns`、`suspends`、`recoveries`、`timestamp_fallbacks`、`discontinuities`；
- gauges：`last_pts_us`、`actual_period_frames`、`actual_buffer_frames`、`partial_samples`。

Health component 使用 `alsa_capture/<node_id>`：

- 正常采集：`kOk`；
- timestamp fallback 或已成功恢复的 XRUN/suspend：`kDegraded`，消息包含累计次数与最近原因；
- 设备丢失、恢复失败或不可继续的 I/O：`kError`；
- 配置阶段能力不匹配直接返回 Status，不把尚未启动的组件伪装成运行健康。

若媒体操作与 Metrics/Health 发布同时失败，返回媒体操作失败；观测写入失败不得覆盖根因，但必须尽最大努力保留已有计数。

## 13. 构建、安装与平台边界

新增 CMake 选项：

```text
EAVP_ENABLE_ALSA=OFF
EAVP_ENABLE_ALSA_DEVICE_TESTS=OFF
```

- ALSA 默认关闭，避免 Core 和所有产品无条件依赖 libasound；开发机 Linux presets 在 0.3b 验证配置中显式开启；
- 开启 ALSA 时必须通过 `find_package(ALSA REQUIRED)` 使用系统 libasound，不下载依赖；
- 开启设备测试必须同时开启 ALSA，否则配置失败；
- 公开 EAVP 头不包含 ALSA 类型，但安装包开启 ALSA 时必须为 `EAVP::platform` 导出可解析的 ALSA 链接依赖；
- Rockchip ARMHF、HiSilicon v600 ARM32 和 aarch64 现有通用 preset 默认关闭 ALSA，只交叉验证 AudioFormat/AudioFrame 等平台无关 Core；
- 只有目标 sysroot 提供匹配的 libasound headers/library 后，厂商 preset 才能另行开启 ALSA；0.3b 不宣称三套 ARM 上的 ALSA target 已链接或设备已验证；
- FFmpeg、libavcodec、libavutil、libswresample、libopus 不进入 0.3b target。

本机设计基线为 libasound `1.2.11`。公共接口不得依赖该版本专有类型，运行行为只使用规格中列出的稳定 PCM API。

## 14. 测试策略

### 14.1 Core 单元测试

必须覆盖：

- 四种 SampleFormat 的 container bytes、mono/stereo frame bytes；
- unknown/非法 rate/layout/domain 和所有乘法溢出；
- AudioFrame memory domain、plane count、精确 buffer size、samples/time base 校验；
- PTS 是首采样点、duration 可推导、discontinuity 可读取；
- AudioFrame 不提供 DTS，MediaPacket 继续独立保存 PTS/DTS。

### 14.2 Fake AlsaSystem 单元测试

必须覆盖：

- 四种格式和 mono/stereo 的精确 ALSA 映射；
- exact rate 拒绝替代值，period hint 与固定输出帧解耦；
- 每个 prepare 步骤失败后的逆序资源释放；
- `-EINTR`、`-EAGAIN`、零读取和多次短读；
- pending Frame 背压时不再次 readi；
- 首帧 htimestamp/fallback 锚定、累计采样 PTS、溢出和无效 timestamp；
- `-EPIPE` prepare 恢复、`-ESTRPIPE` resume 重试与 prepare fallback；
- 恢复丢弃 partial、只在恢复后首帧设置 discontinuity；
- DeviceLost、stop/reset/析构幂等；
- Metrics、Health 和观测失败优先级；
- 公开 API 无异常越界。

### 14.3 确定性集成测试

使用 Fake AlsaSystem 组合真实 `AlsaSourceNode -> bounded Queue -> AudioChecksumSink`，脚本化产生不同短读边界下的 300 个 48 kHz、stereo、S16_LE、480 samples/channel AudioFrame。断言：

- Sink 正好接收 300 帧，无重复或不足帧；
- 每帧 1920 bytes，格式和 Buffer 生命周期正确；
- 相邻正常帧 PTS 正好相差 10000 us；
- 注入一次 XRUN 后只出现一个 discontinuity Frame，恢复后 PTS 重新锚定；
- 校验和、samples/bytes/Metrics 计数与测试向量一致；
- 最终 Health 按注入场景为 Ok 或 Degraded，stop 后资源全部释放。

该测试进入 Debug、Release 和 ASan/UBSan 普通 CTest。

### 14.4 可选真实设备验收

设备测试默认关闭，显式启用后直接消费现有外部 Loopback 数据源，不启动 FFmpeg、arecord 或其他 producer。配置来源：

- `EAVP_ALSA_DEVICE`，默认 `hw:Loopback,1,0`；
- `EAVP_ALSA_SAMPLE_FORMAT`，默认 `s16le`；
- `EAVP_ALSA_SAMPLE_RATE`，默认 `48000`；
- `EAVP_ALSA_CHANNELS`，默认 `2`；
- `EAVP_ALSA_SAMPLES_PER_FRAME`，默认 `480`；
- `EAVP_ALSA_FRAME_COUNT`，默认 `300`；
- `EAVP_ALSA_TIMEOUT_SECONDS`，默认 `10`。

测试必须先检查设备可打开并精确支持配置；在超时内采集指定帧数，验证格式、大小、PTS、帧数、Metrics 和 Health。静音是合法 PCM，不以非零 checksum 作为设备通过条件。设备存在但没有数据时返回明确的环境超时诊断，不冒充 DeviceLost 或测试崩溃。

## 15. 验收标准

0.3b 完成时必须同时满足：

1. 四种 interleaved 格式及 mono/stereo AudioFormat/AudioFrame 单元测试通过；
2. Fake System、300 帧确定性纵切面和 XRUN/suspend 恢复测试通过；
3. 现有 ALSA Loopback 节点在外部数据源下于超时内采集 300 帧；
4. 48 kHz、480 samples/channel 的连续帧 PTS 间隔为 10000 us；
5. Debug、Release、ASan/UBSan CTest 全部通过；
6. 开启 ALSA 的安装结果可由独立 `find_package(EAVP)` 消费并链接 `EAVP::platform`；
7. 三套 ARM preset clean build 平台无关 AudioFormat/AudioFrame，并保持 ALSA/设备测试关闭；
8. 0.3b 不链接 FFmpeg，不生成 AAC/Opus，不宣称跨设备 A/V 同步；
9. 文档以简体中文为主，无未解释的占位标记。

## 16. 后续里程碑边界

### 16.1 0.3c 开发机 FFmpeg 编码验证

0.3c 使用默认关闭、仅 build-tree 可见且不安装导出的 `EAVP::backend_ffmpeg`：

```text
V4L2 VideoFrame -> libx264/libx265 -> H.264/H.265 MediaPacket
ALSA AudioFrame -> frame aggregation / sample conversion
                -> FFmpeg AAC encoder/libopus -> AAC/Opus MediaPacket
```

0.3c 按独立规格扩展 AudioEncoder、audio capability/selection 和 MediaPacket 音频 stream format。必要的 libswresample 只能属于可选 FFmpeg Backend，不进入 Core 或 ARM 默认 preset。编码后 Packet 保留 PTS/DTS/duration；不得向 AudioFrame 添加 DTS。

### 16.2 0.3d 音视频同时采集

0.3d 组合 V4L2 与 ALSA Source，验证二者 PTS 均处于 `CLOCK_MONOTONIC` 微秒域，并发布启动偏差、运行偏差和漂移指标及阈值 Health。首期只测量和告警，不自动重采样、不丢音频、不丢视频，也不声称达到专业 genlock 精度。

## 17. 参考依据

- ALSA libasound PCM API：`https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m.html`
- ALSA PCM HW params：`https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m___h_w___params.html`
- ALSA PCM SW params：`https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m___s_w___params.html`
- FFmpeg AVFrame PTS：`https://www.ffmpeg.org/doxygen/trunk/structAVFrame.html`
- FFmpeg AVPacket PTS/DTS：`https://ffmpeg.org/doxygen/trunk/structAVPacket.html`
- 本机验证：libasound `1.2.11`；ALSA Loopback card 3 的 capture device 0/1 均提供 8 个 subdevices。
