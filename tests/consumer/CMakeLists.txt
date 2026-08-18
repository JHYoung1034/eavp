cmake_minimum_required(VERSION 3.21)
project(EAVPConsumer LANGUAGES CXX)

find_package(EAVP 0.1 CONFIG REQUIRED)

add_executable(eavp_consumer main.cpp)
target_compile_features(eavp_consumer PRIVATE cxx_std_11)
target_link_libraries(eavp_consumer PRIVATE EAVP::platform)

