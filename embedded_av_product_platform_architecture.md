# 嵌入式音视频产品底座架构设计

> 面向 Embedded Linux 音视频设备的软件平台化底座  
> 目标：以统一抽象、动态控制、产品交互、可观测性、可维护性和跨 SoC 能力为核心，支撑 IPC、NVR、编码器、直播终端、导播设备、会议终端、车载音视频、广播设备等产品快速构建。

> 文档状态：长期愿景，属于非规范性架构输入。  
> Core 0.1 的规范性边界见 `docs/superpowers/specs/2026-08-18-eavp-core-baseline-design.md`，文档索引见 `docs/architecture/README.md`。

---

## 1. 平台定位

本平台不是面向某一种固定设备的软件工程，也不是单纯的音视频 Pipeline 框架，而是一套完整的“嵌入式音视频产品底座”。

平台主要解决以下问题：

- 统一视频、音频、编码数据和内存对象模型。
- 统一采集、编解码、处理、封装、传输、存储等媒体模块。
- 统一跨 SoC、跨硬件平台的设备与能力抽象。
- 支持产品运行期间频繁的动态控制和参数修改。
- 支持 WebUI、APP、本地 UI、按键、CLI、云端等多控制入口。
- 支持配置事务、状态管理、资源管理、策略管理和自动恢复。
- 支持日志、Metrics、Trace、Alarm、Health、Crash Dump 等维护能力。
- 支持插件化、模块化、静态裁剪和动态扩展。
- 让具体产品开发从“重新开发大量音视频基础能力”转变为“基于平台进行能力组合和业务编排”。

平台最终目标：

> 产品代码只关心业务意图、产品配置和差异化能力，而采集、编解码、协议、状态管理、运维、资源协调、故障恢复等通用能力全部沉淀到平台。

---

## 2. 总体架构思想

平台采用四个平面：

1. Application Plane
2. Control Plane
3. Data Plane
4. Management Plane

总体架构：

```text
┌──────────────────────────────────────────────────────────────┐
│                         User / Cloud                         │
│                                                              │
│ WebUI │ APP │ LCD │ Button │ CLI │ REST │ MQTT │ SDK      │
└──────────────────────────────┬───────────────────────────────┘
                               │
                               ▼
┌──────────────────────────────────────────────────────────────┐
│                    APPLICATION PLANE                         │
│                                                              │
│ Camera │ Encoder │ NVR │ LiveBox │ Conference │ IPC ...    │
│                                                              │
│ Live │ Record │ Playback │ Preview │ Talk │ Snapshot │ AI  │
└──────────────────────────────┬───────────────────────────────┘
                               │
                               ▼
┌──────────────────────────────────────────────────────────────┐
│                      CONTROL PLANE                           │
│                                                              │
│ Command │ Query │ State │ Config │ Transaction │ Session    │
│ Resource │ Policy │ Permission │ API Gateway │ Reconciler  │
└──────────────────────────────┬───────────────────────────────┘
                               │
                               ▼
┌──────────────────────────────────────────────────────────────┐
│                        DATA PLANE                            │
│                                                              │
│ Buffer │ Frame │ Packet │ Clock │ Port │ Node │ Graph      │
│ Pipeline │ Scheduler │ Codec │ Protocol │ Device │ Storage │
└──────────────────────────────┬───────────────────────────────┘
                               │
                               ▼
┌──────────────────────────────────────────────────────────────┐
│                    DEVICE / BACKEND                          │
│                                                              │
│ V4L2 │ ALSA │ RKMPP │ HiSilicon │ FFmpeg │ HDMI │ ISP     │
│ GPU │ NPU │ DMA │ SRT │ RTSP │ RTMP │ WebRTC ...         │
└──────────────────────────────────────────────────────────────┘


┌──────────────────────────────────────────────────────────────┐
│                    MANAGEMENT PLANE                          │
│                                                              │
│ Metrics │ Logs │ Trace │ Alarm │ Health │ Audit │ Dump     │
│ Diagnostic │ Watchdog │ Upgrade │ Flight Recorder          │
│                                                              │
│              覆盖整个 Application / Control / Data           │
└──────────────────────────────────────────────────────────────┘
```

核心原则：

> Media Pipeline 是执行层，而不是产品控制中心。

真正的产品运行逻辑应由“用户意图 + 系统状态 + 控制事务 + 资源策略”驱动。

---

# 3. Application Plane

Application Plane 面向具体产品和业务能力。

典型产品包括：

- IPC 摄像机
- NVR
- HDMI 编码器
- SRT/RTMP 直播终端
- 采集盒
- 导播设备
- 视频会议终端
- 对讲设备
- AI Camera
- 广播编码设备
- 车载音视频终端

