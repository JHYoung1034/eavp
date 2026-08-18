add_executable(eavp_base_tests base_test.cpp)
target_link_libraries(eavp_base_tests PRIVATE EAVP::base GTest::gtest_main)
gtest_discover_tests(eavp_base_tests)

add_executable(eavp_media_object_tests media_object_test.cpp)
target_link_libraries(eavp_media_object_tests PRIVATE EAVP::media GTest::gtest_main)
gtest_discover_tests(eavp_media_object_tests)

add_executable(eavp_media_runtime_tests media_runtime_test.cpp)
target_link_libraries(eavp_media_runtime_tests PRIVATE EAVP::media GTest::gtest_main)
gtest_discover_tests(eavp_media_runtime_tests)
