// SPDX-License-Identifier: MIT
#pragma once
// GI volumes: per-region overrides of the illumination settings, with priority and blend distance.
//
// `rendering-global-illumination` — "GI volumes".
//
// ================================================================================================
// AN OVERRIDE, NEVER A REQUIREMENT
// ================================================================================================
//
// A volume overrides, inside its bounds, the parameters it declares — quality, probe density,
// update rate, indirect diffuse and specular intensity, tier preference and the far-field
// distance — and nothing else. With no volume placed, `GiVolumeSet::resolve` returns the global
// settings unchanged and every consumer takes the path it took before volumes existed: that is
// "a scene SHALL be correctly lit with none placed", and the suite checks it bit for bit.
//
// ================================================================================================
// HOW OVERLAPS RESOLVE: THE POST-PROCESS STACK'S RULES, APPLIED IN PRIORITY ORDER
// ================================================================================================
//
// Per parameter, as `rendering-post-processing`'s volumes are: a volume that sets only the update
// rate does not drag the probe density toward anything. The resolution starts from the global
// value and visits the contributing volumes from the LOWEST priority to the highest (ties in the
// order they were given), and each moves the value toward its own by its weight:
//
//     value = lerp(value, volume.value, weight)       weight = volume.weight * fade
//
// where `fade` is one inside the box and falls linearly to zero over `blend_distance` outside it.
// So a higher priority dominates — at full weight it replaces whatever is below it — and the
// effective weights of the global value and every contributor always sum to one, which is the
// post stack's normalisation. One deliberate difference from `blend_volume_weights`: the global
// settings are the base of the stack rather than a contributor a higher priority eliminates, so a
// camera crossing a volume's blend band interpolates from the global value instead of switching.
//
// The two discrete parameters cannot be interpolated. Quality is carried as the ray multiplier it
// stands for, which can; the tier preference switches to a volume's at weight one half.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>

namespace cy::rendering::gi {

/// The quality a volume asks for, as the probe ray multiplier it stands for.
enum class GiQuality : u8 {
    Low = 0,
    Medium,
    High,
    Epic,
    Count,
};

[[nodiscard]] const char* gi_quality_name(GiQuality quality) noexcept;
/// Low 0.5, Medium 1 (the global default), High 2, Epic 4.
[[nodiscard]] f32 gi_quality_ray_scale(GiQuality quality) noexcept;

/// Which world tier a region's rays prefer.
enum class GiTierPreference : u8 {
    /// The cheapest sufficient tier: screen first, then hardware where available, else software.
    Automatic = 0,
    /// Never the hardware tier, even where the device has it. A region whose triangles disagree
    /// with its distance field, or one an author wants identical on every device.
    SoftwareOnly,
    /// Skip the screen tier and trace the world directly. An interior whose screen-space answer
    /// leaks the outside through a window frame.
    WorldOnly,
    Count,
};

[[nodiscard]] const char* gi_tier_preference_name(GiTierPreference preference) noexcept;

/// The parameters a volume may override, one bit each in `GiVolume::sets`.
enum class GiVolumeParameter : u8 {
    Quality = 0,
    ProbeDensity,
    UpdateRate,
    DiffuseIntensity,
    SpecularIntensity,
    TierPreference,
    FarFieldDistance,
    Count,
};

[[nodiscard]] const char* gi_volume_parameter_name(GiVolumeParameter parameter) noexcept;

[[nodiscard]] constexpr u32 gi_volume_bit(GiVolumeParameter parameter) noexcept {
    return 1U << static_cast<u32>(parameter);
}

/// The settings a sample resolves to. The defaults are the global settings' neutral values; the
/// far-field distance comes from `ErrorTargets` and is passed to `resolve` instead.
struct GiRegionSettings {
    /// Multiplies the rays a probe gathers per update.
    f32 ray_scale = 1.0F;
    /// Multiplies the probe density: above one, probes are placed at full density whatever the
    /// geometry says; below one, only the sparse lattice is, even near surfaces.
    f32 probe_density = 1.0F;
    /// Multiplies how fast a probe ages in the scheduler, so a rate of two is updated about twice
    /// as often within the same budget.
    f32 update_rate = 1.0F;
    f32 diffuse_intensity = 1.0F;
    f32 specular_intensity = 1.0F;
    GiTierPreference tier = GiTierPreference::Automatic;
    /// Metres from the camera at which the far field begins.
    f32 far_field_begins_metres = 60.0F;
};

struct GiVolume {
    Aabb bounds{};
    /// Higher wins where volumes overlap.
    i32 priority = 0;
    /// Metres outside the bounds over which the volume fades in. Zero switches at the boundary.
    f32 blend_distance_metres = 1.0F;
    /// A master weight in [0, 1], so an author can dial a volume down without moving it.
    f32 weight = 1.0F;
    /// Which parameters this volume sets: `gi_volume_bit` of each.
    u32 sets = 0;
    GiQuality quality = GiQuality::Medium;
    f32 probe_density = 1.0F;
    f32 update_rate = 1.0F;
    f32 diffuse_intensity = 1.0F;
    f32 specular_intensity = 1.0F;
    GiTierPreference tier = GiTierPreference::Automatic;
    f32 far_field_begins_metres = 60.0F;
    /// The author's identity for the volume, for diagnostics.
    u64 id = 0;

    [[nodiscard]] bool sets_parameter(GiVolumeParameter parameter) const noexcept {
        return (sets & gi_volume_bit(parameter)) != 0U;
    }
};

/// Refuses a volume whose numbers cannot mean anything: an inverted box, a negative blend distance,
/// a weight outside [0, 1], a non-positive density, rate or far-field distance, a negative
/// intensity, or an enumerator out of range.
[[nodiscard]] Status validate_gi_volume(const GiVolume& volume) noexcept;

/// The placed volumes, ordered for resolution.
class GiVolumeSet {
public:
    GiVolumeSet() noexcept = default;

    /// Replace the set. Every volume is validated first; on a refusal the set is unchanged.
    [[nodiscard]] Status assign(Span<const GiVolume> volumes) noexcept;
    void clear() noexcept;

    [[nodiscard]] bool empty() const noexcept { return volumes_.empty(); }
    [[nodiscard]] usize size() const noexcept { return volumes_.size(); }
    /// In resolution order: ascending priority, ties in the order given.
    [[nodiscard]] Span<const GiVolume> volumes() const noexcept { return volumes_.span(); }
    /// Bumped by every `assign` and `clear`, so a consumer can tell the set changed.
    [[nodiscard]] u64 revision() const noexcept { return revision_; }

    /// The settings at `point`, starting from `global`. Returns `global` unchanged, bit for bit,
    /// where no volume contributes.
    [[nodiscard]] GiRegionSettings resolve(Vec3 point,
                                           const GiRegionSettings& global) const noexcept;

    /// The weight one volume contributes at `point`, before resolution: `weight * fade`.
    [[nodiscard]] static f32 contribution(const GiVolume& volume, Vec3 point) noexcept;

private:
    Array<GiVolume> volumes_;
    u64 revision_ = 0;
};

}  // namespace cy::rendering::gi
