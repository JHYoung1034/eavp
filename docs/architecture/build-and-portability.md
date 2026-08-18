# 构建、安装与移植

> 状态：已接受  
> 适用版本：EAVP Core 0.1  
> 规范性范围：支持的构建入口与工具链

最低要求为 CMake 3.21、Ninja 1.10 和支持 C++11 的编译器。仓库级 `CMakePresets.json` 可提交，开发者私有的 `CMakeUserPresets.json` 不得提交。

原生验证使用：

```bash
cmake --preset linux-debug-fetch-deps
cmake --build --preset linux-debug-fetch-deps
ctest --preset linux-debug-fetch-deps
```

aarch64 交叉编译要求 PATH 中存在 `aarch64-linux-gnu-gcc` 和 `aarch64-linux-gnu-g++`：

```bash
cmake --preset aarch64-release
cmake --build --preset aarch64-release
```

交叉构建关闭测试和示例，不隐式下载依赖。

如果工具链被解包到非系统目录，应通过标准 CMake 参数提供 sysroot，而不是把本机绝对路径写入仓库 toolchain 文件：

```bash
cmake --preset aarch64-release -DCMAKE_SYSROOT=/path/to/toolchain/root
cmake --build --preset aarch64-release
```

Debug、Release 和 Sanitizer 默认预设不联网。构建机没有系统 GoogleTest 时，可显式使用对应的 `*-fetch-deps` 预设；这些预设仍固定使用登记的提交。

安装后消费方使用：

```cmake
find_package(EAVP 0.1 CONFIG REQUIRED)
target_link_libraries(product PRIVATE EAVP::platform)
```
