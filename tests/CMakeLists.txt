add_subdirectory(unit)
add_subdirectory(integration)

add_test(
    NAME eavp.install_consumer
    COMMAND ${CMAKE_COMMAND}
        -DEAVP_BUILD_DIR=${PROJECT_BINARY_DIR}
        -DEAVP_INSTALL_DIR=${PROJECT_BINARY_DIR}/consumer-prefix
        -DEAVP_CONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/consumer
        -DEAVP_CONSUMER_BUILD=${PROJECT_BINARY_DIR}/consumer-build
        -DEAVP_BUILD_CONFIG=${CMAKE_BUILD_TYPE}
        -P ${PROJECT_SOURCE_DIR}/cmake/RunInstallConsumer.cmake
)
set_tests_properties(eavp.install_consumer PROPERTIES RUN_SERIAL TRUE)
