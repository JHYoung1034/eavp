# Core 0.1 架构总览

> 状态：已接受  
> 适用版本：EAVP Core 0.1  
> 规范性范围：模块边界和依赖方向

```text
example / product
        |
        v
EAVP::platform ------> EAVP::management
        |
        v
EAVP::control
        |
        v
EAVP::media
        |
        v
EAVP::base <---------- EAVP::management
```

`platform` 是唯一组合根，负责把控制状态、媒体执行和管理观察连接起来。`control` 可以驱动抽象媒体 Pipeline，但不依赖具体产品。`management` 只定义指标和健康模型，不读取控制或媒体内部对象。

首期仅提供单进程 Lite Mode。Service Mode 必须复用相同的应用服务语义，并在后续通过 Remote Adapter 替换 Local Adapter，不允许业务代码感知部署方式。

