# EAVP 产品化路线图

> 状态：规划中  
> 适用版本：0.1 之后  
> 规范性范围：里程碑顺序，不冻结接口

1. **Core 0.1**：纯软件核心、状态收敛、可观测模拟纵切面。
2. **Linux Reference Pipeline**：V4L2、软件 H.264、MPEG-TS 和文件输出，验证真实 Buffer 与时间戳。
3. **基础音频与协议**：ALSA、SRT、RTSP、RTMP、MP4，建立协议能力与错误恢复。
4. **设备与静态插件**：Capability、ResourceManager、静态注册、Kconfig 裁剪。
5. **首个 SoC**：优先接入选定厂商编码器与 DMA/DMABUF，完成零拷贝验证。
6. **产品服务**：Live、Record、Preview、配置 Schema、权限和 REST/WebSocket。
7. **管理闭环**：Alarm、Trace、Crash Dump、Flight Recorder、Watchdog 和升级。
8. **Service Mode**：RPC Adapter、多进程隔离与故障域。

每个里程碑必须先有独立规格和可运行验收场景，不一次并行扩展多个真实后端。

