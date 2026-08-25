# EAVP

EAVP（Embedded Audio/Video Platform）是面向 Embedded Linux 音视频设备的通用产品底座。稳定安装包版本仍为 `0.2.0`；仓库同时包含已验收的 `0.3b` Linux ALSA 音频采集开发能力，它不是完整产品功能。

## 当前能力

- C++11 的错误、媒体对象和时间基准模型。
- CPU Buffer 的显式 Plane 映射、VideoFormat、VideoFrame、媒体 Packet、类型化 Port、有界 Queue、Node/Graph/Pipeline。
- Provider Capability、冻结后的确定性 BackendRegistry，以及仅用于测试和集成验证的 Reference Backend。
- Command/Query、Desired/Actual StateStore 与幂等 Reconciler。
- Counter、Gauge、Health，以及单进程模拟媒体纵切面。
- `0.3b` 的 Linux ALSA 非阻塞 PCM Capture：CPU `AudioFormat`/`AudioFrame`、首采样点 PTS、XRUN/suspend discontinuity、Metrics/Health，以及显式 Loopback 设备验收。
- Linux x86_64 原生构建，以及通用 aarch64、Rockchip ARM32 和海思 v600 ARM32 的交叉编译验证配置。

Reference Backend 产生的是内部确定性 `kReference` payload，不是标准 H.264/H.265 码流。`0.3b` 仅增加 Linux ALSA Capture 开发能力；V4L2、FFmpeg、硬件编解码、容器和网络协议、RPC、Web API 与 Service Mode 仍不属于稳定 `0.2.0` 包。

## 快速开始

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

默认优先查找系统安装的 GoogleTest。未安装时可使用允许下载测试依赖的预设：

```bash
cmake --preset linux-debug-fetch-deps
cmake --build --preset linux-debug-fetch-deps
ctest --preset linux-debug-fetch-deps
```

Release 和 Sanitizer 环境没有系统 GoogleTest 时，分别使用 `linux-release-fetch-deps` 和 `linux-asan-fetch-deps`。

安装和消费示例见 `docs/architecture/build-and-portability.md`。

ARM 交叉构建仅验证 Core（含平台无关的 AudioFormat/AudioFrame）和 Reference Backend 可被对应编译器生成和链接，且 ARM 预设关闭 ALSA；不代表 ARM libasound 链接、ALSA 设备验证、Rockchip MPP/RGA 或海思 HI_MPI 已接入。

## 文档

- `embedded_av_product_platform_architecture.md`：平台长期愿景。
- `docs/superpowers/specs/2026-08-18-eavp-media-backend-foundation-design.md`：Media Backend Foundation 0.2 规范。
- `docs/superpowers/specs/2026-08-21-eavp-linux-v4l2-capture-design.md`：Linux V4L2 Capture 0.3a 设计规格。
- `docs/superpowers/specs/2026-08-22-eavp-linux-alsa-capture-design.md`：Linux ALSA Capture 0.3b 设计规格。
- `docs/superpowers/specs/2026-08-25-eavp-linux-platform-runtime-design.md`：Linux Platform Runtime 0.3a 事件驱动与统一线程管理规格。
- `docs/migrations/0.1-to-0.2.md`：从 0.1 到 0.2 的 API 迁移说明。
- `docs/migrations/0.2-to-0.3-audio.md`：从 0.2 到 0.3b 的 AudioFrame API 迁移说明。
- `docs/architecture/README.md`：规范性架构文档索引。
- `docs/roadmap.md`：后续产品化路线。

## 发布属性

本仓库当前按内部专有软件管理，未授予开源许可。第三方测试依赖及许可证记录在 `docs/standards/third-party-dependencies.md`。
