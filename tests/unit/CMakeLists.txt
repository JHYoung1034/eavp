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

add_executable(eavp_platform_runtime_tests platform_runtime_test.cpp)
target_link_libraries(eavp_platform_runtime_tests PRIVATE EAVP::platform GTest::gtest_main)
gtest_discover_tests(eavp_platform_runtime_tests)

add_executable(eavp_alsa_capture_config_tests alsa_capture_config_test.cpp)
target_link_libraries(eavp_alsa_capture_config_tests
                      PRIVATE EAVP::platform GTest::gtest_main)
gtest_discover_tests(eavp_alsa_capture_config_tests)

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
