# 构建、安装与移植

> 状态：已接受\
> 适用版本：EAVP 0.2.0\
> 规范性范围：支持的构建入口与工具链

最低要求为 CMake 3.21、Ninja 1.10 和支持 C++11 的编译器。仓库级 `CMakePresets.json` 可提交，开发者私有的 `CMakeUserPresets.json` 不得提交。

原生验证使用：

```bash
cmake --preset linux-debug-fetch-deps
cmake --build --preset linux-debug-fetch-deps
ctest --preset linux-debug-fetch-deps
```

三套交叉编译预设均为 Release，关闭测试和示例，并禁止下载测试依赖。它们只验证 Core 与 Reference Backend 的生成和链接，不执行目标程序，也不代表任何厂商媒体 API 已接入：

```bash
# 通用 aarch64：aarch64-linux-gnu-gcc / aarch64-linux-gnu-g++
cmake --preset aarch64-release
cmake --build --preset aarch64-release

# Rockchip ARM32：arm-linux-gnueabihf-gcc / arm-linux-gnueabihf-g++
cmake --preset rockchip-armhf-release
cmake --build --preset rockchip-armhf-release

# 海思 v600 ARM32：arm-hisiv600-linux-gcc / arm-hisiv600-linux-g++
cmake --preset hisiv600-release
cmake --build --preset hisiv600-release
```

ARM32 产物的对象文件应由 `readelf -h` 报告为 `Machine: ARM`，aarch64 应报告为 `Machine: AArch64`。该检查只确认目标架构，不验证 Rockchip MPP/RGA、海思 HI_MPI 或其硬件功能。

如果工具链被解包到非系统目录，应通过标准 CMake 参数提供 sysroot，而不是把本机绝对路径写入仓库 toolchain 文件：

```bash
cmake --preset rockchip-armhf-release -DCMAKE_SYSROOT=/path/to/toolchain/root
cmake --build --preset rockchip-armhf-release
```

Debug、Release 和 Sanitizer 默认预设不联网。构建机没有系统 GoogleTest 时，可显式使用对应的 `*-fetch-deps` 预设；这些预设仍固定使用登记的提交。

安装后消费方使用：

```cmake
find_package(EAVP 0.2 CONFIG REQUIRED)
target_link_libraries(product PRIVATE EAVP::platform)
```
