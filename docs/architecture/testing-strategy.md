# 测试策略

> 状态：已接受  
> 适用版本：EAVP Core 0.1  
> 规范性范围：测试分层和完成标准

单元测试验证 Status/Result、Buffer、时间基准、Queue/Port、图验证、Node/Pipeline、StateStore、Reconciler、Metrics 和 Health。每个测试命名其可捕获的行为缺陷，并通过真实组件观察结果。

集成测试使用 FakeSource、PassThrough 和 FakeSink 处理 100 个 Packet，验证命令到状态、数据和指标的完整路径。故障场景使用真实的失败 Node，验证逆序回滚和 Health，而不是断言 mock 调用。

完成标准包括 Debug/Release CTest、ASan/UBSan、安装消费工程和 aarch64 交叉编译。交叉编译只验证生成与链接，不执行目标程序。

