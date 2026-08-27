cmake_minimum_required(VERSION 3.21)

project(EAVP VERSION 0.2.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

option(EAVP_BUILD_TESTS "构建 EAVP 测试" ON)
option(EAVP_BUILD_EXAMPLES "构建 EAVP 示例" ON)
option(EAVP_FETCH_TEST_DEPS "允许下载固定版本的测试依赖" OFF)
option(EAVP_WARNINGS_AS_ERRORS "把编译器警告视为错误" OFF)
option(EAVP_ENABLE_SANITIZERS "启用 AddressSanitizer 和 UndefinedBehaviorSanitizer" OFF)
option(EAVP_ENABLE_THREAD_SANITIZER "启用 ThreadSanitizer" OFF)
option(EAVP_ENABLE_ALSA "启用 Linux ALSA PCM 采集" OFF)
option(EAVP_ENABLE_ALSA_DEVICE_TESTS "启用真实 ALSA 设备测试" OFF)

set(EAVP_LINUX_BACKEND_DEFAULT OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(EAVP_LINUX_BACKEND_DEFAULT ON)
endif()
option(EAVP_ENABLE_LINUX_RUNTIME "启用 Linux 事件驱动 Runtime"
       ${EAVP_LINUX_BACKEND_DEFAULT})
option(EAVP_ENABLE_V4L2 "启用 Linux V4L2 采集"
       ${EAVP_LINUX_BACKEND_DEFAULT})
option(EAVP_ENABLE_V4L2_DEVICE_TESTS "启用真实 V4L2 设备测试" OFF)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(EAVP_ENABLE_LINUX_RUNTIME OFF CACHE BOOL
        "启用 Linux 事件驱动 Runtime" FORCE)
    set(EAVP_ENABLE_V4L2 OFF CACHE BOOL "启用 Linux V4L2 采集" FORCE)
endif()

if(EAVP_ENABLE_THREAD_SANITIZER AND EAVP_ENABLE_SANITIZERS)
    message(FATAL_ERROR "ThreadSanitizer 不能与 AddressSanitizer/UndefinedBehaviorSanitizer 同时启用")
endif()
if(EAVP_ENABLE_V4L2_DEVICE_TESTS AND NOT EAVP_ENABLE_LINUX_RUNTIME)
    message(FATAL_ERROR "真实 V4L2 设备测试要求 EAVP_ENABLE_LINUX_RUNTIME=ON")
endif()
if(EAVP_ENABLE_V4L2_DEVICE_TESTS AND NOT EAVP_ENABLE_V4L2)
    message(FATAL_ERROR "真实 V4L2 设备测试要求 EAVP_ENABLE_V4L2=ON")
endif()
if(EAVP_ENABLE_V4L2)
    include(CheckIncludeFileCXX)
    check_include_file_cxx("linux/videodev2.h" EAVP_HAVE_LINUX_VIDEODEV2_H)
    if(NOT EAVP_HAVE_LINUX_VIDEODEV2_H)
        message(FATAL_ERROR "EAVP_ENABLE_V4L2=ON 要求 <linux/videodev2.h>")
    endif()
endif()

if(EAVP_ENABLE_ALSA_DEVICE_TESTS AND NOT EAVP_ENABLE_ALSA)
    message(FATAL_ERROR "真实 ALSA 设备测试要求 EAVP_ENABLE_ALSA=ON")
endif()
if(EAVP_ENABLE_ALSA)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "ALSA 采集仅支持 Linux")
    endif()
    find_package(ALSA REQUIRED)
endif()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(eavp_project_options INTERFACE)
target_compile_features(eavp_project_options INTERFACE cxx_std_11)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(eavp_project_options INTERFACE -Wall -Wextra -Wpedantic)
    if(EAVP_WARNINGS_AS_ERRORS)
        target_compile_options(eavp_project_options INTERFACE -Werror)
    endif()
    if(EAVP_ENABLE_SANITIZERS)
        target_compile_options(eavp_project_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(eavp_project_options INTERFACE -fsanitize=address,undefined)
    endif()
    if(EAVP_ENABLE_THREAD_SANITIZER)
        target_compile_options(eavp_project_options INTERFACE
            -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(eavp_project_options INTERFACE -fsanitize=thread)
    endif()
endif()

add_subdirectory(src)

if(EAVP_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(eavp_sanitized_target IN ITEMS
            eavp_media eavp_control eavp_management eavp_platform)
        target_compile_options(
            ${eavp_sanitized_target}
            INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer
        )
        target_link_options(
            ${eavp_sanitized_target}
            INTERFACE -fsanitize=address,undefined
        )
    endforeach()
endif()

if(EAVP_ENABLE_THREAD_SANITIZER AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(eavp_tsan_target IN ITEMS
            eavp_media eavp_control eavp_management eavp_platform)
        target_compile_options(
            ${eavp_tsan_target}
            INTERFACE -fsanitize=thread -fno-omit-frame-pointer
        )
        target_link_options(
            ${eavp_tsan_target}
            INTERFACE -fsanitize=thread
        )
    endforeach()
endif()

if(EAVP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(EAVP_BUILD_TESTS)
    include(CTest)
    enable_testing()
    include(cmake/EavpTesting.cmake)
    add_subdirectory(tests)
endif()

install(
    EXPORT EAVPTargets
    FILE EAVPTargets.cmake
    NAMESPACE EAVP::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EAVP
)

configure_package_config_file(
    cmake/EAVPConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/EAVPConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EAVP
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/EAVPConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/EAVPConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/EAVPConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/EAVP
)
