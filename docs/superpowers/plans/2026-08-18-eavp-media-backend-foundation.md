# EAVP Media Backend Foundation 0.2 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**目标：** 交付平台无关的媒体内存、格式、Capability、Provider、视频处理/编码后端接口和确定性 Reference Backend，使后续 oneVPL、MPP/RGA 与 `HI_MPI` 能在不侵入 Core 的前提下接入。

**架构：** `EAVP::media` 持有平台无关契约，`EAVP::platform` 是 Provider 注册、选择和 Pipeline 组合根。后端使用非阻塞 `configure/submit/receive/begin_drain/reset` 语义，Reference Backend 以 CPU 内存和确定性输出验证背压、延迟、drain 与设备丢失，不生成伪标准码流。

**技术栈：** C++11、POSIX、CMake 3.21、Ninja、GoogleTest 1.12.1、CTest；Core 不新增第三方运行时依赖。

**规格：** `docs/superpowers/specs/2026-08-18-eavp-media-backend-foundation-design.md`

## 全局约束

- 新增或修改的项目文档和说明性注释优先使用简体中文。
- 生产核心保持 C++11 与 POSIX 基线，公共头文件不得使用 C++14 或更高版本特性。
- 公共 API 使用 `Status`/`Result<T>`，异常不得跨模块边界。
- 依赖方向保持 `platform -> control -> media -> base` 和 `management -> base`。
- 公共头文件不得包含 FFmpeg、oneVPL、VA-API、MPP、RGA 或 `HI_MPI` 类型。
- 每个行为先写失败测试并确认失败原因，再写最小实现。
- 每个任务结束时运行相关测试和既有测试，并形成可独立审查的 Conventional Commit。
- 不创建真实硬件、容器或协议空实现；0.2 只交付 Reference Backend。

---

## 文件结构

### 新增公共接口

- `include/eavp/media/video_format.hpp`：PixelFormat、色彩描述、VideoFormat 与 Plane 校验。
- `include/eavp/media/video_codec.hpp`：编码配置、RateControlMode、EncodedStreamFormat 与 Codec 配置数据。
- `include/eavp/media/capability.hpp`：范围、处理/编码 Capability、选择请求和协商结果。
- `include/eavp/media/backend.hpp`：VideoProcessor、VideoEncoder、MediaBackendProvider 与生命周期接口。
- `include/eavp/media/backend_registry.hpp`：显式注册、冻结和确定性 Provider 选择。
- `include/eavp/media/reference_backend.hpp`：Reference Backend 选项与工厂。
- `include/eavp/media/backend_node.hpp`：通用 VideoProcessorNode 与 VideoEncoderNode。
- `include/eavp/platform/pipeline_query.hpp`：模拟平台与 Reference 平台共享的 Pipeline Query。
- `include/eavp/platform/reference_media_platform.hpp`：Reference Backend 端到端组合根。

### 新增实现文件

- `src/media/buffer.cpp`：CPU Storage、RAII 映射、DMABUF 句柄与 Buffer 视图。
- `src/media/video_format.cpp`：视频 Plane 和格式校验。
- `src/media/video_codec.cpp`：编码配置及 Packet 码流组合校验。
- `src/media/capability.cpp`：Capability 校验、匹配、评分和协商。
- `src/media/backend_registry.cpp`：Provider 注册、冻结与选择。
- `src/media/reference_backend.cpp`：确定性处理器、编码器和故障注入。
- `src/media/backend_node.cpp`：后端实例与 Port/Node 的非阻塞桥接。
- `src/platform/reference_media_platform.cpp`：Reference 视频纵切面、状态与指标适配。

### 新增测试

- `tests/unit/media_backend_test.cpp`：Capability、Registry、后端接口和 Reference Backend。
- `tests/support/backend_contract.hpp`：每个 Provider 可复用的契约测试函数。
- `tests/integration/reference_media_platform_test.cpp`：100 帧、Query、Metrics、Health 和设备丢失。

### 修改文件

- `include/eavp/base/result.hpp`：支持移动专有值和显式 `take_value()`。
- `include/eavp/base/status.hpp`：新增终止/设备/数据错误和结构化上下文。
- `include/eavp/media/buffer.hpp`：改为 Storage、Plane、映射和句柄模型。
- `include/eavp/media/frame.hpp`：VideoFrame 改用 VideoFormat 与多 Plane Buffer。
- `include/eavp/media/media_packet.hpp`：增加流格式、stream index 和 Codec 配置。
- `include/eavp/media/node.hpp`、`src/media/node.cpp`：区分预期背压、流结束和故障。
- `include/eavp/platform/simulated_platform.hpp`：改用共享 Pipeline Query 类型。
- `src/platform/simulated_platform.cpp`、相关既有测试和示例：迁移 Buffer/Packet API。
- `src/CMakeLists.txt`、测试 CMake：加入实现与测试。
- `CMakeLists.txt`、`README.md`、架构文档和路线图：版本、接口、依赖和迁移说明。

---

### Task 1：扩展 Status 并让 Result 支持专有所有权

**Files:**
- Modify: `include/eavp/base/status.hpp`
- Modify: `include/eavp/base/result.hpp`
- Modify: `tests/unit/base_test.cpp`

**Interfaces:**
- Produces: `StatusCode::{kEndOfStream,kDeviceLost,kCorruptData}`。
- Produces: 后端上下文构造函数及 `provider_id/operation/native_code` getters。
- Produces: move-only `Result<T>` 与 `T take_value()`。

- [ ] **Step 1: 写 Status 上下文和新错误码的失败测试**

```cpp
TEST(StatusTest, BackendFailurePreservesPortableAndNativeContext) {
    const eavp::Status status(eavp::StatusCode::kDeviceLost, "device disappeared",
                              "reference", "receive", -19);

    EXPECT_EQ(eavp::StatusCode::kDeviceLost, status.code());
    EXPECT_EQ("reference", status.provider_id());
    EXPECT_EQ("receive", status.operation());
    EXPECT_TRUE(status.has_native_code());
    EXPECT_EQ(-19, status.native_code());
}
```

- [ ] **Step 2: 写 Result 移动专有值的失败测试**

