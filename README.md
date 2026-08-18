# EAVP

EAVP（Embedded Audio/Video Platform）是面向 Embedded Linux 音视频设备的通用产品底座。当前版本为 `0.1.0`，目标是验证跨 SoC 平台核心边界，而不是提供完整产品功能。

## 当前能力

- C++11 的错误、媒体对象和时间基准模型。
- CPU Buffer、媒体 Packet、类型化 Port、有界 Queue、Node/Graph/Pipeline。
- Command/Query、Desired/Actual StateStore 与幂等 Reconciler。
- Counter、Gauge、Health，以及单进程模拟媒体纵切面。
- Linux x86_64 原生构建和通用 aarch64 交叉编译配置。

真实 V4L2、ALSA、FFmpeg、硬件编解码、网络协议、RPC、Web API 和 Service Mode 不属于 Core 0.1。

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

安装和消费示例见 `docs/architecture/build-and-portability.md`。

## 文档

- `embedded_av_product_platform_architecture.md`：平台长期愿景。
- `docs/superpowers/specs/2026-08-18-eavp-core-baseline-design.md`：Core 0.1 规范。
- `docs/architecture/README.md`：规范性架构文档索引。
- `docs/roadmap.md`：后续产品化路线。

## 发布属性

本仓库当前按内部专有软件管理，未授予开源许可。第三方测试依赖及许可证记录在 `docs/standards/third-party-dependencies.md`。

