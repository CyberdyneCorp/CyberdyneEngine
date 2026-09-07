#pragma once
// Area light evaluation with linearly transformed cosines. M7 task 10.3.
//
// `rendering-lighting-and-shadows` — "Area light evaluation": "Area lights SHALL be evaluated with
// **linearly transformed cosines** (LTC), using precomputed lookup tables for the GGX BRDF, giving
// correct soft shadowing of the highlight and correct diffuse falloff without stochastic sampling."
//
// ================================================================================================
// WHAT AN LTC IS, IN ONE PARAGRAPH, BECAUSE EVERYTHING HERE FOLLOWS FROM IT
// ================================================================================================
//
// A clamped cosine lobe can be integrated over a spherical polygon in closed form. A GGX lobe
// cannot. Heitz et al. observed that a GGX lobe is well approximated by a linear transform of a
// cosine lobe: pick a 3x3 matrix `M` per (roughness, view angle), and the GGX integral over a
// polygon `P` equals the cosine integral over `M^-1 P`. So the shader transforms the light's
// corners and evaluates a closed form. No stochastic sampling, no noise, and the highlight takes
// the light's SHAPE rather than a sphere's, which is the requirement's own scenario.
//
// ================================================================================================
// THE TABLE IS FITTED AT STARTUP, AND WHAT THAT COSTS AND IS WORTH IS MEASURED
// ================================================================================================
//
// The published LTC tables are produced offline by an L-BFGS fit over a 64x64 grid and shipped as a
// binary blob. This implementation fits its own table in `LtcTable::build()` instead — a coordinate
// descent on the same four-parameter matrix, minimising the same L3 error against a numerically
// integrated GGX lobe. That is not a claim to have matched the published fit; it is a claim that a
// table whose provenance is thirty lines of arithmetic in this repository is worth more than one
// whose provenance is a URL.
//
// THREE THINGS ABOUT IT THAT ARE NOT OBVIOUS AND WERE EACH FOUND THE EXPENSIVE WAY:
//
//   * THE WALK RUNS FROM THE ROUGHEST ENTRY TO THE SMOOTHEST, warm-starting each from its
//     neighbour. The roughest, most normal-incidence entry is the one whose answer is known in
//     advance — a fully rough GGX lobe at normal incidence IS a clamped cosine, so the fit starts
//     from the identity already at its optimum. Starting from the smooth corner does not converge
//     at all: a near-mirror lobe is two degrees wide, the direction grid the shape is scored on
//     lands no samples inside it, every candidate scores identically, and the descent walks the
//     matrix to a determinant of 1e-8 that clips every light below the horizon.
//   * THE ENERGY IS IMPORTANCE SAMPLED AND THE SHAPE IS NOT. A uniform grid cannot resolve a
//     near-mirror lobe, and the directional albedo came out as 147% when it tried. Sampling the
//     NDF puts every sample inside the lobe, which is what an albedo integral needs; the shape fit
//     wants the opposite, a uniform set, so that the tail it is matching is represented at all.
//   * `a` AND `c` MOVE MULTIPLICATIVELY AND `b` AND `d` ADDITIVELY. The four parameters differ in
//     scale by two orders of magnitude across the table, and one step size for all four is either
//     useless at one end or catastrophic at the other.
//
// WHAT IT COSTS: about 430 ms of CPU at -O0, once per process, and well under a tenth of that in
// an optimised build.
//
// WHAT IT IS WORTH: `tests/test_reference.cpp` measures it against an integration of the same BRDF
// over the same rectangle — MEAN ABSOLUTE ERROR 2.7% OF THE SET'S PEAK over eight configurations,
// worst 16.4%. The one case that is relatively far off is a narrow lobe pointed away from the
// emitter, where the reference itself is four parts in a thousand of the brightest configuration —
// which is why that file reports the error as a fraction of the SET's peak and not as a ratio per
// configuration, and prints the ratio beside it rather than hiding it.
//
// The diffuse half has no fit in it and is exact: 0.002% against the same reference.
//
// ================================================================================================
// THE FALLBACK IS A REPRESENTATIVE POINT, AND THE APPROXIMATION IS WRITTEN DOWN
// ================================================================================================
//
// "WHEN the shading model does not support LTC (hair, cloth) THEN the area light SHALL be
// approximated by a representative point with a documented approximation."
//
// `representative_point()` is that, and `kRepresentativePointApproximation` is the documentation
// as a string a diagnostic can print — because a documented approximation that lives only in a
// comment is one nobody reads at the moment it matters, which is when a hair shader looks wrong.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>

