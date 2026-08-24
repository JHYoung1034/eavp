# 核心接口与所有权

> 状态：已接受\
> 适用版本：稳定包 EAVP 0.2.0 与 0.3b 开发能力\
> 规范性范围：公共 C++ 接口、对象所有权和媒体后端选择

## 错误模型

`Status` 表达无返回值操作结果，`Result<T>` 同时携带值或失败状态。成功状态不能携带错误文本，失败 Result 不能读取值；`Result<T>` 为 move-only，转移专有值使用 `take_value()`。公共模块边界不抛异常。

## 媒体对象

`Buffer` 使用 `shared_ptr` 共享底层 Storage，切片只改变视图范围。访问字节必须通过 `map_plane()` 取得有生命周期约束的 `MappedRegion`，不得假定单一连续 plane；只读映射不提供可写指针。Storage 声明起始地址的分配器保证，Buffer 结合 plane offset 暴露每个 plane 的有效对齐，Provider 在 submit 时以共享校验入口核对实际 Frame，不能把 selection request 的数字当作实际 Buffer 证据。`VideoFormat` 显式声明像素格式、尺寸、内存域和每个 `PlaneLayout`；VideoFrame 的 Buffer plane 必须与该格式一致。

0.3b 的 `AudioFormat` 明确 SampleFormat、采样率、mono/stereo、interleaved 布局和内存域；`kSigned24In32LittleEndian` 是 32-bit container，不是 packed S24_3LE。`AudioFrame` 的 Buffer 必须为相同内存域的单一 plane，且字节数精确等于 `samples_per_channel * bytes_per_pcm_frame()`。`samples_per_channel` 是每声道数；`pts` 是首个 PCM 采样点的呈现时间，PCM 帧无 DTS，编码 `MediaPacket` 才独立保存 PTS/DTS。`discontinuity` 只表示已知时间线中断后的首个完整帧。

VideoFrame、AudioFrame 和 MediaPacket 是值对象，复制它们不会复制媒体字节。MediaPacket 由 `create()` 建立，并声明 Codec、EncodedStreamFormat 和 codec 配置。时间基准分母必须大于零。

## 后端边界

`MediaBackendProvider` 只暴露 capability probe 和 Processor/Encoder 工厂。`BackendRegistry` 在平台注册完成后冻结；冻结后注册失败，选择结果只由请求、能力和已文档化的偏好排序决定。Provider 实例不拥有 Pipeline、StateStore 或 Metrics。0.2.0 的 Reference Provider 只产生 `kReference` 测试 payload，不得解释为标准 H.264/H.265 码流。

## 图与节点

Pipeline 通过 `unique_ptr` 独占 Node；Node 独占自己的 Port；Graph 记录不拥有对象的连接关系。Pipeline 析构前停止节点。外部不得保存超过 Pipeline 生命周期的 Node/Port 指针。

类型化端口只允许连接相同媒体对象类型，并在运行前验证格式。一个输出可以扇出到多个输入；首期每个输入只能有一个上游。
