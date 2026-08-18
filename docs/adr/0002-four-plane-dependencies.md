# ADR-0002：四平面采用单向模块依赖

> 状态：已接受  
> 日期：2026-08-18

## 决策

代码 target 固定为 `platform -> control -> media -> base` 和 `management -> base`。管理面与其他平面的连接由 platform adapter 完成。

## 原因

单向依赖使媒体核心不感知产品、协议或管理实现，避免观测代码反向侵入数据面，也让未来 Lite/Service 部署共享相同服务接口。

