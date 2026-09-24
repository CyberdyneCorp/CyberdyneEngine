# `cmake/`

The build's own modules, included by the top-level `CMakeLists.txt` in this order. The order is a
dependency order, not a preference: each file uses what the ones above it defined.

| File | Defines | State |
|---|---|---|
| `profiles.cmake` | The four configurations, their flags, and the profile table across CMake, Cargo and Slang | task 0.1 — done |
| `compilers.cmake` | Minimum compiler versions, the strict warning set, `cy_compile_options` | task 1.2.5 — done |
| `launchers.cmake` | What every compile and link runs through: the two modules below, put together, before the first target exists (included by `compilers.cmake`) | M11.c fifth close |
| `jobpool.cmake` | `CY_JOB_POOL`: every compile and link through `tools/workflow/job_slot.py`, the machine-wide cap of cores − 2 | M11.c fifth close |
| `ccache.cmake` | `CY_CCACHE`, `CY_CCACHE_DIR`, `CY_CCACHE_MAX_SIZE`: ccache as the compiler launcher when it is installed | M11.c fifth close |
| `features.cmake` | The `CY_*` option set, `CY_SANITIZE`, dependency validation, the generated headers | tasks 1.4.1–1.4.3 — done |
| `sanitizers.cmake` | `CY_SANITIZE` turned into compile and link flags | task 4.2.3 — **empty stub** |
| `dependencies.cmake` | `FetchContent` driven by `deps/manifest.toml`, and the `cy::dep::*` shims | task 1.6.2 — done |
| `module.cmake` | `cy_add_module()` and the configure-time layer check | task 1.3.1 — done |
| `modules.cmake` | Discovery of `modules/`, `CY_MODULE_<NAME>`, `CY_EXTRA_MODULE_PATHS`, graph validation | task 1.5.2 — done |

`features.cmake` is included **before** `sanitizers.cmake`: `features.cmake` owns the declaration of
`CY_SANITIZE` and `sanitizers.cmake` reads it. One owner per option.

## Declaring a target

**Every engine target is declared through `cy_add_module()`.** A bare `add_library` or
`add_executable` under `src/`, `platform/`, `modules/`, `editor/`, `tools/`, `tests/`, `benchmarks/`
or `samples/` is a lint failure (`tools/layercheck/layercheck.py`), because it is a target that opted
out of the layer check.

```cmake
cy_add_module(
    NAME                 cy_core_base          # required; the real target name
    LAYER                core                  # required; a name or an index — `core` and `0` are the same
    TYPE                 STATIC                # default STATIC; also SHARED OBJECT INTERFACE EXECUTABLE
    ALIAS                cy::core-base
    SOURCES              src/expected.cpp src/error.cpp
    PUBLIC_INCLUDE_DIRS  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PUBLIC_DEFINITIONS   CY_CORE_BASE=1
    PRIVATE_DEFINITIONS  CY_CORE_BASE_INTERNAL=1
    PUBLIC_DEPENDENCIES  cy::core-diagnostics
    PRIVATE_DEPENDENCIES cy::dep::zstd)
```

Layer names, lowest first: `core` 0, `ecs` 1, `servers` 2, `backends` 3 (also spelled `platform`),
`scene` 4, `runtime` 5, `abi` 6, `editor` 7, `tools` 7.

What the function does for you, so that no call site repeats it:

- records `CY_LAYER` and `CY_LAYER_NAME` as target properties and appends the target to the global
  `CY_MODULES` property;
- links `cy::compile-options`, which carries `cxx_std_20`, the strict warning set and
  `-Werror` (`CY_WARNINGS_AS_ERRORS`, default `ON`);
- layer-checks **every** dependency, public and private, and fails the configure naming both targets
  and both layers. A dependency linked before it is declared is queued and checked once the whole
  tree has been processed, so declaration order cannot hide a violation.

An `INTERFACE` module has one scope: it may not take `SOURCES` or any `PRIVATE_*` argument.
A dependency with no layer — a third-party target, an imported target, `cy::compile-options` — is
skipped rather than rejected.

A keyword misspelled after `SOURCES` (`PUBLIC_DEPS foo`) is swallowed by `SOURCES` rather than
reported as unparsed; `cmake_parse_arguments()` cannot know. It still fails the configure, as
`Cannot find source file: PUBLIC_DEPS`, which names the token.

## The language contract is directory-scope, not a usage requirement

`-fno-exceptions` and `-fno-rtti` (`/EHs-c- /GR-` on MSVC) are `add_compile_options()` in the
top-level `CMakeLists.txt`, **not** properties of `cy::compile-options`. The contract is not opt-in:
it holds for every translation unit compiled in this tree, including one whose author forgot to link
the interface target. A third-party subdirectory that genuinely needs exceptions clears them for its
own scope in `dependencies.cmake` — doctest is the one case, and
`DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS` handles it without relaxing anything.

## Generated headers

`cy_features.h` and `cy_modules.h` are written into the **build** tree — they are a function of the
configuration, not of the source — at `${CMAKE_BINARY_DIR}/generated/include/`. That directory is on
the include path of every target in this tree through a top-level `include_directories()`, so:

