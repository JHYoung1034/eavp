# EAVP Linux ALSA Capture 0.3b 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**目标：** 交付可选的 Linux ALSA 非阻塞 PCM 采集纵切面，以严格的 AudioFormat/AudioFrame、首采样点 PTS、XRUN/suspend 恢复、Metrics/Health 和真实 Loopback 验收，为 0.3c AAC/Opus 与 0.3d 音视频同时采集提供稳定输入。

**架构：** `EAVP::media` 只持有平台无关 AudioFormat/AudioFrame；`EAVP::platform` 通过不暴露 ALSA 类型的 AlsaSourceNode 公共接口组合 libasound。内部 `AlsaApi -> AlsaSystem -> AlsaSourceNode::Impl` 逐层隔离原始 API、设备会话和 Port/背压，Fake AlsaApi 驱动全部错误与时间戳分支。

**技术栈：** C++11、POSIX、Linux、CMake 3.21、Ninja、libasound 1.2.11、GoogleTest 1.12.1、CTest、ASan/UBSan。

**规格：** `docs/superpowers/specs/2026-08-22-eavp-linux-alsa-capture-design.md`

## 全局约束

- 新增或修改的项目文档、说明性注释和提交摘要优先使用简体中文。
- 生产核心保持 C++11 与 POSIX 基线；公共头文件不得使用 C++14 或更高版本特性。
- 公共 API 使用 `Status`/`Result<T>`，异常不得跨模块边界。
- 依赖方向保持 `platform -> control -> media -> base` 和 `management -> base`；`media` 不包含 ALSA 头文件。
- ALSA 通过 `EAVP_ENABLE_ALSA=OFF` 保持默认可选；开发机 presets 显式开启，三套 ARM presets 显式关闭。
- 不下载 ALSA、FFmpeg 或其他生产依赖；若既有系统环境缺少必需工具或开发包，停止执行并请求用户安装。
- 0.3b 只实现 `RW_INTERLEAVED`、S16_LE、32-bit-container S24_LE、S32_LE、FLOAT_LE、mono/stereo 和精确采样率。
- 默认输出 48 kHz、每声道 480 samples 的 10 ms AudioFrame；硬件 period 只作协商提示。
- AudioFrame PTS 表示首采样点，TimeBase 固定为 `(1, 1000000)`；AudioFrame 不增加 DTS。
- Node 不创建线程、不调用 `snd_pcm_wait`，所有设备 I/O 和恢复必须非阻塞、有界。
- 每个行为先写失败测试并确认失败原因，再写最小实现；每个任务结束时运行相关既有测试并形成可独立审查的 Conventional Commit。
- 只暂存任务列出的文件；不得触碰或提交 `virtrual-v4l2-test.md`、`virtrual-input-test.md` 等个人学习记录。
- 项目版本保持 `0.2.0`；0.3b 完成前不修改 CMake package version。

---

## 文件结构

### 新增公共接口

- `include/eavp/media/audio_format.hpp`：SampleFormat、AudioSampleLayout、AudioChannelLayout、AudioFormat 与字节数校验。
- `include/eavp/platform/linux/alsa_capture.hpp`：不含 ALSA 类型的 AlsaCaptureConfig 和 AlsaSourceNode factory/PImpl 接口。

### 新增实现文件

- `src/media/audio_format.cpp`：格式校验、container bytes 和 PCM frame bytes 计算。
- `src/media/audio_frame.cpp`：AudioFrame factory、Buffer/格式/时间基准校验与异常转换。
- `src/platform/linux/alsa_api.hpp`：私有、可替换的原始 libasound/POSIX 调用界面。
- `src/platform/linux/alsa_lib_api.cpp`：AlsaApi 到系统 libasound 和 `clock_gettime` 的直接委托。
- `src/platform/linux/alsa_system.hpp`：私有 ALSA 会话类型、协商结果、读取事件和测试工厂。
- `src/platform/linux/alsa_system.cpp`：open/configure/start/read/timestamp/recovery/stop 状态机及错误映射。
- `src/platform/linux/alsa_capture_internal.hpp`：AlsaSourceNode PImpl 测试 peer 和内部观测适配接口。
- `src/platform/linux/alsa_capture.cpp`：AlsaCaptureConfig、AlsaSourceNode factory、固定帧聚合、Port 背压、Metrics/Health。

### 新增测试支持与测试

- `tests/support/fake_alsa_api.hpp`：逐调用脚本、PCM 字节、timestamp 和失败注入，不启动线程。
- `tests/support/audio_test_utils.hpp`：48 kHz stereo S16_LE 配置、确定性 PCM 数据和 checksum helper。
- `tests/unit/alsa_system_test.cpp`：ALSA 协商、生命周期、错误映射、时间戳与恢复。
- `tests/unit/alsa_capture_test.cpp`：Config、Node 聚合、背压、PTS/discontinuity、Metrics/Health。
- `tests/integration/alsa_capture_pipeline_test.cpp`：300 帧 Fake 纵切面与一次 XRUN。
- `tests/integration/alsa_capture_device_test.cpp`：默认关闭的真实 ALSA Loopback 验收。

### 修改文件

- `include/eavp/media/frame.hpp`：AudioFrame 改用 AudioFormat、samples_per_channel 和 discontinuity。
- `src/CMakeLists.txt`：加入 Core 音频实现和条件 ALSA platform sources/link。
- `CMakeLists.txt`、`CMakePresets.json`、`cmake/EAVPConfig.cmake.in`：ALSA 选项、查找、presets 和安装依赖。
- `tests/unit/CMakeLists.txt`、`tests/integration/CMakeLists.txt`、`tests/consumer/main.cpp`：注册测试并验证安装接口。
- `tests/unit/media_object_test.cpp`：迁移并扩展 AudioFormat/AudioFrame 测试。
- `docs/standards/third-party-dependencies.md`：登记 libasound。
- `docs/architecture/core-contracts.md`、`docs/architecture/threading-and-lifecycle.md`、`docs/architecture/build-and-portability.md`、`docs/architecture/testing-strategy.md`：记录 0.3b 已实现边界。
- `docs/architecture/versioning-and-abi.md`、`docs/migrations/0.2-to-0.3-audio.md`：记录实验性 AudioFrame API 迁移。
- `README.md`、`docs/roadmap.md`、设计规格：更新实现状态和验收结果，不改变稳定包版本。

---

### Task 1：实现 AudioFormat 并收紧 AudioFrame 契约

**Files:**
- Create: `include/eavp/media/audio_format.hpp`
- Create: `src/media/audio_format.cpp`
- Create: `src/media/audio_frame.cpp`
- Modify: `include/eavp/media/frame.hpp:7-112`
- Modify: `src/CMakeLists.txt:19-31`
- Modify: `tests/unit/media_object_test.cpp:588-596`

**Interfaces:**
- Produces: `SampleFormat::{kUnknown,kSigned16LittleEndian,kSigned24In32LittleEndian,kSigned32LittleEndian,kFloat32LittleEndian}`。
- Produces: `AudioSampleLayout::kInterleaved`、`AudioChannelLayout::{kMono,kStereo}`。
- Produces: `AudioFormat::create(...)`、`channels()`、`bytes_per_sample()`、`bytes_per_pcm_frame()`。
- Produces: `AudioFrame::create(const Buffer&, const AudioFormat&, int, int64_t, const TimeBase&, bool)` 和 `samples_per_channel()/discontinuity()`。

- [ ] **Step 1: 写 AudioFormat 和 AudioFrame 的失败测试**

将旧 `FrameTest.AudioFramesValidateShapeAndShareBuffer` 替换为以下测试，并增加 `<limits>`：