产品层原则：

```text
Product
   ↓
Service
   ↓
Control Plane
   ↓
Media Runtime
```

产品代码不能直接依赖：

```text
FFmpeg
RKMPP
HiSilicon MPI
V4L2
SRT Library
```

产品主要关注：

- 产品型号。
- 产品能力组合。
- UI。
- 业务规则。
- 用户权限。
- 产品流程。
- 硬件差异。
- 出厂配置。

---

# 4. Service 模型

Service 是面向产品的一级能力，而不是具体媒体 Node。

建议提供：

```text
LiveService
RecordService
PlaybackService
PreviewService
SnapshotService
TalkService
AIService

NetworkService
StorageService
SystemService
UpgradeService
MaintenanceService
```

例如：

```cpp
class LiveService {
public:
    Result start(ChannelId channel);
    Result stop(ChannelId channel);

    Result set_config(
        ChannelId channel,
        const LiveConfig& config);

    LiveStatus status(ChannelId channel);
};
```

LiveService 内部可能建立：

```text
Camera
   ↓
H265Encoder
   ↓
MpegTsMux
   ↓
SRTOutput
```

调用者不需要知道内部媒体拓扑。

---

# 5. Control Plane

Control Plane 是整个产品动态运行能力的核心。

包含：

```text
CommandBus
QueryBus
StateStore
StateReconciler
ConfigManager
ConfigTransaction
SessionManager
ResourceManager
PolicyEngine
PermissionManager
API Gateway
```

核心原则：

> 外部只表达“意图”，不能直接控制平台内部对象。

错误示例：

```text
WebUI
  ↓
encoder->setBitrate()

Cloud
  ↓
pipeline->start()

APP
  ↓
srt->reconnect()
```

正确方式：

```text
用户意图
   ↓
Command
   ↓
Control Plane
   ↓
Desired State
   ↓
Reconcile
   ↓
Service
   ↓
Media Runtime
```

---

# 6. Command 模型

所有修改类操作统一抽象为 Command。

```cpp
struct Command {
    CommandId id;

    std::string source;
    std::string target;

    CommandType type;

    Value payload;

    uint64_t timestamp;
};
```

典型 Command：

```text
StartLive
StopLive
StartRecord
StopRecord

SetVideoBitrate
SetResolution
SetCodec
SetVolume

SwitchInput
EnableAI
DisableAI

Reboot
Upgrade
FactoryReset
```

Command 来源可以是：

```text
REST
WebUI
APP
CLI
Cloud
MQTT
GPIO
LCD
定时任务
自动恢复模块
```

最终统一进入：

```text
CommandBus
```

---

# 7. Query 模型

查询与写操作分离。

写：

```text
Command
```

读：

```text
Query
```

例如：

```text
GET /api/v1/channels/0/status
```

应从：

```text
QueryBus
   ↓
StateStore
```

读取产品状态，而不是直接查询 Encoder 或 SRT Socket。

典型返回：

```json
{
  "channel": 0,
  "state": "running",

  "video": {
    "codec": "h265",
    "width": 3840,
    "height": 2160,
    "fps": 25,
    "bitrate": 7982345
  },

  "srt": {
    "state": "connected",
    "rtt": 67,
    "loss": 0.13
  },

  "record": {
    "state": "running",
    "duration": 2834
  }
}
```

---

# 8. Desired State / Actual State

动态产品必须明确区分：

```text
Desired State
```

和：

```text
Actual State
```

例如用户要求：

```yaml
channel0:
  enabled: true

  video:
    codec: h265
    resolution: 3840x2160
    bitrate: 8000000

  outputs:
    srt:
      enabled: true

    record:
      enabled: true
```

这是期望状态。

当前运行状态可能是：

```text
Camera       RUNNING
Encoder      RUNNING
SRT          RECONNECTING
Record       RUNNING
```

StateReconciler 负责：

```text
Actual State
     ↓
逐渐逼近
     ↓
Desired State
```

例如：

```text
Desired:
SRT = ENABLED

Actual:
SRT = ERROR

Reconciler:
   ↓
Reconnect
   ↓
RUNNING
```

---

# 9. StateStore

StateStore 是产品运行时的统一状态中心。

建议采用树形路径：

```text
/system

/device

/media

/channel
/network

/storage

/service

/alarm

/health
```

例如：

```text
/channel/0/live/state

/channel/0/video/fps
/channel/0/video/bitrate

/channel/0/audio/sample_rate

/network/eth0/state
/network/eth0/ip

/storage/sda/free

/system/cpu/load
/system/memory/free
```

模块更新：

```cpp
state_store.set(
    "/channel/0/video/fps",
    25);
```

