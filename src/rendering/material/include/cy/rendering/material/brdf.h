#pragma once
// The core BRDF, the shading models, and image-based lighting. Task 4.2.3.
//
// ================================================================================================
// WHY A CPU IMPLEMENTATION OF A SHADER'S MATH EXISTS AT ALL
// ================================================================================================
//
// `rendering-materials-and-shading` opens by saying why it specifies the BRDF term by term: "'PBR'
// alone is not a specification and mismatched terms produce subtly wrong lighting that is very hard
// to debug later." Every term below is one of that specification's, written in the same form it is
// stated in, and the suite next to this file checks the properties it states — a metal has no
// diffuse, a rough metal keeps its energy, a dark dielectric does not get a bright rim.
//
// A GOLDEN IMAGE CANNOT DO THAT. It tells you the frame changed; it does not tell you that the
// visibility term lost its `4·NoL·NoV` divisor, and a frame with that defect looks plausible. So
// the engine's BRDF exists twice: here, where it is arithmetic that can be asserted on, and in
// Slang, where it runs. The two are kept in step by this header being the definition the shader is
// written from — the same relationship `GpuInstance` has with the shader's instance struct.
//
// THE SECOND REASON is that M7's material compiler lowers a closure set to one of these models. A
// compiler needs the model set to be data — `ShadingModel`, the table in `rendering-materials-and-
// shading` — rather than a set of shader files, and the lowering has to be testable before the
// shaders exist.
//
// ================================================================================================
// WHAT IS HERE AND WHAT IS NOT
// ================================================================================================
//
// Here: the `Lit` model's terms in full, the three diffuse models, multi-scatter compensation, the
// split-sum DFG table's generator, and spherical-harmonic irradiance.
//
// Not here: the eight other shading models' additional terms. They are enumerated
// (`render::ShadingModel`) and their closure sets are documented, and each one lands with the pass
// that needs it — clearcoat and anisotropy at M3's standard material if the frame reaches them,
// hair and cloth at M7. Writing nine models against no shaders would be nine pieces of untested
// arithmetic rather than one tested one.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/servers/render/types.h>

