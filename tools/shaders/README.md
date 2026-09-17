# tools/shaders — the shader toolchain's front end

`cy_shaderc` compiles the engine's Slang shader set for every target this build can emit, and
measures the agreement between them. `just build-shaders` is this tool.

    cy_shaderc targets
    cy_shaderc build [root...] [--target spirv|msl|dxil]... [--out-dir <dir>] [--verbose]

## What it is for

`shader-system` fixes the offline pipeline as five steps, and step 4 is *"per backend: SPIR-V
retained (Vulkan), or translated (MSL for Metal, DXIL for D3D12)"*. Until M11.c the Slang front end
hard-coded `SLANG_SPIRV`, `SLANG_ENABLE_DXIL` was `OFF`, and neither MSL nor DXIL was emitted
anywhere in the tree — which is why M11.d, whose first task is a Metal and a D3D12 backend, carries
`shader-targets-emitted` as a hard prerequisite and M11.c carries
`shader-targets-for-the-next-rung` as the same claim from the other side.

## What it measures, and what it refuses to accept as evidence

**Emitting a file is not the claim.** A check that a `.metal` and a `.dxil` exist and are not empty
passes on a stub that writes the SPIR-V twice under two names. This tool makes three assertions that
such a stub fails:

1. **Each artefact is in the form its target names** — the SPIR-V magic word, a DXBC container with
   a DXIL chunk inside it, Metal Shading Language that includes `<metal_stdlib>`. The front end
   itself refuses to return an artefact that is not (`cy::shader::bytes_match_target`), so a caller
   that forgot to check cannot be handed one.
2. **The artefacts of one entry point differ from each other**, by content hash.
3. **They declare the same interface**: the same entry point name, the same stage, the same
   workgroup size, and the same parameters in the same order with the same kinds and counts.

What is deliberately **not** compared is where each target put each parameter. Metal gives buffers,
textures and samplers three separate index spaces and D3D12 gives them four register classes, so one
`Texture2D` is `(set 0, binding 2)` in SPIR-V, `texture(0)` in MSL and `t1` in DXIL for the same
declaration. Requiring those to match would be requiring a falsehood; they are printed and not
asserted.

## The set is discovered, not listed

Every `.slang` file under the roots is read into one module set, and an entry point is a
`[shader("stage")]` attribute — which is how Slang itself finds one. A list of shaders maintained
beside a directory of shaders goes stale the day somebody adds a file, and the point of this
exercise is a claim that cannot go stale quietly. Each file is reachable by every suffix of its
dotted path, so `import cy.view` and `#include "hzb_common.slang"` both resolve without the
`-I` directories the checked-in `slangc` invocations use.

A file that declares no entry point by attribute is counted and reported; it is still part of the
module set, because that is what an include-only module is.

## DXIL needs DXC, and the tool says so rather than assuming

DXIL is produced by Microsoft's DXC, which Slang fetches as a prebuilt release (`CY_SHADER_DXIL`,
on by default in Debug and Development) and loads dynamically when it is first asked to emit DXIL.
`cy_shaderc targets` answers by **compiling a shader for each target**, so a machine where that
library is missing is reported as a machine that does not emit DXIL — not as one where an option
was set.