```cpp
TEST(ResultTest, MovesUniqueValueOutOfTheResult) {
    eavp::Result<std::unique_ptr<int> > result(
        std::unique_ptr<int>(new int(42)));
    ASSERT_TRUE(result.ok());
    std::unique_ptr<int> value = result.take_value();
    ASSERT_TRUE(value.get() != NULL);
    EXPECT_EQ(42, *value);
}
```

测试文件加入 `<memory>`。

- [ ] **Step 3: 运行失败测试并确认失败原因**

```bash
cmake --build --preset linux-debug-fetch-deps --target eavp_base_tests
ctest --test-dir build/linux-debug-fetch-deps -R 'StatusTest.BackendFailure|ResultTest.MovesUniqueValue' --output-on-failure
```

Expected: 编译失败，指出新枚举、Status 上下文构造函数或 `take_value()` 尚不存在。

- [ ] **Step 4: 实现最小 Status 上下文**

```cpp
Status(StatusCode code, const std::string& message, const std::string& provider_id,
       const std::string& operation, std::int64_t native_code)
    : code_(code), message_(message), provider_id_(provider_id),
      operation_(operation), native_code_(native_code), has_native_code_(true) {}

const std::string& provider_id() const { return provider_id_; }
const std::string& operation() const { return operation_; }
bool has_native_code() const { return has_native_code_; }
std::int64_t native_code() const { return native_code_; }
```

默认构造和现有二参数构造把 `native_code_` 初始化为 `0`、`has_native_code_` 初始化为 `false`。

- [ ] **Step 5: 实现 move-only Result**

内部值改为 `std::unique_ptr<T>`，并增加：

```cpp
Result(Result&& other) noexcept = default;
Result& operator=(Result&& other) noexcept = default;
Result(const Result&) = delete;
Result& operator=(const Result&) = delete;

T take_value() {
    assert(ok());
    return std::move(*value_);
}
```

现有 `value()` 的 const 与非 const 引用访问保持不变。

- [ ] **Step 6: 运行 Base 和全量现有测试**

```bash
cmake --build --preset linux-debug-fetch-deps
ctest --preset linux-debug-fetch-deps --output-on-failure
```

Expected: 新增测试和既有测试全部通过。

- [ ] **Step 7: 提交**

```bash
git add include/eavp/base/status.hpp include/eavp/base/result.hpp tests/unit/base_test.cpp
git commit -m "feat(base): 扩展后端错误与专有结果"
```

---

### Task 2：实现平台无关 Buffer Storage、Plane 和 RAII 映射

**Files:**
- Modify: `include/eavp/media/buffer.hpp`
- Create: `src/media/buffer.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/media_object_test.cpp`

**Interfaces:**
- Consumes: move-only `Result<T>` 和 `take_value()`。
- Produces: `MemoryDomain`、`MapMode`、`PlaneLayout`、`NativeBufferHandle`、`MappedRegion`、`BufferStorage`。
- Produces: `Buffer::allocate/create/map_plane/export_dmabuf/slice/slice_plane`。

- [ ] **Step 1: 写 CPU 映射、共享和释放顺序的失败测试**

```cpp
TEST(BufferTest, CpuPlaneMappingSharesStorageAndUnmapsOnScopeExit) {
    eavp::Buffer buffer = eavp::Buffer::allocate(8U).take_value();
    {
        eavp::MappedRegion mapped =
            buffer.map_plane(0U, eavp::MapMode::kReadWrite).take_value();
        ASSERT_EQ(8U, mapped.size());
        mapped.mutable_data()[3] = 0x5a;
    }
    const eavp::Buffer copy = buffer;
    eavp::MappedRegion mapped =
        copy.map_plane(0U, eavp::MapMode::kReadOnly).take_value();
    EXPECT_EQ(0x5a, mapped.data()[3]);
    EXPECT_EQ(eavp::MemoryDomain::kCpu, copy.memory_domain());
}
```

- [ ] **Step 2: 写 Plane 边界和不可映射存储的失败测试**

定义 `UnmappableStorage : public BufferStorage`，其 `map` 返回 `kUnsupported`：

```cpp
TEST(BufferTest, RejectsPlaneBeyondStorageAndReportsUnmappableDeviceMemory) {
    std::shared_ptr<eavp::BufferStorage> storage(new UnmappableStorage(64U));
    std::vector<eavp::PlaneLayout> invalid;
    invalid.push_back(eavp::PlaneLayout(48U, 32U, 16U));
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::Buffer::create(storage, invalid).status().code());

    std::vector<eavp::PlaneLayout> valid;
    valid.push_back(eavp::PlaneLayout(0U, 64U, 16U));
    eavp::Buffer buffer = eavp::Buffer::create(storage, valid).take_value();
    EXPECT_EQ(eavp::StatusCode::kUnsupported,
              buffer.map_plane(0U, eavp::MapMode::kReadOnly).status().code());
}
```

- [ ] **Step 3: 写 DMABUF 句柄所有权测试**

使用 `pipe()` 创建有效文件描述符，让测试 Storage 在 `export_dmabuf()` 中返回复制句柄。销毁 `NativeBufferHandle` 后用 `fcntl(fd, F_GETFD)` 断言复制 fd 已关闭、原 fd 仍有效。

- [ ] **Step 4: 运行测试并确认新类型缺失**

```bash
cmake --build --preset linux-debug-fetch-deps --target eavp_media_object_tests
```

Expected: 编译失败，指出 `MemoryDomain`、`MappedRegion`、`BufferStorage` 和新 Buffer API 尚不存在。

- [ ] **Step 5: 实现 Buffer 公共契约**

```cpp
enum class MemoryDomain { kCpu, kMmap, kDmaBuf, kDeviceOpaque };
enum class MapMode { kReadOnly, kReadWrite };

struct PlaneLayout {
    PlaneLayout(std::size_t offset_value, std::size_t size_value,
                std::size_t stride_value)
        : offset(offset_value), size(size_value), stride(stride_value) {}
    std::size_t offset;
    std::size_t size;
    std::size_t stride;
};

class BufferStorage {
public:
    virtual ~BufferStorage() {}
    virtual MemoryDomain memory_domain() const = 0;
    virtual std::size_t capacity() const = 0;
    virtual const std::string& provider_id() const = 0;
    virtual Status map(MapMode, std::uint8_t**, std::size_t*) = 0;
    virtual Status unmap() = 0;
    virtual Result<NativeBufferHandle> export_dmabuf() const = 0;
};
```

