# EAVP Media Backend Foundation 0.2 设计规格

> 状态：已批准
>
> 适用版本：0.2.0
>
> 规范性范围：平台无关媒体内存、能力模型、处理/编码后端接口、Provider 选择、生命周期与验收
>
> 前置规格：`docs/superpowers/specs/2026-08-18-eavp-core-baseline-design.md`

## 1. 背景

EAVP Core 0.1 已验证 CPU Buffer、Frame/Packet、Node/Graph/Pipeline、Desired/Actual State 和模拟纵切面。下一阶段需要让媒体管线能够在 Intel GPU、Rockchip MPP/RGA、海思 `HI_MPI` 等实现之间替换，同时保持业务编排、状态管理和媒体对象不依赖厂商 API。

平台以轻量化为长期约束。生产固件不得为了统一接口而强制携带 FFmpeg、GStreamer、libwebrtc、libsrt 等完整媒体框架。软件参考实现和第三方工具可以用于开发、测试或互操作验证，但不得成为 Core 或所有产品的必选运行时依赖。

## 2. 目标

本规格建立 Media Backend Foundation 0.2，目标如下：

- 把当前只支持 CPU `std::vector` 的 Buffer 扩展为可承载 CPU、MMAP、DMABUF 和设备原生内存的共享句柄。
- 定义平台无关的媒体格式、编码参数、码流格式和 Capability。
- 定义可表达同步软件实现和异步硬件实现的 `VideoProcessor`、`VideoEncoder` 接口。
- 通过显式注册的 `MediaBackendProvider` 创建后端实例，通过确定性规则选择 Provider。
- 提供不执行真实编解码的确定性 Reference Backend，用于自动化验证完整契约。
- 保持 Core 仅依赖 C++11、标准库和 POSIX；所有厂商 SDK 依赖停留在可选后端 target 内。

## 3. 非目标

0.2.0 不实现以下能力：

- V4L2 采集、Intel oneVPL、VA-API、Rockchip MPP/RGA 或海思 `HI_MPI` 适配。
- FFmpeg 软件编码或媒体效果处理。
- MPEG-TS、FLV、MOV/MP4 等容器。
- RTMP、WebRTC、SRT 等传输协议。
- 自动跨后端热切换、后台重试、动态插件或稳定 C ABI。
- 音频编码器、视频解码器或音频处理器接口。

这些能力分别进入独立规格；不得通过空模块、占位类或未测试接口提前进入代码库。

## 4. 架构原则

### 4.1 依赖边界

平台无关接口继续位于 `EAVP::media`。0.2.0 不新增必选公开 target，也不改变现有四平面依赖：

```text
platform -> control -> media -> base
platform -> management -> base
```

未来后端采用独立可选 target：

```text
backend_onevpl  ─┐
backend_rkmpp    ├─> EAVP::media -> EAVP::base
backend_hi_mpi  ─┘

EAVP::platform -> selected backends
```

后端 target 可以包含厂商头文件并链接厂商库，但这些依赖不得通过 `PUBLIC` 链接接口或公共 EAVP 头文件泄漏给上层。`EAVP::platform` 是显式注册 Provider 和组合产品 Pipeline 的唯一组合根。

### 4.2 原生后端优先

生产实现优先直接适配目标平台的原生媒体接口：

- Intel 使用 oneVPL 或经单独规格批准的 VA-API Provider。
- Rockchip 编解码使用 MPP，二维图像处理优先使用 RGA。
- 海思使用目标 SDK 提供的 `HI_MPI` 媒体接口。
- 其他 GPU 或 SoC 使用独立 Provider，不以 CPU 架构名称代替设备能力。

FFmpeg 可以作为默认关闭的开发或软件降级 Provider，但不能成为所有产品的间接依赖。

### 4.3 显式成本

零拷贝是协商结果，不是默认承诺。跨 Provider 仅允许通过标准 DMABUF 或 CPU 映射交换媒体内存。发生格式转换、内存域迁移或复制时，Graph 中必须存在可观测的处理节点，不允许后端静默执行昂贵拷贝。

## 5. 媒体内存模型

### 5.1 MemoryDomain

公共枚举至少包含：

