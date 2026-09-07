#include <cy/rendering/post/volumes.h>

#include <cy/core/math/scalar.h>

namespace cy::rendering {

const char* post_parameter_group_name(PostParameterGroup group) noexcept {
    switch (group) {
        case PostParameterGroup::Exposure:
            return "Exposure";
        case PostParameterGroup::Grading:
            return "Grading";
        case PostParameterGroup::Fog:
            return "Fog";
        case PostParameterGroup::DepthOfField:
            return "DepthOfField";
        case PostParameterGroup::Bloom:
            return "Bloom";
        case PostParameterGroup::AmbientOcclusion:
            return "AmbientOcclusion";
        case PostParameterGroup::Count:
            break;
    }
    return "Unknown";
}

u32 blend_volume_weights(Span<const PostVolume> volumes, Span<const VolumeSample> samples,
                         PostParameterGroup group, f32* out, u32 out_capacity) noexcept {
    if (out == nullptr) {
        return 0;
    }
    const usize count = math::min(volumes.size(), samples.size());
    for (u32 index = 0; index < out_capacity; ++index) {
        out[index] = 0.0F;
    }

    // The highest priority among contributors decides who is in the blend at all: a lower-priority
    // volume overlapping a higher-priority one contributes nothing, which is what "the higher
    // priority SHALL dominate" means.
    i32 top = 0;
    bool any = false;
    for (usize index = 0; index < count; ++index) {
        if (!volumes[index].sets_group(group)) {
            continue;
        }
        const bool inside = volumes[index].unbounded ||
                            samples[index].signed_distance < volumes[index].blend_distance;
        if (!inside) {
            continue;
        }
        if (!any || volumes[index].priority > top) {
            top = volumes[index].priority;
            any = true;
        }
    }
    if (!any) {
        return 0;
    }

    f32 total = 0.0F;
    u32 contributors = 0;
    for (usize index = 0; index < count && index < out_capacity; ++index) {
        const PostVolume& volume = volumes[index];
        if (!volume.sets_group(group) || volume.priority != top) {
            continue;
        }
        f32 weight = math::max(volume.weight, 0.0F);
        if (!volume.unbounded) {
            const f32 distance = samples[index].signed_distance;
            if (distance >= volume.blend_distance) {
                continue;
            }
            // Fades in over the blend distance. A camera crossing into a volume interpolates rather
            // than switching, which is the requirement's first scenario.
            const f32 span = math::max(volume.blend_distance, 1e-4F);
            weight *= math::saturate(1.0F - (math::max(distance, 0.0F) / span));
        }
        if (weight <= 0.0F) {
            continue;
        }
        out[index] = weight;
        total += weight;
        ++contributors;
    }
    if (total <= 0.0F) {
        return 0;
    }
    // Normalised across contributors, so the result does not depend on how many volumes happen to
    // overlap.
    for (usize index = 0; index < count && index < out_capacity; ++index) {
        out[index] /= total;
    }
    return contributors;
}

f32 blend_parameter(Span<const f32> values, Span<const f32> weights, f32 fallback) noexcept {
    const usize count = math::min(values.size(), weights.size());
    f32 accumulated = 0.0F;
    f32 total = 0.0F;
    for (usize index = 0; index < count; ++index) {
        accumulated += values[index] * weights[index];
        total += weights[index];
    }
    return total > 1e-6F ? accumulated / total : fallback;
}

}  // namespace cy::rendering
