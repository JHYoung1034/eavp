# 版本与 ABI 策略

> 状态：已接受\
> 适用版本：EAVP 0.2.0 起\
> 规范性范围：版本号、公共 API、安装导出和插件边界

项目采用语义化版本。1.0 之前的 C++ API 属于实验性接口，minor 版本可以包含破坏性调整，但必须记录迁移说明；patch 版本不得主动破坏源代码兼容。0.2.0 对 Buffer、VideoFrame、MediaPacket 和 `Result<T>` 的调整记录于 `docs/migrations/0.1-to-0.2.md`。

0.2.0 不提供动态插件 ABI。Provider 是进程内 C++ 扩展点，不承诺二进制插件兼容；未来动态插件边界使用 C ABI、显式 `abi_version`、结构体大小字段和函数表，禁止把 STL 类型、异常、RTTI 对象或 C++ 所有权跨越插件边界。

安装包导出且仅导出 `EAVP::base`、`EAVP::media`、`EAVP::control`、`EAVP::management` 和 `EAVP::platform`。消费方通过 `find_package(EAVP 0.2 CONFIG REQUIRED)` 获取这些 target，不得链接未导出的内部 target。