```cpp
enum class MemoryDomain {
    kCpu,
    kMmap,
    kDmaBuf,
    kDeviceOpaque,
};
```

`kDeviceOpaque` 只表示 Buffer 由某个 Provider 私有管理，不公开 MPP、oneVPL、`HI_MPI` 等具体句柄类型。不同 Provider 不得假设可以直接消费另一个 Provider 的 `kDeviceOpaque` Buffer。

### 5.2 PlaneLayout

Buffer 使用一个或多个 Plane 描述图像布局。每个 Plane 至少包含相对存储起点的 offset、有效字节数和 stride。创建 Buffer 或 Frame 时必须验证：

- Plane 范围不超过底层存储容量。
- offset 与 size 的计算无整数溢出。
- stride 为正且满足所声明格式的最小行宽。
- 多 Plane 不产生未声明的重叠。

设备要求的宽高、stride 和地址对齐由 Capability 表达，不在通用类型中写死。

### 5.3 BufferStorage 与 Buffer

`BufferStorage` 是共享底层存储的抽象所有者，负责：

- 报告 MemoryDomain、容量和 Provider ID。
- 按只读或读写模式建立映射。
- 在映射对象析构时执行对应 unmap。
- 在支持时导出拥有独立生命周期的 DMABUF 句柄。
- 在最后一个共享引用释放时归还厂商 Surface、关闭文件描述符或释放 CPU 内存。

`Buffer` 是 `shared_ptr<BufferStorage>`、PlaneLayout 和视图范围组成的值类型。复制 Buffer 只共享所有权，不复制媒体字节。切片不得绕过 Plane 和边界验证。

公共 API 不再假设所有 Buffer 都能直接取得 `mutable_data()`。CPU 访问必须经过 RAII 映射对象；不支持映射的设备内存返回 `UNSUPPORTED`。

DMABUF 导出返回拥有资源所有权的 RAII 句柄，禁止返回所有权含糊的裸文件描述符。Provider 私有句柄只允许在该 Provider 的内部实现中访问。

## 6. 媒体格式与编码数据

### 6.1 VideoFormat

`VideoFormat` 至少描述：

- PixelFormat。
- width、height。
- Plane 数量、stride 与必要对齐。
- 色彩范围、色彩原色、传递特性和矩阵系数；0.2.0 可以只支持 Unknown 与已实现的有限枚举，但必须保留显式字段。
- MemoryDomain。

格式对象创建时完成结构校验。Provider Capability 决定某个有效格式能否被具体实现接受。

### 6.2 VideoEncoderConfig

首期编码配置模型至少包含：

- CodecId：H.264、H.265 的枚举值保留，Reference Backend 使用测试专用实现语义而不生成伪装成标准码流的字节。
- 宽高、帧率及 TimeBase。
- 目标码率、最大码率。
- GOP 长度、B 帧数量。
- RateControlMode。
- Profile、Level 和低延迟要求。

配置分为请求值与协商结果。Provider 不得静默修改必需字段；对偏好字段的调整必须通过协商结果返回给调用方。

### 6.3 EncodedStreamFormat

`MediaPacket` 增加明确的编码流格式，例如 H.264/H.265 Annex-B、AVCC/HVCC。Packet 继续携带 PTS、DTS、duration、TimeBase、stream index 和 key-frame 标志，并能够关联 Codec 配置数据，包括 SPS、PPS、VPS 或对应配置记录。

容器和协议实现根据该描述进行显式 bitstream 转换；不得通过扫描未知字节猜测格式。

## 7. Capability 模型

### 7.1 ProviderCapability

每个 Provider 通过只读快照报告：

- 稳定 Provider ID、实现版本和设备标识。
- 支持的 Operation：视频处理、视频编码。
- 支持的输入/输出 PixelFormat 与 MemoryDomain 组合。
- Codec、Profile、Level、RateControlMode。
- 最小/最大分辨率、宽高步进和对齐。
- 最大并发实例数和已知资源约束。
- 缩放、裁剪、旋转、格式转换等处理能力。
- 是否支持输入到输出的零拷贝路径。

Capability 必须由运行环境探测产生，不能仅根据编译宏宣称硬件能力。设备不存在、驱动不可用或权限不足时，Provider 保留注册信息但报告不可用状态及统一错误。

