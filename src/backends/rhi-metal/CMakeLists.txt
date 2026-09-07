# src/backends/rhi-metal/ — layer 3: the Metal seed. M7 task 10.5; delivered at M11.
#
# `delivery-roadmap` seeds Metal here and the task says why: "to expose Vulkan-specific assumptions
# in `rhi-and-render-graph` WHILE THEY ARE STILL CHEAP. That finding is the point of the seed, not
# the backend."
#
# SO THE MODULE IS SHAPED AROUND THE FINDING RATHER THAN AROUND THE DEVICE, and that is the one
# design decision in this file. `mapping.h`, `mapping.cpp` and `backend.cpp` name no Metal type and
# COMPILE AND RUN EVERYWHERE — so the eight gaps the seed found are checked by `just test-all` on
# the Linux machine this milestone was built on, rather than sitting in a document. `src/device.mm`
# is the Apple-only half and it has never been compiled; its most valuable content is a block of
# static assertions that check `mapping.cpp`'s transcribed MTLPixelFormat values against Metal's own
# header the first time anybody builds it on a Mac.
#
# The target is built unconditionally because the half that carries the findings has no dependency
# on Metal. CY_RENDERER_METAL adds the Apple half, and it defaults ON on Apple and OFF elsewhere —
# see cmake/features.cmake, where that platform-conditional default was added for this module.
set(cy_rhi_metal_sources src/backend.cpp src/mapping.cpp)
set(cy_rhi_metal_definitions "")

if(CY_RENDERER_METAL)
    if(APPLE)
        list(APPEND cy_rhi_metal_sources src/device.mm)
        list(APPEND cy_rhi_metal_definitions CY_METAL_DEVICE_PRESENT)
    else()
        # Named rather than silent. `-DCY_RENDERER_METAL=ON` on Linux is a reasonable thing for
        # somebody to try, and the honest answer is that the findings are built either way and the
        # device is not.
        message(STATUS
            "CY_RENDERER_METAL is ON but this is not an Apple platform: building the Metal seed's "
            "mapping tables and its registration seam, and not src/device.mm.")
    endif()
endif()

cy_add_module(
    NAME cy_rhi_metal
    LAYER backends
    ALIAS cy::rhi-metal
    TYPE STATIC
    SOURCES ${cy_rhi_metal_sources}
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::rhi)

if(cy_rhi_metal_definitions)
    target_compile_definitions(cy_rhi_metal PRIVATE ${cy_rhi_metal_definitions})
endif()

if(CY_RENDERER_METAL AND APPLE)
    # The three frameworks the seed names. Foundation for NSString and NSError, Metal for the
    # device, QuartzCore for CAMetalLayer.
    target_link_libraries(cy_rhi_metal PRIVATE
        "-framework Foundation" "-framework Metal" "-framework QuartzCore")
    set_source_files_properties(src/device.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