WebUI 可以订阅：

```text
/channel/0/*
```

通过 WebSocket 自动推送状态变化。

---

# 10. Configuration / State / Metrics 分离

必须区分：

```text
Configuration
Desired State
Runtime State
Capability
Statistics / Metrics
```

例如：

```text
config.video.bitrate
```

表示用户配置：

```text
8 Mbps
```

```text
state.video.bitrate
```

表示硬件当前实际配置成功：

```text
8 Mbps
```

```text
metrics.video.real_bitrate
```

表示实时输出码率：

```text
7.83 Mbps
```

三者不能混在一起。

---

# 11. 配置事务

多项配置修改必须支持事务。

例如用户同时修改：

```text
codec      H265
width      3840
height     2160
fps        25
bitrate    10Mbps
```

推荐流程：

```text
BEGIN

SET codec
SET width
SET height
SET fps
SET bitrate

VALIDATE

RESOURCE CHECK

APPLY

COMMIT
```

失败：

```text
ROLLBACK
```

接口示例：

```cpp
auto tx = config.begin();

tx.set("video.width", 3840);
tx.set("video.height", 2160);
tx.set("video.codec", "h265");

Result result = tx.commit();
```

---

# 12. 动态配置生效级别

所有属性应该定义动态修改能力：

```text
LIVE

NODE_RESTART

PIPELINE_RESTART

SERVICE_RESTART

SYSTEM_REBOOT
```

例如：

```text
bitrate
→ LIVE

volume
→ LIVE

resolution
→ NODE_RESTART

codec
→ PIPELINE_RESTART

network static IP
→ SERVICE_RESTART

system partition
→ SYSTEM_REBOOT
```

这样 UI 和 Cloud 都能预知修改影响。

---

# 13. Session 模型

Session 是高于 Pipeline 的运行实体。

建议：

```text
LiveSession

RecordSession

PlaybackSession

PreviewSession

TalkSession
```

Session 生命周期：

```text
CREATED

PREPARING

READY

RUNNING

PAUSED

STOPPING

STOPPED

ERROR
```

一个 Session 可以管理多个 Pipeline：

```text
LiveSession
   │
   ├── VideoPipeline
   ├── AudioPipeline
   └── NetworkPipeline
```

---

# 14. ResourceManager

音视频 SoC 的资源都是有限的。

典型资源：

```text
VENC

VDEC

Scaler

GPU

NPU

DMA

Memory

DDR Bandwidth

CPU

Network Bandwidth
```

ResourceManager 负责：

```text
申请
分配
冲突检测
预留
统计
释放
```

例如：

```text
LiveService
   ↓
request H265 Encoder
   ↓
ResourceManager
   ↓
Encoder Channel #2
```

资源不足：

```text
RESOURCE_EXHAUSTED
```

而不是一直调用到底层 SoC SDK 后才失败。

---

# 15. Policy Engine

Policy Engine 用于处理产品动态策略。

例如：

```text
if 4K60 && AI enabled:
    AI fps = 5

if temperature > 90°C:
    encoder bitrate ↓

if network bandwidth < 4Mbps:
    bitrate ↓

if storage free < 5%:
    stop record
```

策略可以包括：

```text
Network Policy

ABR Policy

Thermal Policy

Power Policy

Storage Policy

Resource Policy

Recovery Policy
```

---

# 16. Data Plane

Data Plane 负责真正的音视频处理。

核心对象：

```text
Buffer
MediaBuffer
VideoFrame
AudioFrame
MediaPacket

MediaFormat
MediaClock

Port
Queue

MediaNode

MediaGraph
Pipeline

Scheduler
```

Data Plane 的目标：

```text
高性能

低延迟

少内存分配

少锁

少拷贝

零拷贝

明确的 Backpressure

稳定的 Clock / Timestamp
```

---

# 17. 统一 Buffer

建议：

```cpp
class Buffer {
public:
    virtual void* data() = 0;

    virtual size_t size() const = 0;
    virtual size_t capacity() const = 0;

    virtual MemoryType memory_type() const = 0;

    virtual int fd() const { return -1; }
};
```

MemoryType：

```cpp
enum class MemoryType {
    CPU,
    MMAP,
    DMA,
    DMABUF,
    GPU,
    NPU,
    HW_NATIVE
};
```

支持统一描述：

```text
malloc memory

V4L2 MMAP

DMABUF

Rockchip MB

HiSilicon VB

GPU Memory

NPU Memory
```

---

# 18. Frame / Packet

视频：

```cpp
struct VideoFrame {
    MediaBuffer buffer;

    PixelFormat format;

    int width;
    int height;

    int stride[4];

    int64_t pts;
    int64_t dts;

    TimeBase time_base;

    FrameFlags flags;
};
```

