# 核心接口与所有权

> 状态：已接受\
> 适用版本：EAVP 0.2.0\
> 规范性范围：公共 C++ 接口、对象所有权和媒体后端选择

## 错误模型

`Status` 表达无返回值操作结果，`Result<T>` 同时携带值或失败状态。成功状态不能携带错误文本，失败 Result 不能读取值；`Result<T>` 为 move-only，转移专有值使用 `take_value()`。公共模块边界不抛异常。

## 媒体对象

`Buffer` 使用 `shared_ptr` 共享底层 Storage，切片只改变视图范围。访问字节必须通过 `map_plane()` 取得有生命周期约束的 `MappedRegion`，不得假定单一连续 plane。`VideoFormat` 显式声明像素格式、尺寸、内存域和每个 `PlaneLayout`；VideoFrame 的 Buffer plane 必须与该格式一致。VideoFrame、AudioFrame 和 MediaPacket 是值对象，复制它们不会复制媒体字节。MediaPacket 由 `create()` 建立，并声明 Codec、EncodedStreamFormat 和 codec 配置。时间基准分母必须大于零。

## 后端边界

`MediaBackendProvider` 只暴露 capability probe 和 Processor/Encoder 工厂。`BackendRegistry` 在平台注册完成后冻结；冻结后注册失败，选择结果只由请求、能力和已文档化的偏好排序决定。Provider 实例不拥有 Pipeline、StateStore 或 Metrics。0.2.0 的 Reference Provider 只产生 `kReference` 测试 payload，不得解释为标准 H.264/H.265 码流。

## 图与节点

Pipeline 通过 `unique_ptr` 独占 Node；Node 独占自己的 Port；Graph 记录不拥有对象的连接关系。Pipeline 析构前停止节点。外部不得保存超过 Pipeline 生命周期的 Node/Port 指针。

类型化端口只允许连接相同媒体对象类型，并在运行前验证格式。一个输出可以扇出到多个输入；首期每个输入只能有一个上游。
