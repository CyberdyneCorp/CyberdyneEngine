# cmake/dependencies.cmake — FetchContent driven by deps/manifest.toml.
#
# Tasks 1.6.2 and 1.6.3. The manifest is the source of truth: no version, commit or URL appears in
# this file. What appears here is the build's *behaviour* — how each dependency is configured, and
# the engine-facing target it is reached through.
#
# The top-level CMakeLists.txt includes this file after cmake/features.cmake, because disabling a
# feature must exclude its dependency from the fetch as well as from the link.
#
# Three rules this file keeps:
#
#   * Third-party sources are not compiled under the engine's warning policy. They do not link
#     cy::compile-options, and their headers are added as SYSTEM includes so a warning in somebody
#     else's header is not the engine's -Werror failure.
#   * A disabled feature's dependency is not declared, so FetchContent never sees it and no source
#     is downloaded — `thirdparty-dependencies` requires exclusion, not runtime stubbing.
#   * Engine code links `cy::dep::<name>`, never the upstream target. That is where a system copy
#     and a fetched copy become the same thing, and it is the seam a replacement is made at.
#
# NOTE for task 1.3.2 (the bare-add_library lint): the `cy::dep::<name>` shims below are plain
# INTERFACE libraries by necessity — they wrap targets this project does not own, and an ALIAS
# cannot point at another ALIAS. The lint's scope is the engine tree (src/, platform/, modules/,
# tools/, tests/, benchmarks/, samples/); cmake/ is where the exceptions are declared, so it is not
# in scope.

include_guard(GLOBAL)
include(FetchContent)

set(CY_DEPS_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../deps/manifest.toml"
    CACHE FILEPATH "The dependency manifest that drives the fetch")

# A shared download cache, so that a second build tree reuses the first one's sources and an
# already-populated cache needs no network. CI keys its cache on the manifest.
set(CY_DEPS_CACHE "" CACHE PATH "Shared FetchContent cache directory (empty: use the build tree)")
if(CY_DEPS_CACHE)
    set(FETCHCONTENT_BASE_DIR "${CY_DEPS_CACHE}")
endif()

# Every dependency is pinned to a commit, so there is nothing to update and nothing to ask upstream
# about once the source is present. This is what makes a populated cache work offline; setting
# FETCHCONTENT_FULLY_DISCONNECTED=ON additionally turns a would-be download into a configure error,
# which is how the offline claim is tested rather than assumed.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# The gating options. cmake/features.cmake (task 1.4.1) owns the canonical declaration of the whole
# CY_* set; these fallbacks exist so that this file is usable on its own, which is what the gating
# test in tools/deps/ configures. option() on an already-declared option is a no-op, but the guard
# makes the intent explicit.
if(NOT DEFINED CY_BUILD_TESTS)
    option(CY_BUILD_TESTS "Build the test suites" ON)
endif()
if(NOT DEFINED CY_PROFILING)
    option(CY_PROFILING "Enable profiling instrumentation and the Tracy backend" OFF)
endif()

# --- The manifest ---------------------------------------------------------------------------------

# Fields every entry must declare, beyond `name`. A missing one is a configure error rather than an
# empty string reaching FetchContent.
set(CY_DEP_REQUIRED_FIELDS
    version tag commit repository licence licence_file optional feature
    system_package cmake_target source_subdir interface scope justification)

# Convert one manifest scalar — a quoted string, `true`, or `false` — into a CMake value.
function(cy__manifest_scalar raw out)
    if(raw MATCHES "^\"(.*)\"$")
        set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    elseif(raw STREQUAL "true")
        set(${out} TRUE PARENT_SCOPE)
    elseif(raw STREQUAL "false")
        set(${out} FALSE PARENT_SCOPE)
    else()
        message(FATAL_ERROR
            "${CY_DEPS_MANIFEST}: value is neither a quoted string nor true/false: ${raw}")
    endif()
endfunction()

