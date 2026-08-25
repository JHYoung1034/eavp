# EAVP 产品化路线图

> 状态：稳定基线为 Media Backend Foundation 0.2；0.3b ALSA 音频采集开发能力已完成验收\
> 适用版本：0.2.0 之后\
> 规范性范围：里程碑顺序，不冻结接口

1. **Core 0.1（已完成）**：纯软件核心、状态收敛、可观测模拟纵切面。
2. **Media Backend Foundation 0.2（稳定基线）**：显式 Buffer/Format、Capability、Provider 选择和 Reference Backend；Reference payload 不是标准码流。
3. **0.3 Linux Native**：0.3a 先建立平台统一的 Linux 事件 Reactor，再实现 V4L2 CPU VideoFrame 采集，并为既有 ALSA 增加 readiness 适配；**0.3b（已完成开发验收，稳定包仍为 0.2.0）**实现 ALSA CPU AudioFrame 采集；0.3c 在统一 Runtime 下增加 ComputeWorkerPool 与有界 SPSC Queue，使用开发机可选 FFmpeg Backend 验证 libx264 H.264、libx265 H.265、AAC 和 libopus Opus 编码，不作为 ARM 产品依赖；0.3d 在同一 Reactor 的共同单调时钟域内同时采集音视频，测量偏差和漂移，首期不自动校正。
4. **0.4 Container**：自研 MPEG-TS、H.264/H.265 bitstream 转换和 FileSink。
5. **0.5 WebRTC**：优先实现 WebRTC 媒体传输与 WHIP，密码能力由 CryptoProvider 提供。
6. **0.6 RTMP/FLV**：自研 FLV、AMF 和 RTMP/E-RTMP。
7. **0.7 ISO BMFF**：自研 MOV/MP4 与 Fragmented MP4 的明确子集。
8. **0.8 SRT**：自研 SRT 传输、丢包恢复、拥塞控制和加密协商。

Rockchip MPP/RGA 与海思 `HI_MPI` 分别使用独立硬件规格；取得目标板、匹配 SDK 和发布约束后，可在 0.2 之后按条件插入，不改变上述已批准顺序。每个里程碑必须先有独立规格和可运行验收场景，不一次并行扩展多个真实后端。