```cpp
TEST(AudioFormatTest, DescribesSupportedInterleavedFormatsExactly) {
    struct Case {
        eavp::SampleFormat sample_format;
        std::size_t bytes_per_sample;
    };
    const Case cases[] = {
        {eavp::SampleFormat::kSigned16LittleEndian, 2U},
        {eavp::SampleFormat::kSigned24In32LittleEndian, 4U},
        {eavp::SampleFormat::kSigned32LittleEndian, 4U},
        {eavp::SampleFormat::kFloat32LittleEndian, 4U},
    };
    for (std::size_t index = 0U; index < 4U; ++index) {
        eavp::Result<eavp::AudioFormat> format = eavp::AudioFormat::create(
            cases[index].sample_format, 48000,
            eavp::AudioChannelLayout::kStereo,
            eavp::AudioSampleLayout::kInterleaved,
            eavp::MemoryDomain::kCpu);
        ASSERT_TRUE(format.ok());
        EXPECT_EQ(2, format.value().channels());
        EXPECT_EQ(cases[index].bytes_per_sample,
                  format.value().bytes_per_sample());
        EXPECT_EQ(cases[index].bytes_per_sample * 2U,
                  format.value().bytes_per_pcm_frame());
    }
}

TEST(AudioFrameTest, UsesFirstSamplePtsAndExactPayloadSize) {
    const eavp::AudioFormat format = eavp::AudioFormat::create(
        eavp::SampleFormat::kSigned16LittleEndian, 48000,
        eavp::AudioChannelLayout::kStereo,
        eavp::AudioSampleLayout::kInterleaved,
        eavp::MemoryDomain::kCpu).take_value();
    const eavp::Buffer buffer = eavp::Buffer::allocate(1920U).take_value();
    const eavp::AudioFrame frame = eavp::AudioFrame::create(
        buffer, format, 480, 1234000,
        eavp::TimeBase::create(1, 1000000).take_value(), true).take_value();

    EXPECT_EQ(480, frame.samples_per_channel());
    EXPECT_EQ(1234000, frame.pts());
    EXPECT_TRUE(frame.discontinuity());
    EXPECT_EQ(48000, frame.format().sample_rate());
}

TEST(AudioFrameTest, RejectsWrongBufferAndNonPositiveTimeBase) {
    const eavp::AudioFormat format = eavp::AudioFormat::create(
        eavp::SampleFormat::kSigned16LittleEndian, 48000,
        eavp::AudioChannelLayout::kStereo,
        eavp::AudioSampleLayout::kInterleaved,
        eavp::MemoryDomain::kCpu).take_value();
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch,
              eavp::AudioFrame::create(
                  eavp::Buffer::allocate(1919U).take_value(), format, 480, 0,
                  eavp::TimeBase::create(1, 1000000).take_value(), false)
                  .status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::AudioFrame::create(
                  eavp::Buffer::allocate(1920U).take_value(), format, 480, 0,
                  eavp::TimeBase::create(0, 1000000).take_value(), false)
                  .status().code());
}
```

再增加 table-driven invalid cases：unknown format、rate 0、非法 enum cast、samples 0、两个 planes、memory domain 不匹配，以及 `samples_per_channel * bytes_per_pcm_frame` 溢出。

- [ ] **Step 2: 运行测试并确认因新类型/API 尚不存在而失败**

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug --target eavp_media_object_tests
```

Expected: 编译失败，指出 `AudioFormat`、新枚举值或新 AudioFrame factory/getter 尚不存在；不能接受无关编译错误作为红灯证据。

- [ ] **Step 3: 实现 AudioFormat 最小行为**

`audio_format.hpp` 声明规格中的完整接口；`audio_format.cpp` 使用显式 switch，不依赖 ALSA：

```cpp
std::size_t sample_bytes(SampleFormat format) {
    switch (format) {
    case SampleFormat::kSigned16LittleEndian:
        return 2U;
    case SampleFormat::kSigned24In32LittleEndian:
    case SampleFormat::kSigned32LittleEndian:
    case SampleFormat::kFloat32LittleEndian:
        return 4U;
    case SampleFormat::kUnknown:
        return 0U;
    }
    return 0U;
}

int channel_count(AudioChannelLayout layout) {
    switch (layout) {
    case AudioChannelLayout::kMono:
        return 1;
    case AudioChannelLayout::kStereo:
        return 2;
    }
    return 0;
}
```

factory 在乘法前使用 `std::numeric_limits<std::size_t>::max()` 检查，捕获 `std::bad_alloc` 为 `kResourceExhausted`、其他异常为 `kInternal`。

- [ ] **Step 4: 实现 AudioFrame 精确校验**

从 `frame.hpp` 删除旧 SampleFormat 和 inline AudioFrame，实现迁移到 `audio_frame.cpp`：

```cpp
Result<AudioFrame> AudioFrame::create(
    const Buffer& buffer, const AudioFormat& format,
    int samples_per_channel, std::int64_t pts,
    const TimeBase& time_base, bool discontinuity) {
    if (samples_per_channel <= 0 || time_base.numerator() <= 0) {
        return Result<AudioFrame>(Status(
            StatusCode::kInvalidArgument,
            "audio frame samples and time base must be positive"));
    }
    if (buffer.memory_domain() != format.memory_domain() ||
        buffer.plane_count() != 1U) {
        return Result<AudioFrame>(Status(
            StatusCode::kCapabilityMismatch,
            "audio frame buffer does not match its format"));
    }
    const std::size_t pcm_frames =
        static_cast<std::size_t>(samples_per_channel);
    if (format.bytes_per_pcm_frame() >
        std::numeric_limits<std::size_t>::max() / pcm_frames) {
        return Result<AudioFrame>(Status(StatusCode::kInvalidArgument,
                                         "audio frame size overflows"));
    }
    const Result<PlaneLayout> plane = buffer.plane_layout(0U);
    if (!plane.ok() ||
        plane.value().size != pcm_frames * format.bytes_per_pcm_frame()) {
        return Result<AudioFrame>(Status(
            StatusCode::kCapabilityMismatch,
            "audio frame payload size does not match its format"));
    }
    try {
        return Result<AudioFrame>(AudioFrame(
            buffer, format, samples_per_channel, pts, time_base,
            discontinuity));
    } catch (const std::bad_alloc&) {
        return Result<AudioFrame>(Status(StatusCode::kResourceExhausted));
    } catch (...) {
        return Result<AudioFrame>(Status(StatusCode::kInternal));
    }
}
```

- [ ] **Step 5: 运行 Core 音频测试和既有 Media 测试**

```bash
cmake --build --preset linux-debug --target eavp_media_object_tests
ctest --test-dir build/linux-debug -R 'AudioFormatTest|AudioFrameTest|FrameTest|MediaPacketTest' --output-on-failure
ctest --preset linux-debug --output-on-failure
```

Expected: 新旧全部通过；`rg -n 'kSigned16\b|\.samples\(\)' include src tests examples` 不再发现旧 AudioFrame API 调用。

- [ ] **Step 6: 提交 Core 音频对象**

```bash
git add include/eavp/media/audio_format.hpp include/eavp/media/frame.hpp \
  src/media/audio_format.cpp src/media/audio_frame.cpp src/CMakeLists.txt \
  tests/unit/media_object_test.cpp
git commit -m "feat(media): 完善音频格式与帧契约"
```

---

### Task 2：接入可选 ALSA 构建和 AlsaCaptureConfig

**Files:**
- Create: `include/eavp/platform/linux/alsa_capture.hpp`
- Create: `src/platform/linux/alsa_capture.cpp`
- Create: `tests/unit/alsa_capture_test.cpp`
- Modify: `CMakeLists.txt:5-14`
- Modify: `CMakePresets.json:14-102`
- Modify: `cmake/EAVPConfig.cmake.in:1-8`
- Modify: `src/CMakeLists.txt:97-117`
- Modify: `tests/unit/CMakeLists.txt`
- Modify: `docs/standards/third-party-dependencies.md:3-11`

**Interfaces:**
- Produces: `AlsaCaptureConfig::create(device_name, format, samples_per_frame, period_size_hint, buffer_periods)` 与只读 getters。
- Produces: CMake options `EAVP_ENABLE_ALSA` 和 `EAVP_ENABLE_ALSA_DEVICE_TESTS`，默认均为 OFF。
- Produces: host presets 显式 `EAVP_ENABLE_ALSA=ON`；三套 ARM presets 显式 OFF。

- [ ] **Step 1: 写 AlsaCaptureConfig 失败测试**

```cpp
TEST(AlsaCaptureConfigTest, AcceptsTheApprovedTenMillisecondShape) {
    const eavp::AudioFormat format = eavp::AudioFormat::create(
        eavp::SampleFormat::kSigned16LittleEndian, 48000,
        eavp::AudioChannelLayout::kStereo,
        eavp::AudioSampleLayout::kInterleaved,
        eavp::MemoryDomain::kCpu).take_value();
    const eavp::AlsaCaptureConfig config =
        eavp::AlsaCaptureConfig::create(
            "hw:Loopback,1,0", format, 480, 480, 4).take_value();

    EXPECT_EQ("hw:Loopback,1,0", config.device_name());
    EXPECT_EQ(480, config.samples_per_frame());
    EXPECT_EQ(480, config.period_size_hint());
    EXPECT_EQ(4, config.buffer_periods());
}

