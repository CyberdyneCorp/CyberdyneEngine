#include <cy/rendering/gi/system.h>

#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>

namespace cy::rendering::gi {
namespace {

/// The number of samples `indirect_diffuse` combines. One per source that can answer a diffuse
/// query: the two baked ones, the radiance cache, and the sky.
constexpr usize kDiffuseSampleSlots = 4;

}  // namespace

IlluminationSystem::IlluminationSystem() noexcept = default;

Status IlluminationSystem::configure(const IlluminationSettings& settings) noexcept {
    settings_ = settings;
    scene_.set_error_targets(settings.error_targets);
    if (Status configured = field_.configure(settings.field); !configured) {
        return configured;
    }
    if (Status configured = radiance_.configure(settings.probes); !configured) {
        return configured;
    }
    budget_.configure(settings.budget);
    convergence_.configure(settings.convergence_region_metres, 0.25F);

    acceleration_ = rt::AccelerationService{settings.ray_tracing};

    software_.bind(field_, &surfaces_);
    hardware_.bind(acceleration_, &surfaces_);
    world_.bind(&software_, &hardware_);

    TieredTracer::Config config;
    config.sky = settings.sky;
    config.max_distance_metres = settings.max_ray_distance_metres;
    tracer_.set_config(config);
    tracer_.set_software(&software_);
    tracer_.set_hardware(&hardware_);
    return ok();
}

bool IlluminationSystem::dynamic_enabled() const noexcept {
    return settings_.mode == GiMode::Dynamic || settings_.mode == GiMode::Hybrid;
}

void IlluminationSystem::service_invalidations(IlluminationFrameReport& report,
                                               u64 frame) noexcept {
    (void)frame;
    for (InvalidationRecord& record : scene_.invalidations_mutable()) {
        // Each subsystem services the record and writes back what it cost, so one record accounts
        // for the whole of one change. That is "the system SHALL report which changes caused which
        // invalidations", as a number rather than a log line.
        record.field_bricks = field_.invalidate(record.region);
        record.surface_pages = surfaces_.invalidate(record.region);
        record.probes = radiance_.invalidate(record.region);
        (void)reflections_.invalidate(record.region);
        report.invalidated_field_bricks += record.field_bricks;
        report.invalidated_surface_pages += record.surface_pages;
        report.invalidated_probes += record.probes;
        report.invalidations_serviced += 1;
    }
    scene_.clear_invalidations();
}

IlluminationFrameReport IlluminationSystem::update(const FrameContext& context) noexcept {
    IlluminationFrameReport report;

    // 1. The budget. Every step below reads a lever value from it.
    if (context.permit_relaxation) {
        budget_.permit_relaxation();
    }
    report.budget = budget_.update(context.measured_gi_ms, context.frame);

    // 2. Invalidations, before anything updates something that is about to be invalidated.
    service_invalidations(report, context.frame);

    // 3. The distance field: the software tier traces it.
    report.field = field_.scroll_to(context.camera);

    // 4. The acceleration structures: the hardware tier traces them. Inactive is a no-op that
    //    builds nothing, which is what makes the fallback the default rather than a special case.
    report.acceleration = acceleration_.update();
    report.world_tier = world_.tier();

    if (settings_.mode == GiMode::None) {
        // Ambient and sky only. Nothing below is a bounce, so nothing below runs.
        report.convergence = convergence_.worst_convergence();
        return report;
    }

    // 5. Probe placement.
    ProbePlacementContext placement;
    placement.field = &field_;
    placement.lights = context.lights;
    placement.importance_region = context.importance_region;
    placement.has_importance_region = context.has_importance_region;
    report.placement = radiance_.scroll_to(context.camera, placement);

    if (!dynamic_enabled()) {
        // `Baked` and `Probe` keep the seeded caches and update neither. They are first-class
        // modes, so this is the whole of them rather than a disabled path.
        report.convergence = convergence_.worst_convergence();
        return report;
    }

    // 6. The surface cache: the probes gather from it.
    SurfaceUpdateContext surfaces;
    surfaces.lights = context.lights;
    surfaces.occluder = &world_;
    surfaces.indirect = &radiance_;
    surfaces.frame = context.frame;
    surfaces.budget = static_cast<u32>(budget_.value(GiLever::SurfaceCacheRate));
    report.surfaces =
        context.converged_mode ? surfaces_.update_all(surfaces) : surfaces_.update(surfaces);

    // 7. The radiance cache: the surface cache's next update gathers from it. This is the feedback
    //    loop that makes successive frames approximate additional bounces.
    ProbeUpdateContext probes;
    probes.tracer = &world_;
    probes.radiance = &surfaces_;
    probes.sky = settings_.sky;
    probes.frame = context.frame;
    probes.max_ray_distance_metres = settings_.max_ray_distance_metres;
    probes.budget = static_cast<u32>(budget_.value(GiLever::ProbeUpdates));
    probes.rays_per_probe =
        std::max(1U, static_cast<u32>(std::min(static_cast<f32>(settings_.rays_per_probe),
                                               budget_.value(GiLever::TracedRays))));
    probes.converged = context.converged_mode;
    report.probes = radiance_.update(probes);

    // 8. The reflection probes: the same tracer, the same surface cache.
    ReflectionCaptureContext capture;
    capture.tracer = &world_;
    capture.radiance = &surfaces_;
    capture.sky = settings_.sky;
    capture.frame = context.frame;
    capture.max_distance_metres = settings_.max_ray_distance_metres;
    const f32 reflection_scale = budget_.value(GiLever::ReflectionResolution);
    capture.texel_budget =
        context.converged_mode
            ? kReflectionTexels * std::max(1U, reflections_.probe_count())
            : std::max(1U, static_cast<u32>(16.0F * math::clamp(reflection_scale, 0.1F, 1.0F)));
    report.reflections = reflections_.capture(capture);

    // The denoiser's quality is one of the declared levers, and this is where the budget's decision
    // reaches it. It is the only place: a second call site would be a second policy.
    denoiser_.set_quality_position(static_cast<u32>(budget_.value(GiLever::DenoiserQuality)));

    // 9. Convergence, from what 6 and 7 reported.
    for (const Probe& probe : radiance_.probes()) {
        if (probe.live && probe.valid) {
            convergence_.observe(probe.position, probe.error);
        }
    }
    report.convergence = convergence_.worst_convergence();
    return report;
}

ResolveResult IlluminationSystem::indirect_diffuse(
    Vec3 position, Vec3 normal, const SurfaceProperties& surface) const noexcept {
    RadianceSample samples[kDiffuseSampleSlots];
    usize count = 0;

    if (surface.has_lightmap) {
        samples[count].source = RadianceSource::Lightmap;
        samples[count].radiance = surface.lightmap_radiance;
        samples[count].confidence = 1.0F;
        count += 1;
    }
    if (surface.has_irradiance_volume) {
        samples[count].source = RadianceSource::IrradianceVolume;
        samples[count].radiance = surface.irradiance_volume_radiance;
        samples[count].confidence = 0.9F;
        count += 1;
    }

    // The radiance cache, unless the query is outside every resident cell — in which case the GI
    // scene has no coverage there and the far field answers instead. That is
    // "queries into it SHALL fall back to the far-field representation" as a branch on coverage.
    if (scene_.has_coverage(position)) {
        samples[count] = radiance_.sample(position, normal, 0);
        count += 1;
    }

    // The sky, weighted by how much of it the surface can actually see and by how far into the far
    // field the query is. Both terms are needed: an indoor surface that took full sky would be the
    // leak the field's sky visibility exists to prevent.
    const f32 far = far_field_weight(surface.camera_distance_metres, settings_.error_targets);
    const f32 openness = field_.sky_visibility(position, normal, 8);
    samples[count].source = RadianceSource::Sky;
    samples[count].radiance = settings_.sky.radiance(normal) * openness;
    samples[count].confidence =
        math::clamp((0.35F * openness) + (0.65F * far * openness), 0.0F, 1.0F);
    count += 1;

    const u32 excluded =
        exclusion_for(settings_.mode, surface.has_lightmap, surface.has_irradiance_volume);
    return combine(Span<const RadianceSample>{samples, count}, excluded);
}

ResolveResult IlluminationSystem::indirect_specular(Vec3 position, Vec3 normal, Vec3 view_direction,
                                                    f32 roughness, u32 frame,
                                                    TraceBudget& budget) noexcept {
    const Vec3 incident = normalized_or(view_direction, Vec3{0.0F, 0.0F, -1.0F});
    const Vec3 reflection =
        normalized_or(incident - (normal * (2.0F * dot(incident, normal))), normal);
    const u32 rays = reflection_ray_count(roughness, settings_.reflection_thresholds);

    RadianceSample samples[3];
    usize count = 0;

    if (rays > 0) {
        // Dedicated rays through the SAME tiered tracer diffuse uses. The only difference is the
        // distribution, which is what "differing only in ray distribution and roughness handling"
        // means and is why there is no second tracer here.
        Vec3 total{0.0F, 0.0F, 0.0F};
        f32 confidence = 0.0F;
        u32 taken = 0;
        for (u32 index = 0; index < rays; ++index) {
            const Vec3 direction =
                reflection_lobe_direction(reflection, normal, roughness, index, rays, frame);
            const RadianceSample answer = tracer_.trace(position + (normal * 1.0e-3F), direction,
                                                        settings_.max_ray_distance_metres, budget);
            if (answer.confidence <= 0.0F) {
                continue;
            }
            total = total + answer.radiance;
            confidence = std::max(confidence, answer.confidence);
            taken += 1;
        }
        if (taken > 0) {
            samples[count].radiance = total / static_cast<f32>(taken);
            samples[count].confidence = confidence;
            samples[count].source = tracer_.selected_world_tier();
            count += 1;
        }
    }

    // The radiance cache is the rough answer and the fallback for the smooth one. It costs nothing
    // extra: it is the same cache the diffuse gather reads.
    RadianceSample cached = radiance_.sample(position, reflection, 0);
    if (cached.confidence > 0.0F) {
        // A rough surface trusts the cache as much as anything; a mirror barely at all.
        cached.confidence *= math::clamp(roughness, 0.15F, 1.0F);
        samples[count] = cached;
        count += 1;
    }

    // And the probes, blended in by confidence rather than switched to.
    const RadianceSample probe = reflections_.sample(position, reflection, roughness);
    if (probe.confidence > 0.0F) {
        samples[count] = probe;
        count += 1;
    }

    return combine(Span<const RadianceSample>{samples, count}, 0);
}

u32 IlluminationSystem::advance_to_convergence(const FrameContext& context, f32 threshold,
                                               u32 max_frames) noexcept {
    FrameContext local = context;
    local.converged_mode = true;
    for (u32 index = 0; index < max_frames; ++index) {
        local.frame = context.frame + index;
        const IlluminationFrameReport report = update(local);
        if (report.convergence >= threshold && convergence_.region_count() != 0) {
            return index + 1;
        }
    }
    return max_frames;
}

}  // namespace cy::rendering::gi
