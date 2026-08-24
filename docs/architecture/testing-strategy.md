# 测试策略

> 状态：已接受\
> 适用版本：稳定包 EAVP 0.2.0 与 0.3b 开发能力\
> 规范性范围：测试分层、验证矩阵和完成标准

单元测试验证 Status/Result、Buffer plane 映射、VideoFormat、Packet factory、时间基准、Capability/Registry、Reference Backend、Queue/Port、图验证、Node/Pipeline、StateStore、Reconciler、Metrics 和 Health。每个测试命名其可捕获的行为缺陷，并通过真实组件观察结果。

集成测试使用 FakeSource、PassThrough 和 FakeSink 处理 100 个 Packet，并使用 ReferenceMediaPlatform 处理 100 个 Reference payload，验证命令到状态、数据、后端选择和指标的完整路径。故障场景使用真实的失败 Node 或可注入的 Reference 后端故障，验证逆序回滚、reset 和 Health，而不是断言 mock 调用。

完成标准包括 Debug/Release CTest、ASan/UBSan、安装消费工程，以及 aarch64、Rockchip ARM32 和海思 v600 ARM32 交叉编译。安装消费工程必须通过 `find_package(EAVP 0.2 CONFIG REQUIRED)` 独立配置、编译和运行，覆盖 CPU Buffer、VideoFormat、BackendRegistry 与 Reference Provider 选择。三套交叉编译只验证生成与链接及对象目标架构，不执行目标程序；其结果不得外推为 Rockchip MPP/RGA 或海思 HI_MPI 的接入或功能验证。工具链不可用时必须保留未通过记录和原始错误，不能将其写为通过。

0.3b 追加 AudioFormat/AudioFrame 的精确字节、首样本 PTS、无 DTS 与 discontinuity 单元覆盖；Fake ALSA 集成链路验证 300 个 48 kHz stereo S16_LE、480 samples_per_channel 帧、短读、背压与一次 XRUN。该确定性测试进入 Debug、Release、ASan/UBSan 的普通 CTest。真实 Loopback 测试默认不加入普通矩阵，须显式启用并由环境变量指定设备和 300 帧形状；它验证格式、PTS、Metrics 与 Health，静音 PCM 仍是有效输入。设备无 producer 的超时必须如实记录为环境失败。