TEST(AlsaCaptureConfigTest, RejectsUnsupportedOrUnsafeShapes) {
    const eavp::AudioFormat cpu_format = make_test_audio_format();
    const eavp::AudioFormat mmap_format = eavp::AudioFormat::create(
        eavp::SampleFormat::kSigned16LittleEndian, 48000,
        eavp::AudioChannelLayout::kStereo,
        eavp::AudioSampleLayout::kInterleaved,
        eavp::MemoryDomain::kMmap).take_value();
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::AlsaCaptureConfig::create(
                  "", cpu_format, 480, 480, 4).status().code());
    EXPECT_EQ(eavp::StatusCode::kCapabilityMismatch,
              eavp::AlsaCaptureConfig::create(
                  "hw:Loopback,1,0", mmap_format, 480, 480, 4)
                  .status().code());
    EXPECT_EQ(eavp::StatusCode::kInvalidArgument,
              eavp::AlsaCaptureConfig::create(
                  "hw:Loopback,1,0", cpu_format, 0, 480, 4)
                  .status().code());
}
```

在测试文件定义 `make_test_audio_format()`，并覆盖 period/buffer 非正及 frame bytes 溢出。

同时在 `tests/unit/CMakeLists.txt` 注册红灯 target：

```cmake
add_executable(eavp_alsa_capture_tests alsa_capture_test.cpp)
target_link_libraries(eavp_alsa_capture_tests
                      PRIVATE EAVP::platform GTest::gtest_main)
target_include_directories(eavp_alsa_capture_tests
                           PRIVATE ${PROJECT_SOURCE_DIR}/src
                                   ${PROJECT_SOURCE_DIR}/tests)
gtest_discover_tests(eavp_alsa_capture_tests)
```

- [ ] **Step 2: 运行测试并确认 public header/config 尚不存在**

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug --target eavp_alsa_capture_tests
```

Expected: 配置或编译失败，明确指出 ALSA option/target/header 尚未定义。

- [ ] **Step 3: 加入 CMake 选项、查找和 presets**

根 CMake 使用：

```cmake
option(EAVP_ENABLE_ALSA "启用 Linux ALSA PCM 采集" OFF)
option(EAVP_ENABLE_ALSA_DEVICE_TESTS "启用真实 ALSA 设备测试" OFF)

if(EAVP_ENABLE_ALSA_DEVICE_TESTS AND NOT EAVP_ENABLE_ALSA)
    message(FATAL_ERROR "真实 ALSA 设备测试要求 EAVP_ENABLE_ALSA=ON")
endif()
if(EAVP_ENABLE_ALSA)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "ALSA 采集仅支持 Linux")
    endif()
    find_package(ALSA REQUIRED)
endif()
```

`linux-debug`、`linux-release`、`linux-asan` 写入 `EAVP_ENABLE_ALSA=ON`；aarch64、Rockchip ARMHF、HiSilicon v600 写入 `EAVP_ENABLE_ALSA=OFF`。Package config 使用：

```cmake
if(@EAVP_ENABLE_ALSA@)
    find_dependency(ALSA)
endif()
```

`eavp_platform` 在 ALSA ON 时加入 `platform/linux/alsa_capture.cpp`，并 `PUBLIC` 链接 `ALSA::ALSA`，使静态安装消费能解析依赖。

- [ ] **Step 4: 实现 AlsaCaptureConfig**

公共头只 include EAVP 类型；不 include `<alsa/asoundlib.h>`。factory 执行规格中的空值、CPU/interleaved、正整数和溢出校验，并将 `std::bad_alloc`/未知异常映射为 `kResourceExhausted`/`kInternal`。

```cpp
if (device_name.empty() || samples_per_frame <= 0 ||
    period_size_hint <= 0 || buffer_periods <= 0) {
    return Result<AlsaCaptureConfig>(Status(
        StatusCode::kInvalidArgument,
        "ALSA capture configuration values must be positive"));
}
if (format.memory_domain() != MemoryDomain::kCpu ||
    format.sample_layout() != AudioSampleLayout::kInterleaved) {
    return Result<AlsaCaptureConfig>(Status(
        StatusCode::kCapabilityMismatch,
        "ALSA capture requires interleaved CPU audio"));
}
```

- [ ] **Step 5: 登记 libasound 并运行配置/安装基线**

第三方依赖表增加：

```markdown
| ALSA libasound | `1.2.11`（本机设计基线） | 可选 Linux PCM 采集 | LGPL-2.1-or-later | 仅 `EAVP_ENABLE_ALSA=ON` 的 `EAVP::platform` |
```

执行：

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug --target eavp_alsa_capture_tests
ctest --test-dir build/linux-debug -R AlsaCaptureConfigTest --output-on-failure
cmake --preset aarch64-release
cmake --build --preset aarch64-release
```

Expected: host Config 测试通过；aarch64 不查找目标 libasound 仍可 clean configure/build。

- [ ] **Step 6: 提交构建边界和配置对象**

```bash
git add CMakeLists.txt CMakePresets.json cmake/EAVPConfig.cmake.in \
  include/eavp/platform/linux/alsa_capture.hpp \
  src/platform/linux/alsa_capture.cpp src/CMakeLists.txt \
  tests/unit/alsa_capture_test.cpp tests/unit/CMakeLists.txt \
  docs/standards/third-party-dependencies.md
git commit -m "build(platform): 接入可选 ALSA 采集依赖"
```

---

### Task 3：实现可注入的 libasound 会话与生命周期

**Files:**
- Create: `src/platform/linux/alsa_api.hpp`
- Create: `src/platform/linux/alsa_lib_api.cpp`
- Create: `src/platform/linux/alsa_system.hpp`
- Create: `src/platform/linux/alsa_system.cpp`
- Create: `tests/support/fake_alsa_api.hpp`
- Create: `tests/support/audio_test_utils.hpp`
- Create: `tests/unit/alsa_system_test.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces: 私有 `detail::AlsaApi`，覆盖 PCM open/close、HW/SW params、prepare/start/drop/resume/readi/htimestamp/avail/clock/error text。
- Produces: `detail::AlsaNegotiatedParameters {sample_rate, channels, period_frames, buffer_frames, monotonic_timestamp}`。
- Produces: `detail::AlsaSystem(std::unique_ptr<AlsaApi>)`、`prepare(config)`、`start()`、`stop()`、`negotiated()`。
- Produces: `detail::create_libasound_api()`，只在 ALSA-enabled platform source 内可见。
- Produces: `tests/support/audio_test_utils.hpp` 中的 `make_alsa_config()`、PCM byte helper 和共享 Fake 状态。

- [ ] **Step 1: 写格式映射、精确协商和逆序清理失败测试**

```cpp
TEST(AlsaSystemTest, ConfiguresExactInterleavedCaptureAndReadsBackValues) {
    std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
    FakeAlsaApi* observed = fake.get();
    observed->negotiated_rate = 48000U;
    observed->negotiated_channels = 2U;
    observed->negotiated_period = 512U;
    observed->negotiated_buffer = 2048U;
    eavp::detail::AlsaSystem system(std::move(fake));

    ASSERT_TRUE(system.prepare(make_alsa_config()).ok());
    EXPECT_EQ(SND_PCM_STREAM_CAPTURE, observed->opened_stream);
    EXPECT_EQ(SND_PCM_ACCESS_RW_INTERLEAVED, observed->requested_access);
    EXPECT_EQ(SND_PCM_FORMAT_S16_LE, observed->requested_format);
    EXPECT_EQ(48000U, observed->requested_rate);
    EXPECT_EQ(2U, observed->requested_channels);
    EXPECT_EQ(512, system.negotiated().period_frames);
    EXPECT_EQ(2048, system.negotiated().buffer_frames);
}

TEST(AlsaSystemTest, MapsEveryApprovedSampleFormatExactly) {
    struct Case {
        eavp::SampleFormat eavp_format;
        snd_pcm_format_t alsa_format;
    };
    const Case cases[] = {
        {eavp::SampleFormat::kSigned16LittleEndian,
         SND_PCM_FORMAT_S16_LE},
        {eavp::SampleFormat::kSigned24In32LittleEndian,
         SND_PCM_FORMAT_S24_LE},
        {eavp::SampleFormat::kSigned32LittleEndian,
         SND_PCM_FORMAT_S32_LE},
        {eavp::SampleFormat::kFloat32LittleEndian,
         SND_PCM_FORMAT_FLOAT_LE},
    };
    for (std::size_t index = 0U; index < 4U; ++index) {
        std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
        FakeAlsaApi* observed = fake.get();
        eavp::detail::AlsaSystem system(std::move(fake));
        ASSERT_TRUE(system.prepare(make_alsa_config(cases[index].eavp_format))
                        .ok());
        EXPECT_EQ(cases[index].alsa_format,
                  observed->requested_format);
    }
}

TEST(AlsaSystemTest, ClosesEveryPartiallyPreparedDevice) {
    for (int step = FakeAlsaApi::kOpen;
         step <= FakeAlsaApi::kPcmPrepare; ++step) {
        std::unique_ptr<FakeAlsaApi> fake(new FakeAlsaApi());
        FakeAlsaApi* observed = fake.get();
        observed->fail_step = step;
        eavp::detail::AlsaSystem system(std::move(fake));
        EXPECT_FALSE(system.prepare(make_alsa_config()).ok());
        EXPECT_EQ(observed->successful_open_count,
                  observed->close_count);
        EXPECT_EQ(observed->hw_params_alloc_count,
                  observed->hw_params_free_count);
        EXPECT_EQ(observed->sw_params_alloc_count,
                  observed->sw_params_free_count);
    }
}
```

