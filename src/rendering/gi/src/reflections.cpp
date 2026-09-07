#include <cy/rendering/gi/reflections.h>

#include <cy/core/math/geometry.h>
#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>

namespace cy::rendering::gi {
namespace {

/// The direction one texel of the reflection octahedron stands for.
[[nodiscard]] Vec3 texel_direction(u32 texel) noexcept {
    const u32 x = texel % kReflectionEdge;
    const u32 y = texel / kReflectionEdge;
    const f32 u = ((static_cast<f32>(x) + 0.5F) / static_cast<f32>(kReflectionEdge) * 2.0F) - 1.0F;
    const f32 v = ((static_cast<f32>(y) + 0.5F) / static_cast<f32>(kReflectionEdge) * 2.0F) - 1.0F;
    Vec3 direction{u, v, 1.0F - std::abs(u) - std::abs(v)};
    if (direction.z < 0.0F) {
        const f32 fx = (1.0F - std::abs(v)) * (u >= 0.0F ? 1.0F : -1.0F);
        const f32 fy = (1.0F - std::abs(u)) * (v >= 0.0F ? 1.0F : -1.0F);
        direction.x = fx;
        direction.y = fy;
    }
    return normalized_or(direction, Vec3{0.0F, 0.0F, 1.0F});
}

/// The cosine power each roughness level is filtered with. Level 0 is nearly a mirror, level 2 is
/// nearly a hemisphere, and the middle is where most surfaces live.
[[nodiscard]] f32 level_sharpness(u32 level) noexcept {
    constexpr f32 kSharpness[kReflectionLevels] = {64.0F, 8.0F, 1.0F};
    return kSharpness[std::min(level, kReflectionLevels - 1)];
}

/// The level a roughness reads at, as a continuous coordinate so a sample lerps between two.
[[nodiscard]] f32 level_of(f32 roughness) noexcept {
    return math::clamp(std::sqrt(math::clamp(roughness, 0.0F, 1.0F)), 0.0F, 1.0F) *
           static_cast<f32>(kReflectionLevels - 1);
}

[[nodiscard]] Vec3 fetch(const f32* level_data, Vec3 direction) noexcept {
    // Nearest texel by direction. Four by four is coarse enough that a bilinear fetch over the
    // octahedral seam would cost more in seam handling than it buys in smoothness, and the levels
    // are already filtered — which is where the smoothness comes from.
    u32 best = 0;
    f32 best_alignment = -2.0F;
    for (u32 texel = 0; texel < kReflectionTexels; ++texel) {
        const f32 alignment = dot(texel_direction(texel), direction);
        if (alignment > best_alignment) {
            best_alignment = alignment;
            best = texel;
        }
    }
    return Vec3{level_data[(best * 3) + 0], level_data[(best * 3) + 1], level_data[(best * 3) + 2]};
}

}  // namespace

const char* reflection_strategy_name(ReflectionStrategy strategy) noexcept {
    switch (strategy) {
        case ReflectionStrategy::DedicatedRays:
            return "DedicatedRays";
        case ReflectionStrategy::SparseRaysAndCache:
            return "SparseRaysAndCache";
        case ReflectionStrategy::CacheOnly:
            return "CacheOnly";
        case ReflectionStrategy::Count:
            break;
    }
    return "Unknown";
}

ReflectionStrategy reflection_strategy(f32 roughness,
                                       const ReflectionThresholds& thresholds) noexcept {
    if (roughness <= thresholds.mirror_max_roughness) {
        return ReflectionStrategy::DedicatedRays;
    }
    if (roughness >= thresholds.cache_min_roughness) {
        return ReflectionStrategy::CacheOnly;
    }
    return ReflectionStrategy::SparseRaysAndCache;
}

u32 reflection_ray_count(f32 roughness, const ReflectionThresholds& thresholds) noexcept {
    switch (reflection_strategy(roughness, thresholds)) {
        case ReflectionStrategy::DedicatedRays:
            return std::max(1U, thresholds.max_rays);
        case ReflectionStrategy::SparseRaysAndCache: {
            const f32 span =
                std::max(thresholds.cache_min_roughness - thresholds.mirror_max_roughness, 1.0e-4F);
            const f32 t =
                math::clamp((roughness - thresholds.mirror_max_roughness) / span, 0.0F, 1.0F);
            const f32 rays =
                math::lerp(static_cast<f32>(std::max(1U, thresholds.max_rays)), 1.0F, t);
            return std::max<u32>(1U, static_cast<u32>(std::lround(rays)));
        }
        case ReflectionStrategy::CacheOnly:
        case ReflectionStrategy::Count:
            break;
    }
    // "WHEN a surface is very rough THEN its reflection SHALL be taken from the radiance cache
    // without dedicated rays." Zero is the requirement, not an optimisation.
    return 0;
}

Vec3 reflection_lobe_direction(Vec3 reflection, Vec3 normal, f32 roughness, u32 index, u32 count,
                               u32 rotation) noexcept {
    const Vec3 axis = normalized_or(reflection, normal);
    if (count <= 1 || roughness <= 0.0F) {
        return axis;
    }
    constexpr f32 kGolden = 2.39996323F;
    const f32 offset = (static_cast<f32>(index) + 0.5F) / static_cast<f32>(count);
    // The cone's half-angle grows with roughness. This is the whole difference between a diffuse
    // gather and a specular one: the same tracer, a different distribution.
    const f32 spread = math::clamp(roughness, 0.0F, 1.0F) * 0.7F;
    const f32 radius = std::sqrt(offset) * spread;
    const f32 angle =
        (kGolden * static_cast<f32>(index)) + (static_cast<f32>(rotation) * 0.61803399F);
    Vec3 tangent = std::abs(axis.y) < 0.99F ? cross(Vec3{0.0F, 1.0F, 0.0F}, axis)
                                            : cross(Vec3{1.0F, 0.0F, 0.0F}, axis);
    tangent = normalized_or(tangent, Vec3{1.0F, 0.0F, 0.0F});
    const Vec3 bitangent = cross(axis, tangent);
    const Vec3 direction =
        axis + (tangent * (radius * std::cos(angle))) + (bitangent * (radius * std::sin(angle)));
    const Vec3 unit = normalized_or(direction, axis);
    // A lobe sample that went below the surface is not a reflection. Flipping it back is cheaper
    // and less biased than rejecting it, which would silently reduce the sample count.
    return dot(unit, normal) < 0.0F
               ? normalized_or(unit - (normal * (2.0F * dot(unit, normal))), axis)
               : unit;
}

ReflectionProbeSet::ReflectionProbeSet() noexcept = default;

f32* ReflectionProbeSet::levels(u32 handle) noexcept {
    return payloads_.data() + (static_cast<usize>(handle) * kReflectionFloats);
}

const f32* ReflectionProbeSet::levels(u32 handle) const noexcept {
    return payloads_.data() + (static_cast<usize>(handle) * kReflectionFloats);
}

Expected<u32, Error> ReflectionProbeSet::add(const ReflectionProbe& probe) noexcept {
    u32 handle = ~0U;
    if (!free_probes_.empty()) {
        handle = free_probes_.back();
        free_probes_.pop_back();
    } else {
        ReflectionProbe fresh;
        if (Status pushed = probes_.push_back(fresh); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status sized = payloads_.resize(probes_.size() * kReflectionFloats); !sized) {
            return make_unexpected(sized.error());
        }
        handle = static_cast<u32>(probes_.size() - 1);
    }

    probes_[handle] = probe;
    probes_[handle].live = true;
    probes_[handle].valid = false;
    probes_[handle].captured_texels = 0;
    f32* data = levels(handle);
    for (u32 index = 0; index < kReflectionFloats; ++index) {
        data[index] = 0.0F;
    }
    live_probes_ += 1;
    account();
    return handle;
}

void ReflectionProbeSet::remove(u32 handle) noexcept {
    if (handle >= probes_.size() || !probes_[handle].live) {
        return;
    }
    probes_[handle].live = false;
    probes_[handle].valid = false;
    (void)free_probes_.push_back(handle);
    live_probes_ -= 1;
    account();
}

void ReflectionProbeSet::request_capture(u32 handle) noexcept {
    if (handle < probes_.size() && probes_[handle].live) {
        probes_[handle].captured_texels = 0;
    }
}

u32 ReflectionProbeSet::invalidate(const Aabb& region) noexcept {
    u32 count = 0;
    for (ReflectionProbe& probe : probes_) {
        if (!probe.live) {
            continue;
        }
        const bool overlaps =
            probe.is_box ? probe.box.intersects(region)
                         : region.intersects(Aabb::from_center_extents(
                               probe.position, Vec3{probe.radius, probe.radius, probe.radius}));
        if (overlaps) {
            probe.captured_texels = 0;
            count += 1;
        }
    }
    return count;
}

void ReflectionProbeSet::filter_levels(u32 handle) noexcept {
    f32* data = levels(handle);
    // Level 0 holds the captured radiance. Levels 1 and 2 are cosine-power convolutions of it, so
    // a rough surface reads a wide lobe without a second capture.
    for (u32 level = 1; level < kReflectionLevels; ++level) {
        const f32 sharpness = level_sharpness(level);
        f32* destination = data + (static_cast<usize>(level) * kReflectionTexels * 3);
        for (u32 texel = 0; texel < kReflectionTexels; ++texel) {
            const Vec3 direction = texel_direction(texel);
            Vec3 total{0.0F, 0.0F, 0.0F};
            f32 weight_total = 0.0F;
            for (u32 source = 0; source < kReflectionTexels; ++source) {
                const f32 alignment = std::max(0.0F, dot(texel_direction(source), direction));
                const f32 weight = std::pow(alignment, sharpness);
                if (weight <= 0.0F) {
                    continue;
                }
                total = total + (Vec3{data[(source * 3) + 0], data[(source * 3) + 1],
                                      data[(source * 3) + 2]} *
                                 weight);
                weight_total += weight;
            }
            const Vec3 filtered =
                weight_total > 0.0F ? total / weight_total : Vec3{0.0F, 0.0F, 0.0F};
            destination[(texel * 3) + 0] = filtered.x;
            destination[(texel * 3) + 1] = filtered.y;
            destination[(texel * 3) + 2] = filtered.z;
        }
    }
}

/// The radiance one texel of a probe sees. A trace and a surface-cache lookup, which is the same
/// pair the probe gather and the bake use — a reflection probe is captured through the illumination
/// infrastructure and not through a second render of the scene.
Vec3 ReflectionProbeSet::capture_texel(const ReflectionProbe& probe, Vec3 direction,
                                       const ReflectionCaptureContext& context,
                                       ReflectionCaptureReport& report) noexcept {
    // An interior probe excludes the sky, so a room does not reflect a sky it cannot see.
    Vec3 radiance = probe.interior ? Vec3{0.0F, 0.0F, 0.0F} : context.sky.radiance(direction);
    if (context.tracer == nullptr) {
        return radiance;
    }

    SceneHit hit;
    report.rays += 1;
    if (!context.tracer->trace(probe.position, direction, context.max_distance_metres, hit)) {
        return radiance;
    }
    Vec3 cached{0.0F, 0.0F, 0.0F};
    u32 age = 0;
    if (context.radiance != nullptr &&
        context.radiance->radiance_at(hit.position, hit.normal, cached, age)) {
        return cached;
    }
    // Nothing in the surface cache covers the hit. Black is the honest answer.
    return Vec3{0.0F, 0.0F, 0.0F};
}

/// Whether this probe wants texels captured this call, resetting it when a realtime probe has gone
/// stale. A probe half-captured shows its previous contents, not half a room.
bool ReflectionProbeSet::wants_capture(ReflectionProbe& probe,
                                       const ReflectionCaptureContext& context) noexcept {
    if (probe.captured_texels < kReflectionTexels) {
        return true;
    }
    const bool stale = probe.capture_mode == ProbeCaptureMode::Realtime &&
                       context.frame >= probe.last_capture_frame + context.realtime_interval_frames;
    if (!stale) {
        return false;
    }
    probe.captured_texels = 0;
    return true;
}

ReflectionCaptureReport ReflectionProbeSet::capture(
    const ReflectionCaptureContext& context) noexcept {
    ReflectionCaptureReport report;
    u32 remaining = context.texel_budget;

    for (u32 handle = 0; handle < probes_.size() && remaining > 0; ++handle) {
        ReflectionProbe& probe = probes_[handle];
        if (!probe.live || !wants_capture(probe, context)) {
            continue;
        }

        f32* data = levels(handle);
        report.probes_touched += 1;
        while (probe.captured_texels < kReflectionTexels && remaining > 0) {
            const u32 texel = probe.captured_texels;
            const Vec3 radiance = capture_texel(probe, texel_direction(texel), context, report);
            data[(texel * 3) + 0] = radiance.x;
            data[(texel * 3) + 1] = radiance.y;
            data[(texel * 3) + 2] = radiance.z;
            probe.captured_texels += 1;
            report.texels_captured += 1;
            remaining -= 1;
        }

        if (probe.captured_texels >= kReflectionTexels) {
            filter_levels(handle);
            probe.valid = true;
            probe.last_capture_frame = context.frame;
            report.probes_completed += 1;
        }
    }

    diagnostics_.last_capture = report;
    account();
    return report;
}

f32 ReflectionProbeSet::influence(const ReflectionProbe& probe, Vec3 position) noexcept {
    const f32 blend = std::max(probe.blend_distance, 1.0e-3F);
    if (probe.is_box) {
        if (!probe.box.contains(position)) {
            return 0.0F;
        }
        const Vec3 lower = position - probe.box.min;
        const Vec3 upper = probe.box.max - position;
        const f32 nearest = std::min({lower.x, lower.y, lower.z, upper.x, upper.y, upper.z});
        return math::clamp(nearest / blend, 0.0F, 1.0F) * probe.importance;
    }
    const f32 span = cy::distance(position, probe.position);
    if (span > probe.radius) {
        return 0.0F;
    }
    return math::clamp((probe.radius - span) / blend, 0.0F, 1.0F) * probe.importance;
}

u32 ReflectionProbeSet::assign(Vec3 position, Span<u32> out_handles,
                               Span<f32> out_weights) const noexcept {
    const usize capacity = std::min(out_handles.size(), out_weights.size());
    u32 written = 0;
    for (u32 handle = 0; handle < probes_.size(); ++handle) {
        const ReflectionProbe& probe = probes_[handle];
        if (!probe.live || !probe.valid) {
            continue;
        }
        const f32 weight = influence(probe, position);
        if (weight <= 0.0F) {
            continue;
        }
        // Insertion sort by descending weight, so a caller taking the first N gets the N that
        // matter. The list is short by construction: probe volumes that all overlap one point are
        // an authoring problem the importance value exists to resolve.
        u32 slot = written;
        while (slot > 0 && out_weights[slot - 1] < weight) {
            if (slot < capacity) {
                out_weights[slot] = out_weights[slot - 1];
                out_handles[slot] = out_handles[slot - 1];
            }
            slot -= 1;
        }
        if (slot < capacity) {
            out_weights[slot] = weight;
            out_handles[slot] = handle;
            if (written < capacity) {
                written += 1;
            }
        }
    }
    return written;
}

RadianceSample ReflectionProbeSet::sample(Vec3 position, Vec3 reflection,
                                          f32 roughness) const noexcept {
    diagnostics_.samples += 1;
    RadianceSample answer;
    answer.source = RadianceSource::ReflectionProbe;

    const f32 level = level_of(roughness);
    const auto level0 = static_cast<u32>(level);
    const u32 level1 = std::min(level0 + 1, kReflectionLevels - 1);
    const f32 blend = level - static_cast<f32>(level0);

    Vec3 total{0.0F, 0.0F, 0.0F};
    f32 total_weight = 0.0F;
    for (u32 handle = 0; handle < probes_.size(); ++handle) {
        const ReflectionProbe& probe = probes_[handle];
        if (!probe.live || !probe.valid) {
            continue;
        }
        const f32 weight = influence(probe, position);
        if (weight <= 0.0F) {
            continue;
        }

        Vec3 direction = normalized_or(reflection, Vec3{0.0F, 1.0F, 0.0F});
        if (probe.box_projection && probe.is_box) {
            // Intersect the reflection ray with the probe's box and re-aim at the hit point, so a
            // wall reflects at the right place instead of sliding with the camera.
            Ray ray;
            ray.origin = position;
            ray.direction = direction;
            f32 enter = 0.0F;
            f32 exit = 0.0F;
            if (geom::ray_aabb(ray, probe.box, 1.0e5F, enter, exit) && exit > 0.0F) {
                const Vec3 hit = ray.at(exit);
                direction = normalized_or(hit - probe.position, direction);
            }
        }

        const f32* data = levels(handle);
        const Vec3 sharp =
            fetch(data + (static_cast<usize>(level0) * kReflectionTexels * 3), direction);
        const Vec3 wide =
            fetch(data + (static_cast<usize>(level1) * kReflectionTexels * 3), direction);
        total = total + (lerp(sharp, wide, blend) * weight);
        total_weight += weight;
    }

    if (total_weight <= 0.0F) {
        diagnostics_.sample_misses += 1;
        answer.confidence = 0.0F;
        return answer;
    }
    answer.radiance = total / total_weight;
    // A probe is a captured approximation of a place, not of a point: it is trusted, but never as
    // much as a traced ray, so the resolve prefers a tier that answered and falls back smoothly.
    answer.confidence =
        math::clamp((0.6F * math::clamp(total_weight, 0.0F, 1.0F)) + 0.1F, 0.0F, 1.0F);
    return answer;
}

Status ReflectionProbeSet::seed(u32 handle, Span<const Vec3> directions,
                                Span<const Vec3> radiance) noexcept {
    if (handle >= probes_.size() || !probes_[handle].live) {
        return fail(ErrorCode::NotFound, "ReflectionProbeSet::seed: no such probe");
    }
    if (directions.size() != radiance.size() || directions.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "ReflectionProbeSet::seed: one radiance value per direction is required");
    }
    f32* data = levels(handle);
    for (u32 texel = 0; texel < kReflectionTexels; ++texel) {
        const Vec3 direction = texel_direction(texel);
        Vec3 total{0.0F, 0.0F, 0.0F};
        f32 weight_total = 0.0F;
        for (usize index = 0; index < directions.size(); ++index) {
            const f32 alignment =
                std::max(0.0F, dot(normalized_or(directions[index], direction), direction));
            const f32 weight = std::pow(alignment, 32.0F);
            if (weight <= 0.0F) {
                continue;
            }
            total = total + (radiance[index] * weight);
            weight_total += weight;
        }
        const Vec3 value = weight_total > 0.0F ? total / weight_total : Vec3{0.0F, 0.0F, 0.0F};
        data[(texel * 3) + 0] = value.x;
        data[(texel * 3) + 1] = value.y;
        data[(texel * 3) + 2] = value.z;
    }
    filter_levels(handle);
    probes_[handle].captured_texels = kReflectionTexels;
    probes_[handle].valid = true;
    account();
    return ok();
}

void ReflectionProbeSet::account() noexcept {
    u32 valid = 0;
    for (const ReflectionProbe& probe : probes_) {
        if (probe.live && probe.valid) {
            valid += 1;
        }
    }
    diagnostics_.probe_count = live_probes_;
    diagnostics_.valid_probes = valid;
    diagnostics_.bytes = (static_cast<u64>(probes_.size()) * sizeof(ReflectionProbe)) +
                         (static_cast<u64>(payloads_.size()) * sizeof(f32));
}

}  // namespace cy::rendering::gi
