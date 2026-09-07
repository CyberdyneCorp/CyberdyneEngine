#pragma once
// Shadow bias, derived from three measurements and tuned by nobody. Task 8.2.
//
// `virtual-shadows` — "Bias derived, not tuned": bias SHALL be derived from the shadow texel's
// world-space footprint, the surface slope relative to the light, and the geometric error of the
// caster representation used.
//
// All three are numbers this renderer already has. The footprint is `shadow_texel_world_size()`.
// The slope is N·L, which shading computes anyway. The geometric error is the number
// `virtual-geometry` selects a cluster by, and including it is what makes bias correct when shadow
// rasterisation deliberately uses coarser geometry than the camera view — a coarse caster's surface
// is displaced from the fine one by up to its error, and a bias that ignored that produces acne
// that appears only at distance and is blamed on the cascade.
//
// AN OVERRIDE IS A DEFECT REPORT. "Artists SHALL be able to override the derived values, and an
// override SHALL be reportable, since a scene requiring many overrides indicates a defect in the
// derivation." `BiasOverrideLedger` is that count, and it exists so the derivation gets corrected
// rather than worked around one light at a time.

#include <cy/core/base/types.h>

namespace cy::rendering {

/// What the derivation reads. Every field is measured elsewhere in the frame.
struct BiasInputs {
    /// World edge length of one shadow texel where the receiver is. From
    /// `shadow_texel_world_size()`.
    f32 texel_world_size = 0.01F;
    /// N·L, clamped to [0,1]. At grazing incidence one texel spans a long way along the surface,
    /// which is where acne lives.
    f32 n_dot_l = 1.0F;
    /// The world-space geometric error of the caster representation the shadow pass rasterised.
    f32 geometric_error = 0.0F;
    /// True when the receiver-plane depth gradient is available. It lets the constant term shrink
    /// without detaching contact shadows, which is the trade the requirement names.
    bool receiver_plane_available = false;
};

/// The three terms a shadow lookup applies. Depth units are world units: the address space is
/// orthographic per clip level and projective per spot page, and the conversion belongs to the
/// lookup, not here.
struct DerivedBias {
    /// A flat offset covering quantisation. World units.
    f32 constant = 0.0F;
    /// Multiplies tan(acos(N·L)) at the lookup. World units per unit slope.
    f32 slope_scale = 0.0F;
    /// How far along the surface normal the receiver is moved before projecting. World units. The
    /// term that removes acne without the peter-panning a large constant produces.
    f32 normal_offset = 0.0F;
};

[[nodiscard]] DerivedBias derive_shadow_bias(const BiasInputs& inputs) noexcept;

/// Overrides, counted so that a scene full of them is visible. The ledger holds no storage per
/// light: the renderer already knows which lights carry an override, and what the requirement asks
/// for is the count and the worst offender, not a second copy of the data.
struct BiasOverrideLedger {
    u32 lights_total = 0;
    u32 lights_overridden = 0;
    /// The largest ratio between an override's constant term and the derived one. A number well
    /// above 1 says the derivation is under-biasing; well below says it is over-biasing.
    f32 worst_ratio = 1.0F;

    void record(const DerivedBias& derived, const DerivedBias* override_value) noexcept;

    /// True when enough lights carry overrides that the derivation should be corrected rather than
    /// the scene. A tenth of the lights is the threshold, and it is a stated judgement rather than
    /// a measured one.
    [[nodiscard]] bool derivation_suspect() const noexcept;
};

}  // namespace cy::rendering
