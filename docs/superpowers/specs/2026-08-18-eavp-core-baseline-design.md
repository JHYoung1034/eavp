# EAVP Core 0.1 核心基线设计规格

> 状态：已批准  
> 适用版本：0.1.0  
> 规范性范围：首期代码、公共接口、构建、测试和文档

## 目标

建立可编译、可测试、可安装的 Embedded Linux 音视频平台核心。使用纯软件模拟管线验证 Application、Control、Data、Management 四个平面的边界，不接入真实设备、编解码器或网络协议。

## 模块与依赖

- `base`：Status/Result、强类型 ID、时间基准和基础事件。
- `media`：Buffer、Frame/Packet、Queue/Port、Node、Graph、Pipeline、确定性执行。
- `control`：显式 Command/Query、Desired/Actual StateStore、PipelineReconciler。
- `management`：Counter、Gauge 和 Health 汇总。
- `platform`：四个平面的组合根与模拟直播能力。

依赖必须保持 `platform -> control -> media -> base` 和 `management -> base`。`management` 不反向依赖媒体或控制对象；对应适配器放在 `platform`。

## 所有权与执行模型

Buffer 句柄共享底层存储，媒体元数据是值对象。Pipeline 独占 Node，Node 独占 Port，Pipeline 独占连接和队列。队列中的媒体数据使用共享只读句柄。

Node 不创建线程。首期使用调用者驱动的确定性 Executor；每次 tick 按拓扑顺序推进节点。Queue 的 `BLOCK` 策略返回 `WOULD_BLOCK`，由调度器停止推进上游，不阻塞执行线程。

## 生命周期

Node 状态为 `Created -> Prepared -> Running -> Stopped`，任意执行错误进入 `Error`，`Reset` 返回 `Created`。Pipeline 在开始前验证非空、无环和端口兼容性，按拓扑启动节点；启动失败时按逆序停止已经启动的节点。重复 start/stop 是幂等操作。

## 控制与状态

写操作使用显式类型的 Start/Stop Command；Query 返回只读状态快照。Desired 和 Actual 使用不同的 StateStore 实例，快照带单调递增版本号。

Reconciler 是串行且幂等的。校验失败时不修改 Desired；执行失败时保留 Desired、把 Actual 标记为 Error 并更新 Health。首期不提供后台自动重试，调用者显式执行 `reconcile_once`。

## 模拟纵切面

模拟平台创建 `FakeSource -> PassThrough -> FakeSink`。Start Command 写入 Desired，Reconciler 启动管线并更新 Actual。调用者推进 100 次数据后，Query 必须返回 Running、100 个已处理 Packet、队列深度和健康状态。Stop Command 收敛到 Stopped。

## 非目标

本期不实现 V4L2、ALSA、FFmpeg、SoC SDK、真实协议、配置持久化、权限、REST/WebSocket、RPC、动态插件、Service Mode、升级、Crash Dump、Flight Recorder 或 Kconfig 集成。

## 验收

Debug、Release、ASan/UBSan 测试通过；安装后独立工程可通过 `find_package(EAVP)` 消费；通用 aarch64 工具链可完成交叉编译但不运行测试；所有新增项目文档优先使用简体中文。

