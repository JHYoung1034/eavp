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
gtest_discover_tests(eavp_media_backend_tests)

add_executable(eavp_control_tests control_test.cpp)
target_link_libraries(eavp_control_tests PRIVATE EAVP::control GTest::gtest_main)
gtest_discover_tests(eavp_control_tests)

add_executable(eavp_management_tests management_test.cpp)
target_link_libraries(eavp_management_tests PRIVATE EAVP::management GTest::gtest_main)
gtest_discover_tests(eavp_management_tests)