# Read the manifest into `CY_DEPENDENCIES` (names, in manifest order) and one variable per field,
# `CY_DEP_<name>_<field>`.
function(cy_read_dependency_manifest path)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "dependency manifest not found: ${path}")
    endif()
    # Read and split by hand rather than with file(STRINGS): that command extracts printable-ASCII
    # runs, so a non-ASCII character anywhere in the file — an em dash in a justification — silently
    # cuts the line in two. Semicolons are escaped because CMake's list separator is a semicolon.
    file(READ "${path}" content)
    string(REPLACE ";" "\\;" content "${content}")
    string(REGEX REPLACE "\r?\n" ";" lines "${content}")

    set(ids "")
    set(exported "")
    set(id "")
    foreach(line IN LISTS lines)
        string(STRIP "${line}" line)
        if(line STREQUAL "" OR line MATCHES "^#")
            continue()
        endif()
        if(line STREQUAL "[[dependency]]")
            set(id "")
            continue()
        endif()
        if(NOT line MATCHES "^([A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*(.+)$")
            message(FATAL_ERROR "${path}: expected `key = value` or `[[dependency]]`, got: ${line}")
        endif()
        set(key "${CMAKE_MATCH_1}")
        cy__manifest_scalar("${CMAKE_MATCH_2}" value)
        if(id STREQUAL "")
            if(NOT key STREQUAL "name")
                message(FATAL_ERROR
                    "${path}: `${key}` appears before any `name`. Every [[dependency]] table must "
                    "begin with `name = \"...\"`, because the name is what every other field is "
                    "recorded against.")
            endif()
            set(id "${value}")
            list(APPEND ids "${id}")
        else()
            set(CY_DEP_${id}_${key} "${value}")
            list(APPEND exported CY_DEP_${id}_${key})
        endif()
    endforeach()

    foreach(name IN LISTS exported)
        set(${name} "${${name}}" PARENT_SCOPE)
    endforeach()
    set(CY_DEPENDENCIES "${ids}" PARENT_SCOPE)
endfunction()

# Every entry declares every field, and `optional` and `feature` agree with each other.
function(cy_validate_dependency_manifest)
    foreach(id IN LISTS CY_DEPENDENCIES)
        foreach(field IN LISTS CY_DEP_REQUIRED_FIELDS)
            if(NOT DEFINED CY_DEP_${id}_${field})
                message(FATAL_ERROR
                    "${CY_DEPS_MANIFEST}: dependency `${id}` does not declare `${field}`.")
            endif()
        endforeach()
        string(LENGTH "${CY_DEP_${id}_commit}" commit_length)
        if(NOT commit_length EQUAL 40 OR NOT CY_DEP_${id}_commit MATCHES "^[0-9a-f]+$")
            message(FATAL_ERROR
                "${CY_DEPS_MANIFEST}: `${id}` pins `${CY_DEP_${id}_commit}`, which is not a full "
                "lowercase 40-character SHA. A tag or an abbreviation is not a pin: a tag can be "
                "moved and an abbreviation can become ambiguous.")
        endif()
        if(CY_DEP_${id}_system_package)
            foreach(field IN ITEMS system_find_package system_target)
                if(NOT DEFINED CY_DEP_${id}_${field})
                    message(FATAL_ERROR
                        "${CY_DEPS_MANIFEST}: `${id}` says a system copy is acceptable but does not "
                        "declare `${field}`. Finding one needs both its package name and the target "
                        "it exports, and neither is reliably the name the fetched build uses.")
                endif()
            endforeach()
        endif()
        if(CY_DEP_${id}_optional AND CY_DEP_${id}_feature STREQUAL "")
            message(FATAL_ERROR
                "${CY_DEPS_MANIFEST}: `${id}` is optional but names no gating feature. An optional "
                "dependency that nothing gates cannot be excluded.")
        endif()
        if(NOT CY_DEP_${id}_optional AND NOT CY_DEP_${id}_feature STREQUAL "")
            message(FATAL_ERROR
                "${CY_DEPS_MANIFEST}: `${id}` is not optional but names the gating feature "
                "`${CY_DEP_${id}_feature}`. A gate on a dependency that is always built is a gate "
                "that does nothing.")
        endif()
    endforeach()
endfunction()

# TRUE when the dependency's gating feature is on, or when it is not gated at all.
function(cy_dependency_enabled id out)
    if(NOT DEFINED CY_DEP_${id}_optional)
        message(FATAL_ERROR "no such dependency in ${CY_DEPS_MANIFEST}: ${id}")
    endif()
    if(NOT CY_DEP_${id}_optional)
        set(${out} TRUE PARENT_SCOPE)
        return()
    endif()
    if(${CY_DEP_${id}_feature})
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

