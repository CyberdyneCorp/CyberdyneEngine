# Native Direct3D 12 backend. M11.d.5.

if(NOT WIN32)
    message(FATAL_ERROR
        "CY_RENDERER_D3D12 is ON but the target platform is not Windows. Direct3D 12 requires "
        "the Windows SDK; disable CY_RENDERER_D3D12 on this target.")
endif()

cy_add_module(
    NAME cy_rhi_d3d12
    LAYER backends
    ALIAS cy::rhi-d3d12
    TYPE STATIC
    SOURCES
        src/backend.cpp
        src/command_buffer.cpp
        src/device.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PUBLIC_DEPENDENCIES cy::rhi
    PRIVATE_DEPENDENCIES cy::core-memory cy::dep::pix_events)

target_link_libraries(cy_rhi_d3d12 PRIVATE d3d12 dxgi dxguid)
target_compile_definitions(cy_rhi_d3d12 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX USE_PIX_RETAIL)

if(CY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
