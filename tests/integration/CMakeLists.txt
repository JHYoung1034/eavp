add_executable(eavp_simulated_platform_tests simulated_platform_test.cpp)
target_link_libraries(eavp_simulated_platform_tests PRIVATE EAVP::platform GTest::gtest_main)
gtest_discover_tests(eavp_simulated_platform_tests)

add_executable(eavp_reference_media_platform_tests reference_media_platform_test.cpp)
target_link_libraries(eavp_reference_media_platform_tests PRIVATE EAVP::platform GTest::gtest_main)
gtest_discover_tests(eavp_reference_media_platform_tests)

add_executable(eavp_alsa_capture_pipeline_tests
               alsa_capture_pipeline_test.cpp)
target_link_libraries(eavp_alsa_capture_pipeline_tests
                      PRIVATE EAVP::platform GTest::gtest_main)
target_include_directories(eavp_alsa_capture_pipeline_tests
                           PRIVATE ${PROJECT_SOURCE_DIR}/src
                                   ${PROJECT_SOURCE_DIR}/tests)
gtest_discover_tests(eavp_alsa_capture_pipeline_tests)