# --- Per-dependency build configuration -------------------------------------------------------------
#
# The one thing the manifest cannot hold, because it is specific to somebody else's CMake. Each
# block turns off what the engine does not use: build time and binary size are part of the
# dependency policy's "bounded cost", and an option left at its default is a cost nobody chose.

function(cy__configure_sdl3)
    # Video, events and gamepads, and nothing else. Events are not optional in SDL and are always
    # built. HIDAPI stays on: the controller database and the wireless-controller coverage it brings
    # are the reason SDL3 is here at all.
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)     # one artefact to ship, and the Zlib licence permits it
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_JOYSTICK ON CACHE BOOL "" FORCE)
    set(SDL_HIDAPI ON CACHE BOOL "" FORCE)
    set(SDL_AUDIO OFF CACHE BOOL "" FORCE)      # the engine owns AudioServer, over miniaudio
    set(SDL_RENDER OFF CACHE BOOL "" FORCE)     # the engine owns the renderer
    set(SDL_GPU OFF CACHE BOOL "" FORCE)        # the engine owns the RHI
    set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
    set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)     # rumble arrives with the input action model
    set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
    set(SDL_POWER OFF CACHE BOOL "" FORCE)
    set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
    set(SDL_OPENGL OFF CACHE BOOL "" FORCE)     # the RHI targets Vulkan and Metal
    set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    # X11 extensions the engine does not call. Each one left on is a -dev package every Linux
    # contributor must have installed for a feature no code uses: XScrnSaver inhibits the
    # screensaver, XTest fakes pointer motion for warping an unfocused window. What stays on is what
    # DisplayServer needs — Xcursor, Xrandr for screen enumeration, Xfixes and XInput2 for input.
    # When an idle-inhibition policy lands, XScrnSaver comes back on and joins the documented Linux
    # build dependencies.
    set(SDL_X11_XSCRNSAVER OFF CACHE BOOL "" FORCE)
    set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
endfunction()

function(cy__configure_doctest)
    set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
endfunction()

function(cy__configure_tracy)
    set(TRACY_STATIC ON CACHE BOOL "" FORCE)
    # Set explicitly rather than relied on: CY_PROFILING is the engine's statement that
    # instrumentation is wanted, and it should not become a no-op if upstream flips a default.
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    # On-demand: the client collects nothing until a profiler connects, so a CY_PROFILING build is
    # something a developer can run all day rather than a separate build they have to remember.
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
endfunction()

function(cy__configure_zstd)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)  # the engine writes only current-format frames
endfunction()

function(cy__configure_vulkan_headers)
    # Headers only. Nothing is built and nothing is linked: the loader is resolved at run time by
    # volk, so a build with the Vulkan backend on still links no Vulkan library and still runs on a
    # machine with no driver — which is what lets `just test-all` cover this backend's compile.
    set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
    set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
endfunction()

function(cy__configure_volk)
    # volk fetches every entry point itself and dlopen()s the loader, so it must not link one.
    set(VOLK_STATIC_DEFINES "" CACHE STRING "" FORCE)
    set(VOLK_PULL_IN_VULKAN ON CACHE BOOL "" FORCE)
    set(VOLK_INSTALL OFF CACHE BOOL "" FORCE)
    # Surfaces come from DisplayServer (platform/), never from a window-system call in the backend,
    # so none of the platform-specific surface extensions is compiled in here.
    #
    # AND THE HEADERS ARE THE PINNED ONES. Left to itself, volk's CMake calls find_package(Vulkan)
    # and compiles volk.c against whatever headers the machine happens to have installed — which
    # makes the pin in deps/manifest.toml govern the engine's sources and not volk's, and produces a
    # build where two halves of one library were compiled against two API versions. The path is the
    # one FetchContent will populate `vulkan_headers` into; it is deterministic, and the manifest
    # lists vulkan_headers before volk so it exists by the time volk builds.
    if(FETCHCONTENT_BASE_DIR)
        set(cy_fetch_base "${FETCHCONTENT_BASE_DIR}")
    else()
        set(cy_fetch_base "${CMAKE_BINARY_DIR}/_deps")
    endif()
    set(VULKAN_HEADERS_INSTALL_DIR "${cy_fetch_base}/vulkan_headers-src" CACHE PATH "" FORCE)
endfunction()

