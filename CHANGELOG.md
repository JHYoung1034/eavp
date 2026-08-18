# 变更记录

本文档记录 EAVP 面向使用者的主要变化。版本号遵循语义化版本。

## 0.1.0 - 2026-08-18

### 新增

- 建立 `EAVP::base`、`EAVP::media`、`EAVP::control`、`EAVP::management` 和 `EAVP::platform`。
- 提供 Status/Result、强类型 ID、Buffer、Frame/Packet、Queue/Port、Graph/Pipeline 和确定性 Executor。
- 提供显式 Pipeline Command、Desired/Actual StateStore、Reconciler、Metrics 和 Health。
- 提供处理 100 个 Packet 的模拟媒体纵切面和示例程序。
- 提供 Debug、Release、ASan/UBSan、aarch64 交叉编译、安装导出和独立消费验证。

### 限制

- C++ API 在 1.0 前属于实验性接口。
- 不包含真实设备、编解码器、协议、配置持久化、动态插件或 Service Mode。