`MappedRegion` 与 `NativeBufferHandle` 不可复制、可移动，析构时分别 unmap 与 close。

- [ ] **Step 6: 实现 CPU Storage 与 Buffer 校验**

`buffer.cpp` 内部实现 `CpuBufferStorage`。`Buffer::create` 使用 `offset > capacity` 或 `size > capacity - offset` 避免溢出，并拒绝 stride 为零和未声明重叠 Plane。`map_plane` 先映射 Storage，再把区域限制到目标 Plane；失败时不得留下活动映射。`slice` 只接受单 Plane Buffer，多 Plane 返回 `kUnsupported`；`slice_plane` 显式选择 Plane 并重新执行边界校验。

- [ ] **Step 7: 迁移当前媒体对象测试并运行**

所有直接 `data()`/`mutable_data()` 访问改成 `map_plane()`：

```bash
cmake --build --preset linux-debug-fetch-deps --target eavp_media_object_tests
ctest --test-dir build/linux-debug-fetch-deps -R 'BufferTest' --output-on-failure
```

Expected: 所有 BufferTest 通过，编译器无警告。

- [ ] **Step 8: 运行全量测试并提交**

```bash
ctest --preset linux-debug-fetch-deps --output-on-failure
git add include/eavp/media/buffer.hpp src/media/buffer.cpp src/CMakeLists.txt tests/unit/media_object_test.cpp
git commit -m "feat(media): 引入跨平台媒体内存模型"
```

---

### Task 3：实现 VideoFormat、编码配置和明确码流格式

**Files:**
- Create: `include/eavp/media/video_format.hpp`
- Create: `src/media/video_format.cpp`
- Create: `include/eavp/media/video_codec.hpp`
- Create: `src/media/video_codec.cpp`
- Modify: `include/eavp/media/frame.hpp`
- Modify: `include/eavp/media/media_packet.hpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/media_object_test.cpp`
- Modify: `tests/unit/media_runtime_test.cpp`
- Modify: `src/platform/simulated_platform.cpp`

**Interfaces:**
- Consumes: Buffer Plane 和 MemoryDomain。
- Produces: `VideoFormat::create`、`VideoFrame::create(Buffer, VideoFormat, pts, TimeBase)`。
- Produces: `VideoProcessorConfig`、`VideoEncoderConfig`、`EncodedStreamFormat`、`CodecConfigData`。
- Produces: `MediaPacket::create`，禁止调用方猜测 Annex-B、AVCC 或 HVCC。

- [ ] **Step 1: 写 NV12/RGB24 Plane 校验的失败测试**

```cpp
TEST(VideoFormatTest, ValidatesPixelFormatPlaneCountStrideAndSize) {
    std::vector<eavp::PlaneLayout> nv12_planes;
    nv12_planes.push_back(eavp::PlaneLayout(0U, 128U, 16U));
    nv12_planes.push_back(eavp::PlaneLayout(128U, 64U, 16U));
    EXPECT_TRUE(eavp::VideoFormat::create(eavp::PixelFormat::kNv12, 16, 8,
                                          eavp::MemoryDomain::kCpu,
                                          nv12_planes).ok());

    nv12_planes.pop_back();
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::VideoFormat::create(eavp::PixelFormat::kNv12, 16, 8,
                                         eavp::MemoryDomain::kCpu,
                                         nv12_planes).status().code());
}
```

- [ ] **Step 2: 写 VideoFrame Buffer/Format 不匹配测试**

创建 192 字节、两个 Plane 的 Buffer 和 NV12 VideoFormat，验证 Frame 成功；再以单 Plane Buffer 创建同一 Frame，期望 `kCapabilityMismatch`。

- [ ] **Step 3: 写 MediaPacket 码流组合测试**

```cpp
TEST(MediaPacketTest, RejectsCodecAndStreamFormatMismatch) {
    eavp::Buffer payload = eavp::Buffer::allocate(4U).take_value();
    const eavp::TimeBase time_base = eavp::TimeBase::create(1, 90000).take_value();

    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::MediaPacket::create(
                  payload, eavp::CodecId::kH264,
                  eavp::EncodedStreamFormat::kHvcc, 0, 0, 0, 3600,
                  time_base, true, eavp::CodecConfigData()).status().code());
}
```

- [ ] **Step 4: 运行失败测试**

```bash
cmake --build --preset linux-debug-fetch-deps --target eavp_media_object_tests
```

Expected: 编译失败，指出 VideoFormat、VideoEncoderConfig、EncodedStreamFormat 和 Packet factory 尚不存在。

- [ ] **Step 5: 实现 VideoFormat 校验**

支持 `kRgb24` 单 Plane、`kNv12` 双 Plane、`kYuv420p` 三 Plane。奇数尺寸在 0.2 返回 `kUnsupported`，避免未定义 chroma rounding。校验最小 stride 和每 Plane 最小 size；VideoFormat 保存 PixelFormat、宽高、MemoryDomain、PlaneLayout 和显式色彩字段。

- [ ] **Step 6: 实现编码配置与 Packet factory**

```cpp
enum class RateControlMode { kCbr, kVbr, kConstantQuality };
enum class EncodedStreamFormat { kUnknown, kAnnexB, kAvcc, kHvcc, kReference };
enum class CodecProfile {
    kUnknown,
    kH264Baseline,
    kH264Main,
    kH264High,
    kH265Main,
};

struct VideoProcessorConfig {
    VideoFormat input_format;
    VideoFormat output_format;
    int crop_x;
    int crop_y;
    int crop_width;
    int crop_height;
    int rotation_degrees;
};

struct VideoEncoderConfig {
    CodecId codec;
    int width;
    int height;
    int frame_rate_numerator;
    int frame_rate_denominator;
    std::int64_t target_bitrate;
    std::int64_t max_bitrate;
    int gop_length;
    int b_frames;
    RateControlMode rate_control;
    CodecProfile profile;
    int level_idc;
    bool low_latency;
};

struct CodecConfigData {
    std::vector<std::uint8_t> bytes;
};
```

