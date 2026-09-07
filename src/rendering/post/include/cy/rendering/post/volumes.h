#pragma once
// Post-process volumes: a priority stack, blended per parameter by weight. Task 8.4.
//
// `rendering-post-processing` — "Post-process configuration and volumes": a stack with priorities,
// blend distances and an unbounded global volume, "blended per-parameter by weight — so a camera
// entering a volume smoothly transitions its grading, exposure, and fog".
//
// PER PARAMETER, NOT PER VOLUME. The distinction is the whole design: a volume that only overrides
// exposure must not also drag grading toward its defaults, so a volume declares WHICH parameters it
// sets and contributes weight only to those. Otherwise every volume in a level has to restate every
// parameter, and adding a parameter to the engine changes every volume in every project.
//
// THE WEIGHTS ARE NORMALISED ACROSS CONTRIBUTORS. "WHEN two volumes overlap THEN the higher
// priority SHALL dominate, with weights normalised across contributors." Normalising is what makes
// the result independent of how many volumes happen to overlap, and it is why an unbounded global
// volume with weight one does not halve every interior volume's effect.

#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::rendering {

/// Which parameter groups a volume sets. A group rather than a field per parameter: the blend is
/// per parameter, and the declaration is per group, because "this volume sets the grading" is what
/// an author means and enumerating forty fields is not.
enum class PostParameterGroup : u8 {
    Exposure = 0,
    Grading,
    Fog,
    DepthOfField,
    Bloom,
    AmbientOcclusion,
    Count,
};

[[nodiscard]] const char* post_parameter_group_name(PostParameterGroup group) noexcept;

inline constexpr u32 kPostParameterGroupCount = static_cast<u32>(PostParameterGroup::Count);

struct PostVolume {
    /// Higher wins where volumes overlap.
    i32 priority = 0;
    /// True for the level's global volume, which is always a contributor at full weight.
    bool unbounded = false;
    /// Distance over which the volume fades in, in world units. Zero switches rather than blends,
    /// which is occasionally what a trigger volume wants and is never what a room wants.
    f32 blend_distance = 1.0F;
    /// A per-volume master weight, so an author can dial one down without moving its geometry.
    f32 weight = 1.0F;
    /// Which groups this volume sets. Groups it does not set contribute nothing to them.
    bool sets[kPostParameterGroupCount] = {};

    [[nodiscard]] bool sets_group(PostParameterGroup group) const noexcept {
        return sets[static_cast<usize>(group)];
    }
};

/// Where the camera is relative to one volume.
struct VolumeSample {
    /// Signed distance from the volume's boundary: negative inside, positive outside. An unbounded
    /// volume's is ignored.
    f32 signed_distance = 0.0F;
};

/// The normalised weight each volume contributes to one parameter group. `out` receives one weight
/// per volume, in the order they were given, summing to one when any volume contributes and to zero
/// when none does.
///
/// Returns the number of contributing volumes.
[[nodiscard]] u32 blend_volume_weights(Span<const PostVolume> volumes,
                                       Span<const VolumeSample> samples, PostParameterGroup group,
                                       f32* out, u32 out_capacity) noexcept;

/// Blend one scalar parameter through those weights. The caller supplies the per-volume values,
/// which is what keeps this module free of every parameter type in the chain.
[[nodiscard]] f32 blend_parameter(Span<const f32> values, Span<const f32> weights,
                                  f32 fallback) noexcept;

}  // namespace cy::rendering
