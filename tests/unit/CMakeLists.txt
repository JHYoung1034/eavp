add_executable(eavp_base_tests base_test.cpp)
target_link_libraries(eavp_base_tests PRIVATE EAVP::base GTest::gtest_main)
gtest_discover_tests(eavp_base_tests)

add_executable(eavp_media_object_tests media_object_test.cpp)
target_link_libraries(eavp_media_object_tests PRIVATE EAVP::media GTest::gtest_main)
gtest_discover_tests(eavp_media_object_tests)

add_executable(eavp_media_runtime_tests media_runtime_test.cpp)
target_link_libraries(eavp_media_runtime_tests PRIVATE EAVP::media GTest::gtest_main)
gtest_discover_tests(eavp_media_runtime_tests)

add_executable(eavp_media_backend_tests media_backend_test.cpp)
target_link_libraries(eavp_media_backend_tests PRIVATE EAVP::media GTest::gtest_main)
target_include_directories(eavp_media_backend_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
gtest_discover_tests(eavp_media_backend_tests)

add_executable(eavp_control_tests control_test.cpp)
target_link_libraries(eavp_control_tests PRIVATE EAVP::control GTest::gtest_main)
gtest_discover_tests(eavp_control_tests)

add_executable(eavp_management_tests management_test.cpp)
target_link_libraries(eavp_management_tests PRIVATE EAVP::management GTest::gtest_main)
gtest_discover_tests(eavp_management_tests)

if(EAVP_ENABLE_LINUX_RUNTIME)
    add_executable(eavp_platform_runtime_tests platform_runtime_test.cpp)
    target_link_libraries(eavp_platform_runtime_tests
                          PRIVATE EAVP::platform GTest::gtest_main)
    gtest_discover_tests(eavp_platform_runtime_tests)
endif()

add_executable(eavp_alsa_capture_config_tests alsa_capture_config_test.cpp)
target_link_libraries(eavp_alsa_capture_config_tests
                      PRIVATE EAVP::platform GTest::gtest_main)
gtest_discover_tests(eavp_alsa_capture_config_tests)

if(EAVP_ENABLE_V4L2)
    add_executable(eavp_v4l2_capture_config_tests v4l2_capture_config_test.cpp)
    target_link_libraries(eavp_v4l2_capture_config_tests
                          PRIVATE EAVP::platform GTest::gtest_main)
    target_include_directories(eavp_v4l2_capture_config_tests
                               PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    gtest_discover_tests(eavp_v4l2_capture_config_tests)

    add_executable(eavp_v4l2_system_tests v4l2_system_test.cpp)
    target_link_libraries(eavp_v4l2_system_tests
                          PRIVATE EAVP::platform GTest::gtest_main)
    target_include_directories(eavp_v4l2_system_tests
                               PRIVATE ${PROJECT_SOURCE_DIR}/src
                                       ${PROJECT_SOURCE_DIR}/tests)
    gtest_discover_tests(eavp_v4l2_system_tests)

    add_executable(eavp_v4l2_capture_tests v4l2_capture_test.cpp)
    target_link_libraries(eavp_v4l2_capture_tests
                          PRIVATE EAVP::platform GTest::gtest_main)
    target_include_directories(eavp_v4l2_capture_tests
                               PRIVATE ${PROJECT_SOURCE_DIR}/src
                                       ${PROJECT_SOURCE_DIR}/tests)
    gtest_discover_tests(eavp_v4l2_capture_tests)
endif()

if(EAVP_ENABLE_ALSA)
    add_executable(eavp_alsa_capture_tests alsa_capture_test.cpp)
    target_link_libraries(eavp_alsa_capture_tests
                          PRIVATE EAVP::platform GTest::gtest_main)
    target_include_directories(eavp_alsa_capture_tests
                               PRIVATE ${PROJECT_SOURCE_DIR}/src
                                       ${PROJECT_SOURCE_DIR}/tests)
    gtest_discover_tests(eavp_alsa_capture_tests)

    add_executable(eavp_alsa_system_tests alsa_system_test.cpp)
    target_link_libraries(eavp_alsa_system_tests
                          PRIVATE EAVP::platform GTest::gtest_main)
    target_include_directories(eavp_alsa_system_tests
                               PRIVATE ${PROJECT_SOURCE_DIR}/src
                                       ${PROJECT_SOURCE_DIR}/tests)
    gtest_discover_tests(eavp_alsa_system_tests)
endif()

set(eavp_configure_test_root "${PROJECT_BINARY_DIR}/configure-tests")

add_test(
    NAME eavp.configure.v4l2_device_requires_runtime
    COMMAND ${CMAKE_COMMAND}
        -S ${PROJECT_SOURCE_DIR}
        -B ${eavp_configure_test_root}/v4l2-device-without-runtime
        -G Ninja
        -DEAVP_BUILD_TESTS=OFF
        -DEAVP_BUILD_EXAMPLES=OFF
        -DEAVP_ENABLE_V4L2_DEVICE_TESTS=ON
        -DEAVP_ENABLE_V4L2=ON
        -DEAVP_ENABLE_LINUX_RUNTIME=OFF
)
set_tests_properties(
    eavp.configure.v4l2_device_requires_runtime
    PROPERTIES WILL_FAIL TRUE RUN_SERIAL TRUE)

add_test(
    NAME eavp.configure.v4l2_device_requires_v4l2
    COMMAND ${CMAKE_COMMAND}
        -S ${PROJECT_SOURCE_DIR}
        -B ${eavp_configure_test_root}/v4l2-device-without-v4l2
        -G Ninja
        -DEAVP_BUILD_TESTS=OFF
        -DEAVP_BUILD_EXAMPLES=OFF
        -DEAVP_ENABLE_V4L2_DEVICE_TESTS=ON
        -DEAVP_ENABLE_V4L2=OFF
        -DEAVP_ENABLE_LINUX_RUNTIME=ON
)
set_tests_properties(
    eavp.configure.v4l2_device_requires_v4l2
    PROPERTIES WILL_FAIL TRUE RUN_SERIAL TRUE)

add_test(
    NAME eavp.configure.tsan_rejects_asan
    COMMAND ${CMAKE_COMMAND}
        -S ${PROJECT_SOURCE_DIR}
        -B ${eavp_configure_test_root}/tsan-with-asan
        -G Ninja
        -DEAVP_BUILD_TESTS=OFF
        -DEAVP_BUILD_EXAMPLES=OFF
        -DEAVP_ENABLE_THREAD_SANITIZER=ON
        -DEAVP_ENABLE_SANITIZERS=ON
)
set_tests_properties(
    eavp.configure.tsan_rejects_asan
    PROPERTIES WILL_FAIL TRUE RUN_SERIAL TRUE)

add_test(
    NAME eavp.configure.non_linux_disables_linux_backends
    COMMAND /bin/sh -c
        "\"${CMAKE_COMMAND}\" -S \"${PROJECT_SOURCE_DIR}\" -B \"${eavp_configure_test_root}/non-linux\" -G Ninja -DCMAKE_SYSTEM_NAME=Generic -DEAVP_BUILD_TESTS=OFF -DEAVP_BUILD_EXAMPLES=OFF -DEAVP_ENABLE_ALSA=OFF -DEAVP_ENABLE_LINUX_RUNTIME=ON -DEAVP_ENABLE_V4L2=ON >/dev/null && test \"$(sed -n 's/^EAVP_ENABLE_LINUX_RUNTIME:BOOL=//p' \"${eavp_configure_test_root}/non-linux/CMakeCache.txt\")\" = OFF && test \"$(sed -n 's/^EAVP_ENABLE_V4L2:BOOL=//p' \"${eavp_configure_test_root}/non-linux/CMakeCache.txt\")\" = OFF"
)
set_tests_properties(
    eavp.configure.non_linux_disables_linux_backends
    PROPERTIES RUN_SERIAL TRUE)
