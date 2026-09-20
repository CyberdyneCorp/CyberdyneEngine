set(CY_IOS_BUNDLE_IDENTIFIER "com.cyberdyne.engine.ship" CACHE STRING
    "Bundle identifier for the iOS device sample")
set(CY_IOS_DEVELOPMENT_TEAM "" CACHE STRING
    "Apple development team used to sign the iOS device sample")

cy_add_module(
    NAME cy_sample_ship_ios
    LAYER tools
    TYPE EXECUTABLE
    SOURCES main.mm
    PRIVATE_DEPENDENCIES
        cy::platform-ios cy::rhi cy::rhi-metal cy::core-memory
        "-framework UIKit" "-framework QuartzCore" "-framework Metal")
set_source_files_properties(main.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")

set_target_properties(cy_sample_ship_ios PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${CY_IOS_BUNDLE_IDENTIFIER}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME ""
    XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@executable_path/Frameworks")
if(CY_IOS_DEVELOPMENT_TEAM)
    set_target_properties(cy_sample_ship_ios PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${CY_IOS_DEVELOPMENT_TEAM}")
else()
    # Cross-compilation and simulator CI do not need an identity. A developer supplies a team only
    # for the installable device build, keeping credentials out of the source tree and presets.
    set_target_properties(cy_sample_ship_ios PROPERTIES
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO")
endif()
