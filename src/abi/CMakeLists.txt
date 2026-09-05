# src/abi/ — layer 6: the stable flat C ABI. Section 2 of the M4 tasks.
#
# `native-abi`. This is the boundary at which C++ types stop: everything below is C++20 with
# -fno-exceptions and -fno-rtti, and everything above — the Swift overlay, a native game module, an
# editor plugin, the Rust SDK at M5 — sees only `cy/abi/cy_abi.h`.
#
# WHY IT IS BUILT UNCONDITIONALLY, AND NOT BEHIND CY_SCRIPTING. M3's gate found the shape of the
# mistake to avoid: `CY_RENDERER_VULKAN` defaulted off, so the *real* backend was the one nothing
# tested and the sample rendered black while exiting zero. An ABI has the same property and worse
# consequences — the compatibility obligation starts with the first exported symbol, and a table
# that is compiled only in some configurations is a table whose gate runs only in some
# configurations. So there is no CY_ABI option: this module is in every build, in every profile, and
# `just quality-abi` therefore has something to check on every machine.
#
# WHAT BELONGS HERE. The C header itself; the exported table and `cy_get_interface`; the
# `cy::Expected` to `CyResult` mapping and the thread-local last error; `CyVar` marshalling; the
# objects behind the opaque handles; the module manifest, the image loader, and the reload sequence.
#
# WHAT DOES NOT. Anything that assumes the caller is C++, any function that can throw, and any
# subsystem the table merely *exposes* — the entries below the marker in `CyInterface` are added by
# the module that owns the subsystem, appended and never inserted.
#
# LAYER 6 IS ABOVE EVERYTHING BUT tools/. That is deliberate and it is why the dependency list can be
# short: the ABI is the last thing in the stack, so it may name the ECS and the core, and nothing in
# the engine may name it back. A module that wants to be reachable from Swift does not link this one;
# it appends an entry to the table, which is the inversion `engine-architecture` asks for.
cy_add_module(
    NAME cy_abi
    LAYER abi
    ALIAS cy::abi
    TYPE STATIC
    SOURCES
        src/errors.cpp
        src/host.cpp
        src/interface.cpp
        src/manifest.cpp
        src/module.cpp
        src/var.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::ecs
    # The dynamic loader. `${CMAKE_DL_LIBS}` is empty on the platforms whose libc already has
    # dlopen, and is `dl` on the ones that keep it separate — which is why it is spelled with the
    # variable rather than with the name.
    PRIVATE_DEPENDENCIES ${CMAKE_DL_LIBS})

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