另写 `RejectsNegotiatedRateOrFormatChanges`、`StartAndStopAreIdempotent`、`MapsMissingDeviceAndCapabilityMismatch` 和 `UnsupportedMonotonicTimestampEnablesFallbackWithoutUsingRealtime`。最后一个测试令 `sw_params_set_tstamp_type` 返回 `-EINVAL`，断言 prepare 仍成功、`negotiated().monotonic_timestamp` 为 false，后续不得消费非 monotonic htimestamp。

在 `tests/unit/CMakeLists.txt` 注册：

```cmake
add_executable(eavp_alsa_system_tests alsa_system_test.cpp)
target_link_libraries(eavp_alsa_system_tests
                      PRIVATE EAVP::platform GTest::gtest_main)
target_include_directories(eavp_alsa_system_tests
                           PRIVATE ${PROJECT_SOURCE_DIR}/src
                                   ${PROJECT_SOURCE_DIR}/tests)
gtest_discover_tests(eavp_alsa_system_tests)
```

- [ ] **Step 2: 运行测试并确认内部 ALSA seam 尚不存在**

```bash
cmake --build --preset linux-debug --target eavp_alsa_system_tests
```

Expected: 编译失败，指出 `alsa_system.hpp`、AlsaApi 或 FakeAlsaApi 尚不存在。

- [ ] **Step 3: 定义完整私有 AlsaApi seam**

`alsa_api.hpp` 可包含 `<alsa/asoundlib.h>`，但不得被公开头 include。接口至少声明以下精确方法：

```cpp
class AlsaApi {
public:
    virtual ~AlsaApi() {}
    virtual int pcm_open(snd_pcm_t** pcm, const char* name,
                         snd_pcm_stream_t stream, int mode) = 0;
    virtual int pcm_close(snd_pcm_t* pcm) = 0;
    virtual int hw_params_malloc(snd_pcm_hw_params_t** params) = 0;
    virtual void hw_params_free(snd_pcm_hw_params_t* params) = 0;
    virtual int hw_params_any(snd_pcm_t*, snd_pcm_hw_params_t*) = 0;
    virtual int hw_params_set_access(snd_pcm_t*, snd_pcm_hw_params_t*,
                                     snd_pcm_access_t) = 0;
    virtual int hw_params_set_format(snd_pcm_t*, snd_pcm_hw_params_t*,
                                     snd_pcm_format_t) = 0;
    virtual int hw_params_set_channels(snd_pcm_t*, snd_pcm_hw_params_t*,
                                       unsigned int) = 0;
    virtual int hw_params_set_rate(snd_pcm_t*, snd_pcm_hw_params_t*,
                                   unsigned int, int) = 0;
    virtual int hw_params_set_period_size_near(
        snd_pcm_t*, snd_pcm_hw_params_t*, snd_pcm_uframes_t*, int*) = 0;
    virtual int hw_params_set_buffer_size_near(
        snd_pcm_t*, snd_pcm_hw_params_t*, snd_pcm_uframes_t*) = 0;
    virtual int hw_params(snd_pcm_t*, snd_pcm_hw_params_t*) = 0;
    virtual int hw_params_get_access(const snd_pcm_hw_params_t*,
                                     snd_pcm_access_t*) = 0;
    virtual int hw_params_get_format(const snd_pcm_hw_params_t*,
                                     snd_pcm_format_t*) = 0;
    virtual int hw_params_get_channels(const snd_pcm_hw_params_t*,
                                       unsigned int*) = 0;
    virtual int hw_params_get_rate(const snd_pcm_hw_params_t*,
                                   unsigned int*, int*) = 0;
    virtual int hw_params_get_period_size(const snd_pcm_hw_params_t*,
                                          snd_pcm_uframes_t*, int*) = 0;
    virtual int hw_params_get_buffer_size(const snd_pcm_hw_params_t*,
                                          snd_pcm_uframes_t*) = 0;
    virtual int sw_params_malloc(snd_pcm_sw_params_t** params) = 0;
    virtual void sw_params_free(snd_pcm_sw_params_t* params) = 0;
    virtual int sw_params_current(snd_pcm_t*, snd_pcm_sw_params_t*) = 0;
    virtual int sw_params_set_tstamp_mode(snd_pcm_t*, snd_pcm_sw_params_t*,
                                          snd_pcm_tstamp_t) = 0;
    virtual int sw_params_set_tstamp_type(snd_pcm_t*, snd_pcm_sw_params_t*,
                                          snd_pcm_tstamp_type_t) = 0;
    virtual int sw_params_set_avail_min(snd_pcm_t*, snd_pcm_sw_params_t*,
                                        snd_pcm_uframes_t) = 0;
    virtual int sw_params(snd_pcm_t*, snd_pcm_sw_params_t*) = 0;
    virtual int pcm_prepare(snd_pcm_t*) = 0;
    virtual int pcm_start(snd_pcm_t*) = 0;
    virtual int pcm_drop(snd_pcm_t*) = 0;
    virtual int pcm_resume(snd_pcm_t*) = 0;
    virtual snd_pcm_sframes_t pcm_readi(snd_pcm_t*, void*,
                                        snd_pcm_uframes_t) = 0;
    virtual int pcm_htimestamp(snd_pcm_t*, snd_pcm_uframes_t*,
                               snd_htimestamp_t*) = 0;
    virtual snd_pcm_sframes_t pcm_avail_update(snd_pcm_t*) = 0;
    virtual int monotonic_now(struct timespec*) = 0;
    virtual const char* error_string(int error) const = 0;
};
```

- [ ] **Step 4: 实现 AlsaSystem prepare/start/stop**

`prepare()` 使用规格要求的 open flags 和精确顺序；每次原始失败立刻转为 `Status`，并通过单一 `close_resources()` 逆序释放。错误 helper 固定保留原始负 code：

```cpp
Status alsa_failure(StatusCode code, const char* operation,
                    int native_code, const AlsaApi& api) {
    return Status(code, api.error_string(native_code), "alsa",
                  operation, native_code);
}
```

读回 access/format/rate/channels 必须精确相等；实际 buffer 小于两个 period 时返回 `kCapabilityMismatch`。`stop()` 在已打开时调用 `pcm_drop` 后 close；即使 drop 失败仍尝试 close，并返回第一个失败。析构执行不抛异常的同等清理。

- [ ] **Step 5: 实现 LibasoundApi 的直接委托**

每个 override 只调用同名 libasound API；`monotonic_now` 调用：

```cpp
int LibasoundApi::monotonic_now(struct timespec* value) {
    return ::clock_gettime(CLOCK_MONOTONIC, value);
}
```

不得在委托层吞掉或改写负 ALSA error。

- [ ] **Step 6: 运行系统测试和全量 Debug**

```bash
cmake --build --preset linux-debug --target eavp_alsa_system_tests
ctest --test-dir build/linux-debug -R AlsaSystemTest --output-on-failure
ctest --preset linux-debug --output-on-failure
```

Expected: 所有 prepare failure step 均 close/free 一次，既有测试全部通过。

- [ ] **Step 7: 提交 ALSA 会话基础**

```bash
git add src/platform/linux/alsa_api.hpp src/platform/linux/alsa_lib_api.cpp \
  src/platform/linux/alsa_system.hpp src/platform/linux/alsa_system.cpp \
  tests/support/fake_alsa_api.hpp tests/support/audio_test_utils.hpp \
  tests/unit/alsa_system_test.cpp \
  src/CMakeLists.txt tests/unit/CMakeLists.txt
git commit -m "feat(platform): 实现 ALSA 非阻塞设备会话"
```

---

### Task 4：实现固定长度 AudioFrame 聚合与 Port 背压

**Files:**
- Create: `src/platform/linux/alsa_capture_internal.hpp`
- Modify: `include/eavp/platform/linux/alsa_capture.hpp`
- Modify: `src/platform/linux/alsa_system.hpp`
- Modify: `src/platform/linux/alsa_system.cpp`
- Modify: `src/platform/linux/alsa_capture.cpp`
- Modify: `tests/support/audio_test_utils.hpp`
- Modify: `tests/unit/alsa_capture_test.cpp`