音频：

```cpp
struct AudioFrame {
    MediaBuffer buffer;

    SampleFormat format;

    int sample_rate;
    int channels;

    int samples;

    int64_t pts;

    TimeBase time_base;
};
```

编码数据：

```cpp
struct MediaPacket {
    MediaBuffer buffer;

    CodecId codec;

    int stream_index;

    int64_t pts;
    int64_t dts;
    int64_t duration;

    PacketFlags flags;
};
```

---

# 19. MediaNode / Pipeline

所有媒体组件统一为 Node：

```text
SourceNode

ProcessorNode

SinkNode
```

例如：

```text
V4L2CaptureNode
ALSACaptureNode

H264EncoderNode
H265EncoderNode

ScalerNode
OSDNode
AudioResampleNode

MpegTsMuxNode
Mp4MuxNode

SRTOutputNode
RTSPOutputNode
RTMPOutputNode

FileSinkNode
DisplaySinkNode
```

Pipeline 示例：

```text
Camera
   ↓
H265 Encoder
   ↓
MPEGTS Mux
   ↓
SRT
```

一个输入多输出：

```text
                 ┌→ SRT
Camera → Encoder ┼→ RTSP
                 ├→ Record
                 └→ Preview
```

---

# 20. Node / Port

Node 不能直接彼此调用。

统一通过：

```text
Port
+
Queue
+
Buffer
```

连接。

```cpp
class MediaPort {
public:
    MediaType type();

    MediaFormat format();

    int connect(MediaPort* peer);

    int disconnect();
};
```

---

# 21. Backpressure

队列必须统一定义容量和阻塞策略。

```cpp
struct QueuePolicy {
    size_t max_frames;

    OverflowPolicy overflow;
};
```

支持：

```text
BLOCK

DROP_OLDEST

DROP_NEWEST

DROP_NON_KEY

FLUSH_TO_KEYFRAME
```

直播场景：

```text
网络拥塞
   ↓
丢弃过时 P/B
   ↓
等待 IDR
   ↓
快速恢复实时性
```

录像场景可能采用：

```text
BLOCK
```

---

# 22. Clock / 时间戳

平台统一管理：

```text
MediaClock

TimeBase

Timestamp

ClockDomain

Synchronizer
```

支持：

```text
MONOTONIC

REALTIME

AUDIO CLOCK

VIDEO CLOCK

PCR

RTP

NTP

PTP

EXTERNAL CLOCK
```

为后续支持：

```text
AV Sync

PCR Precision

Multi-camera Sync

PTP

SMPTE ST 2110
```

打基础。

---

# 23. Device / HAL / Backend

禁止直接把某家 SoC SDK 封成一个巨型 HAL。

采用：

```text
Abstract Device
      ↓
Provider / Backend
```

例如：

```text
VideoEncoder
   │
   ├── RKMPPBackend
   ├── HiSiliconBackend
   ├── V4L2M2MBackend
   └── FFmpegBackend
```

上层：

```cpp
auto encoder =
    DeviceManager::create<VideoEncoder>("encoder0");
```

不感知底层实现。

---

# 24. Capability

不同硬件能力必须显式描述。

例如：

```cpp
struct VideoEncoderCapability {
    std::vector<CodecId> codecs;

    Range width;
    Range height;

    Range bitrate;

    bool support_roi;
    bool support_bframe;
    bool support_low_delay;

    int max_channels;
};
```

Capability 同时用于：

```text
WebUI 参数限制

配置校验

资源管理

产品能力展示

API 校验

动态策略
```

---

# 25. Plugin

所有可选能力统一插件化：

```text
codec

protocol

device

filter

storage

AI

service
```

目录：

```text
plugins/
├── codec/
│   ├── ffmpeg/
│   ├── rkmpp/
│   └── hisi/
│
├── protocol/
│   ├── srt/
│   ├── rtmp/
│   ├── rtsp/
│   ├── webrtc/
│   └── gb28181/
│
├── device/
│   ├── v4l2/
│   ├── alsa/
│   └── hdmi/
│
├── ai/
│   ├── rknn/
│   └── tensorrt/
│
└── storage/
```

支持：

```text
STATIC PLUGIN

DYNAMIC PLUGIN
```

嵌入式设备优先支持静态裁剪。

---

# 26. Management Plane

Management Plane 负责整个产品的可维护性和可观测性。

包含：

```text
Logging

Metrics

Trace

Alarm

Health

Audit

Diagnostic

Crash Dump

Watchdog

Upgrade

Flight Recorder
```

---

# 27. Logging

建议统一日志级别：

