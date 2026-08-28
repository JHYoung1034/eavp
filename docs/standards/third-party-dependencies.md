# 第三方依赖登记

> 状态：已接受  
> 适用版本：EAVP 稳定版本 0.2.0 与 0.3a/0.3b 开发能力
> 规范性范围：上述版本的直接依赖和构建/测试依赖

| 名称 | 固定版本 | 用途 | 许可证 | 目标产物依赖 |
| --- | --- | --- | --- | --- |
| GoogleTest | `58d77fa8070e8cec2dc1ed015d66b454c8d78850`（release-1.12.1） | 单元及集成测试 | BSD-3-Clause | 否 |
| ALSA libasound | `1.2.11`（本机设计基线） | 可选 Linux PCM 采集 | LGPL-2.1-or-later | 仅 `EAVP_ENABLE_ALSA=ON` 的 `EAVP::platform` |

Linux Runtime 与 V4L2 采集不新增第三方生产库，只使用 C++11、Threads、POSIX 与 Linux UAPI。ThreadSanitizer、AddressSanitizer 和 UndefinedBehaviorSanitizer 只属于编译器验证工具，不进入目标产物依赖。

生产核心当前没有第三方运行时依赖。