**Interfaces:**
- Produces: `AlsaReadResult {int frames_read; bool would_block; bool timeline_discontinuity}`。
- Produces: `AlsaSystem::read_interleaved(uint8_t*, int requested_frames)`；正短读、0/EAGAIN、EINTR 和 fatal Status 语义固定。
- Produces: `AlsaSourceNode::create(id, config, metrics, health)`、`output()` 和 PImpl；公共头无 ALSA 类型。
- Produces: 私有 `AlsaSourceNodeTestPeer::create(..., std::unique_ptr<AlsaSystem>)`，仅测试注入使用。
- Produces: `ScriptedAlsa` 保留共享 Fake 状态并提供 `take_system()`、`take_started_system()`、`append_complete_frames()`、`append_patterned_frames()`、`append_error()`、anchor/recovery setter 和调用计数 getter。
- Produces: `NodeFixture` 持有 MetricRegistry、HealthManager、InputPort、Node，并提供 `start()`、`tick_until_frames()`、`take_frame()`、`tick_once_running()`。

- [ ] **Step 1: 写短读聚合和背压失败测试**

```cpp
TEST(AlsaSourceNodeTest, AggregatesShortReadsIntoOneExactFrame) {
    ScriptedAlsa source;
    source.read_frames.push_back(120);
    source.read_frames.push_back(360);
    NodeFixture fixture(source.take_system(), make_alsa_config());
    ASSERT_TRUE(fixture.start());

    EXPECT_TRUE(fixture.node->tick().ok());
    EXPECT_EQ(0U, fixture.input.queue_size());
    EXPECT_TRUE(fixture.node->tick().ok());
    ASSERT_EQ(1U, fixture.input.queue_size());
    const std::shared_ptr<const eavp::AudioFrame> frame =
        fixture.input.receive().take_value();
    EXPECT_EQ(480, frame->samples_per_channel());
    EXPECT_EQ(1920U, frame->buffer().plane_layout(0U).value().size);
}

TEST(AlsaSourceNodeTest, PendingOutputStopsFurtherDeviceReads) {
    ScriptedAlsa source;
    source.append_complete_frames(3);
    NodeFixture fixture(source.take_system(), make_alsa_config(), 1U);
    ASSERT_TRUE(fixture.start());
    ASSERT_TRUE(fixture.node->tick().ok());
    ASSERT_EQ(eavp::StatusCode::kWouldBlock,
              fixture.node->tick().code());
    const int reads_before_blocked_retry = source.observed_read_calls();

    EXPECT_EQ(eavp::StatusCode::kWouldBlock,
              fixture.node->tick().code());
    EXPECT_EQ(reads_before_blocked_retry, source.observed_read_calls());
}
```

增加 `ZeroAndEagainReturnWouldBlock`、`EintrRetriesSameRead`、`StopDiscardsPartialAndPending`、`FactoryRejectsEmptyIdOrObservers`。

- [ ] **Step 2: 运行测试并确认 Node factory/read API 缺失**

```bash
cmake --build --preset linux-debug --target eavp_alsa_capture_tests
```

Expected: 编译失败，指出 AlsaSourceNode 或 AlsaSystem read 接口缺失。

- [ ] **Step 3: 实现基础非阻塞读取结果**

```cpp
Result<AlsaReadResult> AlsaSystem::read_interleaved(
    std::uint8_t* destination, int requested_frames) {
    if (!running_ || destination == NULL || requested_frames <= 0) {
        return Result<AlsaReadResult>(Status(
            StatusCode::kInvalidArgument, "ALSA read request is invalid"));
    }
    snd_pcm_sframes_t result;
    do {
        result = api_->pcm_readi(pcm_, destination,
                                 static_cast<snd_pcm_uframes_t>(requested_frames));
    } while (result == -EINTR);
    if (result > 0) {
        return Result<AlsaReadResult>(AlsaReadResult(
            static_cast<int>(result), false, false));
    }
    if (result == 0 || result == -EAGAIN) {
        return Result<AlsaReadResult>(AlsaReadResult(0, true, false));
    }
    return Result<AlsaReadResult>(map_read_failure(result));
}
```

同时拒绝驱动返回大于 requested frames 或无法放入 int 的正值为 `kCorruptData`。

- [ ] **Step 4: 实现 AlsaSourceNode PImpl 和固定帧聚合**

factory 创建 production `AlsaSystem(create_libasound_api())`；测试 peer 注入 Fake。`Impl` 持有当前 CPU Buffer、已填 samples、pending Frame、累计输出 samples 和 discontinuity flag。每 tick 最多一次 `read_interleaved`：

```cpp
const int missing = config.samples_per_frame() - partial_samples;
Result<MappedRegion> mapped =
    partial_buffer.map_plane(0U, MapMode::kReadWrite);
std::uint8_t* destination = mapped.value().mutable_data() +
    static_cast<std::size_t>(partial_samples) *
        config.format().bytes_per_pcm_frame();
Result<AlsaReadResult> read = system->read_interleaved(destination, missing);
```

读满后创建 `shared_ptr<const AudioFrame>`；发送返回 `kWouldBlock` 时保留 pending，不读取下一帧。`on_stop/on_reset` 清空 partial/pending 并调用 session stop；析构不抛异常。

- [ ] **Step 5: 运行聚合/背压和既有 Runtime 测试**

```bash
cmake --build --preset linux-debug --target \
  eavp_alsa_capture_tests eavp_media_runtime_tests
ctest --test-dir build/linux-debug \
  -R 'AlsaSourceNodeTest|PortTest|QueueTest|PipelineTest' \
  --output-on-failure
```

Expected: 短读只输出完整 1920-byte Frame，pending 时 read call count 不增加。

- [ ] **Step 6: 提交 Source Node 数据流**

```bash
git add include/eavp/platform/linux/alsa_capture.hpp \
  src/platform/linux/alsa_capture_internal.hpp \
  src/platform/linux/alsa_capture.cpp src/platform/linux/alsa_system.hpp \
  src/platform/linux/alsa_system.cpp tests/support/audio_test_utils.hpp \
  tests/unit/alsa_capture_test.cpp
git commit -m "feat(platform): 实现 ALSA 音频帧采集节点"
```

---

### Task 5：实现首采样点 PTS、XRUN/suspend 与设备丢失

**Files:**
- Modify: `src/platform/linux/alsa_system.hpp`
- Modify: `src/platform/linux/alsa_system.cpp`
- Modify: `src/platform/linux/alsa_capture.cpp`
- Modify: `tests/support/fake_alsa_api.hpp`
- Modify: `tests/unit/alsa_system_test.cpp`
- Modify: `tests/unit/alsa_capture_test.cpp`

**Interfaces:**
- Produces: `AlsaAnchor {std::int64_t first_unread_pts_us; bool used_fallback}`。
- Produces: `AlsaSystem::capture_anchor()`，优先 htimestamp，fallback 为 monotonic clock + avail。
- Produces: EPIPE 后 `prepare+start`、ESTRPIPE 非阻塞 resume/fallback、timeline reset 事件。
- Produces: ENODEV/ENXIO `kDeviceLost` 及所有 enriched ALSA Status。

- [ ] **Step 1: 写 htimestamp/fallback 和累计 PTS 失败测试**

```cpp
TEST(AlsaSystemTest, ConvertsAvailableFramesToFirstUnreadSamplePts) {
    ScriptedAlsa source;
    source.set_htimestamp(2000000, 480);
    eavp::detail::AlsaSystem system = source.take_started_system();
    const eavp::detail::AlsaAnchor anchor =
        system.capture_anchor().take_value();
    EXPECT_EQ(1990000, anchor.first_unread_pts_us);
    EXPECT_FALSE(anchor.used_fallback);
}

TEST(AlsaSourceNodeTest, DerivesPtsFromCumulativeSamplesWithoutJitter) {
    ScriptedAlsa source;
    source.set_initial_anchor(5000000);
    source.append_complete_frames(3);
    NodeFixture fixture(source.take_system(), make_alsa_config(), 4U);
    ASSERT_TRUE(fixture.start());
    ASSERT_TRUE(fixture.tick_until_frames(3));
    EXPECT_EQ(5000000, fixture.take_frame()->pts());
    EXPECT_EQ(5010000, fixture.take_frame()->pts());
    EXPECT_EQ(5020000, fixture.take_frame()->pts());
}
```

再写 invalid timespec、negative avail、未来 timestamp、换算溢出和 fallback counter event 的测试。

- [ ] **Step 2: 写 XRUN/suspend/DeviceLost 失败测试**

