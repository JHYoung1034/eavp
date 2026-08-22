# 第三方依赖登记

> 状态：已接受  
> 适用版本：EAVP Core 0.1  
> 规范性范围：直接依赖和构建/测试依赖

| 名称 | 固定版本 | 用途 | 许可证 | 目标产物依赖 |
| --- | --- | --- | --- | --- |
| GoogleTest | `58d77fa8070e8cec2dc1ed015d66b454c8d78850`（release-1.12.1） | 单元及集成测试 | BSD-3-Clause | 否 |
| ALSA libasound | `1.2.11`（本机设计基线） | 可选 Linux PCM 采集 | LGPL-2.1-or-later | 仅 `EAVP_ENABLE_ALSA=ON` 的 `EAVP::platform` |

生产核心当前没有第三方运行时依赖。

