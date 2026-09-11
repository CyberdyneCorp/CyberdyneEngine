#pragma once
// THE STEP THAT MAKES A GENERATED MATERIAL COMPILE. M8.c task 1b.3.
//
// ================================================================================================
// THE GAP THIS CLOSES, MEASURED RATHER THAN ASSERTED
// ================================================================================================
//
// M7 recorded that "the material compiler fills a GPU material table nothing draws with" and, in
// `emit.h`, that "nothing here invokes the Slang compiler ... the text names an interface the
// engine's shader standard library supplies — `CyMaterialContext`, `CyClosure`, `cy_closure_*` —
// and `shader-system` owns compiling it".
//
// **Nothing owned compiling it.** Handing `cy_material compile --slang` output to `slangc` against
// `src/rendering/shaders/` produced, before this milestone:
//
//     error[E30015]: undefined identifier 'CyMaterialContext'
//     error[E30015]: undefined identifier 'CySurface'
//     error[E30015]: undefined identifier 'CY_TEXTURE_base_color_map'
//     error[E30015]: undefined identifier 'cy_material_sample'
//     error[E30015]: undefined identifier 'CyClosure'
//
// Two separate causes, and they are closed in two separate places:
//
//  1. **The closure vocabulary did not exist.** `cy/material.slang` declared `Surface`,
//     `SurfaceInput` and `IMaterial` and none of the `Cy*` names the emitter writes. That half is
//     material-INDEPENDENT and now lives in the standard library where the emitter always said it
//     did.
//
//  2. **A generated program is not self-contained, and never was.** `write_header` emits a comment
//     and `import cy.material;` and nothing else — so `ctx.params.metallic`, `ctx.attributes.uv0`
//     and `CY_TEXTURE_base_color_map` name a parameter struct, an attribute struct and a slot
//     constant that appear in NO file. They cannot be in the standard library: they are different
//     for every material. `emit_prelude` below generates them from the module's own declarations,
//     which is the only place they can come from.
//
// ================================================================================================
// A SEPARATE TARGET, BECAUSE M7's IS CLOSED WORK
// ================================================================================================
//
// `cy::rendering-material-slang` is a second target in this directory. `cy::rendering-material` is
// not modified and `emit.cpp` is not touched: `docs/ROADMAP.md` records that
// "`src/rendering/material/` is not modified — it is M7's closed work and one extension invalidates
// its cook keys", and every byte the emitter produces is part of a cook key. The prelude is written
// AROUND that output, never into it, so the cook key of every material in the tree is unchanged —
// which is checkable, and `tools/material/tests/` already checks it.
//
// ================================================================================================
// WHAT IS NOT CLOSED, AND IT IS NAMED RATHER THAN PAPERED OVER
// ================================================================================================
//
//  * **A field sample is a float.** `Op::Field` carries a type and this prelude declares
//    `cy_field_sample` returning `float`, because the call is `cy_field_sample(ctx, CY_FIELD_x)`
//    with the slot as a value — so two fields of different types cannot be told apart by overload
//    resolution. `environment-fields` is M10's; when it arrives, either the emitter spells the type
//    into the call or the slot becomes a type-tagged constant.
//  * **The probe entry point is a compute shader, not the frame's fragment shader.** What this
//    module proves is that the generated program COMPILES and reflects; wiring a compiled material
//    into `src/rendering/pipeline/`'s forward pass is a permutation and pipeline-cache question,
//    and `material-compiler`'s "Secondary material programs" is where it belongs. A consequence
//    follows and is generated into the prelude with its reason: `cy_material_sample` uses the
//    EXPLICIT level of detail, because implicit derivatives exist only in a pixel stage.
//  * **A NESTED AUTHORED IMPORT DOES NOT RESOLVE THROUGH `cy::shader`'s SLANG FRONT END, and this
//    milestone is the first thing that ever asked it to.** `src/backends/shader/slang/`'s
//    `RegistryFileSystem` implements `ISlangFileSystem` and not `ISlangFileSystemExt`, so it
//    supplies no `calcCombinedPath` and Slang combines the importing file's directory with the
//    imported name itself. Compiling a module that imports `cy.material`, which imports `cy.brdf`,
//    asks the resolver for `cy.cy.brdf` and gets nothing. `smoke.shader_slang` compiles ONE
//    generated module importing ONE authored module, one level deep — and every module of the
//    standard library imports at least one other, so the library has never been compiled through
//    the engine's own front end. `smoke.material_slang` works around it in its own resolver and
//    says so; the fix is `calcCombinedPath` in the front end, which is below this layer.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/material/emit.h>
#include <cy/rendering/material/ir.h>

namespace cy::rendering::material {

/// The entry point `assemble_translation_unit` appends. Named here so a caller compiles it without
/// spelling a string twice.
inline constexpr const char* kMaterialProbeEntryPoint = "cyMaterialProbe";

struct PreludeOptions {
    /// Which descriptor set the material's own parameter block is bound to. Set 3 is "per draw" in
    /// `cy/backends/shader/reflection.h`'s convention, which is exactly what a material instance
    /// is.
    u32 material_set = 3;
};

/// What the prelude declared. Every number is counted off the module rather than predicted.
struct PreludeReport {
    u32 parameters = 0;
    u32 attributes = 0;
    u32 textures = 0;
    u32 fields = 0;
};

/// Generate the per-material declarations the emitted program refers to and does not declare.
///
/// Appends to `out`; does not clear it.
[[nodiscard]] Expected<PreludeReport, Error> emit_prelude(const Module& module,
                                                          const PreludeOptions& options,
                                                          Array<char>& out) noexcept;

/// prelude + the emitter's own text + a probe entry point, as ONE compilable Slang unit.
///
/// The emitter's text is copied BYTE FOR BYTE, which is the property that keeps this step outside
/// M7's cook key rather than inside it.
[[nodiscard]] Expected<PreludeReport, Error> assemble_translation_unit(
    const Module& module, const GeneratedSource& generated, ProgramKind kind, QualityTier tier,
    const PreludeOptions& options, Array<char>& out) noexcept;

}  // namespace cy::rendering::material