```cpp
TEST(AlsaSourceNodeTest, XrunDropsPartialAndMarksExactlyOneFrame) {
    ScriptedAlsa source;
    source.read_frames.push_back(240);
    source.read_errors.push_back(-EPIPE);
    source.append_complete_frames(2);
    source.set_recovery_anchor(7000000);
    NodeFixture fixture(source.take_system(), make_alsa_config(), 4U);
    ASSERT_TRUE(fixture.start());
    ASSERT_TRUE(fixture.tick_until_frames(2));

    EXPECT_TRUE(fixture.take_frame()->discontinuity());
    EXPECT_FALSE(fixture.take_frame()->discontinuity());
    EXPECT_EQ(1, source.prepare_after_xrun_calls());
    EXPECT_EQ(1, source.start_after_xrun_calls());
}

TEST(AlsaSystemTest, SuspendedResumeNeverBlocksExecutor) {
    ScriptedAlsa source;
    source.read_errors.push_back(-ESTRPIPE);
    source.resume_results.push_back(-EAGAIN);
    source.resume_results.push_back(0);
    eavp::detail::AlsaSystem system = source.take_started_system();
    EXPECT_TRUE(system.read_interleaved(source.bytes(), 480)
                    .value().would_block);
    EXPECT_TRUE(system.read_interleaved(source.bytes(), 480)
                    .value().would_block);
}
```

再写 resume fatal 后 prepare+start fallback、恢复失败 enriched Status、ENODEV/ENXIO、普通 EIO 和原始 `operation/native_code` 断言。

- [ ] **Step 3: 运行新测试并确认时间戳/恢复行为缺失**

```bash
cmake --build --preset linux-debug --target \
  eavp_alsa_system_tests eavp_alsa_capture_tests
ctest --test-dir build/linux-debug \
  -R 'AlsaSystemTest.*(Timestamp|Suspend|Device)|AlsaSourceNodeTest.*(Pts|Xrun)' \
  --output-on-failure
```

Expected: 断言失败，表现为 PTS 未锚定、partial 未丢弃或 recovery calls 缺失。

- [ ] **Step 4: 实现首采样点锚定**

htimestamp 成功时：

```cpp
first_unread_pts_us = timespec_to_us(timestamp) -
    rescale_frames_to_us(available_frames, negotiated_.sample_rate);
```

失败时调用 `pcm_avail_update` 和 `monotonic_now` 做同样计算，并设置 `used_fallback=true`。Node 只在每段时间线第一次正数读取时提交 anchor；后续 PTS 使用：

```cpp
pts = anchor_pts_us + rescale_frames_to_us(
    samples_emitted_since_anchor, config.format().sample_rate());
```

累计值和所有 timespec/乘加运算使用显式 overflow check；不能逐帧重读 htimestamp。

- [ ] **Step 5: 实现有界恢复和错误映射**

- EPIPE：当次 read 返回 `timeline_discontinuity=true` 和 `would_block=true`；内部执行一次 prepare+start，Node 立即丢弃 partial 并等待新 anchor。
- ESTRPIPE：第一次检测时标记 timeline reset；每个后续 tick 只调用一次 resume。`-EAGAIN` 返回 would-block；resume 成功后等待数据；其他结果执行一次 prepare+start fallback。
- ENODEV/ENXIO：返回 `StatusCode::kDeviceLost`；不在 AlsaSystem 内 reopen。
- 任何 recovery 失败保留 `provider_id="alsa"`、具体 operation 与原始负 error。

- [ ] **Step 6: 运行时间戳、恢复和 ASan 定向测试**

```bash
cmake --build --preset linux-debug
ctest --test-dir build/linux-debug \
  -R 'AlsaSystemTest|AlsaSourceNodeTest' --output-on-failure
cmake --preset linux-asan
cmake --build --preset linux-asan --target \
  eavp_alsa_system_tests eavp_alsa_capture_tests
ctest --test-dir build/linux-asan \
  -R 'AlsaSystemTest|AlsaSourceNodeTest' --output-on-failure
```

Expected: 正常 480-frame 间隔 10000 us；恢复后仅首帧 discontinuity；ASan/UBSan 无报告。

- [ ] **Step 7: 提交时间线和恢复语义**

```bash
git add src/platform/linux/alsa_system.hpp src/platform/linux/alsa_system.cpp \
  src/platform/linux/alsa_capture.cpp tests/support/fake_alsa_api.hpp \
  tests/unit/alsa_system_test.cpp tests/unit/alsa_capture_test.cpp
git commit -m "feat(platform): 实现 ALSA 时间戳与采集中断恢复"
```

---

### Task 6：实现 ALSA Metrics、Health 和失败优先级

**Files:**
- Modify: `src/platform/linux/alsa_capture_internal.hpp`
- Modify: `src/platform/linux/alsa_capture.cpp`
- Modify: `tests/support/audio_test_utils.hpp`
- Modify: `tests/unit/alsa_capture_test.cpp`

**Interfaces:**
- Produces: 私有 `AlsaObserver` 事件接口及 `RegistryAlsaObserver` 生产适配。
- Produces: 扩展 `AlsaSourceNodeTestPeer` 和 `NodeFixture`，允许传入非 owning `AlsaObserver*`；空指针继续使用 production adapter。
- Produces: `alsa_capture.<node_id>.{frames,samples,bytes,short_reads,would_block,xruns,suspends,recoveries,timestamp_fallbacks,discontinuities}`。
- Produces: gauges `last_pts_us`、`actual_period_frames`、`actual_buffer_frames`、`partial_samples`。
- Produces: Health component `alsa_capture/<node_id>` 的 Ok/Degraded/Error 转换。

- [ ] **Step 1: 写 Metrics 与 Health 失败测试**

```cpp
TEST(AlsaSourceNodeTest, PublishesFrameAndRecoveryObservability) {
    ScriptedAlsa source;
    source.read_frames.push_back(240);
    source.read_errors.push_back(-EPIPE);
    source.append_complete_frames(1);
    NodeFixture fixture(source.take_system(), make_alsa_config(), 2U);
    ASSERT_TRUE(fixture.start());
    ASSERT_TRUE(fixture.tick_until_frames(1));

    EXPECT_EQ(1U, fixture.metrics.counter(
        "alsa_capture.mic0.frames").value());
    EXPECT_EQ(480U, fixture.metrics.counter(
        "alsa_capture.mic0.samples").value());
    EXPECT_EQ(1920U, fixture.metrics.counter(
        "alsa_capture.mic0.bytes").value());
    EXPECT_EQ(1U, fixture.metrics.counter(
        "alsa_capture.mic0.xruns").value());
    EXPECT_EQ(1U, fixture.metrics.counter(
        "alsa_capture.mic0.discontinuities").value());
    EXPECT_EQ(eavp::HealthStatus::kDegraded,
              fixture.health.component("alsa_capture/mic0")
                  .value().status);
}

TEST(AlsaSourceNodeTest, MediaFailureWinsOverObserverFailure) {
    ScriptedAlsa source;
    source.read_errors.push_back(-ENODEV);
    FailingAlsaObserver observer;
    observer.fail_next_report = true;
    NodeFixture fixture(source.take_system(), make_alsa_config(),
                        2U, &observer);
    const eavp::Status status = fixture.tick_once_running();
    EXPECT_EQ(eavp::StatusCode::kDeviceLost, status.code());
    EXPECT_EQ("alsa", status.provider_id());
}
```

再写 normal Ok、timestamp fallback Degraded、suspend counters、DeviceLost Error、partial_samples gauge 和单独 observer failure 向上传播。

- [ ] **Step 2: 运行测试并确认指标/健康尚未发布**

```bash
cmake --build --preset linux-debug --target eavp_alsa_capture_tests
ctest --test-dir build/linux-debug \
  -R 'AlsaSourceNodeTest.*(Observability|Observer|Health|Metrics)' \
  --output-on-failure
```

Expected: counters/Health 查询返回 NotFound 或与预期不符。

- [ ] **Step 3: 实现可测试观测适配**

```cpp
class AlsaObserver {
public:
    virtual ~AlsaObserver() {}
    virtual Status on_negotiated(int period_frames,
                                 int buffer_frames) = 0;
    virtual Status on_partial(int partial_samples) = 0;
    virtual Status on_would_block() = 0;
    virtual Status on_frame(const AudioFrame& frame) = 0;
    virtual Status on_timestamp_fallback() = 0;
    virtual Status on_recovery(bool xrun) = 0;
    virtual Status on_fatal(const Status& failure) = 0;
};
```

生产适配以 node id 构造固定 metric/health 名称。成功恢复后 Health 保持 Degraded，不由后续普通帧自动改回 Ok；新 start/reset 会话重新初始化为 Ok。

- [ ] **Step 4: 实现媒体失败优先级**

每个 tick 分别保存 `media_status` 和 `observer_status`：

```cpp
if (!media_status.ok()) {
    observer->on_fatal(media_status);
    return media_status;
}
return observer_status;
```

`kWouldBlock` 是预期调度结果，增加 counter 但不把 Health 设为 Error。observer 自身抛出的异常在适配边界转换为 `kResourceExhausted`/`kInternal`。

- [ ] **Step 5: 运行观测测试和 Management 回归**

```bash
cmake --build --preset linux-debug --target \
  eavp_alsa_capture_tests eavp_management_tests
ctest --test-dir build/linux-debug \
  -R 'AlsaSourceNodeTest|Management|Metric|Health' --output-on-failure
```

