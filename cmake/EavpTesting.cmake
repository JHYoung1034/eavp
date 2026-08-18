find_package(GTest 1.12 CONFIG QUIET)

if(NOT GTest_FOUND)
    if(NOT EAVP_FETCH_TEST_DEPS)
        message(FATAL_ERROR
            "未找到 GoogleTest 1.12。请安装兼容包，或显式设置 EAVP_FETCH_TEST_DEPS=ON。")
    endif()

    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG 58d77fa8070e8cec2dc1ed015d66b454c8d78850
        GIT_SHALLOW FALSE
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

