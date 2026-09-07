#pragma once
// Lowering: closure sets to shading models, the program family, and the quality tiers. Task 6.3.
//
// `material-compiler` — "Lowering to shading models": "The compiler SHALL match a material's
// closure set against the engine's known shading models and lower to that model's evaluation path
// when it matches. A closure set with no matching model SHALL lower to a generic layered evaluator,
// whose higher cost SHALL be reported at cook time. A material whose closures match a known model
// SHALL cost the same as a material authored directly against that model."
//
// — and "Secondary material programs", whose table this file implements: `Primary`, `Secondary`,
// `FarField` and `Shadow`, derived automatically and overridable per material.
//
// ================================================================================================
// THREE FINDINGS FROM THE SPIKE THAT THIS FILE IS SHAPED BY (design.md §1.5)
// ================================================================================================
//
// 1. MATCHING IS OVER THE SET OF LEAF CLOSURE KINDS REACHABLE THROUGH THE COMBINATORS, not over the
//    root's op. `{diffuse, specular}` and `{diffuse, specular, emission}` are the standard model;
//    adding `coat` is clear coat; anything else takes the generic layered evaluator and the cook
//    report says so.
//
// 2. DERIVATION DROPS ONLY LEAF CLOSURES. A combinator is never dropped, because dropping one
//    deletes everything beneath it — see closure_algebra.h.
//
// 3. "A WRONG DERIVATION IS VISIBLE" IS A REACHABILITY QUESTION, NOT A NUMERIC ONE. The
//    specification asks the cook report to flag a derivation that changes a material's average
//    albedo. A numeric fold gives up the moment a runtime parameter is in the path — worn metal
//    multiplies albedo by `1 - metallic` and never folds. What DOES answer it for every material is
//    the SET OF TEXTURES REACHING A DIFFUSE OR SPECULAR COLOUR, compared between the primary and
//    the derived program: worn metal is `{base_color} | {base_color}` and is not flagged, and a
//    material whose microdetail `grime` also feeds albedo is `{base_color, grime} | {base_color}`
//    and is.
//
// And the fourth, which is structural rather than a heuristic: WHEN OPACITY FOLDS TO A CONSTANT 1
// THERE IS NO SHADOW PROGRAM AT ALL. `material-compiler`'s "Opaque materials SHALL produce no
// fragment work in the shadow program" is then a fact about the module — it has no roots — rather
// than an empty function somebody has to remember not to bind.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/material/emit.h>
#include <cy/rendering/material/ir.h>
#include <cy/servers/render/types.h>

namespace cy::rendering::material {

using render::ShadingModel;

/// The set of leaf closure kinds a module's surface reaches. A bit set rather than a list, because
/// matching a shading model is a comparison of sets and nothing else.
struct ClosureSet {
    u32 mask = 0;

    [[nodiscard]] bool has(Op leaf) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return mask == 0; }
    [[nodiscard]] u32 count() const noexcept;
    friend bool operator==(const ClosureSet& a, const ClosureSet& b) noexcept {
        return a.mask == b.mask;
    }
};

/// The closure set reachable from the surface root, through `ClosureScale`, `ClosureAdd` and
/// `ClosureLayer`.
[[nodiscard]] ClosureSet closure_set(const Module& module) noexcept;

/// What a closure set lowered to.
struct Lowered {
    ShadingModel model = ShadingModel::Lit;
    /// True when no known model matched. The material still renders; it costs more, and the cook
    /// report says so — "Generality SHALL be priced, not hidden".
    bool generic_evaluator = false;
    /// The multiple of the matched model's cost the generic evaluator is estimated to cost. Used
    /// by the cost report's "the cook report SHALL state the cost difference".
    f32 generic_cost_multiple = 1.0F;
};

[[nodiscard]] Lowered match_shading_model(const ClosureSet& closures) noexcept;

/// A renderer profile's restrictions. `material-compiler`: "WHEN a renderer profile does not
/// support the generic evaluator THEN cooking a material that requires it for that profile SHALL
/// fail with a diagnostic naming the closures responsible."
struct Profile {
    const char* name = "desktop";
    bool supports_generic_evaluator = true;
    /// Closures the profile cannot evaluate at all, as a `ClosureSet` mask.
    u32 unsupported_closures = 0;
    /// Whether the profile shades through a visibility buffer. Under a vertex-stage pipeline a
    /// material needs one variant per geometry source, and the count is reported.
    bool visibility_buffer = true;
};

[[nodiscard]] Profile desktop_profile() noexcept;
[[nodiscard]] Profile mobile_profile() noexcept;

/// How a program is derived from the primary module.
struct DerivationOptions {
    ProgramKind kind = ProgramKind::Primary;
    QualityTier tier = QualityTier::High;
};

/// Derive one program's module from the primary one.
///
/// The result is un-optimised: the caller runs `optimise` over it, which is what folds the averaged
/// constants a far-field derivation substitutes and what collapses an opacity that became one.
[[nodiscard]] Expected<Module, Error> derive_program(const Module& primary,
                                                     const DerivationOptions& options) noexcept;

/// The textures reaching a diffuse or specular colour input, sorted by name so two runs compare
/// equal. Finding 3 above.
[[nodiscard]] Status albedo_textures(const Module& module, Array<Name>& out) noexcept;

/// What changed between the primary program and a derived one.
struct DerivationDifference {
    u32 primary_albedo_textures = 0;
    u32 derived_albedo_textures = 0;
    /// A texture that reaches albedo in the primary and does not in the derived program. THE flag:
    /// "WHEN automatic derivation changes a material's average albedo or opacity coverage
    /// significantly THEN the cook report SHALL flag it, so an author can override the derivation."
    bool albedo_changed = false;
    /// The derived program's opacity is not the primary's.
    bool opacity_changed = false;
    /// The derived module has no roots: an opaque material's shadow program.
    bool empty_program = false;
    /// The first texture that stopped reaching albedo, so the diagnostic names it.
    Name responsible;
};

[[nodiscard]] Expected<DerivationDifference, Error> compare_derivation(
    const Module& primary, const Module& derived) noexcept;

// --- Quality tier selection --------------------------------------------------------------------

/// The thresholds tier selection is made against, and the hysteresis that keeps it from pulsing.
///
/// THE HYSTERESIS IS NOT OPTIONAL AND IT IS NOT SYMMETRIC. `material-compiler` requires that "WHEN
/// an instance crosses a tier boundary THEN the change SHALL be applied with hysteresis, so it does
/// not oscillate frame to frame", and design.md §2.7 measured what a margin-free test costs on the
/// other side of the engine: a control loop whose tighten test has no margin "loses a step to every
/// noise excursion and never gets it back ... it looks like a scene that mysteriously gets coarser
/// the longer you stand still". The two margins here straddle each threshold for that reason.
struct TierSelection {
    /// Below this weight an instance is at most `Medium`.
    f32 medium_threshold = 0.25F;
    /// Below this weight it is `Low`.
    f32 low_threshold = 0.08F;
    /// The fraction a weight must clear a threshold by to improve, and fall short by to worsen.
    f32 hysteresis = 0.15F;
};

/// Choose a tier for one instance this frame.
///
/// `weight` is the instance's projected screen fraction scaled by its declared importance and by
/// the renderer budget arbiter's current allocation — the three inputs the specification names.
/// Passing the current tier in is what makes the decision hysteretic rather than memoryless.
[[nodiscard]] QualityTier select_tier(QualityTier current, f32 weight,
                                      const TierSelection& thresholds) noexcept;

}  // namespace cy::rendering::material