增加 `CodecId::kReference`。`MediaPacket::create` 对 H.264、H.265、Reference 与流格式进行组合校验，并保存 stream index 和 CodecConfigData。

- [ ] **Step 7: 迁移现有 Frame、Packet 和模拟平台调用**

所有旧构造调用改成 factory，所有 Buffer 写入改成映射。既有模拟 Packet 使用 `CodecId::kReference` 与 `EncodedStreamFormat::kReference`，避免把测试字节标记为 H.264。

- [ ] **Step 8: 运行媒体对象、运行时和集成测试**

```bash
cmake --build --preset linux-debug-fetch-deps
ctest --preset linux-debug-fetch-deps --output-on-failure
```

Expected: 新格式测试与既有测试全部通过。

- [ ] **Step 9: 提交**

```bash
git add include/eavp/media src/media tests/unit src/platform/simulated_platform.cpp
git commit -m "feat(media): 明确视频格式与编码码流"
```

---

### Task 4：实现 Capability、请求匹配和确定性评分

**Files:**
- Create: `include/eavp/media/capability.hpp`
- Create: `src/media/capability.cpp`
- Modify: `src/CMakeLists.txt`
- Create: `tests/unit/media_backend_test.cpp`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Consumes: VideoFormat、VideoEncoderConfig、MemoryDomain。
- Produces: `DimensionRange`、`VideoProcessorCapability`、`VideoEncoderCapability`、`ProviderCapability`。
- Produces: 将规格中的 SelectionRequest 分为类型安全的 `VideoProcessorRequest`、`VideoEncoderRequest` 与对应匹配结果。

- [ ] **Step 1: 写范围、对齐和内存域匹配失败测试**

```cpp
TEST(CapabilityTest, RejectsResolutionAlignmentAndMemoryDomainMismatch) {
    const eavp::VideoEncoderCapability capability =
        make_h264_capability_for_test();

    EXPECT_FALSE(capability.supports(make_encoder_request(
        1919, 1080, eavp::MemoryDomain::kDmaBuf)));
    EXPECT_FALSE(capability.supports(make_encoder_request(
        1920, 1080, eavp::MemoryDomain::kCpu)));
    EXPECT_TRUE(capability.supports(make_encoder_request(
        1920, 1080, eavp::MemoryDomain::kDmaBuf)));
}
```

同一测试文件定义以下 helper；它们只用字面量填充 value object，不调用生产匹配逻辑生成期望值：

```cpp
eavp::VideoEncoderCapability make_h264_capability_for_test();
eavp::VideoEncoderRequest make_encoder_request(
    int width, int height, eavp::MemoryDomain memory_domain);
```

`make_h264_capability_for_test` 固定 width range `(16, 1920, 2, 16)`、height range `(16, 1088, 2, 16)`、H.264、NV12、DMABUF、CBR 和 zero-copy=true。

- [ ] **Step 2: 写 Required 与 Preferred 分离测试**

验证 `require_zero_copy=true` 会排除不支持零拷贝的候选；`prefer_hardware=true` 只影响候选排序，不把软件候选判为无效。

- [ ] **Step 3: 运行失败测试**

```bash
cmake --build --preset linux-debug-fetch-deps --target eavp_media_backend_tests
```

Expected: 编译失败，Capability 类型和匹配函数尚不存在。

- [ ] **Step 4: 实现 DimensionRange 和 Capability value objects**

```cpp
enum class ProviderKind { kReference, kSoftware, kHardware };

class DimensionRange {
public:
    DimensionRange(int minimum, int maximum, int step, int alignment);
    bool valid() const;
    bool contains(int value) const;
};
```

`DimensionRange::contains` 同时检查 min/max、step 和 alignment。`ProviderCapability` 保存 Provider ID、实现版本、设备 ID、可用状态、ProviderKind、处理与编码能力列表。

- [ ] **Step 5: 实现匹配和协商结果**

匹配函数返回 `Status` 形式的拒绝原因；布尔 `supports` 只封装该结果。必需字段不允许静默修改。偏好评分只使用 preferred provider rank、ProviderKind、zero-copy 和稳定 Provider ID。

- [ ] **Step 6: 运行 Capability 测试与全量测试**

```bash
cmake --build --preset linux-debug-fetch-deps
ctest --test-dir build/linux-debug-fetch-deps -R 'CapabilityTest' --output-on-failure
ctest --preset linux-debug-fetch-deps --output-on-failure
```

- [ ] **Step 7: 提交**

```bash
git add include/eavp/media/capability.hpp src/media/capability.cpp src/CMakeLists.txt tests/unit
git commit -m "feat(media): 定义媒体后端能力模型"
```

---

### Task 5：定义后端生命周期并实现 Registry

**Files:**
- Create: `include/eavp/media/backend.hpp`
- Create: `include/eavp/media/backend_registry.hpp`
- Create: `src/media/backend_registry.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/media_backend_test.cpp`

**Interfaces:**
- Consumes: Capability、VideoFrame、MediaPacket、move-only Result。
- Produces: `BackendState`、`VideoProcessor`、`VideoEncoder`、`MediaBackendProvider`。
- Produces: `BackendRegistry::{register_provider,freeze,select_video_processor,select_video_encoder}`。
- Produces: `ProcessorSelection` 与 `EncoderSelection`。

- [ ] **Step 1: 写 Registry 重复注册和冻结测试**

```cpp
TEST(BackendRegistryTest, RejectsDuplicateAndRegistrationAfterFreeze) {
    eavp::BackendRegistry registry;
    std::shared_ptr<eavp::MediaBackendProvider> first =
        make_test_provider("same", true);
    std::shared_ptr<eavp::MediaBackendProvider> second =
        make_test_provider("same", true);

    ASSERT_TRUE(registry.register_provider(first).ok());
    EXPECT_EQ(eavp::StatusCode::kAlreadyExists,
              registry.register_provider(second).code());
    ASSERT_TRUE(registry.freeze().ok());
    EXPECT_EQ(eavp::StatusCode::kInvalidState,
              registry.register_provider(make_test_provider("late", true)).code());
}
```

