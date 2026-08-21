# EAVP 产品化路线图

> 状态：当前里程碑为 Media Backend Foundation 0.2\
> 适用版本：0.2.0 之后\
> 规范性范围：里程碑顺序，不冻结接口

1. **Core 0.1（已完成）**：纯软件核心、状态收敛、可观测模拟纵切面。
2. **Media Backend Foundation 0.2（当前）**：显式 Buffer/Format、Capability、Provider 选择和 Reference Backend；Reference payload 不是标准码流。
3. **0.3 Linux Native**：V4L2 与首个实际可用的原生硬件 Provider；具备 Intel GPU 时优先 oneVPL。
4. **0.4 Container**：自研 MPEG-TS、H.264/H.265 bitstream 转换和 FileSink。
5. **0.5 WebRTC**：优先实现 WebRTC 媒体传输与 WHIP，密码能力由 CryptoProvider 提供。
6. **0.6 RTMP/FLV**：自研 FLV、AMF 和 RTMP/E-RTMP。
7. **0.7 ISO BMFF**：自研 MOV/MP4 与 Fragmented MP4 的明确子集。
8. **0.8 SRT**：自研 SRT 传输、丢包恢复、拥塞控制和加密协商。

Rockchip MPP/RGA 与海思 `HI_MPI` 分别使用独立硬件规格；取得目标板、匹配 SDK 和发布约束后，可在 0.2 之后按条件插入，不改变上述已批准顺序。每个里程碑必须先有独立规格和可运行验收场景，不一次并行扩展多个真实后端。
