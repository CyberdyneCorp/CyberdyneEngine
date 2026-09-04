#pragma once
// The standard material, and the fallbacks for when a material is missing or broken.
// Tasks 4.2.4 and 4.2.5.
//
// `rendering-materials-and-shading` — "Standard material": "The engine SHALL provide a standard
// material covering common needs without custom shader authoring", and it lists the slots. All of
// them are declared here, by the name the specification uses, so that a project's content and the
// engine's shaders agree on the spelling without a table anybody maintains by hand.
//
// --- WHY THE SLOT SET IS DECLARED IN CODE RATHER THAN IN AN ASSET
// ---------------------------------
//
// Because the shader is written against it. A slot's name resolves to a `ParameterId` at compile
// time (`parameter_id`), the shader reaches the same offset through the same identifier, and a slot
// that existed in the asset but not in the shader would be a parameter nothing reads — which
// validation reports rather than tolerating. When M7's material compiler arrives, an authored graph
// lowers to a program whose parameters it declares itself; the standard material is then one such
// program that the engine happens to ship, and nothing about the shape changes.
//
// --- THE PACKED ORM SLOT IS THE INTERESTING ONE
// ---------------------------------------------------
//
// "WHEN occlusion, roughness, and metallic share one texture THEN the material SHALL sample it once
// and route channels, rather than three samples." That is expressed as ONE texture slot
// (`occlusion_roughness_metallic`) plus three channel-routing parameters, rather than as three
// texture slots the author is expected to point at the same image — because three slots pointing at
// one image is a thing a shader compiler cannot detect, and one slot with routing is a thing it
// cannot get wrong.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/material/material.h>

namespace cy::rendering {

/// The standard material's parameter identifiers, resolved once.
///
/// Every field is a `ParameterId` computed from the name in the comment beside it, at compile time.
/// A consumer holds this struct rather than calling `parameter_id("base_color_factor")` per frame —
/// "Parameter names SHALL resolve to compile-time identifiers; per-frame string lookup SHALL NOT be
/// required", enforced by there being nowhere in the frame path that takes a string.
struct StandardParameters {
    ParameterId base_color_factor = parameter_id("base_color_factor");
    ParameterId base_color_texture = parameter_id("base_color_texture");
    ParameterId metallic_factor = parameter_id("metallic_factor");
    ParameterId roughness_factor = parameter_id("roughness_factor");
    ParameterId specular_factor = parameter_id("specular_factor");
    /// The packed occlusion-roughness-metallic texture. See the header comment.
    ParameterId orm_texture = parameter_id("orm_texture");
    ParameterId normal_texture = parameter_id("normal_texture");
    ParameterId normal_scale = parameter_id("normal_scale");
    ParameterId occlusion_strength = parameter_id("occlusion_strength");
    ParameterId emission_color = parameter_id("emission_color");
    ParameterId emission_texture = parameter_id("emission_texture");
    ParameterId opacity = parameter_id("opacity");
    ParameterId alpha_cutoff = parameter_id("alpha_cutoff");
    ParameterId height_texture = parameter_id("height_texture");
    ParameterId height_scale = parameter_id("height_scale");
    ParameterId clearcoat = parameter_id("clearcoat");
    ParameterId clearcoat_roughness = parameter_id("clearcoat_roughness");
    ParameterId anisotropy = parameter_id("anisotropy");
    ParameterId anisotropy_direction = parameter_id("anisotropy_direction");
    ParameterId subsurface_color = parameter_id("subsurface_color");
    ParameterId thickness_texture = parameter_id("thickness_texture");
    ParameterId detail_texture = parameter_id("detail_texture");
    ParameterId detail_uv_scale = parameter_id("detail_uv_scale");
    ParameterId uv_tiling_offset = parameter_id("uv_tiling_offset");
    /// Which UV channel each texture slot samples, packed one nibble per slot. "Texture slots SHALL
    /// support per-slot UV channel selection" — as data rather than as a permutation, because the
    /// alternative is a permutation per slot per channel.
    ParameterId uv_channel_mask = parameter_id("uv_channel_mask");
    ParameterId vertex_color_modulation = parameter_id("vertex_color_modulation");
    ParameterId distance_fade = parameter_id("distance_fade");
    ParameterId proximity_fade = parameter_id("proximity_fade");

    // --- Static parameters: these are permutations, not data ---------------------------------
    //
    // Each is a static boolean, so each doubles the program's permutation count. That is why there
    // are four of them and not fourteen: `MaterialProgram::permutation_count()` measures it and
    // `validate_material` reports it against a budget, which is the mechanism that keeps this list
    // from growing by one convenience at a time.
    ParameterId use_triplanar = parameter_id("use_triplanar");
    ParameterId use_world_space_uv = parameter_id("use_world_space_uv");
    ParameterId use_detail_layer = parameter_id("use_detail_layer");
    ParameterId two_sided = parameter_id("two_sided");
};

/// Declare the standard material's parameters into `program`.
///
/// `program` must be freshly initialised; the function declares every slot in a fixed order, so two
/// runs produce byte-identical offsets — which matters because those offsets are what the shader is
/// compiled against.
[[nodiscard]] Status describe_standard_material(MaterialProgram& program, Name name,
                                                ShadingModel model, BlendMode blend) noexcept;

/// Write the standard material's defaults into a slot: a mid-grey dielectric, fully opaque, with no
/// textures bound. What a material with nothing authored looks like, and it is deliberately a
/// plausible surface rather than a black one — a black default reads as "the lighting is broken".
[[nodiscard]] Status apply_standard_defaults(const MaterialProgram& program, MaterialTable& table,
                                             u32 index, const StandardParameters& ids) noexcept;

// --- Fallback materials -------------------------------------------------------------------------

/// `rendering-materials-and-shading` — "Fallback materials": "The engine SHALL provide fallback
/// materials for error states: missing material, failed shader compilation, and missing texture —
/// each visually distinctive (magenta checkerboard) so problems are immediately obvious rather than
/// silently black."
///
/// Three kinds rather than one, because they are three different bugs and the person looking at the
/// screen should be able to tell them apart without opening the log.
enum class FallbackKind : u8 {
    MissingMaterial = 0,
    ShaderCompilationFailed,
    MissingTexture,
    Count,
};

[[nodiscard]] const char* fallback_kind_name(FallbackKind kind) noexcept;

/// The two colours a kind's checkerboard alternates, linear and un-premultiplied.
///
/// All three are magenta-dominant — that is the specification's word — and they differ in the
/// second colour so the three states are distinguishable at a glance: a missing material is
/// magenta and black, a failed shader magenta and red, a missing texture magenta and white.
void fallback_colors(FallbackKind kind, Vec4& out_primary, Vec4& out_secondary) noexcept;

/// Generate a checkerboard into `out` as RGBA8 texels, row-major.
///
/// `size` is the edge in texels and `checker` the square size. Produced in code rather than shipped
/// as an asset, because a fallback that lives in an asset is a fallback that is missing exactly
/// when the asset system is what failed.
[[nodiscard]] Status generate_fallback_texture(FallbackKind kind, u32 size, u32 checker,
                                               Array<u8>& out) noexcept;

/// Describe a fallback material's program: unlit, opaque, emissive magenta, and no textures — so it
/// renders identically whether or not lighting, shadows or the texture system are working. An
/// error material that needed a light to be visible would be invisible in exactly the frames where
/// something is wrong.
[[nodiscard]] Status describe_fallback_material(MaterialProgram& program,
                                                FallbackKind kind) noexcept;

}  // namespace cy::rendering
