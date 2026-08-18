# 核心接口与所有权

> 状态：已接受  
> 适用版本：EAVP Core 0.1  
> 规范性范围：公共 C++ 接口和对象所有权

## 错误模型

`Status` 表达无返回值操作结果，`Result<T>` 同时携带值或失败状态。成功状态不能携带错误文本，失败 Result 不能读取值。公共模块边界不抛异常。

## 媒体对象

`Buffer` 使用 `shared_ptr` 共享底层 Storage，切片只改变视图范围。VideoFrame、AudioFrame 和 MediaPacket 是值对象，复制它们不会复制媒体字节。时间基准分母必须大于零。

## 图与节点

Pipeline 通过 `unique_ptr` 独占 Node；Node 独占自己的 Port；Graph 记录不拥有对象的连接关系。Pipeline 析构前停止节点。外部不得保存超过 Pipeline 生命周期的 Node/Port 指针。

类型化端口只允许连接相同媒体对象类型，并在运行前验证格式。一个输出可以扇出到多个输入；首期每个输入只能有一个上游。

