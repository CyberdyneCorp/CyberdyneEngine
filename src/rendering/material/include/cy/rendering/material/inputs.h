#pragma once
// WHICH OF A MATERIAL'S INPUTS ARE TEXTURES AND WHICH ARE CONSTANTS. M11.c task 1.2.
//
// `material-compiler` — "The compile report says which inputs are textures and which are
// constants": "The compile report SHALL state, per material, which inputs are bound to textures and
// which are constants, and the count of each SHALL be readable by a tool without opening the shader
// source."
//
// ================================================================================================
// WHY THIS EXISTS, AND IT IS NOT A CONVENIENCE
// ================================================================================================
//
// Every material in every picture this project has published is made of constants. Outside `docs/`
// the tree holds six image files, four of them editor identity marks and two of them golden
// references, and the only texture any sample binds is a checkerboard generated on the first frame.
// A constants-only material renders, shades and photographs — `~/cyberdyne-spikes/m11c-material-
// spike/out/shot-average-post.png` is a lit, tone-mapped, entirely plausible picture of one — and
// NOTHING IN THE COMPILED ARTEFACT DISTINGUISHED IT FROM A FULLY TEXTURED MATERIAL. So a sentence
// about what a published picture demonstrates could not be checked, only believed.
//
// This is the fact that makes it checkable, computed off the IR rather than asserted in a caption.
//
// ================================================================================================
// WHAT AN "INPUT" IS HERE, STATED SO THAT THE COUNT MEANS SOMETHING
// ================================================================================================
//
// The inputs of the SURFACE: one per operand of every leaf closure the surface root reaches, plus
// opacity. `diffuse.colour`, `specular.colour`, `specular.roughness`, `emission.colour`,
// `coat.roughness`, `opacity` — the values a shading model is made of, which is what somebody
// means by "is the albedo a texture?".
//
// NOT the declared parameter list and NOT the declared texture list. A material can declare a
// texture, never sample it into a closure, and still count as "has a texture" — that is precisely
// the shape a caption would overstate. Classification walks from the closure operand DOWNWARD and
// answers what actually reaches the surface.
//
// The four classes are ordered by strength, and the order is the whole content of the rule: a
// texture reaching an input beats a parameter reaching it, which beats an attribute or a field,
// which beats a literal. `diffuse.colour = sample(albedo_map, uv0).xyz * base_color` is a TEXTURE
// input, because the picture it produces is a picture of the map.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/material/ir.h>

namespace cy::rendering::material {

/// What supplies one surface input. Ordered by strength — see the header note.
enum class InputBinding : u8 {
    /// A literal, after folding. Nothing reads it at runtime and no picture can show a change in
    /// it.
    Constant = 0,
    /// A geometry attribute or an environment field: the mesh or the world supplies it.
    Varying = 1,
    /// A runtime material parameter: data, changeable without recompilation.
    Parameter = 2,
    /// A texture sample reaches it.
    Texture = 3,
};

[[nodiscard]] const char* input_binding_name(InputBinding binding) noexcept;

/// One surface input and what supplies it.
struct MaterialInput {
    /// `diffuse.colour`, `specular.roughness`, `opacity` — the closure's own name and its
    /// operand's. A literal rather than a `Name`, because these are fixed by the closure vocabulary
    /// and interning them would put a string table between a report and its reader.
    const char* name = "";
    /// The IR value the input is, so an editor can preview exactly this.
    NodeId value = kInvalidNode;
    InputBinding binding = InputBinding::Constant;
    /// The first texture reaching it, in sorted order, or an empty `Name`. Named so a report can
    /// say WHICH map, which is what an author asks next.
    Name texture;
};

/// The counts, which are what a tool reads without opening the shader source.
struct InputReport {
    explicit InputReport(Allocator& allocator) noexcept : inputs(allocator) {}

    InputReport(const InputReport&) = delete;
    InputReport& operator=(const InputReport&) = delete;
    InputReport(InputReport&&) noexcept = default;
    InputReport& operator=(InputReport&&) noexcept = default;

    Array<MaterialInput> inputs;
    u32 constants = 0;
    u32 varying = 0;
    u32 parameters = 0;
    u32 textures = 0;

    /// True when NOT ONE surface input is supplied by a texture: the shape every picture this
    /// project has published was made of.
    [[nodiscard]] bool constants_only() const noexcept { return textures == 0; }
};

/// Classify every surface input of a module. Deterministic: inputs come out in canonical order, so
/// two runs of the same material produce the same report and two reports can be compared.
[[nodiscard]] Expected<InputReport, Error> classify_inputs(const Module& module) noexcept;

}  // namespace cy::rendering::material