在测试文件定义 `TestBackendProvider`，构造函数接收 Provider ID、可用状态、ProviderKind 和 zero-copy 标志；`make_test_provider(id, available)` 返回该类型的 `shared_ptr<MediaBackendProvider>`，其 factory 返回最小 TestProcessor/TestEncoder。

- [ ] **Step 2: 写确定性选择与显式 Provider 不回退测试**

注册两个都满足 Required 的 Provider，反向改变注册顺序并验证选择结果相同。显式请求不可用 Provider 时期望原始不可用 Status，不得自动选择另一个候选。

- [ ] **Step 3: 写后端接口编译契约测试**

定义最小 TestEncoder，实现以下接口并验证可经 `unique_ptr<VideoEncoder>` 销毁：

```cpp
class VideoEncoder {
public:
    virtual ~VideoEncoder() {}
    virtual BackendState state() const = 0;
    virtual Status configure(const VideoFormat& input,
                             const VideoEncoderConfig& config) = 0;
    virtual Status submit(const std::shared_ptr<const VideoFrame>& frame) = 0;
    virtual Result<std::shared_ptr<const MediaPacket> > receive() = 0;
    virtual Status begin_drain() = 0;
    virtual Status reset() = 0;
};
```

VideoProcessor 使用同样生命周期，输入输出都是 `VideoFrame`。

后端实例在第一次 configure 时记录 `std::this_thread::get_id()`；后续来自不同线程的 submit、receive、begin_drain 或 reset 返回 `kInvalidState`。在本步骤增加跨线程调用测试，证明 Executor affinity 可观察。

- [ ] **Step 4: 运行失败测试**

```bash
cmake --build --preset linux-debug-fetch-deps --target eavp_media_backend_tests
```

Expected: Backend、Provider、Registry 和 Selection 类型尚不存在。

- [ ] **Step 5: 实现后端抽象和 Provider factory**

Provider factory 返回 `Result<std::unique_ptr<VideoProcessor> >` 和 `Result<std::unique_ptr<VideoEncoder> >`。调用方使用 `take_value()` 转移所有权。Provider 只读持有 Capability，不引用 Pipeline、StateStore 或 Metrics。

- [ ] **Step 6: 实现确定性 Registry**

Registry 内部按 Provider ID 保存共享只读 Provider。选择时先收集 Required 匹配候选，再按以下顺序排序：

1. `preferred_provider_ids` 中的显式位置；未列出的排在其后。
2. 当 `prefer_hardware` 为真时，Hardware 优于 Software，Software 优于 Reference。
3. 当 `prefer_zero_copy` 为真时，零拷贝候选优先。
4. Provider ID 字典序打破平局。

无候选时返回 `kCapabilityMismatch`，message 汇总每个候选的首个拒绝原因。

- [ ] **Step 7: 运行 Registry、Capability 和全量测试**

```bash
cmake --build --preset linux-debug-fetch-deps
ctest --test-dir build/linux-debug-fetch-deps -R 'BackendRegistryTest|CapabilityTest' --output-on-failure
ctest --preset linux-debug-fetch-deps --output-on-failure
```

- [ ] **Step 8: 提交**

```bash
git add include/eavp/media/backend.hpp include/eavp/media/backend_registry.hpp src/media/backend_registry.cpp src/CMakeLists.txt tests/unit/media_backend_test.cpp
git commit -m "feat(media): 实现后端注册与确定性选择"
```

---

### Task 6：实现 Reference Backend 与可复用契约测试

**Files:**
- Create: `include/eavp/media/reference_backend.hpp`
- Create: `src/media/reference_backend.cpp`
- Create: `tests/support/backend_contract.hpp`
- Modify: `tests/unit/media_backend_test.cpp`
- Modify: `tests/unit/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

**Interfaces:**
- Consumes: MediaBackendProvider、VideoProcessor、VideoEncoder。
- Produces: `ReferenceBackendOptions` 和 `create_reference_backend(options)`。
- Produces: `run_backend_contract(provider)`、`make_reference_frame(pts)` 和 `create_configured_reference_encoder(options)` 测试 helper，后续真实 Provider 测试直接复用。

- [ ] **Step 1: 写 Reference Provider Capability 契约失败测试**

```cpp
TEST(ReferenceBackendTest, AdvertisesOnlyTheBehaviorItImplements) {
    std::shared_ptr<eavp::MediaBackendProvider> provider =
        eavp::create_reference_backend(eavp::ReferenceBackendOptions());
    const eavp::ProviderCapability capability = provider->probe().take_value();

    EXPECT_EQ("reference", capability.provider_id());
    EXPECT_EQ(eavp::ProviderKind::kReference, capability.kind());
    EXPECT_TRUE(capability.available());
    EXPECT_TRUE(capability.supports_reference_encoding());
    EXPECT_FALSE(capability.supports_h264_encoding());
}
```

- [ ] **Step 2: 写处理器非阻塞、背压和 reset 失败测试**

配置容量为 1 的 Reference Processor：第一次 submit 成功，第二次返回 `kWouldBlock`；receive 返回与输入共享 Buffer 的 Frame；reset 后状态回到 Created 且队列为空。

- [ ] **Step 3: 写编码延迟、drain 和 EOS 失败测试**

```cpp
TEST(ReferenceBackendTest, EncoderDelaysOutputAndDrainEndsExactlyOnce) {
    eavp::ReferenceBackendOptions options;
    options.output_delay = 1U;
    std::unique_ptr<eavp::VideoEncoder> encoder =
        create_configured_reference_encoder(options);

    ASSERT_TRUE(encoder->submit(make_reference_frame(0)).ok());
    EXPECT_EQ(eavp::StatusCode::kWouldBlock, encoder->receive().status().code());
    ASSERT_TRUE(encoder->submit(make_reference_frame(1)).ok());
    EXPECT_EQ(0, encoder->receive().value()->pts());

    ASSERT_TRUE(encoder->begin_drain().ok());
    EXPECT_EQ(1, encoder->receive().value()->pts());
    EXPECT_EQ(eavp::StatusCode::kEndOfStream,
              encoder->receive().status().code());
}
```

- [ ] **Step 4: 写设备丢失上下文失败测试**

设置 `device_lost_after_submissions = 2U`，第三次 submit 返回 `kDeviceLost`，Status 的 provider 为 `reference`、operation 为 `submit`，实例进入 Error。

- [ ] **Step 5: 运行失败测试**

```bash
cmake --build --preset linux-debug-fetch-deps --target eavp_media_backend_tests
ctest --test-dir build/linux-debug-fetch-deps -R 'ReferenceBackendTest' --output-on-failure
```

Expected: ReferenceBackend 工厂和行为尚不存在。

- [ ] **Step 6: 实现 Reference Processor**

```cpp
struct ReferenceBackendOptions {
    ReferenceBackendOptions()
        : queue_capacity(4U), output_delay(0U),
          device_lost_after_submissions(0U) {}
    std::size_t queue_capacity;
    std::size_t output_delay;
    std::size_t device_lost_after_submissions;
};
```

值 `0U` 表示不注入设备丢失。Processor 只接受输入输出格式完全相同的 CPU Frame，并按队列顺序返回共享 Frame；不得声明缩放或格式转换能力。

- [ ] **Step 7: 实现 Reference Encoder**

Encoder 将每帧生成 16 字节确定性 payload，CodecId 与 EncodedStreamFormat 都使用 `kReference`。payload 内容由固定四字节 magic、八字节 PTS 和四字节输入首 Plane 校验值组成，字节序固定为网络序。该格式只用于测试，不作为公开容器输入。

`receive` 在未 drain 且队列数量不超过 output_delay 时返回 `kWouldBlock`；drain 后输出全部剩余 Packet，随后稳定返回 `kEndOfStream`。reset 清空队列、计数器和错误状态。

- [ ] **Step 8: 抽取 Backend Contract Tests 并运行**

`tests/support/backend_contract.hpp` 提供接收 Provider factory 的测试 helper，覆盖 configure、背压、drain、reset、Capability 一致性和 Buffer 生命周期。Reference Backend 测试调用这些 helper，并保留 Reference 特有的确定性 payload 断言。

测试支持头精确定义：

```cpp
std::shared_ptr<const eavp::VideoFrame> make_reference_frame(std::int64_t pts);
std::unique_ptr<eavp::VideoEncoder> create_configured_reference_encoder(
    const eavp::ReferenceBackendOptions& options);
