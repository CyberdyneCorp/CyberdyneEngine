#include <cy/rendering/gi/tracing.h>

#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>

namespace cy::rendering::gi {
namespace {

/// How close to the viewport border a screen-traced hit is, as a fraction, and the confidence that
/// falls out of it. Zero at the border and one a tenth of the way in.
[[nodiscard]] f32 edge_confidence(f32 x, f32 y, u32 width, u32 height) noexcept {
    const f32 border = 0.1F;
    const f32 u = x / static_cast<f32>(width);
    const f32 v = y / static_cast<f32>(height);
    const f32 nearest = std::min({u, 1.0F - u, v, 1.0F - v});
    return math::clamp(nearest / border, 0.0F, 1.0F);
}

/// A screen-space sample carries last frame's colour, so it can never be fully trusted even in the
/// middle of the frame with a perfect thickness match. This is that cap.
constexpr f32 kScreenConfidenceCap = 0.92F;

/// What a software hit is worth. A sphere trace that grazed a surface — small cone ratio — is a
/// hit whose position is uncertain, and a cache lookup that missed is worth much less again.
constexpr f32 kSoftwareBaseConfidence = 0.85F;

/// What a hardware hit is worth before the proxy penalty. Higher than software because the
/// intersection is exact; not one, because the surface it resolves through is still a cache.
constexpr f32 kHardwareBaseConfidence = 0.95F;

}  // namespace

RadianceSample ScreenTracer::trace(const ScreenView& view, Vec3 origin, Vec3 direction,
                                   f32 max_distance) noexcept {
    RadianceSample sample;
    sample.source = RadianceSource::ScreenTrace;
    if (view.width == 0 || view.height == 0 || view.depth.empty() || view.colour.empty()) {
        return sample;
    }

    const u32 steps = std::max(1U, view.max_steps);
    const f32 step = max_distance / static_cast<f32>(steps);
    for (u32 index = 1; index <= steps; ++index) {
        const f32 t = step * static_cast<f32>(index);
        const Vec3 point = origin + (direction * t);
        const Vec3 in_view = transform_point(view.world_to_view, point);
        const f32 ray_depth = -in_view.z;
        if (ray_depth <= 0.0F) {
            // Behind the eye. Nothing on the screen can answer.
            sample.confidence = 0.0F;
            return sample;
        }
        const Vec3 clip = project_point(view.world_to_clip, point);
        const f32 x = ((clip.x * 0.5F) + 0.5F) * static_cast<f32>(view.width);
        const f32 y = (0.5F - (clip.y * 0.5F)) * static_cast<f32>(view.height);
        if (x < 0.0F || y < 0.0F || x >= static_cast<f32>(view.width) ||
            y >= static_cast<f32>(view.height)) {
            // "WHEN a traced ray exits the viewport THEN confidence SHALL fall toward zero near the
            // edge and a higher tier or cached source SHALL take over smoothly."
            sample.confidence = 0.0F;
            return sample;
        }

        const auto pixel = (static_cast<usize>(y) * view.width) + static_cast<usize>(x);
        const f32 buffer_depth = view.depth[pixel];
        if (buffer_depth <= 0.0F) {
            continue;  // sky: the ray passes through it
        }
        const f32 behind = ray_depth - buffer_depth;
        if (behind <= 0.0F) {
            continue;  // still in front of the surface
        }
        if (behind > view.thickness_metres) {
            // The ray is far behind whatever is in the buffer. That surface's thickness is unknown,
            // so this is not a hit and it is not a miss either — the tier cannot see behind it.
            // Reporting a hit here is exactly "returning a plausible-looking wrong answer".
            sample.confidence = 0.0F;
            return sample;
        }

        sample.radiance = view.colour[pixel];
        sample.hit_distance = t;
        const f32 thickness_agreement = 1.0F - (behind / std::max(view.thickness_metres, 1.0e-4F));
        sample.confidence = math::clamp(edge_confidence(x, y, view.width, view.height) *
                                            thickness_agreement * kScreenConfidenceCap,
                                        0.0F, 1.0F);
        return sample;
    }

    // The ray stayed on screen and met nothing. A screen tracer cannot prove a miss — the surface
    // may be behind something the depth buffer shows — so this is a low confidence rather than a
    // confident sky answer, and the world tier is what settles it.
    sample.confidence = 0.2F;
    sample.hit_distance = -1.0F;
    return sample;
}

bool SoftwareTracer::trace(Vec3 origin, Vec3 direction, f32 max_distance,
                           SceneHit& hit) const noexcept {
    hit = SceneHit{};
    if (field_ == nullptr) {
        return false;
    }
    Ray ray;
    ray.origin = origin;
    ray.direction = normalized_or(direction, Vec3{0.0F, 0.0F, -1.0F});
    const SphereTraceHit traced = field_->sphere_trace(ray, max_distance);
    if (!traced.hit) {
        return false;
    }
    hit.hit = true;
    hit.t = traced.t;
    hit.position = traced.position;
    hit.normal = traced.normal;
    // A sphere trace that came close to a surface long before it hit one is a grazing ray, and its
    // hit point is the least certain thing the software tier produces. The closest approach is free
    // and it is measured against the field's own finest resolution: closer than four voxels and the
    // ray spent its march inside the cone a surface subtends at that resolution.
    const f32 grazing_scale = field_->settings().base_extent_metres /
                              static_cast<f32>(std::max(field_->settings().resolution, 1U)) * 4.0F;
    hit.confidence =
        kSoftwareBaseConfidence *
        math::clamp(traced.closest_approach_metres / std::max(grazing_scale, 1.0e-4F), 0.25F, 1.0F);
    return true;
}

bool SoftwareTracer::occluded(Vec3 from, Vec3 to) const noexcept {
    if (field_ == nullptr) {
        return false;
    }
    const Vec3 offset = to - from;
    const f32 span = length(offset);
    if (span <= 1.0e-4F) {
        return false;
    }
    Ray ray;
    ray.origin = from;
    ray.direction = offset / span;
    // Clear the surface the ray started on. See `DistanceField::sphere_trace`: without this a
    // shadow ray leaving a wall at a grazing angle reports the wall itself, and every surface in a
    // room lit from the side loses its direct term.
    const f32 bias = field_->voxel_size() * 2.0F;
    if (span <= bias) {
        return false;
    }
    const SphereTraceHit traced = field_->sphere_trace(ray, span * 0.999F, bias);
    return traced.hit;
}

RadianceSample SoftwareTracer::radiance(const SceneHit& hit, const SkyTerm& sky,
                                        Vec3 direction) const noexcept {
    RadianceSample sample;
    if (!hit.hit) {
        sample.source = RadianceSource::Sky;
        sample.radiance = sky.radiance(direction);
        sample.confidence = 1.0F;
        sample.hit_distance = -1.0F;
        return sample;
    }
    sample.source = RadianceSource::SoftwareTrace;
    sample.hit_distance = hit.t;
    Vec3 cached{0.0F, 0.0F, 0.0F};
    u32 age = 0;
    if (radiance_ != nullptr && radiance_->radiance_at(hit.position, hit.normal, cached, age)) {
        sample.radiance = cached;
        // "Stale cache is distrusted": the age the lookup reported is the second half of the
        // confidence, and the tier's own certainty is the first.
        const f32 staleness = math::clamp(static_cast<f32>(age) / 240.0F, 0.0F, 1.0F);
        sample.confidence = hit.confidence * (1.0F - (staleness * 0.6F));
        return sample;
    }
    // Nothing covers the hit. Black with a low confidence, not black with a high one: the resolve
    // must be able to weight this away rather than darken a surface with it.
    sample.confidence = hit.confidence * 0.2F;
    return sample;
}

bool HardwareTracer::trace(Vec3 origin, Vec3 direction, f32 max_distance,
                           SceneHit& hit) const noexcept {
    hit = SceneHit{};
    if (!available()) {
        return false;
    }
    rt::RayQuery query;
    query.ray.origin = origin;
    query.ray.direction = normalized_or(direction, Vec3{0.0F, 0.0F, -1.0F});
    query.t_max = max_distance;
    query.consumer = rt::Consumer::GlobalIllumination;
    const rt::RayHit answer = service_->trace(query);
    if (!answer.hit) {
        return false;
    }
    hit.hit = true;
    hit.t = answer.t;
    hit.position = answer.position;
    hit.normal = answer.normal;
    hit.declared_error_metres = answer.declared_error_metres;
    // The proxy penalty. A hit against virtual geometry's proxy is exact against the proxy and
    // approximate against what was rasterised, and the amount is the number the adapter declared.
    const f32 proxy_penalty = math::clamp(answer.declared_error_metres * 2.0F, 0.0F, 0.4F);
    hit.confidence = kHardwareBaseConfidence - proxy_penalty;
    return true;
}

bool HardwareTracer::occluded(Vec3 from, Vec3 to) const noexcept {
    if (!available()) {
        return false;
    }
    const Vec3 offset = to - from;
    const f32 span = length(offset);
    if (span <= 1.0e-4F) {
        return false;
    }
    Ray ray;
    ray.origin = from;
    ray.direction = offset / span;
    return service_->occluded(ray, span * 0.999F, rt::Consumer::Shadows);
}

RadianceSample HardwareTracer::radiance(const SceneHit& hit, const SkyTerm& sky,
                                        Vec3 direction) const noexcept {
    RadianceSample sample;
    if (!hit.hit) {
        sample.source = RadianceSource::Sky;
        sample.radiance = sky.radiance(direction);
        sample.confidence = 1.0F;
        sample.hit_distance = -1.0F;
        return sample;
    }
    sample.source = RadianceSource::HardwareTrace;
    sample.hit_distance = hit.t;
    Vec3 cached{0.0F, 0.0F, 0.0F};
    u32 age = 0;
    // "A hardware hit is still a cache lookup." The same lookup the software tier does, which is
    // what makes the two tiers agree about what a surface looks like even when they disagree about
    // where it is.
    if (radiance_ != nullptr && radiance_->radiance_at(hit.position, hit.normal, cached, age)) {
        sample.radiance = cached;
        const f32 staleness = math::clamp(static_cast<f32>(age) / 240.0F, 0.0F, 1.0F);
        sample.confidence = hit.confidence * (1.0F - (staleness * 0.6F));
        return sample;
    }
    sample.confidence = hit.confidence * 0.2F;
    return sample;
}

RadianceSource TieredTracer::selected_world_tier() const noexcept {
    if (hardware_ != nullptr && hardware_->available()) {
        return RadianceSource::HardwareTrace;
    }
    if (software_ != nullptr) {
        return RadianceSource::SoftwareTrace;
    }
    return RadianceSource::None;
}

RadianceSample TieredTracer::trace(Vec3 origin, Vec3 direction, f32 max_distance,
                                   TraceBudget& budget) noexcept {
    const Vec3 unit = normalized_or(direction, Vec3{0.0F, 0.0F, -1.0F});
    const f32 reach = std::min(max_distance, config_.max_distance_metres);

    RadianceSample screen;
    if (screen_ != nullptr) {
        diagnostics_.screen_rays += 1;
        screen = ScreenTracer::trace(*screen_, origin, unit, reach);
        if (screen.confidence >= config_.escalation_threshold) {
            // "WHEN a screen trace hits with high confidence THEN no world trace SHALL be issued."
            return screen;
        }
    }

    const RadianceSource tier = selected_world_tier();
    if (tier == RadianceSource::None) {
        return screen;
    }
    if (budget.world_rays == 0 || (screen_ != nullptr && budget.escalations == 0)) {
        diagnostics_.budget_refusals += 1;
        return screen;
    }
    budget.world_rays -= 1;
    if (screen_ != nullptr) {
        budget.escalations -= 1;
        diagnostics_.escalations += 1;
    }

    SceneHit hit;
    RadianceSample world;
    if (tier == RadianceSource::HardwareTrace) {
        diagnostics_.hardware_rays += 1;
        (void)hardware_->trace(origin, unit, reach, hit);
        world = hardware_->radiance(hit, config_.sky, unit);
    } else {
        diagnostics_.software_rays += 1;
        (void)software_->trace(origin, unit, reach, hit);
        world = software_->radiance(hit, config_.sky, unit);
    }
    if (world.source == RadianceSource::Sky) {
        diagnostics_.sky_answers += 1;
    } else if (world.confidence < 0.3F * kSoftwareBaseConfidence) {
        diagnostics_.cache_misses += 1;
    }

    if (screen_ == nullptr || screen.confidence <= 0.0F) {
        return world;
    }
    // Both answered. Blend by the screen's confidence rather than switching: a ray approaching the
    // screen edge fades into the world answer, and that is the whole of "blending, not switching".
    RadianceSample blended;
    blended.radiance = lerp(world.radiance, screen.radiance, screen.confidence);
    blended.confidence = std::max(screen.confidence, world.confidence);
    blended.source = screen.confidence >= world.confidence ? screen.source : world.source;
    blended.hit_distance =
        screen.confidence >= world.confidence ? screen.hit_distance : world.hit_distance;
    return blended;
}

bool WorldTracer::trace(Vec3 origin, Vec3 direction, f32 max_distance,
                        SceneHit& hit) const noexcept {
    if (hardware_ != nullptr && hardware_->available()) {
        return hardware_->trace(origin, direction, max_distance, hit);
    }
    if (software_ != nullptr) {
        return software_->trace(origin, direction, max_distance, hit);
    }
    hit = SceneHit{};
    return false;
}

bool WorldTracer::occluded(Vec3 from, Vec3 to) const noexcept {
    if (hardware_ != nullptr && hardware_->available()) {
        return hardware_->occluded(from, to);
    }
    if (software_ != nullptr) {
        return software_->occluded(from, to);
    }
    return false;
}

RadianceSource WorldTracer::tier() const noexcept {
    if (hardware_ != nullptr && hardware_->available()) {
        return RadianceSource::HardwareTrace;
    }
    if (software_ != nullptr) {
        return RadianceSource::SoftwareTrace;
    }
    return RadianceSource::None;
}

}  // namespace cy::rendering::gi