namespace cy::rendering {

/// The area light shapes the engine evaluates. `rendering-lighting-and-shadows`' light types name
/// rect, disc, sphere and tube; every one of them reaches `integrate_cosine_polygon` as a quad —
/// `area_light_quad()` is the substitution — so the evaluator has one path.
enum class AreaLightShape : u8 {
    Rect = 0,
    Disc,
    /// A sphere light: the representative-point form is exact for the diffuse term and the LTC
    /// form uses the sphere's projected disc.
    Sphere,
    /// A capsule; two spheres and a cylinder, evaluated as the swept disc.
    Tube,
    Count,
};

[[nodiscard]] const char* area_light_shape_name(AreaLightShape shape) noexcept;

/// One area light, in the shading frame's own space: the surface point is the origin and `+Z` is
/// the surface normal. Putting the light in that frame on the CPU is what lets the evaluator be a
/// pure function of four vectors.
struct AreaLight {
    AreaLightShape shape = AreaLightShape::Rect;
    /// The emitter's centre, relative to the shading point.
    Vec3 center{0.0F, 0.0F, 1.0F};
    /// Half-extents along the emitter's own two tangent axes, in metres. For a sphere, `half_x` is
    /// the radius and `half_y` is ignored; for a tube, `half_x` is the radius and `half_y` the half
    /// length.
    f32 half_x = 0.5F;
    f32 half_y = 0.5F;
    /// The emitter's tangent frame. `tangent_x` and `tangent_y` span it; their cross product is the
    /// emitting normal.
    Vec3 tangent_x{1.0F, 0.0F, 0.0F};
    Vec3 tangent_y{0.0F, 1.0F, 0.0F};
    /// True when the emitter only emits into the hemisphere its normal faces, which is what a real
    /// panel does and what a "double sided" checkbox turns off.
    bool single_sided = true;
};

/// The four corners of the emitter as the evaluator sees it, in the shading frame. Exposed because
/// a caller that already has them (a rect light, or a disc already approximated) can skip the
/// construction, and because a test wants to state them directly.
struct AreaQuad {
    Vec3 corner[4] = {};
};

/// The quad an `AreaLight` presents to a shading point. A disc becomes the inscribed quad scaled by
/// `2/sqrt(pi)` so the AREAS match — the standard substitution, and the reason a disc light and a
/// rect light of the same area read as the same brightness rather than differing by 21%.
[[nodiscard]] AreaQuad area_light_quad(const AreaLight& light) noexcept;

/// The LTC table's dimensions. 24x24 rather than the published 64x64, and the reason is the fit
/// rather than the memory: the table is fitted at startup and the cost is quadratic in this number,
/// so it buys accuracy against a startup cost. 24 lands the mean error at 2.5% of a scene's peak
/// specular value (`tests/test_reference.cpp` measures it), and the fit's own error dominates the
/// interpolation error well before a larger table would help.
inline constexpr u32 kLtcTableSize = 24;

/// One entry: the inverse transform the evaluator applies to the light's corners, plus the two
/// scalar terms the split-sum needs.
struct LtcEntry {
    /// `M^-1`, the matrix that takes a direction in the GGX lobe's space to the cosine lobe's.
    Mat3 inverse_transform = Mat3::identity();
    /// The BRDF's total energy at this (roughness, view angle) — the "magnitude" term, which is
    /// what stops a rough surface losing energy at grazing angles.
    f32 magnitude = 1.0F;
    /// The Fresnel fit's second term, so that F0 and F90 can be applied without a second table.
    f32 fresnel = 0.0F;
};

/// The table. Built once and shared; 32 x 32 x 44 bytes is 45 KiB, which is why it is a value a
/// caller owns rather than a global.
class LtcTable {
public:
    LtcTable() noexcept = default;

