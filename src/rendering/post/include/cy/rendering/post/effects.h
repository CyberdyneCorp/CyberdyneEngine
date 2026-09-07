#pragma once
// The scene-referred effects' parameter derivations: ambient occlusion, volumetric fog, depth of
// field, motion blur and bloom. Task 8.4.
//
// `rendering-post-processing` — one requirement each. What is here is the arithmetic that decides
// what each effect does, which is the part that is wrong when an effect looks wrong: a circle of
// confusion that ignores the sensor, an AO that darkens direct light, a froxel distribution that
// puts half its slices in the last ten metres, a bloom threshold that is not energy conserving.
// The gathers and the blurs themselves are shaders and are not here.
//
// TWO OF THESE ARE DEFAULTS THAT ARE ALSO REQUIREMENTS:
//
//   * "AO SHALL modulate **indirect** lighting only; applying it to direct light SHALL be an
//     explicitly non-physical artistic option, off by default." `apply_ambient_occlusion()` takes
//     the two terms separately so that the direct one is visibly untouched.
//   * "Bloom SHALL be produced by a progressive downsample and upsample chain with a **soft
//     threshold** knee, physically-motivated by default… total image energy SHALL be approximately
//     preserved, redistributed rather than added."

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>

namespace cy::rendering {

// --- Ambient occlusion ---------------------------------------------------------------------------

struct AmbientOcclusionSettings {
    /// GTAO's horizon search radius, in world units.
    f32 radius = 1.5F;
    /// Raises the falloff to this power. Above one darkens contact, below one flattens it.
    f32 power = 1.0F;
    /// Fraction of the render resolution the term is computed at.
    f32 resolution_scale = 0.5F;
    /// The non-physical artistic option, off by default and named so that switching it on is a
    /// decision somebody made rather than a default nobody noticed.
    bool apply_to_direct = false;
    /// How much of the direct term is occluded when the option above is on.
    f32 direct_strength = 0.5F;
};

/// Combine direct and indirect lighting with a visibility term. Returns the lit result.
///
/// The two terms are separate parameters and not a single colour, which is what makes "AO does not
/// darken direct light" a property of the signature rather than a promise in a comment.
[[nodiscard]] Vec3 apply_ambient_occlusion(Vec3 direct, Vec3 indirect, f32 visibility,
                                           const AmbientOcclusionSettings& settings) noexcept;

/// Specular occlusion from the bent normal and the visibility term: how much of the reflection lobe
/// survives the occluded cone. "WHEN bent normals are available THEN indirect specular SHALL use
/// them for occlusion, avoiding reflections from occluded directions."
[[nodiscard]] f32 specular_occlusion(Vec3 bent_normal, Vec3 reflection_direction, f32 visibility,
                                     f32 roughness) noexcept;

// --- Volumetric fog ------------------------------------------------------------------------------

struct FroxelVolume {
    u32 width = 160;
    u32 height = 90;
    u32 depth = 64;
    f32 near_plane = 0.1F;
    f32 far_plane = 64.0F;
    /// The exponent of the depth distribution. 1 is linear; above 1 concentrates slices near the
    /// camera, which is where a froxel's world size is smallest and its contribution largest.
    f32 depth_exponent = 2.0F;
};

/// The view-space depth at the far edge of slice `index`. Exponential, so the last slices are the
/// thick ones and a 64-slice volume covers sixty metres without wasting resolution at the far end.
[[nodiscard]] f32 froxel_slice_depth(const FroxelVolume& volume, u32 index) noexcept;

/// Which slice a view-space depth falls in. The inverse of the above; asserted as such, because a
/// distribution and its inverse that disagree produce fog that swims as the camera moves.
[[nodiscard]] u32 froxel_slice_of(const FroxelVolume& volume, f32 view_depth) noexcept;

/// The Henyey-Greenstein phase function. `g` above zero scatters forward, which is what brightens
/// fog when the view looks toward a light.
[[nodiscard]] f32 henyey_greenstein(f32 g, f32 cos_theta) noexcept;

/// Front-to-back integration of one froxel: the scattering it adds and the transmittance it leaves.
struct ScatteringStep {
    Vec3 scattering{0.0F, 0.0F, 0.0F};
    f32 transmittance = 1.0F;
};

[[nodiscard]] ScatteringStep integrate_froxel(Vec3 in_scattering, f32 density, f32 thickness,
                                              Vec3 accumulated, f32 transmittance) noexcept;

// --- Depth of field ------------------------------------------------------------------------------

struct DepthOfFieldSettings {
    /// Millimetres.
    f32 focal_length_mm = 50.0F;
    /// f-number.
    f32 aperture = 2.8F;
    /// Metres.
    f32 focus_distance = 5.0F;
    /// Sensor height in millimetres. 24 is full frame.
    f32 sensor_height_mm = 24.0F;
    /// Multiplies the physical result. 1.0 is physical; the requirement allows an artistic override
    /// and this is it, kept as a scale so that the physical derivation is still what varies.
    f32 artistic_scale = 1.0F;
    /// Focus tracking speed, in fraction per second.
    f32 autofocus_speed = 4.0F;
};

/// Signed circle of confusion, as a fraction of the sensor height. **Negative in front of the focus
/// plane and positive behind it**, which is the sign the near/far split reads: near-field blur must
/// bleed OVER in-focus geometry, and a magnitude-only CoC cannot express which side a sample is on.
[[nodiscard]] f32 circle_of_confusion(const DepthOfFieldSettings& settings,
                                      f32 distance_metres) noexcept;

/// One frame of autofocus toward a target distance.
[[nodiscard]] f32 track_focus(f32 current_distance, f32 target_distance, f32 delta_seconds,
                              const DepthOfFieldSettings& settings) noexcept;

// --- Motion blur ---------------------------------------------------------------------------------

/// Blur length in pixels for a velocity in pixels per frame at a given shutter angle. At 180° the
/// length is half a frame of motion, which is the requirement's scenario stated as an equation.
[[nodiscard]] f32 motion_blur_length(f32 velocity_pixels, f32 shutter_degrees,
                                     f32 scale = 1.0F) noexcept;

/// The depth-aware gather weight for one sample: a background sample does not smear over a
/// foreground one, which is the second scenario.
[[nodiscard]] f32 motion_blur_weight(f32 centre_depth, f32 sample_depth, f32 sample_velocity,
                                     f32 distance_pixels) noexcept;

// --- Bloom ---------------------------------------------------------------------------------------

struct BloomSettings {
    /// Luminance above which a pixel contributes. In scene-referred units, which is meaningful only
    /// because bloom runs before exposure — see `chain.h`.
    f32 threshold = 1.0F;
    /// Width of the soft knee around the threshold, as a fraction of it. Zero is a hard cut and
    /// produces a visible boundary crawling across a gradient.
    f32 knee = 0.5F;
    /// How much of the scattered energy is redistributed. 0.04 is the physically motivated default:
    /// a real lens scatters a few percent.
    f32 intensity = 0.04F;
    u32 mip_count = 6;
    /// Horizontal stretch of the first mip. 1.0 is circular.
    f32 anamorphic = 1.0F;
};

/// The soft-thresholded contribution of a colour: zero below the knee, ramping to the full excess
/// above it.
[[nodiscard]] Vec3 bloom_prefilter(Vec3 colour, const BloomSettings& settings) noexcept;

/// The Karis average weight for a sample of this luminance: `1 / (1 + luma)`. It is what stops one
/// very bright pixel producing a flickering star, and it is a weight rather than a clamp because a
/// clamp also removes the energy.
[[nodiscard]] f32 karis_weight(f32 luminance) noexcept;

/// Composite bloom back over the scene, conserving energy: the scene is scaled down by exactly the
/// fraction the bloom adds, so total image energy is redistributed rather than increased.
[[nodiscard]] Vec3 bloom_composite(Vec3 scene, Vec3 bloom, const BloomSettings& settings) noexcept;

}  // namespace cy::rendering