### 7.2 SelectionRequest

选择请求分为：

- Required：不满足即排除候选，包括 Codec、格式、分辨率、MemoryDomain 和必须的处理操作。
- Preferred：硬件加速、零拷贝、低功耗、低延迟、指定 Provider 顺序等偏好。

选择算法必须确定：相同注册集合和相同请求总是得到相同结果。显式指定 Provider 时只验证该 Provider，不进行隐式替换；自动选择时按满足 Required、偏好得分、配置优先级和 Provider ID 稳定排序。

选择结果返回 Provider ID、协商配置、实际格式和是否需要显式转换节点。

## 8. Provider 与处理接口

### 8.1 MediaBackendProvider

Provider 是无状态或只读状态的工厂，职责如下：

- `probe`：生成 Capability 和可用状态。
- `create_video_processor`：根据已协商配置创建处理实例。
- `create_video_encoder`：根据已协商配置创建编码实例。

Provider 不拥有 Pipeline，不更新 Desired/Actual State，不直接写 Metrics。管理面适配继续由 `platform` 完成。

### 8.2 BackendRegistry

Registry 在平台启动阶段显式注册 Provider。禁止依赖静态对象构造函数完成自动注册，以避免初始化顺序和裁剪不可控。

Registry 支持注册、重复 ID 拒绝、Capability 快照和确定性选择。平台完成注册后冻结 Registry；运行阶段不得修改注册集合。动态插件和热插拔注册不属于 0.2.0。

### 8.3 VideoProcessor

处理器采用非阻塞接口语义：

```text
configure -> submit(frame) -> receive(frame) -> begin_drain -> receive -> END_OF_STREAM
```

一次 submit 不保证立即产生输出。输入或输出资源暂时不可用时返回 `WOULD_BLOCK`，由 Executor 在后续 tick 重试。处理器不得在调用线程中无限等待硬件。

首期配置可表达缩放、裁剪、旋转和 PixelFormat 转换，但 Reference Backend 只实现规格验收所需的确定性操作。未实现的组合必须返回 `UNSUPPORTED`，不得静默跳过。

### 8.4 VideoEncoder

编码器采用与处理器相同的 submit/receive/drain 模型，以覆盖 B 帧重排、硬件队列和延迟输出。编码器在 `begin_drain` 后拒绝新输入，并持续输出缓存 Packet，最终返回 `END_OF_STREAM`。

`reset` 释放运行资源并回到可重新配置状态。设备丢失、非法码流或不可恢复的厂商错误进入 Error；只有显式 reset 或销毁实例才能恢复。

### 8.5 通用 Node

`VideoProcessorNode` 和 `VideoEncoderNode` 只依赖上述抽象实例。Node 不知道 Provider 的具体类型，不包含厂商条件编译，并遵守现有 Pipeline 生命周期和确定性 Executor 调度。

Node 将 `WOULD_BLOCK` 传播为背压，不把它记录为 Health 错误。不可恢复错误进入 Node/Pipeline Error，由 platform adapter 更新 Actual State、Metrics 和 Health。

## 9. 所有权、线程与生命周期

- Platform 独占 Registry，Registry 只保存 Provider 的共享只读引用。
- Pipeline 独占通用 Node，Node 独占 Processor/Encoder 实例。
- 实例独占厂商上下文和 Surface 池；Buffer 通过共享 Storage 延长单个 Surface 的有效期。
- Provider 工厂不保存对 Node、Pipeline 或 Control Plane 的反向引用。
- Processor/Encoder 实例不可复制，并绑定到创建它的 Executor。
- 0.2.0 后端不得创建不可见的控制线程。厂商 SDK 内部线程可以存在，但其完成事件必须通过非阻塞 submit/receive 语义进入 EAVP。

实例生命周期为：

```text
Created -> Configured -> Running -> Draining -> Stopped
                       \---------------------> Error
Stopped/Error -> reset -> Created
```

重复 stop/reset 保持幂等。Pipeline 停止时先停止上游提交，随后按拓扑排空；资源释放和失败回滚仍按逆拓扑执行。

## 10. 错误模型

公共 API 继续使用 `Status` 和 `Result<T>`，不得让异常跨越模块边界。StatusCode 增加：