function(cy__configure_slang)
    # WHAT IS TURNED OFF, AND WHY EACH ONE. The engine wants one thing from this dependency: a
    # library that turns Slang source into SPIR-V. Everything else in the upstream tree is a tool,
    # a sample, or a second graphics abstraction the engine already has.
    set(SLANG_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_REPLAYER OFF CACHE BOOL "" FORCE)
    # slang-rhi and gfx are Slang's own rendering abstractions. The engine has one (cy::rhi), and
    # building a second would double the Vulkan surface in the tree that the layer checker guards.
    set(SLANG_ENABLE_SLANG_RHI OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_GFX OFF CACHE BOOL "" FORCE)
    # The language server and the interpreter are developer tools, not compilation.
    set(SLANG_ENABLE_SLANGD OFF CACHE BOOL "" FORCE)
    set(SLANG_ENABLE_SLANGI OFF CACHE BOOL "" FORCE)
    # DXIL needs DXC, which is a second downstream toolchain for a backend the roadmap does not
    # reach until M11. SPIR-V is the interchange form (`shader-system`), and D3D12 turns this on.
    set(SLANG_ENABLE_DXIL OFF CACHE BOOL "" FORCE)
    # slang-llvm is a host-execution back end for running Slang on the CPU. Nothing here does.
    set(SLANG_SLANG_LLVM_FLAVOR DISABLE CACHE STRING "" FORCE)
    # BUT slang-glslang STAYS ON, and this is the one that cost a build to find out: Slang's SPIR-V
    # emission calls out to spirv-opt, which ships inside the slang-glslang shared module. With it
    # off, every compilation — including at -O0 — fails with "failed to load downstream compiler
    # 'spirv-opt'". It is not optional for a SPIR-V target.
    set(SLANG_ENABLE_SLANG_GLSLANG ON CACHE BOOL "" FORCE)
    # slangc is how a cook step and a test fixture compile a shader without linking the engine.
    set(SLANG_ENABLE_SLANGC ON CACHE BOOL "" FORCE)
endfunction()

# Slang's own sources use C++ exceptions and `dynamic_cast`. The engine compiles with
# -fno-exceptions and -fno-rtti as a *directory* property (see the top-level CMakeLists.txt), which
# every target created under it inherits — including the ones FetchContent creates for a dependency.
# The top-level file already anticipates this: "a third-party subdirectory that genuinely requires
# exceptions clears them for its own scope in cmake/dependencies.cmake". This is that scope.
#
# WHY IT APPENDS RATHER THAN REMOVES. A directory's COMPILE_OPTIONS are copied into a target when
# the target is created, so editing the directory property afterwards changes nothing. Appending to
# each target works because a target's own options come after the directory's on the command line
# and the compiler takes the last of a contradictory pair — which is also why this cannot silently
# half-apply: either the flag is on the line after -fno-exceptions or the build fails as before.
function(cy__slang_allow_exceptions directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(type STREQUAL "INTERFACE_LIBRARY" OR type STREQUAL "UTILITY")
            continue()
        endif()
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-fexceptions> $<$<COMPILE_LANGUAGE:CXX>:-frtti>)
    endforeach()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        cy__slang_allow_exceptions("${child}")
    endforeach()
endfunction()

function(cy__finalise_slang target)
    if(NOT MSVC AND slang_SOURCE_DIR)
        cy__slang_allow_exceptions("${slang_SOURCE_DIR}")
    endif()
endfunction()