namespace cy::rendering {

using render::ShadingModel;

// --- Constants the specification fixes --------------------------------------------------------

/// "roughness SHALL be perceptual in material authoring (α = roughness²), and SHALL be clamped to a
/// minimum (default 0.045) to keep highlights representable."
///
/// The clamp is not cosmetic: at α below about 0.002 the GGX lobe is narrower than a pixel and the
/// highlight either disappears or aliases into a crawling sparkle, depending on the sampling.
inline constexpr f32 kMinPerceptualRoughness = 0.045F;

/// The dielectric `f0` at normal incidence for `specular = 1`. 0.04 is the specification's, and it
/// is about right for most non-metals (glass is 0.04, water 0.02, gemstones higher).
inline constexpr f32 kDielectricF0 = 0.04F;

/// `f90 = saturate(dot(f0, vec3(50/3)))`. The constant is the specification's, and what it does is
/// remove the grazing-angle rim from a surface whose `f0` is so dark that a full Fresnel rim would
/// be unphysical — specular occlusion, expressed as a property of `f0` rather than as a texture.
inline constexpr f32 kF90Scale = 50.0F / 3.0F;

/// α from perceptual roughness, with the clamp applied. The ONE conversion site: a second one that
/// forgot the clamp would produce a highlight that is correct in one pass and aliased in another.
[[nodiscard]] f32 alpha_from_roughness(f32 perceptual_roughness) noexcept;

// --- The specular terms -----------------------------------------------------------------------

/// GGX / Trowbridge-Reitz, in the numerically stable form the specification names:
///
///     k = α / (1 − NoH² + (NoH·α)²),  D = k² / π
///
/// The stability matters. The textbook form divides by `(NoH²(α²−1) + 1)²`, and at α near zero and
/// NoH near one that denominator is a difference of nearly equal f32 quantities — the highlight of
/// a smooth surface, which is exactly where the term is largest. The form above is that same
/// denominator regrouped so nothing cancels: `1 − NoH² + (NoH·α)² = 1 + NoH²(α² − 1)`, term by
/// term. `rendering-materials-and-shading` writes the numerator of `k` as `α` and its denominator
/// as `1 − NoH² + α²`; the `NoH` inside the squared term is what makes the two expressions equal
/// and the distribution normalised, and src/brdf.cpp says what dropping it costs.
[[nodiscard]] f32 distribution_ggx(f32 n_dot_h, f32 alpha) noexcept;

/// Smith height-correlated visibility, ALREADY DIVIDED BY `4·NoL·NoV` (Hammon's approximation):
///
///     V = 0.5 / lerp(2·NoL·NoV, NoL + NoV, α)
///
/// "Already divided by" is the whole reason this is called visibility rather than geometry, and
/// losing that divisor is the mistake this file exists to make checkable: a frame missing it is too
/// bright at grazing angles and looks like a lighting-intensity problem.
[[nodiscard]] f32 visibility_smith_hammon(f32 n_dot_l, f32 n_dot_v, f32 alpha) noexcept;

/// Schlick's Fresnel: `f0 + (f90 − f0)·(1 − VoH)⁵`.
[[nodiscard]] Vec3 fresnel_schlick(Vec3 f0, f32 f90, f32 v_dot_h) noexcept;

/// `f0 = lerp(vec3(0.04 · specular²), albedo, metallic)`.
///
/// A metal's `f0` IS its albedo, which is why a metal has no diffuse colour to lose — the two facts
/// are one fact, and the scenario "WHEN metallic is 1.0 THEN the diffuse term SHALL be zero and f0
/// SHALL be the albedo" is a consequence of this line rather than a special case elsewhere.
[[nodiscard]] Vec3 compute_f0(Vec3 albedo, f32 metallic, f32 specular) noexcept;

[[nodiscard]] f32 compute_f90(Vec3 f0) noexcept;

// --- The diffuse terms ------------------------------------------------------------------------

/// "Diffuse — selectable per material." The choice is the material's, and the default is Lambert.
enum class DiffuseModel : u8 {
    Lambert = 0,
    Burley,
    OrenNayar,
    Count,
};

[[nodiscard]] const char* diffuse_model_name(DiffuseModel model) noexcept;

/// `albedo / π`. The `1/π` is the normalisation that makes a Lambertian surface conserve energy;
/// leaving it out makes everything π times too bright, which is the second-most-common defect in a
/// hand-written BRDF after the missing `4·NoL·NoV`.
[[nodiscard]] f32 diffuse_lambert() noexcept;

/// Disney's, with `FD90 = 0.5 + 2·VoH²·roughness`. Retro-reflective at grazing angles, which is
/// what makes cloth and unpolished stone read correctly.
[[nodiscard]] f32 diffuse_burley(f32 n_dot_v, f32 n_dot_l, f32 v_dot_h,
                                 f32 perceptual_roughness) noexcept;

/// Oren-Nayar, "for rough dielectrics where retro-reflection matters". The qualitative-model form,
/// which is the one worth having in real time.
[[nodiscard]] f32 diffuse_oren_nayar(f32 n_dot_v, f32 n_dot_l, f32 l_dot_v,
                                     f32 perceptual_roughness) noexcept;

// --- The surface, and one light -----------------------------------------------------------------

/// A shading point, in the parameters the material authored.
struct SurfaceParameters {
    Vec3 albedo{0.5F, 0.5F, 0.5F};
    f32 metallic = 0.0F;
    /// PERCEPTUAL. `alpha_from_roughness` is what turns it into α, once.
    f32 roughness = 0.5F;
    /// The dielectric specular intensity, in [0, 1]; 1 is the 0.04 default.
    f32 specular = 1.0F;
    DiffuseModel diffuse = DiffuseModel::Lambert;
    /// Multi-scatter compensation, from the DFG table. See `multiscatter_compensation`.
    bool energy_compensation = true;
};

/// The geometry of one light at one point. Every member is a cosine, so the evaluator does no
/// vector arithmetic and the terms can be exercised at values a hand calculation can check.
struct LightSample {
    f32 n_dot_l = 1.0F;
    f32 n_dot_v = 1.0F;
    f32 n_dot_h = 1.0F;
    f32 v_dot_h = 1.0F;
    /// Only Oren-Nayar reads it.
    f32 l_dot_v = 0.0F;
};

/// The `Lit` model's direct lighting: `(diffuse + specular) · NoL`, without the light's own colour
/// or attenuation, which are the caller's.
[[nodiscard]] Vec3 evaluate_direct(const SurfaceParameters& surface,
                                   const LightSample& light) noexcept;

/// The specular half alone, for a test that wants to look at it without the diffuse.
[[nodiscard]] Vec3 evaluate_specular(const SurfaceParameters& surface,
                                     const LightSample& light) noexcept;

// --- Energy conservation ------------------------------------------------------------------------

/// The multi-scatter compensation factor.
///
/// "A multi-scatter compensation term SHALL be applied to specular so rough metals do not lose
/// energy, using the split-sum DFG table's average." Single-scatter GGX drops the light that
/// bounces more than once between microfacets, and the loss grows with roughness — a metal at
/// roughness 0.9 loses roughly forty per cent of its reflectance and reads as dirty rather than
/// rough. The factor is `1 + f0·(1/Ess − 1)`, where `Ess` is the directional albedo the DFG table
/// carries.
[[nodiscard]] Vec3 multiscatter_compensation(Vec3 f0, f32 dfg_scale, f32 dfg_bias) noexcept;

// --- Image-based lighting -----------------------------------------------------------------------

/// One entry of the split-sum DFG table: the `(scale, bias)` a material's `f0` is combined with.
struct DfgEntry {
    f32 scale = 0.0F;
    f32 bias = 0.0F;
};

/// Generate the DFG lookup table — "a 2D RG16F texture indexed by NoV and roughness, generated with
/// GGX importance sampling".
///
/// `out` is `size * size` entries in row-major order, rows indexed by roughness and columns by NoV,
/// both sampled at texel centres. `samples` is the importance-sample count; 128 is enough for a
/// table this smooth and 1024 is what an offline bake would use.
///
/// Deterministic: the sample sequence is Hammersley, which is a closed-form function of the sample
/// index, so two runs produce byte-identical tables. That matters more than it sounds — a table
/// baked with a random sequence would make every golden image depend on a seed.
[[nodiscard]] Status generate_dfg_table(u32 size, u32 samples, Array<DfgEntry>& out) noexcept;

/// Sample the table with bilinear filtering, the way the shader will.
[[nodiscard]] DfgEntry sample_dfg(Span<const DfgEntry> table, u32 size, f32 n_dot_v,
                                  f32 perceptual_roughness) noexcept;

/// The edge of the built-in directional-albedo table. See `directional_albedo`.
inline constexpr u32 kDirectionalAlbedoSize = 16;

/// `Ess`, the single-scattering directional albedo — the fraction of the incoming energy a
/// single-scatter GGX lobe returns, which is `scale + bias` of the DFG table for a white `f0`.
///
/// WHY A SECOND, SMALLER TABLE RATHER THAN A FORMULA OR THE BIG ONE.
///
///   * `evaluate_direct` is the CPU reference for the shading model and must be callable with
///     nothing bound. Taking a `DfgEntry` parameter would push the table into every caller and make
///     the BRDF's energy conservation depend on which table the caller happened to bake.
///   * The published closed-form fits (Lazarov's, which Karis republished as `EnvBRDFApprox`) are
///     fitted against a DIFFERENT visibility term — UE4's Schlick-GGX with `k = α/2` — and are off
///     by up to 0.5 in `Ess` against the height-correlated Smith this engine uses. Adopting one
///     would mean the compensation corrected an energy loss the engine does not have.
///   * So the values below are baked from THIS engine's own `generate_dfg_table`, at sixteen by
///     sixteen with 16 384 samples, and `material_ibl` re-bakes a finer table at run time and holds
///     this one to it. A constant that drifts from what it approximates is caught by the suite
///     rather than by a screenshot.
///
/// Bilinear, clamped at the edges, in the same convention `sample_dfg` uses.
[[nodiscard]] f32 directional_albedo(f32 n_dot_v, f32 perceptual_roughness) noexcept;

/// Indirect specular, split-sum: `prefiltered · (f0·scale + f90·bias)`.
[[nodiscard]] Vec3 indirect_specular(Vec3 prefiltered_radiance, Vec3 f0, f32 f90,
                                     DfgEntry dfg) noexcept;

/// Which pre-filtered mip a roughness selects, given how many the environment has. Linear in
/// perceptual roughness, which is the convention the pre-filter bake must match — and it is a
/// function here rather than a line in two shaders for exactly that reason.
[[nodiscard]] f32 prefiltered_mip_for(f32 perceptual_roughness, u32 mip_count) noexcept;

// --- Octahedral environment maps ------------------------------------------------------------

/// "Environment maps SHALL be stored as octahedral maps rather than cubemaps, giving simpler
/// filtering, mipmapping, and array storage."
///
/// The mapping is the same one the vertex encoder uses (`render::octahedral_encode`), in [0, 1] UV
/// space rather than snorm.
[[nodiscard]] Vec2 octahedral_uv_from_direction(Vec3 direction) noexcept;
[[nodiscard]] Vec3 direction_from_octahedral_uv(Vec2 uv) noexcept;

/// The seam correction. "WHEN an octahedral map is filtered THEN border texels SHALL be replicated
/// so bilinear filtering across the octahedral seam is correct."
///
/// Given a texel coordinate that may be one outside the map, this returns the coordinate inside it
/// that holds the same direction — which is what a border replication pass copies from. Returning
/// the mapping rather than performing the copy keeps it testable without an image.
void octahedral_border_source(i32 x, i32 y, u32 size, u32& out_x, u32& out_y) noexcept;

// --- Irradiance ---------------------------------------------------------------------------------

/// Second-order spherical harmonics: nine coefficients per colour channel.
///
/// "Indirect diffuse SHALL use the environment's irradiance, stored as spherical harmonics (L2) or
/// a small irradiance map." L2 is 27 floats for a whole environment, which is why it is the one
/// worth having.
struct ShL2 {
    /// Zero-initialised, so a default-constructed set is a black environment rather than nine
    /// uninitialised colours — which would be an environment that looks like a shader bug.
    Vec3 coefficients[9] = {};

    /// Accumulate one radiance sample arriving from `direction`, weighted by its solid angle.
    void accumulate(Vec3 direction, Vec3 radiance, f32 solid_angle) noexcept;
    /// The irradiance leaving a surface with this normal, with the cosine-lobe convolution folded
    /// in — so a caller multiplies by albedo/π and nothing else.
    [[nodiscard]] Vec3 irradiance(Vec3 normal) const noexcept;
};

/// The nine basis functions at a direction. Public because a test checks the orthonormality that
/// every other property here rests on.
void sh_basis_l2(Vec3 direction, f32 out[9]) noexcept;

}  // namespace cy::rendering