```text
TRACE

DEBUG

INFO

WARN

ERROR

FATAL
```

日志上下文应包含：

```text
timestamp

module

thread

session

pipeline

device
```

例如：

```text
2026-08-18 10:22:31.213
INFO
[srt]
[pipeline=live0]
[session=23]
connected remote=xxx
```

必须支持运行时调整：

```bash
avctl log set srt debug
```

必要时支持：

```bash
avctl log set pipeline.live0 trace
```

并允许自动过期恢复。

---

# 28. Metrics

基础指标建议至少包括：

```text
video.capture.fps

video.encode.input_fps
video.encode.output_fps
video.encode.bitrate

audio.capture.samples

pipeline.queue.depth

buffer.pool.used

network.tx.bitrate

network.rtt

network.loss

memory.rss

cpu.usage

temperature

av.sync.diff
```

现场查看：

```text
camera
  fps          25

encoder
  input        25
  output       25
  bitrate      4.02Mbps

srt
  send         4.12Mbps
  rtt          83ms
  loss         0.3%

queue
  depth        2/8
```

---

# 29. Frame Trace

建议为媒体数据增加 Trace ID。

```text
Frame #105832

Capture
10:22:15.123

ISP
+2.1ms

Encoder IN
+3.3ms

Encoder OUT
+12.8ms

Mux
+0.8ms

SRT Send
+1.5ms
```

总延迟：

```text
20.5 ms
```

正式版本可以采用采样策略：

```text
1 / 1000 frames
```

诊断模式提高采样率。

---

# 30. Alarm

Alarm 是结构化故障，而不是普通日志。

```cpp
struct Alarm {
    AlarmId id;

    Severity severity;

    std::string source;

    int code;

    std::string message;

    uint64_t timestamp;

    RecoveryState recovery;
};
```

典型：

```text
CAMERA_SIGNAL_LOST

VIDEO_ENCODER_TIMEOUT

SRT_DISCONNECTED

DISK_FULL

TEMPERATURE_HIGH

MEMORY_PRESSURE
```

级别：

```text
INFO

WARNING

MINOR

MAJOR

CRITICAL
```

---

# 31. Health

HealthManager 统一聚合系统健康状态。

```text
SYSTEM HEALTH

OK

DEGRADED

ERROR

CRITICAL
```

例如：

```text
System
 ├─ CPU           OK
 ├─ Memory        OK
 ├─ Temperature   WARNING
 ├─ Video         OK
 ├─ Audio         OK
 ├─ Network       DEGRADED
 ├─ Storage       OK
 └─ SRT           ERROR
```

面向用户：

```text
直播网络异常
```

面向运维：

```text
SRT reconnect #37

RTT 182 ms

loss 7.3%

socket errno=...
```

---

# 32. Runtime Inspector

建议建立类似 `/proc` 的运行时对象树：

```text
/runtime

├── pipelines
│   └── live0
│       ├── node0
│       ├── node1
│       └── node2
│
├── threads
├── buffers
├── memory
├── devices
├── sessions
└── sockets
```

例如：

```bash
avctl inspect /runtime/pipelines/live0
```

---

# 33. CLI / avctl

平台应从第一版就提供维护 CLI：

```text
avctl status

avctl pipeline

avctl node

avctl metrics

avctl buffers

avctl threads

avctl network

avctl config

avctl log

avctl dump

avctl health

avctl inspect
```

例如：

```bash
avctl pipeline show live0
```

输出：

```text
Pipeline: live0

HDMI0
 |
 | NV12
 | 3840x2160@25
 |
 +--> H265_ENCODER
       input fps: 25.00
       output fps: 25.00
       bitrate: 7.96Mbps
       queue: 1/8
       |
       +--> MPEGTS
             |
             +--> SRT
                   state: CONNECTED
                   rtt: 72ms
                   loss: 0.13%
```

---

# 34. Crash Dump

CrashManager 应统一处理：

```text
SIGSEGV

SIGABRT

SIGBUS

SIGFPE
```

Crash Report 包含：

```text
Firmware Version

Build ID

Git Commit

Uptime

Signal

Backtrace

Loaded Modules

Thread List

Memory Usage

Recent Logs

Recent Alarms

Pipeline State

Device State
```

输出：

```text
crash-20260818-102312.tar.gz
```

---

# 35. Flight Recorder

对于难复现的现场问题，应建立 Flight Recorder。

采用环形内存/文件记录最近一段时间：

```text
关键日志

状态变化

网络统计

队列深度

CPU

Memory

Temperature

Pipeline Events

Alarm
```

例如：

```text
T - 60 秒
   ↓
故障发生
   ↓
T + 10 秒
```

