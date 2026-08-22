# Task 3 报告：可注入的 libasound 会话与生命周期

## 实现摘要

- 新增私有 `detail::AlsaApi` seam 及逐一委托 libasound 的 `LibasoundApi`；ALSA 类型只存在于 `src/platform/linux` 私有头，未进入公开头。
- 新增 `detail::AlsaSystem`：以 capture/nonblock/no-auto-* flags 打开设备，按规格顺序配置 HW/SW params，精确读回 access、format、channel、rate，并记录实际 period/buffer 与单调 timestamp 可用性。
- `prepare()` 对原生调用失败返回带 `provider_id="alsa"`、operation 和原始 negative native code 的 `Status`；任一部分失败经统一逆序清理释放 SW params、HW params 和 PCM。
- `start()`/`stop()` 幂等；`stop()` 即使 `snd_pcm_drop` 失败仍关闭设备并返回首个错误；析构为 `noexcept` 清理。
- 新增可脚本化 Fake、共享音频测试工具、8 个 ALSA 会话测试，并仅在 `EAVP_ENABLE_ALSA` 时编译 native ALSA 源和测试目标。

## RED 证据

命令：

```sh
cmake --build --preset linux-debug --target eavp_alsa_system_tests
```

预期：测试已注册但内部 ALSA seam 尚不存在，编译失败并指出缺少 `alsa_system.hpp`、`AlsaApi` 或 `FakeAlsaApi`。

实际：命令以失败退出；编译 `tests/unit/alsa_system_test.cpp` 时报告：

```text
fatal error: platform/linux/alsa_system.hpp: No such file or directory
```

这证明失败来自尚未实现的目标 seam，而非测试拼写或配置问题。

## GREEN 与回归证据

命令：

```sh
cmake --build --preset linux-debug --target eavp_alsa_system_tests
ctest --test-dir build/linux-debug -R AlsaSystemTest --output-on-failure
```

输出：目标构建成功；`AlsaSystemTest` 共 `8/8` 通过，`100% tests passed, 0 tests failed`。

最终全量命令：

```sh
cmake --build --preset linux-debug
ctest --preset linux-debug --output-on-failure
git diff --check
```

输出：全量 Debug 构建成功；CTest `124` 项中 `0` 项失败（其中既有
`AlsaCaptureConfigTest.RejectsFrameByteCountThatOverflowsSizeType` 在 64-bit
环境按既有条件跳过）；`git diff --check` 无输出且以 0 退出。

## 覆盖测试

- `tests/unit/alsa_system_test.cpp`
  - 精确 capture/interleaved 配置、参数读回与 negotiated 结果；
  - 四种批准的 `SampleFormat` 到 ALSA format 映射；
  - 从 open 至 prepare 的每个准备步骤失败均释放已拥有资源；
  - rate/format 协商改变拒绝；
  - start/stop 幂等、drop 失败仍 close 且保留首错；
  - 缺失设备和 capability mismatch 的 Status 映射/原生码；
  - `sw_params_set_tstamp_type=-EINVAL` 降级，不消费 realtime htimestamp。
- `tests/support/fake_alsa_api.hpp` 提供可观察的请求参数、失败注入、资源计数和后续 Task 4/5 可复用的共享状态。

## 自审与边界

- 已检查公共 `include/eavp/...` 未包含 ALSA 类型；native 源与测试均受 `EAVP_ENABLE_ALSA` 控制。
- 对协商 API 的 `-ENODEV/-ENXIO/-ENOMEM` 保持为 `kDeviceLost`/`kResourceExhausted`，其余协商失败为 `kCapabilityMismatch`；其他 PCM I/O 保持 `kIoError` 和原始 native code。
- 本任务未实现 Task 4 的 read/聚合，也未实现 Task 5 的 timestamp 锚定、htimestamp 消费、XRUN 或 suspend 恢复；当前 seam 只定义这些后续所需原始方法。
- 已按任务限制未安装软件、未修改个人学习文档或构建产物。