Expected: 指标精确、Health 聚合正确、DeviceLost 不被 observer failure 覆盖。

- [ ] **Step 6: 提交可观测性**

```bash
git add src/platform/linux/alsa_capture_internal.hpp \
  src/platform/linux/alsa_capture.cpp tests/support/audio_test_utils.hpp \
  tests/unit/alsa_capture_test.cpp
git commit -m "feat(platform): 发布 ALSA 采集指标与健康状态"
```

---

### Task 7：实现 300 帧确定性 Pipeline 纵切面

**Files:**
- Create: `tests/integration/alsa_capture_pipeline_test.cpp`
- Modify: `tests/integration/CMakeLists.txt`

**Interfaces:**
- Consumes: AlsaSourceNode test peer、AudioFrame OutputPort、MediaPipeline、MetricRegistry、HealthManager。
- Produces: 300 帧 Fake 集成验收，包含短读、checksum、PTS、一次 XRUN 和资源释放。

- [ ] **Step 1: 写 300 帧 Pipeline 失败测试**

```cpp
TEST(AlsaCapturePipelineTest,
     DeliversThreeHundredExactFramesAcrossShortReadsAndOneXrun) {
    ScriptedAlsa source;
    source.append_patterned_frames(150, 120, 360);
    source.append_error(-EPIPE);
    source.set_next_anchor(9000000);
    source.append_patterned_frames(150, 200, 280);

    eavp::MetricRegistry metrics;
    eavp::HealthManager health;
    std::unique_ptr<eavp::AlsaSourceNode> capture =
        AlsaSourceNodeTestPeer::create(
            "mic0", make_alsa_config(), &metrics, &health,
            source.take_system()).take_value();
    std::unique_ptr<AudioChecksumSink> sink(new AudioChecksumSink());
    AudioChecksumSink* observed_sink = sink.get();
    ASSERT_TRUE(eavp::connect(capture->output(), sink->input()).ok());

    eavp::MediaPipeline pipeline("alsa-live");
    ASSERT_TRUE(pipeline.add_node(std::move(capture)).ok());
    ASSERT_TRUE(pipeline.add_node(std::move(sink)).ok());
    ASSERT_TRUE(pipeline.connect("mic0", "audio-checksum").ok());
    ASSERT_TRUE(pipeline.start().ok());
    for (std::size_t turn = 0U;
         turn < 2000U && observed_sink->frames() < 300U; ++turn) {
        ASSERT_TRUE(pipeline.tick().ok());
    }

    EXPECT_EQ(300U, observed_sink->frames());
    EXPECT_EQ(300U * 480U, observed_sink->samples());
    EXPECT_EQ(source.expected_checksum(), observed_sink->checksum());
    EXPECT_EQ(1U, observed_sink->discontinuities());
    EXPECT_TRUE(observed_sink->normal_pts_steps_are(10000));
    EXPECT_EQ(eavp::HealthStatus::kDegraded, health.aggregate());
    ASSERT_TRUE(pipeline.stop().ok());
    EXPECT_EQ(0, source.open_handles());
}
```

`AudioChecksumSink::on_tick()` 每次最多 receive 一个 Frame，map read-only 后计算稳定 FNV-1a checksum；空队列返回 `kWouldBlock`。

同时在 `tests/integration/CMakeLists.txt` 注册红灯 target：

```cmake
add_executable(eavp_alsa_capture_pipeline_tests
               alsa_capture_pipeline_test.cpp)
target_link_libraries(eavp_alsa_capture_pipeline_tests
                      PRIVATE EAVP::platform GTest::gtest_main)
target_include_directories(eavp_alsa_capture_pipeline_tests
                           PRIVATE ${PROJECT_SOURCE_DIR}/src
                                   ${PROJECT_SOURCE_DIR}/tests)
gtest_discover_tests(eavp_alsa_capture_pipeline_tests)
```

- [ ] **Step 2: 运行测试并确认集成 target/纵切面尚不存在**

```bash
cmake --build --preset linux-debug --target \
  eavp_alsa_capture_pipeline_tests
```

Expected: target 或测试文件尚不存在而失败。

- [ ] **Step 3: 实现测试 Sink 和确定性脚本**

测试内实现 `AudioChecksumSink : MediaNode`，其 InputPort 容量为 4、OverflowPolicy 为 `kBlock`。Fake 数据按 frame index、sample index、channel 生成确定性 little-endian S16，XRUN 前的 240-sample partial 不计入 expected checksum。

- [ ] **Step 4: 运行 Debug/Release/ASan 集成测试**

```bash
for preset in linux-debug linux-release linux-asan; do
  cmake --preset "$preset"
  cmake --build --preset "$preset" --target \
    eavp_alsa_capture_pipeline_tests
  ctest --test-dir "build/$preset" \
    -R AlsaCapturePipelineTest --output-on-failure
done
```

Expected: 三套构建均接收 300 帧、一个 discontinuity、资源计数归零，无 sanitizer 报告。

- [ ] **Step 5: 提交确定性纵切面**

```bash
git add tests/integration/alsa_capture_pipeline_test.cpp \
  tests/integration/CMakeLists.txt
git commit -m "test(platform): 验证 ALSA 三百帧纵切面"
```

---

### Task 8：实现安装消费和可选真实 Loopback 验收

**Files:**
- Create: `tests/integration/alsa_capture_device_test.cpp`
- Modify: `tests/integration/CMakeLists.txt`
- Modify: `tests/consumer/main.cpp:1-54`

**Interfaces:**
- Produces: CTest `eavp.alsa_device`，仅在 `EAVP_ENABLE_ALSA_DEVICE_TESTS=ON` 时注册。
- Produces: 环境变量 device/format/rate/channels/frame samples/frame count/timeout 解析与明确诊断。
- Produces: 安装消费工程对 AudioFormat、AlsaCaptureConfig、AlsaSourceNode factory 和 `EAVP::platform` 的编译链接证明。

- [ ] **Step 1: 扩展并运行安装消费回归测试**

在 `tests/consumer/main.cpp` 增加：

```cpp
#include "eavp/media/audio_format.hpp"
#include "eavp/platform/linux/alsa_capture.hpp"

eavp::Result<eavp::AudioFormat> audio_format =
    eavp::AudioFormat::create(
        eavp::SampleFormat::kSigned16LittleEndian, 48000,
        eavp::AudioChannelLayout::kStereo,
        eavp::AudioSampleLayout::kInterleaved,
        eavp::MemoryDomain::kCpu);
if (!audio_format.ok() ||
    !eavp::AlsaCaptureConfig::create(
         "hw:Loopback,1,0", audio_format.value(), 480, 480, 4).ok()) {
    return 1;
}
```

执行：

```bash
cmake --build --preset linux-debug
ctest --test-dir build/linux-debug -R eavp.install_consumer --output-on-failure
```

Expected: Task 2 已闭合 ALSA package dependency，独立 consumer 配置、链接和运行继续通过；若失败，先修复 export，再进入设备测试。

- [ ] **Step 2: 写真实设备测试**

测试读取以下默认值并拒绝非法文本/范围：

```cpp
const std::string device = env_or("EAVP_ALSA_DEVICE", "hw:Loopback,1,0");
const int sample_rate = env_int("EAVP_ALSA_SAMPLE_RATE", 48000, 1, 768000);
const int channels = env_int("EAVP_ALSA_CHANNELS", 2, 1, 2);
const int samples = env_int("EAVP_ALSA_SAMPLES_PER_FRAME", 480, 1, 48000);
const int frame_count = env_int("EAVP_ALSA_FRAME_COUNT", 300, 1, 100000);
const int timeout_seconds = env_int("EAVP_ALSA_TIMEOUT_SECONDS", 10, 1, 600);
```

只接受 `s16le/s24le/s32le/f32le`。测试用 production factory 构建 Source 和计数 Sink；基于 monotonic deadline 反复 `pipeline.tick()`，允许 `kWouldBlock`，直到达到 frame_count 或超时。断言精确格式/大小、PTS 单调、正常相邻帧 10000 us（默认配置）、Metrics/Health；不要求 PCM 非零。

- [ ] **Step 3: 条件注册设备测试并闭合安装依赖**

```cmake
if(EAVP_ENABLE_ALSA_DEVICE_TESTS)
    add_executable(eavp_alsa_capture_device_tests
                   alsa_capture_device_test.cpp)
    target_link_libraries(eavp_alsa_capture_device_tests
                          PRIVATE EAVP::platform GTest::gtest_main)
    gtest_discover_tests(eavp_alsa_capture_device_tests
        TEST_PREFIX "eavp.alsa_device.")
endif()
```

确认安装的 `EAVPConfig.cmake` 在 ALSA-enabled build 中先 `find_dependency(ALSA)`，再 include targets。公共 ALSA header 不包含 `<alsa/asoundlib.h>`。

