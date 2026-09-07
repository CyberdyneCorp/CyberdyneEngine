#include <cy/rendering/shadows/bias.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering {

DerivedBias derive_shadow_bias(const BiasInputs& inputs) noexcept {
    DerivedBias bias;
    const f32 texel = math::max(inputs.texel_world_size, 0.0F);
    const f32 n_dot_l = math::clamp(inputs.n_dot_l, 0.01F, 1.0F);
    const f32 error = math::max(inputs.geometric_error, 0.0F);

    // The constant term covers depth quantisation and is proportional to the texel: a coarse clip
    // level needs more of it, and that is why it is derived rather than authored per light.
    // Receiver-plane depth lets it shrink, which is the trade the requirement names — a smaller
    // constant is what keeps contact shadows attached.
    const f32 quantisation = inputs.receiver_plane_available ? 0.25F : 1.0F;
    bias.constant = texel * quantisation;

    // The slope term is the depth a texel spans across the surface at this incidence. tan(theta)
    // from N·L directly: sqrt(1 - c²)/c, clamped because a surface exactly edge-on to the light is
    // not shadowed by anything a bias can fix.
    const f32 tangent =
        math::min(std::sqrt(math::max(1.0F - (n_dot_l * n_dot_l), 0.0F)) / n_dot_l, 8.0F);
    bias.slope_scale = texel * tangent;

    // The geometric error term is the one a conventional shadow map has no reason to have. Shadow
    // rasterisation deliberately selects coarser geometry than the camera view, so the caster's
    // surface can sit up to its declared error away from the one being shaded; a bias that ignored
    // that produces acne that appears only at distance and gets blamed on the cascade split.
    bias.normal_offset = (texel * 0.5F) + error;
    return bias;
}

void BiasOverrideLedger::record(const DerivedBias& derived,
                                const DerivedBias* override_value) noexcept {
    ++lights_total;
    if (override_value == nullptr) {
        return;
    }
    ++lights_overridden;
    const f32 reference = math::max(derived.constant, 1e-6F);
    const f32 ratio = override_value->constant / reference;
    if (std::fabs(ratio - 1.0F) > std::fabs(worst_ratio - 1.0F)) {
        worst_ratio = ratio;
    }
}

bool BiasOverrideLedger::derivation_suspect() const noexcept {
    if (lights_total == 0) {
        return false;
    }
    return lights_overridden * 10U > lights_total;
}

}  // namespace cy::rendering