- `kEndOfStream`：drain 已完成，不表示故障。
- `kDeviceLost`：设备、驱动或硬件上下文不可继续使用。
- `kCorruptData`：输入媒体数据或编码输出违反已声明格式。

`kWouldBlock` 继续表示预期背压。Capability 不匹配使用 `kCapabilityMismatch`，资源池耗尽使用 `kResourceExhausted`。

Status 可以附带平台无关的错误上下文，包括 Provider ID、操作名称和原生数值错误码。错误消息用于诊断，不作为程序分支条件。厂商枚举、结构体、指针和所有权不得进入 Status。

运行中不得静默切换 Provider。`kDeviceLost` 使 Pipeline 和 Actual State 进入 Error；Reconciler 根据后续恢复策略完整停止、重新探测、重新选择并重启 Pipeline，以保持 GOP、时间戳和 Buffer 所有权边界清晰。

## 11. Reference Backend

Reference Backend 是生产代码中的最小确定性实现，用于证明公共契约，但不冒充真实 Codec：

- Reference Processor 对固定 CPU Frame 执行可验证的有限转换。
- Reference Encoder 将输入帧转换为带明确测试 CodecId 的确定性 Packet。
- 可配置输入队列容量、输出延迟和故障注入点。
- 支持 `WOULD_BLOCK`、drain、`END_OF_STREAM`、reset 和设备丢失模拟。

Reference Backend 不依赖 FFmpeg，不生成可对外发布的 H.264/H.265 码流，也不进入产品默认 Pipeline。

## 12. 测试策略

### 12.1 单元测试

- Buffer 共享所有权、切片、Plane 边界、整数溢出和释放顺序。
- CPU 映射、不可映射设备内存、DMABUF 导出成功及失败语义。
- VideoFormat、EncoderConfig 和 EncodedStreamFormat 校验。
- Capability 范围、对齐和 MemoryDomain 匹配。
- Provider 重复注册、Registry 冻结和确定性选择。
- Required/Preferred 协商及显式 Provider 不回退。
- Processor/Encoder 的 submit/receive、背压、延迟输出、drain、reset 和 Error 转移。

### 12.2 Backend Contract Tests

建立一套可复用于 Reference、oneVPL、MPP/RGA、`HI_MPI` 等实现的契约测试。每个真实 Provider 必须验证：

- Capability 与实际创建结果一致。
- 不支持的配置在资源分配前失败。
- 输入 Buffer 在异步处理完成前保持有效。
- `WOULD_BLOCK` 不丢失数据、不阻塞 Executor。
- drain 输出全部缓存数据且只产生一次 `END_OF_STREAM` 终态。
- reset 和销毁不泄漏 Surface、文件描述符或厂商上下文。

### 12.3 集成测试

Reference Pipeline 连续处理 100 个 VideoFrame，验证：

- 选择结果、协商格式和实际 Provider 可通过 Query 读取。
- Processor 与 Encoder 的输入/输出计数准确。
- 人为背压不会死锁或越过有界队列。
- 注入设备丢失后 Desired 保留，Actual 与 Health 报告 Error。
- 显式重置与 Reconcile 能够完整重建 Pipeline。

真实硬件测试使用独立标签。缺少设备时可以跳过开发机构建，但对应平台包发布前必须在目标硬件通过 Backend Contract Tests 和端到端测试。

### 12.4 构建验证

Debug、Release、ASan/UBSan、安装消费工程和通用 aarch64 交叉构建继续作为完成条件。Core 构建不得查找或下载 FFmpeg、oneVPL、MPP、RGA、海思 SDK 或 OpenSSL。

## 13. 后续模块边界

后续容器与传输模块遵守“协议自研、密码学例外”原则：

- `EAVP::container` 自行实现 MPEG-TS、FLV、ISO BMFF、MP4 和 Fragmented MP4，不链接第三方容器实现。
- `EAVP::transport` 自行实现 RTMP/E-RTMP、WebRTC 媒体传输和 SRT，不链接 libwebrtc、libsrt 或同类完整协议栈。
- WebRTC 和 SRT 所需密码学通过 `CryptoProvider` 隔离；OpenSSL 等经过审计的密码学实现是允许的可选依赖。
- 密码学原语不自行设计或实现。