- [ ] **Step 4: 运行安装消费测试**

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --test-dir build/linux-debug -R eavp.install_consumer --output-on-failure
```

Expected: 临时安装、独立 configure/build/run 全部通过。

- [ ] **Step 5: 在现有外部数据源下运行真实 Loopback 测试**

```bash
cmake -S . -B build/linux-alsa-device -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DEAVP_BUILD_TESTS=ON \
  -DEAVP_BUILD_EXAMPLES=OFF \
  -DEAVP_FETCH_TEST_DEPS=OFF \
  -DEAVP_WARNINGS_AS_ERRORS=ON \
  -DEAVP_ENABLE_ALSA=ON \
  -DEAVP_ENABLE_ALSA_DEVICE_TESTS=ON
cmake --build build/linux-alsa-device
EAVP_ALSA_DEVICE=hw:Loopback,1,0 \
EAVP_ALSA_FRAME_COUNT=300 \
EAVP_ALSA_TIMEOUT_SECONDS=10 \
ctest --test-dir build/linux-alsa-device \
  -R '^eavp\.alsa_device\.' --output-on-failure
```

Expected: 现有 producer 持续提供 PCM 时 300 帧通过。若节点不存在、权限不足、配置不支持或无数据，保留准确环境诊断并停止；不得启动 FFmpeg/arecord，也不得把环境失败改写成测试通过。

- [ ] **Step 6: 提交安装与设备验收**

```bash
git add tests/integration/alsa_capture_device_test.cpp \
  tests/integration/CMakeLists.txt tests/consumer/main.cpp
git commit -m "test(platform): 增加 ALSA 设备与安装验收"
```

---

### Task 9：完成全矩阵验证、迁移说明和规格闭合

**Files:**
- Create: `docs/migrations/0.2-to-0.3-audio.md`
- Modify: `docs/architecture/core-contracts.md`
- Modify: `docs/architecture/threading-and-lifecycle.md`
- Modify: `docs/architecture/build-and-portability.md`
- Modify: `docs/architecture/testing-strategy.md`
- Modify: `docs/architecture/versioning-and-abi.md`
- Modify: `README.md`
- Modify: `docs/roadmap.md`
- Modify: `docs/superpowers/specs/2026-08-22-eavp-linux-alsa-capture-design.md`

**Interfaces:**
- Consumes: Tasks 1-8 的全部实现和验收。
- Produces: 可复现的 Debug/Release/ASan、安装消费、三套 ARM Core 交叉编译、真实 Loopback 验证记录和 AudioFrame 迁移说明。

- [ ] **Step 1: clean 运行三套 host 构建和完整 CTest**

```bash
for preset in linux-debug linux-release linux-asan; do
  cmake --build --preset "$preset" --target clean
  cmake --preset "$preset"
  cmake --build --preset "$preset"
  ctest --preset "$preset" --output-on-failure
done
```

Expected: 所有普通测试和 `eavp.install_consumer` 通过；真实设备测试因默认 OFF 不在普通矩阵中；ASan/UBSan 无报告。

- [ ] **Step 2: clean 运行三套 ARM Core 交叉构建并检查对象架构**

```bash
for preset in rockchip-armhf-release hisiv600-release aarch64-release; do
  cmake --preset "$preset"
  cmake --build --preset "$preset" --target clean
  cmake --build --preset "$preset"
done

arm-linux-gnueabihf-readelf -h \
  build/rockchip-armhf-release/src/CMakeFiles/eavp_media.dir/media/audio_format.cpp.o \
  | rg 'Machine:.*ARM'
arm-hisiv600-linux-readelf -h \
  build/hisiv600-release/src/CMakeFiles/eavp_media.dir/media/audio_format.cpp.o \
  | rg 'Machine:.*ARM'
aarch64-linux-gnu-readelf -h \
  build/aarch64-release/src/CMakeFiles/eavp_media.dir/media/audio_format.cpp.o \
  | rg 'Machine:.*AArch64'
```

Expected: 两套 ARM32 为 `Machine: ARM`，aarch64 为 `Machine: AArch64`；配置日志不查找或链接目标 libasound。若任一已批准工具链命令缺失或 sysroot 异常，停止并请求用户修复环境，不安装替代工具链。

- [ ] **Step 3: 复跑真实 Loopback 验收并记录精确结果**

```bash
EAVP_ALSA_DEVICE=hw:Loopback,1,0 \
EAVP_ALSA_SAMPLE_FORMAT=s16le \
EAVP_ALSA_SAMPLE_RATE=48000 \
EAVP_ALSA_CHANNELS=2 \
EAVP_ALSA_SAMPLES_PER_FRAME=480 \
EAVP_ALSA_FRAME_COUNT=300 \
EAVP_ALSA_TIMEOUT_SECONDS=10 \
ctest --test-dir build/linux-alsa-device \
  -R '^eavp\.alsa_device\.' --output-on-failure
```

Expected: 300 帧、PTS/格式/Metrics/Health 验收通过；不得把无 producer 的 timeout 记录成代码通过。

- [ ] **Step 4: 编写 AudioFrame 迁移说明**

迁移文档必须包含以下 before/after：

```cpp
// 0.2
AudioFrame::create(buffer, SampleFormat::kSigned16,
                   48000, 2, 480, pts, time_base);

// 0.3b
AudioFormat format = AudioFormat::create(
    SampleFormat::kSigned16LittleEndian, 48000,
    AudioChannelLayout::kStereo,
    AudioSampleLayout::kInterleaved,
    MemoryDomain::kCpu).take_value();
AudioFrame::create(buffer, format, 480, pts, time_base,
                   discontinuity);
```

明确 `samples -> samples_per_channel`、S24_LE 32-bit container、首采样点 PTS、无 DTS 和 discontinuity 语义。

- [ ] **Step 5: 更新架构、README、路线图和规格状态**

- Core contracts：加入 AudioFormat、AudioFrame 精确 Buffer 和 PTS/DTS 分层。
- Threading/lifecycle：加入非阻塞 ALSA tick、短读、背压和恢复。
- Build/portability：加入 ALSA option、host ON、ARM OFF 的诚实边界。
- Testing strategy：加入 Fake 300 帧和显式设备测试。
- Versioning/ABI：链接迁移文档；保持 C++ API 1.0 前实验性。
- README：声明 0.3b 开发能力，但当前稳定 package 仍为 0.2.0。
- Roadmap：仅在全部验收通过后把 0.3b 标记已完成。
- 设计规格：状态改为“已实现”，附实际命令、测试总数和设备结果；若设备环境未通过则保持“已批准/实现中”。

- [ ] **Step 6: 执行最终静态检查**

```bash
git diff --check
bad_markers='TO''DO|TB''D|FIX''ME|待''定|占''位'
rg -n "$bad_markers" \
  README.md docs include src tests CMakeLists.txt CMakePresets.json
rg -n 'kSigned16\b|\.samples\(\)' include src tests examples
rg -n '#include <alsa/|#include <alsa/asoundlib.h>' include/eavp
git status --short
```

Expected: diff check 通过；无未完成标记；无旧 AudioFrame API；公共头不包含 ALSA；status 只包含本任务文件和用户原有个人文档状态。

- [ ] **Step 7: 提交文档与验收记录**

```bash
git add README.md docs/roadmap.md \
  docs/migrations/0.2-to-0.3-audio.md \
  docs/architecture/core-contracts.md \
  docs/architecture/threading-and-lifecycle.md \
  docs/architecture/build-and-portability.md \
  docs/architecture/testing-strategy.md \
  docs/architecture/versioning-and-abi.md \
  docs/superpowers/specs/2026-08-22-eavp-linux-alsa-capture-design.md
git commit -m "docs(platform): 闭合 ALSA 音频采集验收"
```

---

## 完成判据

- 四种 interleaved SampleFormat、mono/stereo、AudioFormat/AudioFrame 精确校验全部通过。
- 300 个 48 kHz stereo S16_LE Frame 无重复、无不足帧，正常相邻 PTS 为 10000 us。
- XRUN/suspend 恢复有界，partial 被丢弃，恢复后仅首帧 discontinuity。
- DeviceLost 保留 `provider_id/operation/native_code`，不自动无限重连。
- Debug、Release、ASan/UBSan、安装消费全部通过。
- 真实 ALSA Loopback 在现有 producer 下采集 300 帧；静音不被误判失败。
- Rockchip ARMHF、HiSilicon v600、aarch64 clean build 平台无关音频 Core，对象架构正确；不虚报 ARM ALSA 链接或设备验证。
- Core/公共头不依赖 ALSA 或 FFmpeg；0.3b target 不链接 FFmpeg。
- 文档、迁移说明和规格状态与实际证据一致，个人学习记录未被触碰或提交。