void run_backend_contract(const std::shared_ptr<eavp::MediaBackendProvider>& provider);
```

```bash
cmake --build --preset linux-debug-fetch-deps
ctest --test-dir build/linux-debug-fetch-deps -R 'ReferenceBackendTest|BackendContractTest' --output-on-failure
ctest --preset linux-debug-fetch-deps --output-on-failure
```

- [ ] **Step 9: 提交**

```bash
git add include/eavp/media/reference_backend.hpp src/media/reference_backend.cpp tests/support/backend_contract.hpp tests/unit/media_backend_test.cpp tests/unit/CMakeLists.txt src/CMakeLists.txt
git commit -m "feat(media): 实现确定性参考后端"
```

---

### Task 7：实现通用后端 Node 与 Reference 端到端纵切面

**Files:**
- Create: `include/eavp/media/backend_node.hpp`
- Create: `src/media/backend_node.cpp`
- Modify: `include/eavp/media/node.hpp`
- Modify: `src/media/node.cpp`
- Create: `include/eavp/platform/reference_media_platform.hpp`
- Create: `include/eavp/platform/pipeline_query.hpp`
- Modify: `include/eavp/platform/simulated_platform.hpp`
- Create: `src/platform/reference_media_platform.cpp`
- Create: `tests/integration/reference_media_platform_test.cpp`
- Modify: `tests/integration/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/media_backend_test.cpp`

**Interfaces:**
- Consumes: Processor/Encoder 实例、类型化 Port、BackendRegistry、Control/Management。
- Produces: `VideoProcessorNode`、`VideoEncoderNode`。
- Produces: `ReferenceMediaPlatform` 的 initialize、dispatch、reconcile、tick、query、metrics 和 health。

- [ ] **Step 1: 写 VideoEncoderNode 背压不丢帧失败测试**

构建容量为 1 的输入/输出端口和 Reference Encoder。让输出队列先占满，调用 Node tick 后断言待发 Packet 被 Node 保留；消费下游 Packet 后再次 tick，PTS 顺序仍为 0、1。

- [ ] **Step 2: 写 100 帧 Reference Platform 集成失败测试**

```cpp
TEST(ReferenceMediaPlatformTest, ProcessesOneHundredFramesAndPublishesSelection) {
    eavp::ReferenceMediaPlatform platform;
    ASSERT_TRUE(platform.initialize().ok());
    ASSERT_TRUE(platform.dispatch(
        eavp::StartPipelineCommand("start-ref", "integration", "reference0")).ok());
    ASSERT_TRUE(platform.reconcile_once().ok());
    ASSERT_TRUE(platform.tick(100U).ok());

    const eavp::StateSnapshot snapshot =
        platform.query(eavp::PipelineStateQuery("reference0")).take_value();
    EXPECT_EQ("running", snapshot.get("/pipelines/reference0/state")
                             .value().as_string().value());
    EXPECT_EQ("reference", snapshot.get("/pipelines/reference0/provider")
                               .value().as_string().value());
    EXPECT_EQ(100U, platform.metrics().counter("media.frames.encoded").value());
    EXPECT_EQ(eavp::HealthStatus::kOk, platform.health().aggregate());
}
```

- [ ] **Step 3: 写设备丢失状态收敛失败测试**

构造在第 3 次 submit 后失败的平台，启动并 tick；断言返回 `kDeviceLost`，Desired 仍为 running，Actual state 为 error，error 字段含 provider/operation，Health aggregate 为 Error。

- [ ] **Step 4: 运行失败测试**

```bash
cmake --build --preset linux-debug-fetch-deps --target eavp_media_backend_tests eavp_reference_media_platform_tests
```

Expected: Backend Node 和 ReferenceMediaPlatform 尚不存在。

- [ ] **Step 5: 实现通用 Processor/Encoder Node**

每个 Node 保存一个 `pending_output_`。`on_tick` 的固定顺序为：

1. 尝试发送 pending output；下游 `WOULD_BLOCK` 时立即返回且保留对象。
2. 从后端 receive 一次；成功则保存并尝试发送。
3. 从输入 Port receive 一次；`kNotFound` 转为成功空 tick。
4. submit 输入；`WOULD_BLOCK` 时把输入保存为 `pending_input_`，后续 tick 先重试。

`kWouldBlock`、`kNotFound` 和 `kEndOfStream` 不使 MediaNode 进入 Error；`kDeviceLost` 与其他失败保持现有 Error 行为。Node 的 `on_stop` 调用 `begin_drain`，持续接收可立即获得的输出并发送；返回 `kWouldBlock` 时保留 Draining 状态，后续 stop 调用继续排空，返回 `kEndOfStream` 后 reset 并进入 Stopped。

- [ ] **Step 6: 实现 ReferenceMediaPlatform**

平台显式注册并冻结 Reference Provider，选择 CPU Reference Processor/Encoder，组装：

```text
ReferenceFrameSource -> VideoProcessorNode -> VideoEncoderNode -> ReferencePacketSink
```

FrameSource 每 tick 生成一帧 16x16 RGB24 CPU Frame。PacketSink 递增 `media.frames.encoded` 并记录队列深度。平台在 Actual Store 发布 state、provider、pixel format、memory domain；tick 故障时写入 error 并更新 Health。

- [ ] **Step 7: 实现显式恢复路径**

ReferenceMediaPlatform 以 `unique_ptr<MediaPipeline>` 和 `unique_ptr<PipelineReconciler>` 持有可重建对象，并提供 `reset_pipeline()`。该方法只允许 Pipeline 为 Error 时调用：完整停止并销毁后端实例、重新 probe/select、重建 Pipeline，然后调用 Reconciler 恢复 Desired。测试验证恢复不会复用旧 Buffer 或旧实例计数。

- [ ] **Step 8: 运行单元、集成和全量测试**

```bash
cmake --build --preset linux-debug-fetch-deps
ctest --test-dir build/linux-debug-fetch-deps -R 'BackendNodeTest|ReferenceMediaPlatformTest' --output-on-failure
ctest --preset linux-debug-fetch-deps --output-on-failure
```

Expected: 100 帧计数准确，故障和恢复测试通过，既有模拟平台不回归。

- [ ] **Step 9: 提交**

```bash
git add include/eavp/media/backend_node.hpp src/media/backend_node.cpp include/eavp/media/node.hpp src/media/node.cpp include/eavp/platform/pipeline_query.hpp include/eavp/platform/simulated_platform.hpp include/eavp/platform/reference_media_platform.hpp src/platform/reference_media_platform.cpp tests/integration/reference_media_platform_test.cpp tests/integration/CMakeLists.txt src/CMakeLists.txt tests/unit/media_backend_test.cpp
git commit -m "feat(platform): 贯通参考媒体后端纵切面"
```

---

### Task 8：完成版本、安装、迁移文档和验证矩阵

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `README.md`
- Modify: `docs/architecture/core-contracts.md`
- Modify: `docs/architecture/threading-and-lifecycle.md`
- Modify: `docs/architecture/testing-strategy.md`
- Modify: `docs/architecture/versioning-and-abi.md`
- Modify: `docs/roadmap.md`
- Create: `docs/migrations/0.1-to-0.2.md`
- Modify: `tests/consumer/main.cpp`
- Modify: `docs/superpowers/plans/2026-08-18-eavp-media-backend-foundation.md`

**Interfaces:**
- Consumes: 完整 0.2 公共 API。
- Produces: EAVP 0.2.0 安装包、迁移说明、架构基线和验证记录。

- [x] **Step 1: 扩展安装消费测试使其先失败**

在 `tests/consumer/main.cpp` 创建 CPU Buffer、VideoFormat、BackendRegistry 和 Reference Provider，完成一次选择。先不修改安装规则，运行：

```bash
ctest --test-dir build/linux-debug-fetch-deps -R eavp.install_consumer --output-on-failure
```

Expected: 消费工程因新头文件、源文件或导出配置未完整安装而失败。

- [x] **Step 2: 修正安装导出并升级版本**

把项目版本改为 `0.2.0`。确认新增头文件由现有 `install(DIRECTORY include/eavp/media ...)` 安装，所有非头文件实现都加入 `eavp_media`，ReferenceMediaPlatform 加入 `eavp_platform`。不得新增第三方 `find_package`。

- [x] **Step 3: 编写迁移说明和架构更新**

`docs/migrations/0.1-to-0.2.md` 给出以下前后对照：

- `data()/mutable_data()` 到 `map_plane()`。
- 单 stride VideoFrame 到 VideoFormat/PlaneLayout。
- MediaPacket 构造函数到 `MediaPacket::create` 与 EncodedStreamFormat。
- move-only Result 的 `take_value()`。
- Provider 注册、冻结和选择的完整最小示例。

架构文档更新状态、适用版本和规范性范围；路线图把 Media Backend Foundation 标记为当前里程碑，并保留后续独立规格顺序。

- [x] **Step 4: 运行格式和占位符检查**

```bash
git diff --check
if rg -n 'T[O]DO|T[B]D|待[定]|未[决]|[?][?][?]|PLACE[HOLDER]' README.md docs include src tests; then exit 1; fi
```

Expected: 两个命令退出码均为 0。

- [x] **Step 5: 运行 Debug 验证**

```bash
cmake --preset linux-debug-fetch-deps
cmake --build --preset linux-debug-fetch-deps
ctest --preset linux-debug-fetch-deps --output-on-failure
```

Expected: 所有单元、集成和安装消费测试通过。

- [x] **Step 6: 运行 Release 验证**

```bash
cmake --preset linux-release-fetch-deps
cmake --build --preset linux-release-fetch-deps
ctest --preset linux-release-fetch-deps --output-on-failure
```

Expected: 所有测试通过，编译器无警告。

- [x] **Step 7: 运行 ASan/UBSan 验证**

```bash
cmake --preset linux-asan-fetch-deps
cmake --build --preset linux-asan-fetch-deps
ctest --preset linux-asan-fetch-deps --output-on-failure
```

Expected: 所有测试通过，无 sanitizer 报告、文件描述符或映射生命周期错误。

- [x] **Step 8: 运行三套 ARM 交叉构建**

```bash
cmake --preset rockchip-armhf-release
cmake --build --preset rockchip-armhf-release
cmake --preset hisiv600-release
cmake --build --preset hisiv600-release
cmake --preset aarch64-release
cmake --build --preset aarch64-release
```

Expected: 三套预设均使用对应交叉编译器完成五个公开 target 和 0.2 实现的编译链接，不下载或运行测试；ARM32 对象为 `Machine: ARM`，aarch64 对象为 `Machine: AArch64`。只有三套实际通过后才勾选本步骤。

- [x] **Step 9: 记录验证结果并勾选计划**

在本计划末尾增加“验证记录”，写明每个 preset 的测试数量、结果和交叉构建结论；把已执行任务的 checkbox 改为 `[x]`。记录必须来自本次实际命令输出。

- [x] **Step 10: 提交**

```bash
git add CMakeLists.txt README.md docs tests/consumer/main.cpp
git commit -m "docs: 完成 Media Backend Foundation 0.2 基线"
```

---

## 完成条件

- 所有八个任务均形成独立、可构建、可测试的提交。
- 规格中的 0.2.0 验收标准全部有对应自动化测试或构建证据。
- Core 不新增第三方运行时依赖。
- Reference Backend 不生成或宣称标准 H.264/H.265 码流。
- 真实硬件、容器和协议仍由后续独立规格管理。

## 验证记录

- 2026-08-20：先修改 `tests/consumer/main.cpp`，未修改安装规则即执行 `ctest --test-dir build/linux-debug-fetch-deps -R eavp.install_consumer --output-on-failure`，结果 `1/1` 通过。该特征化结果表明既有 `install(DIRECTORY include/eavp/media ...)`、target source 和导出配置已经完整覆盖扩展 consumer；按裁定未人为破坏安装规则制造失败。
- 2026-08-20：`cmake --preset linux-debug-fetch-deps`、`cmake --build --preset linux-debug-fetch-deps` 和 `ctest --preset linux-debug-fetch-deps --output-on-failure` 均退出 0，CTest 为 `95/95` 通过，包含安装消费。
- 2026-08-20：`cmake --preset linux-release-fetch-deps`、`cmake --build --preset linux-release-fetch-deps` 和 `ctest --preset linux-release-fetch-deps --output-on-failure` 均退出 0，CTest 为 `95/95` 通过，编译启用 `EAVP_WARNINGS_AS_ERRORS=ON`。
- 2026-08-20：生命周期回归 RED 为 `ctest --test-dir build/linux-asan-fetch-deps -R 'ReferenceMediaPlatformTest.(ProcessesOneHundredFramesAndPublishesSelection|DeviceLossPublishesStructuredErrorAndExplicitResetRebuildsPipeline)' --output-on-failure`，结果 `0/2`，AddressSanitizer 报告析构期 heap-use-after-free。通过调整 ReferenceMediaPlatform 和 SimulatedPlatform 成员声明顺序，使 pipeline/reconciler 在其借用的服务之前析构；同一聚焦命令转为 `2/2`。随后 `ctest --preset linux-asan-fetch-deps --output-on-failure` 为 `95/95`，无 sanitizer 报告，Step 7 已勾选。
- 2026-08-20：`command -v aarch64-linux-gnu-gcc` 和 `command -v aarch64-linux-gnu-g++` 均无输出。toolchain 文件明确要求这两个名称；`cmake --preset aarch64-release` 退出 1，CMake 报告两个编译器不在 PATH，未执行 build（记录为退出 125）。未下载依赖、未运行交叉测试，Step 8 保持未勾选。
- 2026-08-20：`git diff --check` 退出 0。原始占位符命令仅命中 `docs/standards/project-conventions.md` 中解释“不得留下未解释的 `TODO` 或 `TBD`”的规范文字，故按语义排除该解释性术语和计划目录后重新检查，退出 0；未删除有意义规范。
- 2026-08-21：先执行 `cmake --preset rockchip-armhf-release` 与 `cmake --preset hisiv600-release`，两者均以“`No such preset`”退出 1，确认两个 ARM32 验证入口在实现前缺失。新增预设及 toolchain 后，Rockchip 的 `arm-linux-gnueabihf-gcc/g++` 为 8.3.0，海思 v600 的 `arm-hisiv600-linux-gcc/g++` 为 4.9.4；各自 `cmake --preset` 与 `cmake --build --preset` 均退出 0，实际 `backend_registry.cpp.o` 分别报告 `ELF32` 与 `Machine: ARM`。三套原生 `linux-debug-fetch-deps`、`linux-release-fetch-deps`、`linux-asan-fetch-deps` 的 configure/build/CTest 均退出 0、各为 `95/95`（包含安装 consumer）。
- 2026-08-21：当前 PATH 可解析 aarch64 编译器，`aarch64-linux-gnu-gcc/g++` 均为 GCC 16.1.0；但 `cmake --preset aarch64-release` 退出 1，编译器调用的 `as` 报告 `unrecognized option '-EL'`。按环境指示将 `/home/yjh/work/toolchain/usr/bin` 前置 PATH 后重试，错误未变；未执行 build（退出 125），没有 aarch64 `.o` 可作 `Machine: AArch64` 检查。Step 8 保持未勾选，不能将三套 ARM 交叉构建宣称为全部通过。
- 2026-08-21：环境改用兼容的系统 `aarch64-linux-gnu-gcc/g++` 13.3.0 后，先执行 `cmake --fresh --preset aarch64-release` 清除旧 GCC 16.1.0 的 CMake 缓存，再执行普通 `cmake --preset aarch64-release` 和 `cmake --build --preset aarch64-release`，均退出 0、完成 22 个构建步骤。`aarch64-linux-gnu-readelf -h build/aarch64-release/src/CMakeFiles/eavp_media.dir/media/backend_registry.cpp.o` 报告 `ELF64` 与 `Machine: AArch64`。随后 Rockchip 与海思 v600 的 configure/build 均退出 0，实际对象仍分别报告 `ELF32` 与 `Machine: ARM`；三套 ARM 交叉构建均已通过，Step 8 勾选。
