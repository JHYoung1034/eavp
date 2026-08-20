# EAVP 产品化路线图

> 状态：当前里程碑为 Media Backend Foundation 0.2\
> 适用版本：0.2.0 之后\
> 规范性范围：里程碑顺序，不冻结接口

1. **Core 0.1（已完成）**：纯软件核心、状态收敛、可观测模拟纵切面。
2. **Media Backend Foundation 0.2（当前）**：显式 Buffer/Format、Capability、Provider 选择和 Reference Backend；Reference payload 不是标准码流。
3. **Linux Reference Pipeline**：在独立规格中定义 V4L2、软件 H.264、MPEG-TS 和文件输出，验证真实 Buffer 与时间戳。
4. **基础音频与协议**：在独立规格中定义 ALSA、SRT、RTSP、RTMP、MP4，建立协议能力与错误恢复。
5. **设备与静态插件**：在独立规格中定义 Capability、ResourceManager、静态注册、Kconfig 裁剪。
6. **首个 SoC**：在独立硬件规格中接入选定厂商编码器与 DMA/DMABUF，完成零拷贝验证。
7. **产品服务**：在独立规格中定义 Live、Record、Preview、配置 Schema、权限和 REST/WebSocket。
8. **管理闭环**：在独立规格中定义 Alarm、Trace、Crash Dump、Flight Recorder、Watchdog 和升级。
9. **Service Mode**：在独立规格中定义 RPC Adapter、多进程隔离与故障域。

每个里程碑必须先有独立规格和可运行验收场景，不一次并行扩展多个真实后端。
