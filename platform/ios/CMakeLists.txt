if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(FATAL_ERROR "platform/ios is available only when targeting iOS")
endif()

cy_add_module(
    NAME cy_platform_ios
    LAYER platform
    ALIAS cy::platform-ios
    SOURCES
        src/ios_platform.mm
        src/ios_display_server.mm
        src/ios_touch_input_source.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-platform cy::servers-input
    PRIVATE_DEPENDENCIES "-framework Foundation" "-framework UIKit" "-framework QuartzCore")

set_source_files_properties(src/ios_platform.mm src/ios_display_server.mm
                            PROPERTIES COMPILE_FLAGS "-fobjc-arc")
