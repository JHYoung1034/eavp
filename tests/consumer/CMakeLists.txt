cmake_minimum_required(VERSION 3.21)
project(EAVPConsumer LANGUAGES CXX)

find_package(EAVP 0.2 CONFIG REQUIRED)

if(NOT DEFINED EAVP_LINUX_RUNTIME_ENABLED)
    message(FATAL_ERROR "EAVP package 未导出 Linux Runtime 能力")
endif()
if(NOT DEFINED EAVP_V4L2_ENABLED)
    message(FATAL_ERROR "EAVP package 未导出 V4L2 能力")
endif()

add_executable(eavp_consumer main.cpp)
target_compile_features(eavp_consumer PRIVATE cxx_std_11)
target_link_libraries(eavp_consumer PRIVATE EAVP::platform)
if(EAVP_LINUX_RUNTIME_ENABLED)
    target_compile_definitions(
        eavp_consumer PRIVATE EAVP_CONSUMER_LINUX_RUNTIME_ENABLED=1)
endif()
if(EAVP_V4L2_ENABLED)
    target_compile_definitions(
        eavp_consumer PRIVATE EAVP_CONSUMER_V4L2_ENABLED=1)
endif()
if(EAVP_ALSA_ENABLED)
    target_compile_definitions(eavp_consumer PRIVATE EAVP_CONSUMER_ALSA_ENABLED=1)
endif()