故障触发时冻结上下文。

特别适合排查：

```text
偶发卡顿

直播断流

编码器超时

网络抖动

CPU 异常

内存压力
```

---

# 36. Audit

所有重要用户操作都应该记录 Audit。

例如：

```text
10:20:03

user=admin

source=WebUI

action=SetBitrate

channel=0

old=4Mbps

new=8Mbps
```

Audit 可用于确认：

```text
设备自动异常

VS

用户修改配置
```

---

# 37. 权限模型

建议至少提供：

```text
Viewer

Operator

Administrator

Maintenance
```

例如：

```text
Viewer
→ 查看

Operator
→ 启停直播

Administrator
→ 修改系统配置

Maintenance
→ 调试、Dump、诊断
```

---

# 38. Upgrade

UpgradeManager 支持：

```text
IDLE

DOWNLOAD

VERIFY

PREPARE

INSTALL

REBOOT_REQUIRED

ROLLBACK

SUCCESS

FAILED
```

建议包含：

```text
Firmware Signature

Version Check

A/B Upgrade

Rollback

Boot Health Check

Upgrade Progress

Upgrade Log
```

---

# 39. Config Schema Migration

配置必须有版本：

```text
schema_version = 3
```

升级：

```text
v1

 ↓ migrate

v2

 ↓ migrate

v3
```

保证长期产品升级。

---

# 40. Runtime

平台底层 Runtime 建议统一提供：

```text
Thread

Mutex

Condition

Event

Task

Executor

Timer

EventLoop

Reactor

Worker

MessageQueue

ThreadPool
```

网络统一基于：

```text
epoll
  ↓
Reactor
  ↓
SocketChannel
```

避免：

```text
RTSP 建线程

RTMP 建线程

SRT 建线程

WebSocket 建线程
```

造成线程失控。

---

# 41. Lite / Service 两种部署模式

## 41.1 Lite Mode

适用于：

```text
IPC

低成本编码器

小型直播终端
```

结构：

```text
Application

Service

Control

Media Runtime

Backend
```

全部运行于一个进程。

## 41.2 Service Mode

适用于：

```text
NVR

大型编码设备

导播

广播设备
```

结构：

```text
Application
     │
     │ IPC
     ↓

MediaDaemon

DeviceDaemon

NetworkDaemon

ManagementDaemon
```

上层 Service API 保持一致。

---

# 42. IPC

IPC 接口应抽象。

例如：

```cpp
class IMediaService {
public:
    virtual Result start(...) = 0;
    virtual Result stop(...) = 0;
};
```

Lite：

```text
LocalMediaService
```

Service Mode：

```text
RemoteMediaService
   ↓
RPC
   ↓
MediaDaemon
```

业务逻辑无需改变。

---

# 43. 推荐源码结构

```text
eavp/
│
├── cmake/
├── configs/
├── docs/
├── examples/
├── scripts/
├── tests/
├── tools/
│
├── base/
│   ├── memory/
│   ├── thread/
│   ├── task/
│   ├── timer/
│   ├── event/
│   ├── logging/
│   └── utility/
│
├── runtime/
│   ├── executor/
│   ├── reactor/
│   ├── timer/
│   └── worker/
│
├── media/
│   ├── buffer/
│   ├── frame/
│   ├── packet/
│   ├── format/
│   ├── clock/
│   ├── queue/
│   ├── port/
│   ├── node/
│   ├── graph/
│   └── pipeline/
│
├── device/
│   ├── manager/
│   ├── video/
│   ├── audio/
│   ├── display/
│   └── capability/
│
├── codec/
├── filter/
├── container/
├── protocol/
├── network/
├── storage/
├── ai/
│
├── control/
│   ├── command/
│   ├── query/
│   ├── state/
│   ├── transaction/
│   ├── session/
│   ├── resource/
│   ├── policy/
│   └── permission/
│
├── services/
│   ├── live/
│   ├── record/
│   ├── playback/
│   ├── preview/
│   ├── snapshot/
│   ├── talk/
│   ├── network/
│   ├── storage/
│   └── system/
│
├── management/
│   ├── config/
│   ├── logging/
│   ├── metrics/
│   ├── tracing/
│   ├── alarm/
│   ├── health/
│   ├── audit/
│   ├── diagnostic/
│   ├── crash/
│   ├── watchdog/
│   ├── upgrade/
│   └── flight_recorder/
│
├── interfaces/
│   ├── rest/
│   ├── websocket/
│   ├── mqtt/
│   ├── cli/
│   ├── rpc/
│   └── local_ui/
│
├── plugin/
│
├── plugins/
│   ├── codec/
│   ├── protocol/
│   ├── device/
│   ├── ai/
│   └── storage/
│
├── products/
│   ├── ip_camera/
│   ├── srt_encoder/
│   ├── live_box/
│   └── nvr/
│
└── third_party/
```