function(cy__configure_vma)
    set(VMA_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    set(VMA_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
    set(VMA_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
endfunction()

function(cy__configure_blake3)
    set(BLAKE3_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BLAKE3_TESTING OFF CACHE BOOL "" FORCE)
    set(BLAKE3_USE_TBB OFF CACHE BOOL "" FORCE)       # the engine's own job system parallelises this
endfunction()

# Everything that has to happen after the dependency's targets exist.
function(cy__finalise_doctest target)
    # doctest's REQUIRE family reports a failure by throwing. With -fno-exceptions in force it must
    # abort instead, which is what this configuration selects; without it, REQUIRE would silently
    # become CHECK and a failing precondition would carry on into the code it was guarding.
    target_compile_definitions(${target} INTERFACE
        DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
endfunction()

# --- Acquisition ------------------------------------------------------------------------------------

cy_read_dependency_manifest("${CY_DEPS_MANIFEST}")
cy_validate_dependency_manifest()

set(_cy_to_fetch "")
set(_cy_linked "")
foreach(_cy_id IN LISTS CY_DEPENDENCIES)
    cy_dependency_enabled("${_cy_id}" _cy_enabled)
    if(NOT _cy_enabled)
        message(STATUS
            "dependency ${_cy_id}: excluded — ${CY_DEP_${_cy_id}_feature} is off, so it is neither "
            "fetched, built nor linked")
        continue()
    endif()

    string(TOUPPER "${_cy_id}" _cy_upper)

    # A system copy is offered only where the manifest says one is acceptable.
    if(CY_DEP_${_cy_id}_system_package)
        option(CY_SYSTEM_${_cy_upper} "Use the system-provided ${_cy_id} instead of fetching it" OFF)
    endif()

    if(COMMAND cy__configure_${_cy_id})
        cmake_language(CALL cy__configure_${_cy_id})
    endif()

    if(CY_SYSTEM_${_cy_upper})
        find_package(${CY_DEP_${_cy_id}_system_find_package} CONFIG REQUIRED)
        set(CY_DEP_${_cy_id}_cmake_target "${CY_DEP_${_cy_id}_system_target}")
        message(STATUS "dependency ${_cy_id}: system copy, target ${CY_DEP_${_cy_id}_cmake_target}")
    else()
        FetchContent_Declare(${_cy_id}
            GIT_REPOSITORY "${CY_DEP_${_cy_id}_repository}"
            GIT_TAG "${CY_DEP_${_cy_id}_commit}"
            # A shallow fetch of an arbitrary commit needs a server that allows it. GitHub does,
            # and it is the difference between a few megabytes and several hundred for SDL and
            # zstd. A mirror that refuses will fail the fetch loudly; the fix is GIT_SHALLOW FALSE.
            GIT_SHALLOW TRUE
            GIT_PROGRESS TRUE
            SOURCE_SUBDIR "${CY_DEP_${_cy_id}_source_subdir}"
            EXCLUDE_FROM_ALL
            SYSTEM)
        list(APPEND _cy_to_fetch ${_cy_id})
        message(STATUS
            "dependency ${_cy_id}: ${CY_DEP_${_cy_id}_version} (${CY_DEP_${_cy_id}_tag}) "
            "${CY_DEP_${_cy_id}_licence}")
    endif()
    list(APPEND _cy_linked ${_cy_id})
endforeach()

if(_cy_to_fetch)
    FetchContent_MakeAvailable(${_cy_to_fetch})
endif()

# FETCHCONTENT_FULLY_DISCONNECTED does not fail when a source is missing — it simply does not
# populate, and the first symptom would otherwise be a missing target several lines below. Say what
# actually happened instead.
foreach(_cy_id IN LISTS _cy_to_fetch)
    if(NOT EXISTS "${${_cy_id}_SOURCE_DIR}")
        message(FATAL_ERROR
            "dependency ${_cy_id} is not present in ${FETCHCONTENT_BASE_DIR} and downloading is "
            "disabled (FETCHCONTENT_FULLY_DISCONNECTED). Populate the cache with one connected "
            "configure, or point CY_DEPS_CACHE at a cache that already has it.")
    endif()
endforeach()

# The engine-facing targets. Every one is checked, because a dependency whose upstream target was
# renamed otherwise surfaces as a link error in whichever module happened to use it first.
foreach(_cy_id IN LISTS _cy_linked)
    set(_cy_target "${CY_DEP_${_cy_id}_cmake_target}")
    if(NOT TARGET ${_cy_target})
        message(FATAL_ERROR
            "dependency ${_cy_id} was made available but provides no target `${_cy_target}`. "
            "Upstream renamed it: correct `cmake_target` in ${CY_DEPS_MANIFEST}.")
    endif()
    add_library(cy_dep_${_cy_id} INTERFACE)
    target_link_libraries(cy_dep_${_cy_id} INTERFACE ${_cy_target})
    add_library(cy::dep::${_cy_id} ALIAS cy_dep_${_cy_id})
    if(COMMAND cy__finalise_${_cy_id})
        cmake_language(CALL cy__finalise_${_cy_id} cy_dep_${_cy_id})
    endif()
endforeach()

# Recorded for the licence report: exactly what this configuration links, which is what
# `thirdparty-dependencies` requires a generated attribution to cover.
set(CY_ENABLED_DEPENDENCIES "${_cy_linked}" CACHE INTERNAL "Dependencies this configuration links")
message(STATUS "dependencies linked: ${CY_ENABLED_DEPENDENCIES}")
