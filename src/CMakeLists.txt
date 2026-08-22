add_library(eavp_base INTERFACE)
add_library(EAVP::base ALIAS eavp_base)

target_link_libraries(eavp_base INTERFACE $<BUILD_INTERFACE:eavp_project_options>)
target_include_directories(
    eavp_base
    INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set_target_properties(eavp_base PROPERTIES EXPORT_NAME base)

install(TARGETS eavp_base EXPORT EAVPTargets)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/eavp/base
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/eavp
        OPTIONAL)

add_library(eavp_media STATIC
    media/audio_format.cpp
    media/audio_frame.cpp
    media/backend_node.cpp
    media/backend_registry.cpp
    media/buffer.cpp
    media/capability.cpp
    media/graph.cpp
    media/media_packet.cpp
    media/node.cpp
    media/pipeline.cpp
    media/reference_backend.cpp
    media/video_codec.cpp
    media/video_format.cpp
)
add_library(EAVP::media ALIAS eavp_media)

target_link_libraries(
    eavp_media
    PUBLIC EAVP::base
    PRIVATE $<BUILD_INTERFACE:eavp_project_options> Threads::Threads
)
target_include_directories(
    eavp_media
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
set_target_properties(eavp_media PROPERTIES EXPORT_NAME media)

install(TARGETS eavp_media EXPORT EAVPTargets)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/eavp/media
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/eavp
        OPTIONAL)

add_library(eavp_control STATIC
    control/command.cpp
    control/pipeline_reconciler.cpp
    control/state_store.cpp
)
add_library(EAVP::control ALIAS eavp_control)
target_link_libraries(
    eavp_control
    PUBLIC EAVP::media Threads::Threads
    PRIVATE $<BUILD_INTERFACE:eavp_project_options>
)
target_include_directories(
    eavp_control
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
set_target_properties(eavp_control PROPERTIES EXPORT_NAME control)
install(TARGETS eavp_control EXPORT EAVPTargets)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/eavp/control
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/eavp
        OPTIONAL)

add_library(eavp_management STATIC
    management/health.cpp
    management/metrics.cpp
)
add_library(EAVP::management ALIAS eavp_management)
target_link_libraries(
    eavp_management
    PUBLIC EAVP::base Threads::Threads
    PRIVATE $<BUILD_INTERFACE:eavp_project_options>
)
target_include_directories(
    eavp_management
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
set_target_properties(eavp_management PROPERTIES EXPORT_NAME management)
install(TARGETS eavp_management EXPORT EAVPTargets)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/eavp/management
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/eavp
        OPTIONAL)

add_library(eavp_platform STATIC
    platform/reference_media_platform.cpp
    platform/simulated_platform.cpp
)
add_library(EAVP::platform ALIAS eavp_platform)
target_link_libraries(
    eavp_platform
    PUBLIC EAVP::control EAVP::management
    PRIVATE $<BUILD_INTERFACE:eavp_project_options>
)
target_include_directories(
    eavp_platform
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
set_target_properties(eavp_platform PROPERTIES EXPORT_NAME platform)
install(TARGETS eavp_platform EXPORT EAVPTargets)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/eavp/platform
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/eavp
        OPTIONAL)