---

# 44. API / ABI

建议采用：

```text
C++11 Application API

+

C Plugin ABI
```

C++ 用于平台内部：

```text
RAII

shared_ptr

unique_ptr

vector

string

template
```

插件边界使用稳定 C ABI：

```c
typedef struct av_node av_node_t;

typedef struct {
    uint32_t abi_version;

    int (*open)(av_node_t *);
    int (*start)(av_node_t *);
    int (*stop)(av_node_t *);
    int (*close)(av_node_t *);

} av_node_ops_t;
```

避免不同编译器和不同 C++ ABI 造成插件兼容问题。

---

# 45. Build System

推荐：

```text
CMake

+

Kconfig
```

例如：

```text
Device Drivers
  [*] V4L2
  [*] ALSA

Hardware Codec
  [*] Rockchip MPP
  [ ] HiSilicon MPI

Protocols
  [*] RTSP
  [*] RTMP
  [*] SRT
  [ ] WebRTC
  [ ] GB28181

Containers
  [*] MPEGTS
  [*] MP4

Features
  [*] Recording
  [*] Snapshot
  [ ] AI

Management
  [*] Metrics
  [*] Alarm
  [*] Crash Dump
  [*] Flight Recorder
```

生成：

```c
#define CONFIG_SRT 1
#define CONFIG_RTSP 1
#define CONFIG_WEBRTC 0
```

---

# 46. SDK 分层

最终建议对外发布三个 SDK。

## Base SDK

```text
Memory

Thread

Task

Timer

Event

Socket

Config

Logging

Metrics

Plugin

IPC
```

## Media SDK

```text
Buffer

Frame

Packet

Clock

Node

Graph

Pipeline

Codec

Mux

Protocol

Device
```

## Application SDK

```text
Live

Record

Playback

Preview

Snapshot

Talk

AI

Device Management

System Management
```

---

# 47. 动态修改示例

用户从 WebUI 将：

```text
H265 bitrate

4Mbps
  ↓
8Mbps
```

完整流程：

```text
WebUI
  │
  │ PUT /channel/0/video/bitrate
  ▼
API Gateway
  │
  ▼
Permission
  │
  ▼
CommandBus
  │
  ▼
ConfigTransaction
  │
  ├─ Capability Validate
  │
  ├─ Resource Validate
  │
  ▼
LiveService
  │
  ▼
Desired State = 8Mbps
  │
  ▼
MediaController
  │
  ▼
EncoderNode
  │
  ▼
RKMPP / HiSilicon
  │
  ▼
SUCCESS
  │
  ▼
Actual State = 8Mbps
  │
  ├────→ Audit
  ├────→ Event
  ├────→ Metrics
  └────→ WebSocket → WebUI
```

失败：

```text
Encoder Reject
   ↓
Rollback
   ↓
Restore Desired State
   ↓
Alarm
   ↓
WebUI Failure
```

---

# 48. 核心抽象

## 48.1 Media Core

```text
Buffer

Frame

Packet

MediaFormat

Clock

Port

Node

Graph

Pipeline
```

## 48.2 Control Core

```text
Command

Query

State

Transaction

Session

Resource

Policy
```

## 48.3 Management Core

```text
Config

Metric

Trace

Alarm

Health

Audit

Diagnostic
```

## 48.4 Product Core

```text
Service

Capability

API

Plugin

ProductProfile
```

其中：

> Media Core 决定性能。  
> Control Core 决定产品动态能力。  
> Management Core 决定产品是否真正可维护。  
> Product Core 决定平台是否能够快速构建不同产品。

---

# 49. 核心依赖方向

平台必须严格控制依赖方向：

```text
Product
   ↓
Service
   ↓
Control Plane
   ↓
Media Framework
   ↓
Abstract Device / Codec / Protocol
   ↓
Plugin Interface
   ↓
Concrete Backend
   ↓
Linux / SoC SDK
```

禁止：

```text
Product → RKMPP

Product → HiSilicon MPI

Service → FFmpeg

Media Framework → 某个 SoC SDK

HAL → Service

Protocol → Product
```

基本原则：

> 越往下越通用，越往上越业务化。

---

# 50. 产品配置示例

例如 SRT 编码器：

```yaml
product:
  name: srt_encoder

features:

  video:
    hdmi_input: true

    codec:
      h264: true
      h265: true

  audio:
    alsa: true
    aac: true

  output:
    srt: true
    rtmp: true
    rtsp: true

  record:
    enabled: true

  ai:
    enabled: false

management:

  metrics: true

  alarm: true

  crash_dump: true

  flight_recorder: true
```

