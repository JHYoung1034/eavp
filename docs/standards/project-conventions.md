# EAVP 工程规范

> 状态：已接受  
> 适用版本：EAVP Core 0.1 起  
> 规范性范围：仓库内代码、文档、构建、测试和提交

## 文档语言

所有新增或修改的项目文档、ADR、实施计划、README 和面向用户的说明优先使用简体中文。下列内容可使用英文：代码和标识符、协议字段、命令及输出、文件路径、标准和第三方项目名称、必须保真的引文。

文档必须使用 UTF-8、LF 换行并以换行符结尾。规范性文档应声明状态、适用版本和规范性范围，不得留下未解释的 `TODO` 或 `TBD`。

## C++ 与接口

- 语言基线为 C++11；最低工程验证编译器为 GCC 7.5。
- 命名空间为 `eavp`，安装后的 CMake targets 使用 `EAVP::` 命名空间。
- 类型和类使用 `UpperCamelCase`，函数和变量使用 `lower_snake_case`，常量使用 `kUpperCamelCase`。
- 公共方法不允许异常跨模块边界，使用 `Status` 或 `Result<T>`。
- 公共 API 在 1.0 前属于实验性接口；破坏性变化必须更新 minor 版本和变更记录。

## 构建与依赖

- 最低 CMake 版本为 3.21，首选 Ninja 1.10 及以上。
- 生产核心仅依赖 C++11 标准库和 POSIX。
- 测试使用 GoogleTest `release-1.12.1` 的固定提交。默认不隐式联网；仅显式启用 `EAVP_FETCH_TEST_DEPS` 时下载。
- 引入依赖前必须记录版本、来源、用途、许可证、目标侧链接方式和升级责任人。

## 格式、静态检查与提交

- C++ 使用仓库 `.clang-format`；格式化属于显式操作，不在普通构建中自动改写源码。
- `.clang-tidy` 只提供检查基线，不自动修复。
- CI 使用 `-Wall -Wextra -Wpedantic`；`EAVP_WARNINGS_AS_ERRORS` 在验证预设中开启。
- 提交使用 Conventional Commits 类型前缀和简体中文摘要。

