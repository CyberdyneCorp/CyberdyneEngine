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

function(cy__configure_jolt)
    # WHAT IS TURNED OFF, AND WHY EACH ONE. The engine wants one thing from this dependency: the
    # solver, as a static library. Everything else upstream is a sample, a viewer or a test runner,
    # and several of them pull in a window and a rendering path the engine already owns.
    set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
    set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
    set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
    set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
    # Jolt compiles without exceptions and without RTTI by default — it has its own reflection — so
    # unlike Slang it needs no scope that clears the engine's -fno-exceptions/-fno-rtti. Stated
    # rather than left to be rediscovered: the absence of a cy__jolt_allow_exceptions() below is a
    # fact about Jolt, not an omission.
    set(CPP_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
    set(CPP_RTTI_ENABLED OFF CACHE BOOL "" FORCE)
    # DETERMINISM IS THE REASON THIS OPTION EXISTS AND THE REASON IT IS ON. `physics` requires
    # `SamePlatformDeterministic` as the default policy. Jolt's cross-platform determinism switch
    # also fixes the floating-point mode within one platform — no fused multiply-add reassociation,
    # no fast-math — which is what makes two runs of the same binary agree. The engine claims only
    # the same-platform half (determinism.h says why), and this is what buys even that.
    set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
    # Object streams are Jolt's own serialisation format. The engine serialises through
    # `serialization-and-prefabs` and would otherwise carry a second one into every build.
    set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
    # Jolt's profiler and debug renderer are a second timeline and a second draw path beside the
    # engine's trace and `cy::physics::DebugDrawSink`. Physics statistics reach the engine through
    # `StepStatistics`, and shapes reach it through the sink.
    set(PROFILER_ENABLED OFF CACHE BOOL "" FORCE)
    set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
    set(DEBUG_RENDERER_IN_DISTRIBUTION OFF CACHE BOOL "" FORCE)
    # JOLT'S GPU COMPUTE BACKENDS, ALL FOUR OFF. They exist for its GPU-accelerated soft-body hair
    # solver, which this engine does not use and could not use where it sits: JPH_USE_VK would put a
    # second Vulkan surface inside a physics dependency, above nothing and beside `cy::rhi`, which
    # is exactly what the layer checker's `gpuapi` rule exists to prevent.
    #
    # It is also not merely unwanted — with JPH_USE_VK on, Jolt's build tries to compile HLSL
    # compute shaders with `Vulkan_GLSLC_EXECUTABLE`, and on a machine with no Vulkan SDK on PATH
    # that is `-NOTFOUND` and the BUILD FAILS. Measured here before this line existed.
    set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
    set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
    set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
    set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
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

function(cy__configure_recast)
    # ONLY THE GENERATOR IS BUILT. `source_subdir` in the manifest points FetchContent at upstream's
    # `Recast/` subproject, so `Detour/`, `DetourCrowd/`, `DetourTileCache/`, `DebugUtils/`, the
    # SDL-based demo and the test suite are never added — which is what makes deps/manifest.toml's
    # "Detour is excluded by source_subdir" a build fact rather than a promise. It is also why the
    # root project's own options (RECASTNAVIGATION_DEMO, _TESTS, _EXAMPLES) are not set here: the
    # file that reads them is not part of this build.
    #
    # WHAT THIS FUNCTION IS FOR. `Recast/CMakeLists.txt` reads five variables the root project sets
    # before it descends — two version numbers and three GNUInstallDirs paths — and with the root
    # skipped they would be empty, which turns its `install(TARGETS ...)` into a configure error
    # naming an empty ARCHIVE DESTINATION. Supplying them is not editing upstream's build; it is
    # giving that build the context it was written to run in. The install rules themselves are
    # inert: the engine never installs a dependency's targets.
    set(SOVERSION 1 CACHE INTERNAL "")
    set(LIB_VERSION "1.6.0" CACHE INTERNAL "")
    set(CMAKE_INSTALL_BINDIR "bin" CACHE INTERNAL "")
    set(CMAKE_INSTALL_LIBDIR "lib" CACHE INTERNAL "")
    set(CMAKE_INSTALL_INCLUDEDIR "include" CACHE INTERNAL "")
endfunction()

# --- Dependencies that ship no CMake project ------------------------------------------------------
#
# `asset-import-pipeline`'s two source-format libraries — ufbx and xatlas — are each one translation
# unit and a header in a tree with no CMakeLists.txt. FetchContent populates them happily; what it
# cannot do is produce a target, and the acquisition loop below fails naming the missing one, which
# is the right failure for a dependency whose upstream target was renamed and the wrong one here.
#
# `cy__provide_<name>(source_dir)` is the hook that closes it. It runs after population and BEFORE
# the target check, so a single-file dependency is declared exactly where every other one is
# configured, with the same manifest entry, the same pin, the same attribution row, and the same
# `cy::dep::<name>` seam above it. What it must NOT become is a place to build a dependency that
# does ship a CMake project: four lines here in place of somebody's tested build is how a fetch
# turns into a fork.
#
# Both targets are declared with `SYSTEM` include directories and are not linked to
# cy::compile-options, which is this file's first rule: third-party sources are not compiled under
# the engine's warning policy.

function(cy__provide_ufbx source_dir)
    add_library(ufbx STATIC "${source_dir}/ufbx.c")
    target_include_directories(ufbx SYSTEM PUBLIC "${source_dir}")
    # ufbx reads a file it is handed and never opens one itself here — the importer is given bytes
    # (tools/import/include/cy/import/importer.h) — so its stdio-backed convenience layer is dead
    # weight in this build. It stays compiled rather than being switched off: UFBX_NO_STDIO is a
    # configuration upstream tests less than the default, and the cost is a few kilobytes.
    #
    # The threaded loader is left off. Import parallelism is the job system's, one asset per job
    # (`asset-import-pipeline`: "Import SHALL run in parallel on the job system"), and a library
    # spawning its own threads underneath that would contend with it for the same cores.
    set_target_properties(ufbx PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
endfunction()

function(cy__provide_xatlas source_dir)
    add_library(xatlas STATIC "${source_dir}/source/xatlas/xatlas.cpp")
    target_include_directories(xatlas SYSTEM PUBLIC "${source_dir}/source/xatlas")
    set_target_properties(xatlas PROPERTIES CXX_STANDARD 17 POSITION_INDEPENDENT_CODE ON)
endfunction()

# Everything that has to happen after the dependency's targets exist.
# --- ONNX Runtime (M8.c) --------------------------------------------------------------------------
#
# The engine wants one thing from this dependency: a shared library that loads an ONNX model and
# runs it on the CPU. Everything else upstream builds by default is a test binary, a language
# binding, or an execution provider for hardware this build does not target.
#
# TWO THINGS HERE ARE NOT OBVIOUS AND BOTH WERE MEASURED RATHER THAN GUESSED.
#
# 1. FETCHCONTENT_TRY_FIND_PACKAGE_MODE. ONNX Runtime's own dependency helper calls find_package()
#    before fetching, so a system copy of re2 or abseil is preferred when one is installed. On a
#    machine with Anaconda on PATH that finds `${HOME}/anaconda3/lib/cmake/absl`, whose exported
#    target set does not match what re2's config package expects, and the configure fails inside
#    somebody else's find_dependency() with "Targets not yet defined: absl::tracing_internal". The
#    fix is CMake's own control for exactly this, set for the duration of the fetch: NEVER means a
#    declared dependency is fetched rather than substituted. It changes nothing for the other
#    entries in this file, none of which relies on that substitution — CY_SYSTEM_<NAME> uses an
#    explicit find_package(CONFIG REQUIRED) and is unaffected.
#
# 2. EXCEPTIONS AND RTTI. ONNX Runtime uses both, the engine compiles with -fno-exceptions and
#    -fno-rtti as a directory property, and the top-level CMakeLists.txt says a third-party
#    subdirectory that needs them "clears them for its own scope in cmake/dependencies.cmake". That
#    is cy__slang_allow_exceptions() above, which is not Slang-specific despite its name: it walks a
#    directory's targets and their subdirectories and appends -fexceptions -frtti to each.
# Eigen is fetched but not added as a subproject — see deps/manifest.toml, which explains both the
# licence position and why the engine acquires it at all. All this has to do is publish a target
# with its include directory on it, so the `cy::dep::eigen` shim and the manifest's target check
# have something to point at.
function(cy__provide_eigen source_dir)
    if(NOT TARGET cy_eigen_headers)
        add_library(cy_eigen_headers INTERFACE)
        target_include_directories(cy_eigen_headers SYSTEM INTERFACE "${source_dir}")
    endif()
endfunction()

function(cy__configure_onnxruntime)
    set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER CACHE STRING "" FORCE)
    # AND THE OTHER HALF OF THE SAME PROBLEM, which cost a fifteen-minute build to find. CMake's
    # USER PACKAGE REGISTRY (~/.cmake/packages/<Name>/) is consulted by find_package() before any
    # prefix path, and a developer who has ever built Eigen through vcpkg has an entry in it. ONNX
    # Runtime's dependency helper calls find_package() directly — not through FetchContent — so
    # FETCHCONTENT_TRY_FIND_PACKAGE_MODE does not cover it, and the configure silently picked up
    # `~/vcpkg/buildtrees/eigen3` (Eigen 5.0.1) in place of the Eigen 3.4 upstream pins. The build
    # then failed inside somebody else's header on `std::hardware_destructive_interference_size`.
    #
    # A dependency that resolves differently depending on what else the developer has ever built is
    # not pinned, whatever the manifest says. Turning the registry off makes this configure read the
    # same on every machine. Nothing in this repository publishes to or reads from that registry —
    # CY_SYSTEM_<NAME> uses an explicit find_package(CONFIG REQUIRED) against a prefix path and is
    # unaffected.
    set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF CACHE BOOL "" FORCE)
    set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY OFF CACHE BOOL "" FORCE)
    set(onnxruntime_BUILD_SHARED_LIB ON CACHE BOOL "" FORCE)
    set(onnxruntime_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)   # ~2000 files of somebody else's tests
    set(onnxruntime_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(onnxruntime_USE_FULL_PROTOBUF OFF CACHE BOOL "" FORCE)  # protobuf-lite is enough to load a model
    set(onnxruntime_BUILD_WEBASSEMBLY OFF CACHE BOOL "" FORCE)
    set(onnxruntime_ENABLE_PYTHON OFF CACHE BOOL "" FORCE)
    set(onnxruntime_BUILD_JAVA OFF CACHE BOOL "" FORCE)
    set(onnxruntime_BUILD_NODEJS OFF CACHE BOOL "" FORCE)
    set(onnxruntime_BUILD_OBJC OFF CACHE BOOL "" FORCE)

    # EIGEN IS SUPPLIED RATHER THAN FETCHED BY UPSTREAM, and this is the third thing here that was
    # measured rather than guessed: upstream downloads Eigen as a zip from gitlab.com, and gitlab.com
    # answers that URL with HTTP 403 from this network. `git ls-remote` against the same repository
    # works, so deps/manifest.toml carries Eigen at the commit upstream's own deps.txt pins its
    # archive at, and it is populated HERE — before ONNX Runtime's CMake is read — so that
    # `eigen_SOURCE_PATH` can be handed to it.
    #
    # FetchContent_MakeAvailable is idempotent, so the acquisition loop below calling it again for
    # the same entry costs nothing.
    if(CY_ML_ONNXRUNTIME)
        FetchContent_MakeAvailable(eigen)
        set(onnxruntime_USE_PREINSTALLED_EIGEN ON CACHE BOOL "" FORCE)
        set(eigen_SOURCE_PATH "${eigen_SOURCE_DIR}" CACHE PATH "" FORCE)
        message(STATUS "dependency onnxruntime: using the manifest's Eigen at ${eigen_SOURCE_DIR}")
    endif()
endfunction()

# THE ENGINE HIDES EVERY SYMBOL BY DEFAULT, AND A SHARED LIBRARY WITH NO EXPORTS IS NOT A LIBRARY.
#
# The top-level CMakeLists.txt sets `CMAKE_CXX_VISIBILITY_PRESET hidden` and
# `CMAKE_VISIBILITY_INLINES_HIDDEN ON` as directory variables, which every target created below them
# inherits as a property — including a FetchContent dependency's. ONNX Runtime's C API is exported
# through a version script and ordinary default visibility, so under `-fvisibility=hidden` it built a
# 724 MB `libonnxruntime.so` that exports NOTHING: `nm -D --defined-only` finds no `OrtGetApiBase`,
# and the link of the first consumer fails with an undefined reference to it.
#
# That is the same class of problem as -fno-exceptions above and takes the same shape of fix: the
# engine's language and linkage contract is the ENGINE's, and a third-party subdirectory that cannot
# hold it is restored to the compiler's defaults for its own scope. Target properties are read at
# generate time, so setting them here — after the targets exist — is enough.
function(cy__default_visibility directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(type STREQUAL "INTERFACE_LIBRARY" OR type STREQUAL "UTILITY")
            continue()
        endif()
        set_target_properties(${target} PROPERTIES
            C_VISIBILITY_PRESET default
            CXX_VISIBILITY_PRESET default
            VISIBILITY_INLINES_HIDDEN OFF)
    endforeach()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        cy__default_visibility("${child}")
    endforeach()
endfunction()

function(cy__finalise_onnxruntime target)
    if(NOT MSVC AND onnxruntime_SOURCE_DIR)
        cy__slang_allow_exceptions("${onnxruntime_SOURCE_DIR}/cmake")
        cy__default_visibility("${onnxruntime_SOURCE_DIR}/cmake")
    endif()
    # Upstream's `onnxruntime` target publishes its public headers only under an INSTALL_INTERFACE,
    # so an in-tree consumer inherits no include directory at all. The wrapper adds the one
    # directory the C API lives in — SYSTEM, so a warning in somebody else's header is not this
    # project's -Werror failure.
    if(onnxruntime_SOURCE_DIR)
        target_include_directories(${target} SYSTEM INTERFACE
            "${onnxruntime_SOURCE_DIR}/include/onnxruntime/core/session")
    endif()
endfunction()

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
        # A shallow fetch of an arbitrary commit needs a server that allows it — git's
        # `uploadpack.allowReachableSHA1InWant`. GitHub does, and it is the difference between a few
        # megabytes and several hundred for SDL and zstd.
        #
        # GITLAB DOES NOT, and M8.c is where that stopped being a footnote. The Eigen entry is
        # pinned at a commit on gitlab.com and a shallow fetch of it fails with "Failed to checkout
        # tag", which reads like a bad pin rather than a server policy. The host decides, so the
        # host is what this reads, rather than a list of names somebody has to remember to extend.
        set(_cy_shallow TRUE)
        if(NOT CY_DEP_${_cy_id}_repository MATCHES "^https://github\\.com/")
            set(_cy_shallow FALSE)
        endif()
        FetchContent_Declare(${_cy_id}
            GIT_REPOSITORY "${CY_DEP_${_cy_id}_repository}"
            GIT_TAG "${CY_DEP_${_cy_id}_commit}"
            GIT_SHALLOW ${_cy_shallow}
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
    # The single-file dependencies. See cy__provide_<name> above.
    if(COMMAND cy__provide_${_cy_id})
        cmake_language(CALL cy__provide_${_cy_id} "${${_cy_id}_SOURCE_DIR}")
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