产品最终只需要：

```text
能力配置

业务状态机

UI

产品差异

出厂参数
```

---

# 51. 典型产品代码

例如：

```text
HDMI → H265 → SRT
```

产品代码可以简化为：

```cpp
int main()
{
    Platform platform;

    platform.initialize();

    auto live =
        platform.services().live();

    LiveConfig config;

    config.video.source = "hdmi0";
    config.video.codec = CodecId::H265;

    config.output.url =
        "srt://server:9000?streamid=test";

    auto session =
        live->create(config);

    session->start();

    platform.run();

    return 0;
}
```

内部：

```text
Product
   ↓
LiveService
   ↓
Control Plane
   ↓
PipelineManager
   ↓
MediaGraph
   ↓

HDMI
 ↓
H265 Encoder
 ↓
MPEGTS
 ↓
SRT
```

---

# 52. 推荐开发阶段

建议不要一开始就开发大量协议。

## Phase 01：Base

```text
Error

Memory

Thread

Task

Timer

Event

Logging
```

## Phase 02：Media Object

```text
Buffer

MediaFormat

VideoFrame

AudioFrame

MediaPacket

Clock
```

## Phase 03：Media Runtime

```text
Port

Queue

Node

Graph

Pipeline

Scheduler
```

## Phase 04：Control Core

```text
CommandBus

QueryBus

StateStore

ConfigTransaction

SessionManager
```

## Phase 05：Device / Plugin

```text
DeviceManager

Capability

PluginManager

Provider / Backend
```

## Phase 06：首个真实 Pipeline

```text
V4L2
 ↓
H264
 ↓
MPEGTS
 ↓
File
```

## Phase 07：基础协议

```text
ALSA

SRT

RTSP

RTMP

MP4
```

## Phase 08：Service

```text
LiveService

RecordService

PreviewService
```

## Phase 09：Management

```text
Metrics

Alarm

Health

Crash Dump

Flight Recorder

avctl
```

## Phase 10：跨 SoC

```text
RKMPP

HiSilicon

Ambarella

V4L2 M2M
```

## Phase 11：产品

```text
IPC

SRT Encoder

LiveBox

NVR
```

---

# 53. 架构结论

一个真正面向商业产品的嵌入式音视频平台，不应该只是：

```text
Camera → Encoder → Protocol
```

更完整的结构应该是：

```text
               User / Cloud
                    ↓
             Product / Service
                    ↓
               Control Plane
                    ↓
              Desired State
                    ↓
                Reconcile
                    ↓
                Data Plane
                    ↓
               Device / SoC

Management Plane
    ↑
    └── Logging / Metrics / Trace / Alarm / Health /
        Audit / Crash / Diagnostic / Upgrade
```

平台应坚持以下设计原则：

1. 外部只表达用户意图，不直接操作媒体对象。
2. 产品状态与 Pipeline 状态分离。
3. Configuration、Desired State、Runtime State、Metrics 分离。
4. 所有重要配置修改支持事务和回滚。
5. 所有硬件能力显式 Capability 化。
6. 所有有限资源统一由 ResourceManager 管理。
7. 数据面与控制面严格分离。
8. 媒体框架不依赖具体 SoC 和具体协议实现。
9. 可观测性和维护能力从平台第一版开始建设。
10. 支持静态裁剪和动态插件两种模式。
11. 支持单进程 Lite Mode 和多进程 Service Mode。
12. 平台接口优先保持长期稳定，具体 Backend 可以持续替换。

最终希望达到：

> 同一套平台可以长期支撑不同 SoC、不同协议、不同产品形态；面对新产品时，主要工作从“重复开发基础能力”变成“产品业务编排 + 能力配置 + 少量差异化开发”。

---

# 54. 下一步建议

在完成总体架构之后，建议优先正式定义 `EAVP Core 1.0` 的接口边界。

建议第一批冻结接口：

```text
Buffer

MediaFormat

VideoFrame

AudioFrame

MediaPacket

MediaClock

MediaPort

MediaNode

MediaGraph

MediaPipeline

Command

State

ConfigTransaction

Session

ResourceManager

Service

Capability

Plugin
```

下一阶段重点不是增加更多协议，而是把这些核心接口的：

```text
类关系

所有权

生命周期

线程模型

调用模型

状态机

错误码

事件模型

ABI

配置 Schema
```

设计稳定。

当这些基础稳定后，V4L2、ALSA、FFmpeg、SRT、RTSP、RKMPP、HiSilicon MPI 等都可以逐步作为标准模块接入，而不会反向侵蚀平台核心设计。