```cpp
#include <cy_features.h>
#include <cy_modules.h>
```

works from any engine target with nothing to declare. A target declared **outside** this tree links
`cy::generated-headers` instead.

An enabled feature or module is `#define`d to `1`; a disabled one is not defined at all, so `#ifdef`
and `#if defined()` both answer correctly. `CY_SANITIZE` is a valued setting and is defined to its
string. Regeneration is reproducible and gated by `just generate-check`.

## Linking a third-party dependency

Engine code links `cy::dep::<name>`, never the upstream target: the shim is what makes a fetched copy
and a system copy identical above this file, and it is where doctest's no-exceptions configuration
rides. Available today, from `deps/manifest.toml`:

| Shim | Gated by | For |
|---|---|---|
| `cy::dep::sdl3` | — | windowing, input and gamepads, under `platform/desktop-sdl3/` only |
| `cy::dep::doctest` | `CY_BUILD_TESTS` | the test harness behind `CY_TEST_CASE` |
| `cy::dep::tracy` | `CY_PROFILING` | a backend of the engine's own trace |
| `cy::dep::zstd` | — | compression |
| `cy::dep::blake3` | — | hashing |

A gated dependency that is off has **no target at all** — guard the link with the option rather than
expecting an empty shim. Nothing outside `deps/manifest.toml` carries a version, URL or commit.

## Where a new subdirectory is registered

`src/CMakeLists.txt` adds the layers that contain targets, low to high; `platform/CMakeLists.txt`
adds the backends; `samples/CMakeLists.txt` adds the samples; `tests/CMakeLists.txt` adds the test
suites. `modules/` is not a list — `modules.cmake` discovers manifests. The top-level file adds
`tools/` under `CY_BUILD_TOOLS` and `tests/` and `benchmarks/` under `CY_BUILD_TESTS`, and calls
`enable_testing()` under the same option, which is what makes an `add_test()` anywhere below it
visible to `ctest --preset <profile>`.

**Governed by**: `build-system-and-platforms`.

## How a compiler is invoked: the job pool and ccache

`launchers.cmake` (included at the end of `compilers.cmake`, before any target exists) sets
`CMAKE_{C,CXX}_COMPILER_LAUNCHER` and `CMAKE_{C,CXX}_LINKER_LAUNCHER` as ordinary variables:

* **The job pool** (`jobpool.cmake`, `CY_JOB_POOL`, default ON except where `CI` is set): every
  compile and link waits for one of `tools/workflow/jobs.sh machine` slots shared by every build on
  the machine, so the machine-wide total of compile jobs stays at cores − 2 however many builds
  overlap. The options are baked into `<build>/cy-launchers/job-slot` and `job-slot-link`, one path
  each, because ccache looks every word of a prefix up as a program. `tools/workflow/README.md`
  has the design.
* **ccache** (`ccache.cmake`, `CY_CCACHE`, default ON except where `CI` is set, and only when
  `ccache` is found): a module rather than `CMakePresets.json`, because a preset can only name a
  launcher unconditionally and would break every configure on a machine without it. The pool is
  handed to ccache as `prefix_command`, so a cache hit takes no slot. The store is the project's
  own, `~/.cache/cyberdyne-ccache` (`CY_CCACHE_DIR`), limited by `CY_CCACHE_MAX_SIZE` (default
  60G) passed on ccache's command line — the user's global ccache configuration is never edited.
  `base_dir` is not set, so `-fmacro-prefix-map` keeps working; the sanitizer trees work unchanged
  (checked: `-fsanitize=address,undefined` with the prefix map is a direct hit on the second
  compile and `__FILE__` still reads `src/…`). Swift and Rust do not go through it.

A caller's own launcher (`-D CMAKE_CXX_COMPILER_LAUNCHER=…` or the environment variable) wins.
Turning either on or off changes every compile's command line, so Ninja recompiles the tree once;
with ccache in front that recompile is answered from the cache.

**Measured on the 24-core workstation, M11.c fifth close, `CY_JOBS=5`, `build/m11c-speed` (dev):**

| | |
|---|---|
| full build, cold cache | 3232 steps in 1721 s (28.7 min), while three other agents built and tested beside it |
| the 1376 objects that include `cy/core/base/types.h`, rebuilt **without** ccache (`CCACHE_DISABLE=1`) | 535 s |
| the same 1376 objects, rebuilt **with** ccache | 110 s (the store counted 1371 direct hits over that window) |
| store after that full tree plus parts of two other agents' trees | 0.8 GiB (3.5 GB of object files in the tree; ccache compresses) |

The 60G default is therefore room for about seventy dev trees at one per path, which covers the
~40 trees a ledger, its matrix and a few agents keep. **Not cached**: Slang's 387 translation units
that use a precompiled header ("could not use precompiled header"); caching those needs
`sloppiness=pch_defines,time_macros`, which was not turned on because it also caches `__DATE__` and
`__TIME__`.
