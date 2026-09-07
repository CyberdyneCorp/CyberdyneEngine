# `src/backends/shader/` — layer 3

The shader system: **Slang** in, **SPIR-V** out, and the caching, permutation, reflection and
pipeline-state machinery built around somebody else's compiler.

**Governed by**: `shader-system`. Landed at M3, section 3 of that milestone's tasks.

The engine does not author a shading language and does not write a shader optimiser
(`thirdparty-dependencies`). What it owns is the pipeline: how source is found, how variation is
declared and counted, how bindings are derived, how results are keyed and shared, how an edit
reaches a running frame, and how pipeline states are collected, warmed and fallen back from.

## The two targets

| Target | Built when | What it is |
|---|---|---|
| `cy::shader` | always | the pipeline: source registry, permutations, SPIR-V reflection, the tiered cache, the shader library, global parameters, hot reload, pipeline state management — and the **SPIR-V passthrough** front end |
| `cy::shader-slang` | `CY_SHADER_SLANG` | Slang source → SPIR-V. The only directory in the engine that names a Slang type |

**The SPIR-V passthrough is not a stub in the fallback position.** `shader-system` requires that "a
shipping build SHALL contain compiled backend-native shader artefacts and no Slang compiler", so
consuming an already-compiled module *is* the shipping path. It is also what lets continuous
integration exercise reflection, permutations, the cache, the library, hot reload and the pipeline
manifest on a machine with no shader toolchain — which is how the two default suites run.

**`smoke.shader_slang` does not run under a sanitizer, and that is a stated exclusion.**
`just test-sanitize` configures its tree with `-D CY_SHADER_SLANG=OFF`, so `cy::shader-slang` and
this suite are not built there at all. Three independent reasons, each measured at M4's gate and all
of them in Slang rather than here: LeakSanitizer fails the BUILD on `slang-fiddle`, ThreadSanitizer's
shadow mapping stops `slang-embed` from starting, and under UndefinedBehaviorSanitizer the suite
fails first on Slang's own COM smart pointer and then — with that quietened — on Slang failing to
`dlopen` its downstream plugins in that tree. `just/test.just` carries the reproductions. The
passthrough front end and everything above it *are* sanitized, which is where the engine's own code
is.

## Why layer 3

Reflection reads the SPIR-V binary, and a SPIR-V header is a graphics-API header by
`tools/layercheck/layercheck.py`'s `gpuapi` rule: it may not appear above `src/backends/`. The Slang
front end names Slang's own types for the same reason. Everything above consumes `cy::shader`'s
engine-owned vocabulary, and reflection produces `rhi::DescriptorBinding`, `rhi::PushConstantRange`
and `rhi::VertexAttribute` **directly** — so there is no second description of a layout to drift
from the first.

## The decisions worth knowing

**Bindings are derived, not declared** (`include/cy/backends/shader/reflection.h`). Descriptor set
layouts, push-constant ranges and vertex inputs come out of the compiled module. That is the same
argument that made M1's type reflection generated rather than macro-declared: a hand-maintained
table describing something else's contents is correct on the day it is written and wrong on a later
day nobody can name. Reflection reads SPIR-V rather than Slang, so a module from a cache tier, from
a shipped library, or from a generator that already compiled it reflects identically to one compiled
locally.

**Specialization costs no compilation** (`include/cy/backends/shader/permutation.h`).
`pipeline_variants()` counts every combination; `compiled_variants()` counts only the axes that need
their own SPIR-V. Moving an axis from `Preprocessor` to `Specialization` changes the second number
and nothing else, which is what makes `shader-system`'s ordering of the three mechanisms something a
build report can show rather than something a reviewer has to notice.

**Generated source has one entry point, and it is small**
(`include/cy/backends/shader/source.h`). `SourceRegistry::add_generated()` is where M7's material
compiler hands the engine its lowered Slang. Nothing downstream branches on
`SourceUnit::origin`: the compiler, the reflection, the cache key, the library and the hot-reload
path all take a `SourceUnit` and cannot tell an authored module from a generated one. An `import`
resolves through the registry rather than through the operating system, so a generated module is
importable by exactly the same syntax as a file.

**The cache key contains what the specification says it contains**
(`include/cy/backends/shader/cache.h`). Source hash, compiler name and version, artefact version,
target platform, renderer profile, feature set, entry point, permutation, optimisation, debug level,
SPIR-V version — and there is no way to build a key from anything less. A hit in a slower tier is
written back into the writable tiers in front of it, which is "CI populates, developers consume" in
one line of behaviour.

**M7 folded it into the one derivation key** (task 1.2). It used to hash its fields straight into a
`ContentHasher`; it is now built through `cy::assets::DerivationKeyBuilder`, so it gets that type's
framed, prefix-free encoding, and it contributes `cy::assets::ToolchainFingerprint`, so an artefact
written by an engine binary compiled at `-O0` is not served to one compiled at `-O2`. The merged key
is the union of what the tree's three key functions each got right: the version of the tool this one
INVOKES (Slang) and the artefact-format version from here, the producing binary's toolchain from
`tools/build/`, and the framing from layer 0. `derive_cache_key` returns `Expected` for the reason
`cy::build::derivation_key` does — an incomplete fingerprint is refused, never defaulted.

**A miss never compiles inline** (`include/cy/backends/shader/pipeline.h`).
`PipelineStateCache::request()` returns the fallback and queues the state; `build_pending()` does the
work, wherever the caller likes. "Blocking the frame to compile a pipeline state SHALL NOT occur in
shipping builds" is a property of that split rather than a rule somebody remembers.

**Hot reload detects on one thread and compiles on another** (`include/cy/backends/shader/hot_reload.h`).
`poll()` drives `cy::assets::FileWatcher`, which reads files and must not run in a job body;
`rebuild()` compiles and swaps, and is safe on a worker. A failed rebuild calls
`ShaderLibrary::replace()` zero times, so the old artefact is still there and the pipeline built from
it is still valid — which is the whole of "a broken shader does not break the frame".

## Layout

```
include/cy/backends/shader/
  source.h       where source comes from, and the generated-source seam (task 3.8)
  compiler.h     the request, the artefact, the front-end registry, the SPIR-V passthrough
  permutation.h  axes, the mixed-radix key, the budget report
  reflection.h   SPIR-V reflection and the descriptor set convention
  cache.h        the key, the tiers, the search with write-back
  library.h      variants, programs, deduplication, the artefact encoding
  globals.h      the global parameter block and its std140 layout
  pipeline.h     pipeline state keys, the manifest, warming and the fallback
  hot_reload.h   the watcher wiring and the rebuild
  diagnostics.h  structured diagnostics and the compiler-output parser
src/spirv.h      the subset of the SPIR-V binary format the parser reads
slang/           the Slang front end, and nothing else
tests/fixtures/  probe.slang, and the compiled modules the reflection suite reads
```

The engine's own Slang source — the shader standard library `shader-system` requires — is
`src/rendering/shaders/`, at layer 4, because it is content rather than code.
