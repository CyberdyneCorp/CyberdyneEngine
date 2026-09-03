# src/core/diagnostics/ — layer 0: the trace, structured logging, breadcrumbs, and the crash
# artefact. Governed by `diagnostics-profiling-and-crash`; the invariant it carries into every later
# milestone is that a diagnostic field cannot be declared without a privacy classification.

# std::thread for the drain consumer. An imported target with no layer, so it passes the layer check
# unremarked, and it keeps PRIVATE_DEPENDENCIES non-empty when CY_PROFILING is off.
find_package(Threads REQUIRED)

# Platform-specific translation units, selected by the build rather than by an #ifdef in a shared
# file — the rule src/core/README.md states for this layer. The Windows unit is unverified; see the
# comment at the top of it.
if(WIN32)
    set(cy_diagnostics_platform_sources src/crash_handler_windows.cpp)
else()
    set(cy_diagnostics_platform_sources src/crash_handler_posix.cpp)
endif()

set(cy_diagnostics_dependencies Threads::Threads)
if(CY_PROFILING)
    list(APPEND cy_diagnostics_dependencies cy::dep::tracy)
endif()

cy_add_module(
    NAME cy_core_diagnostics
    LAYER core
    ALIAS cy::core-diagnostics
    SOURCES
        src/breadcrumb.cpp
        src/bridge.cpp
        src/crash_report.cpp
        src/log.cpp
        src/names.cpp
        src/registry.cpp
        src/trace.cpp
        src/tracy_sink.cpp
        src/writer.cpp
        ${cy_diagnostics_platform_sources}
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE_DEFINITIONS
        CY_DIAG_ENGINE_VERSION="${PROJECT_VERSION}"
        CY_DIAG_BUILD_CONFIGURATION="$<CONFIG>"
    PUBLIC_DEPENDENCIES cy::core-base
    PRIVATE_DEPENDENCIES ${cy_diagnostics_dependencies})


if(CY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
