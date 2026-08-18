execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${EAVP_BUILD_DIR}" --prefix "${EAVP_INSTALL_DIR}"
            --config "${EAVP_BUILD_CONFIG}"
    RESULT_VARIABLE install_result
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "EAVP 安装失败，退出码：${install_result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${EAVP_CONSUMER_SOURCE}" -B "${EAVP_CONSUMER_BUILD}"
            -G Ninja -DCMAKE_BUILD_TYPE=${EAVP_BUILD_CONFIG}
            -DCMAKE_PREFIX_PATH=${EAVP_INSTALL_DIR}
    RESULT_VARIABLE configure_result
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "消费工程配置失败，退出码：${configure_result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${EAVP_CONSUMER_BUILD}"
    RESULT_VARIABLE build_result
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "消费工程构建失败，退出码：${build_result}")
endif()

execute_process(
    COMMAND "${EAVP_CONSUMER_BUILD}/eavp_consumer"
    RESULT_VARIABLE run_result
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "消费工程运行失败，退出码：${run_result}")
endif()