协议实现固定具体规范版本和文档哈希，不直接追随会变化的 `latest` 指针。当前规划基线为：

- MPEG-TS：ITU-T H.222.0（08/2023）。
- ISO BMFF：ISO/IEC 14496-12:2026。
- RTMP/FLV：Adobe legacy 规范与 E-RTMP v2 发布版。
- WebRTC：IETF RTCWEB RFC 集、W3C WebRTC Recommendation 与 WHIP RFC 9725。
- SRT：Haivision `srt-rfc` 工作副本；由于其仍为 Internet-Draft，实施规格必须固定 commit 并以官方参考实现进行互操作验证。

上述条目只规定未来边界，不授权在 0.2.0 中实现这些模块。

## 14. 里程碑顺序

1. **0.2 Media Backend Foundation**：本规格定义的 Buffer、Capability、Provider、Processor/Encoder、Reference Backend。
2. **0.3 Linux Native Pipeline**：V4L2 与首个实际可用的原生硬件 Provider；具备 Intel GPU 时优先 oneVPL。
3. **0.4 Container Foundation**：自研 MPEG-TS、H.264/H.265 bitstream 转换和 FileSink。
4. **0.5 WebRTC Transport**：优先实现 WebRTC 媒体传输与 WHIP，密码能力由 CryptoProvider 提供。
5. **0.6 RTMP/FLV**：自研 FLV、AMF 和 RTMP/E-RTMP。
6. **0.7 ISO BMFF**：自研 MOV/MP4 与 Fragmented MP4 的明确子集。
7. **0.8 SRT**：自研 SRT 传输、丢包恢复、拥塞控制和加密协商。

Rockchip MPP/RGA 与海思 `HI_MPI` 分别使用独立硬件规格。取得目标板、匹配 SDK 和发布约束后，可以插入 0.2 之后实施，不与其他厂商后端合并成一个交付批次。

## 15. 0.2.0 验收标准

- Core 无新增第三方运行时依赖，C++11 与 POSIX 基线不变。
- 公共头文件不包含任何厂商或 FFmpeg 类型。
- CPU、不可映射设备内存和 DMABUF 测试存储通过统一 Buffer 契约。
- Capability 选择在相同输入下结果确定，并能解释排除候选的原因。
- Reference Processor/Encoder 支持背压、延迟输出、drain、reset 和设备丢失。
- Reference Pipeline 处理 100 帧，Packet 数量、状态、Metrics 和 Health 与预期一致。
- 设备丢失不触发运行中静默后端切换；Desired 保留，Actual 进入 Error。
- Debug、Release、ASan/UBSan、安装消费和 aarch64 交叉构建全部通过。
- 所有新增说明性文档以简体中文为主，不包含未解释的占位内容。

## 16. 兼容性与迁移

0.2.0 会调整 `Buffer`、`VideoFrame` 和 `MediaPacket` 的实验性 C++ API。项目尚未达到 1.0，该破坏性调整允许发生，但实施时必须提供迁移说明：

- `Buffer::mutable_data()` 和直接 `data()` 访问迁移到 RAII map。
- 单 stride VideoFrame 迁移到多 Plane VideoFormat。
- MediaPacket 调用方必须声明 EncodedStreamFormat 和 Codec 配置数据。
- 现有 FakeSource、PassThrough、FakeSink 和模拟平台必须迁移并保持行为测试通过。

动态插件 C ABI 仍不实现。未来 C ABI 必须以独立规格定义稳定的结构体大小、ABI 版本、函数表和所有权规则，不能直接暴露本规格的实验性 C++ 虚接口。

## 17. 已确认决策

- 平台无关接口先于任何真实硬件后端实现。
- 原生硬件 Provider 是生产首选，FFmpeg 只作为可选开发、兼容或软件降级后端。
- 产品固件只携带当前平台所需 Provider。
- 容器和传输协议自行实现，不依赖完整第三方媒体或协议框架。
- OpenSSL 等经过审计的密码学依赖允许通过 CryptoProvider 接入。
- 自动化验收优先使用确定性 Reference Backend；真实设备作为独立发布门禁。
- 每个真实后端、容器和协议使用独立规格，禁止一次并行扩展多个高风险子系统。
