# 版本与 ABI 策略

> 状态：已接受  
> 适用版本：EAVP Core 0.1 起  
> 规范性范围：版本号、公共 API 和插件边界

项目采用语义化版本。1.0 之前的 C++ API 属于实验性接口，minor 版本可以包含破坏性调整，但必须记录迁移说明；patch 版本不得主动破坏源代码兼容。

Core 0.1 不提供动态插件 ABI。未来插件边界使用 C ABI、显式 `abi_version`、结构体大小字段和函数表，禁止把 STL 类型、异常、RTTI 对象或 C++ 所有权跨越插件边界。

安装包导出 `EAVP::base`、`EAVP::media`、`EAVP::control`、`EAVP::management` 和 `EAVP::platform`，消费方不得链接未导出的内部 target。