    /// Fill the table by moment matching against the GGX BRDF. See the header's second section for
    /// exactly what is and is not fitted here.
    void build() noexcept;

    [[nodiscard]] bool built() const noexcept { return built_; }

    /// Bilinearly sampled at `roughness` in [0, 1] and `cos_theta` in [0, 1], both clamped. The
    /// parameterisation is `sqrt(1 - cos_theta)` against `sqrt(roughness)`, which is the published
    /// one and is why the table is usable at 32 entries: it spends its resolution where the lobe
    /// changes shape fastest.
    [[nodiscard]] LtcEntry sample(f32 roughness, f32 cos_theta) const noexcept;

private:
    LtcEntry entries_[kLtcTableSize * kLtcTableSize];
    bool built_ = false;
};

/// The closed-form integral of a clamped cosine over a spherical polygon, with the horizon
/// clipping. `count` is 3 or 4. This is the whole of what an LTC evaluation costs once the corners
/// are transformed, and it is separated out because it is exact and therefore testable against a
/// case whose answer is known — a quad subtending a hemisphere integrates to 1.
[[nodiscard]] f32 integrate_cosine_polygon(const Vec3* corners, u32 count) noexcept;

/// Evaluate an area light's contribution through the LTC.
///
/// `normal` and `view` are in the same space as the light's `center`; `roughness` is the perceptual
/// roughness (alpha = roughness^2). Returns the BRDF-weighted irradiance, unmultiplied by the
/// light's colour or intensity — the caller has those and multiplying here would put a unit
/// conversion in a place `units.h` does not own.
[[nodiscard]] f32 ltc_evaluate(const LtcTable& table, const AreaLight& light, Vec3 normal,
                               Vec3 view, f32 roughness) noexcept;

/// The diffuse term of the same light: the LTC with the identity transform, which is exactly the
/// clamped-cosine integral and needs no table at all. Separate because a shading model that has no
/// specular still has this one, and because it is the reference the specular case's energy is
/// sanity-checked against.
[[nodiscard]] f32 ltc_evaluate_diffuse(const AreaLight& light, Vec3 normal) noexcept;

/// The documented approximation, as a string, so a diagnostic prints it rather than a reader
/// hunting for a comment.
inline constexpr const char* kRepresentativePointApproximation =
    "the emitter is replaced by the point on it closest to the reflection ray, and its solid angle "
    "is folded into an effective roughness; the highlight keeps the light's SIZE and loses its "
    "SHAPE, so a long tube light reads as a round one and a rect light's corners are not visible "
    "in the highlight";

/// What the fallback produces.
struct RepresentativePoint {
    /// The point on the emitter, relative to the shading point.
    Vec3 position{0.0F, 0.0F, 1.0F};
    /// The roughness a punctual evaluation should use in place of the surface's own, widened by the
    /// emitter's solid angle. This is the half of the approximation that makes a large light give a
    /// broad highlight rather than a point one.
    f32 effective_roughness = 0.0F;
    /// The energy normalisation that goes with the widening, so the highlight does not brighten as
    /// it spreads.
    f32 energy_scale = 1.0F;
};

/// The fallback for a shading model with no LTC — hair and cloth, which the requirement names.
[[nodiscard]] RepresentativePoint representative_point(const AreaLight& light, Vec3 normal,
                                                       Vec3 view, f32 roughness) noexcept;

/// Which filtered level of a textured emitter to sample, given the roughness and the solid angle
/// the emitter subtends. "Textured area lights SHALL sample a filtered representation of the
/// emitter, selected by roughness."
///
/// `mip_count` is the emitter texture's chain length. A mirror samples level 0 and reads the
/// texture; a rough surface samples the top of the chain and reads the emitter's average colour,
/// which is what makes a textured area light cost the same as an untextured one on a rough surface.
[[nodiscard]] f32 emitter_filter_level(f32 roughness, f32 solid_angle_steradians,
                                       u32 mip_count) noexcept;

/// The solid angle an area light subtends from the shading point, in steradians. Used by the
/// filter-level selection and by the representative-point widening, so both read one number.
[[nodiscard]] f32 area_light_solid_angle(const AreaLight& light) noexcept;

}  // namespace cy::rendering
