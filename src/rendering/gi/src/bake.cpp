#include <cy/rendering/gi/bake.h>

#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>

namespace cy::rendering::gi {
namespace {

/// A small counter-based hash. Deterministic across platforms and compilers, which is what makes a
/// bake reproducible and a reference comparison a measurement rather than a draw.
[[nodiscard]] f32 hashed_unit(u32& sequence) noexcept {
    sequence = (sequence * 747796405U) + 2891336453U;
    u32 value = ((sequence >> ((sequence >> 28U) + 4U)) ^ sequence) * 277803737U;
    value = (value >> 22U) ^ value;
    return static_cast<f32>(value & 0xFFFFFFU) / static_cast<f32>(0x1000000U);
}

/// A cosine-weighted direction about `normal`. Cosine weighted so the estimator's weight is one and
/// the loop stays a plain mean.
[[nodiscard]] Vec3 cosine_direction(Vec3 normal, u32& sequence) noexcept {
    const f32 u1 = hashed_unit(sequence);
    const f32 u2 = hashed_unit(sequence);
    const f32 radius = std::sqrt(u1);
    const f32 angle = 6.2831853F * u2;
    const f32 x = radius * std::cos(angle);
    const f32 y = radius * std::sin(angle);
    const f32 z = std::sqrt(std::max(0.0F, 1.0F - u1));

    Vec3 tangent = std::abs(normal.y) < 0.99F ? cross(Vec3{0.0F, 1.0F, 0.0F}, normal)
                                              : cross(Vec3{1.0F, 0.0F, 0.0F}, normal);
    tangent = normalized_or(tangent, Vec3{1.0F, 0.0F, 0.0F});
    const Vec3 bitangent = cross(normal, tangent);
    return normalized_or((tangent * x) + (bitangent * y) + (normal * z), normal);
}

[[nodiscard]] Vec3 modulate(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.x * b.x, a.y * b.y, a.z * b.z};
}

}  // namespace

Vec3 PathTracer::radiance(Vec3 origin, Vec3 direction, u32 bounces, f32 max_distance,
                          u32& sequence) const noexcept {
    Vec3 total{0.0F, 0.0F, 0.0F};
    Vec3 throughput{1.0F, 1.0F, 1.0F};
    Vec3 position = origin;
    Vec3 heading = normalized_or(direction, Vec3{0.0F, 1.0F, 0.0F});

    for (u32 bounce = 0; bounce <= bounces; ++bounce) {
        SceneHit hit;
        rays_ += 1;
        if (!tracer_->trace(position, heading, max_distance, hit)) {
            total = total + modulate(throughput, sky_.radiance(heading));
            break;
        }

        // The shared representation: the surface's material comes from the GI scene, which is the
        // same set of cards the surface cache is built from and the software tier resolves against.
        const u32 slot = scene_->nearest_surfel(hit.position, hit.normal, 1.0F);
        Vec3 albedo{0.5F, 0.5F, 0.5F};
        Vec3 emission{0.0F, 0.0F, 0.0F};
        if (slot != GiScene::kInvalidSurfel) {
            albedo = scene_->surfel(slot).albedo;
            emission = scene_->surfel(slot).emission;
        }

        total = total + modulate(throughput, emission);
        const Vec3 direct = shaded_direct(lights_, hit.position, hit.normal, occluder_);
        total = total + modulate(throughput, modulate(albedo, direct));

        throughput = modulate(throughput, albedo);
        if (bounce == bounces) {
            break;
        }
        position = hit.position + (hit.normal * 1.0e-3F);
        heading = cosine_direction(hit.normal, sequence);
    }
    return total;
}

Vec3 PathTracer::irradiance(Vec3 position, Vec3 normal,
                            const BakeSettings& settings) const noexcept {
    const u32 samples = std::max(1U, settings.samples);
    u32 sequence = settings.seed;
    Vec3 total{0.0F, 0.0F, 0.0F};
    const Vec3 origin = position + (normal * 1.0e-3F);
    for (u32 index = 0; index < samples; ++index) {
        const Vec3 direction = cosine_direction(normal, sequence);
        total = total + radiance(origin, direction, settings.bounces, settings.max_distance_metres,
                                 sequence);
    }
    return total / static_cast<f32>(samples);
}

namespace {

constexpr u32 kSeedDirections = 6;
constexpr Vec3 kSeedAxes[kSeedDirections] = {{1.0F, 0.0F, 0.0F}, {-1.0F, 0.0F, 0.0F},
                                             {0.0F, 1.0F, 0.0F}, {0.0F, -1.0F, 0.0F},
                                             {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, -1.0F}};

/// The six axis directions, as a span. Every seeded payload is built from the same set, so the
/// probes and the reflection probes are seeded from one sampling and not two.
[[nodiscard]] Span<const Vec3> seed_directions() noexcept {
    return {kSeedAxes, kSeedDirections};
}

/// Seed every live probe of the radiance cache from the path tracer's ground-truth irradiance.
[[nodiscard]] Expected<u32, Error> seed_probes(const PathTracer& tracer, RadianceCache& cache,
                                               const BakeSettings& settings) noexcept {
    Array<Vec3> radiance_values;
    if (Status reserved = radiance_values.reserve(kSeedDirections); !reserved) {
        return make_unexpected(reserved.error());
    }

    u32 seeded = 0;
    const Span<const Probe> probes = cache.probes();
    for (u32 index = 0; index < probes.size(); ++index) {
        if (!probes[index].live) {
            continue;
        }
        radiance_values.clear();
        for (const Vec3 axis : kSeedAxes) {
            if (Status pushed = radiance_values.push_back(
                    tracer.irradiance(probes[index].position, axis, settings));
                !pushed) {
                return make_unexpected(pushed.error());
            }
        }
        if (Status stored = cache.seed(index, seed_directions(), radiance_values.span()); !stored) {
            return make_unexpected(stored.error());
        }
        seeded += 1;
    }
    return seeded;
}

/// The same for the reflection probes, which want radiance along a direction rather than an
/// irradiance over a hemisphere: a probe is a mirror of the room, filtered afterwards by roughness.
[[nodiscard]] Expected<u32, Error> seed_reflection_probes(const PathTracer& tracer,
                                                          ReflectionProbeSet& reflections,
                                                          const BakeSettings& settings) noexcept {
    Array<Vec3> radiance_values;
    if (Status reserved = radiance_values.reserve(kSeedDirections); !reserved) {
        return make_unexpected(reserved.error());
    }

    u32 seeded = 0;
    for (u32 handle = 0; handle < reflections.handle_capacity(); ++handle) {
        const ReflectionProbe& probe = reflections.probe(handle);
        if (!probe.live) {
            continue;
        }
        radiance_values.clear();
        for (const Vec3 axis : kSeedAxes) {
            u32 sequence = settings.seed + handle;
            if (Status pushed = radiance_values.push_back(
                    tracer.radiance(probe.position, axis, settings.bounces,
                                    settings.max_distance_metres, sequence));
                !pushed) {
                return make_unexpected(pushed.error());
            }
        }
        if (Status stored = reflections.seed(handle, seed_directions(), radiance_values.span());
            !stored) {
            return make_unexpected(stored.error());
        }
        seeded += 1;
    }
    return seeded;
}

}  // namespace

Expected<BakeReport, Error> seed_caches(const PathTracer& tracer, SurfaceCache& surfaces,
                                        RadianceCache& cache, ReflectionProbeSet* reflections,
                                        Span<const GiLight> lights, const Occluder* occluder,
                                        const BakeSettings& settings) noexcept {
    BakeReport report;
    const u64 before = tracer.rays_traced();

    // The surface cache first: the probes gather from it, so seeding it first is one bounce the
    // probes get for free.
    SurfaceUpdateContext context;
    context.lights = lights;
    context.occluder = occluder;
    context.indirect = nullptr;
    context.frame = 0;
    report.surface_pages_seeded = surfaces.update_all(context).pages_updated;

    // Then the probes, from the path tracer rather than from one frame's gather — which is what
    // makes the seed better than the first frame the runtime would have produced.
    Expected<u32, Error> probes = seed_probes(tracer, cache, settings);
    if (!probes) {
        return make_unexpected(probes.error());
    }
    report.probes_seeded = probes.value();

    if (reflections != nullptr) {
        Expected<u32, Error> seeded = seed_reflection_probes(tracer, *reflections, settings);
        if (!seeded) {
            return make_unexpected(seeded.error());
        }
        report.reflection_probes_seeded = seeded.value();
    }

    report.rays = tracer.rays_traced() - before;
    return report;
}

ReferenceComparison compare_against_reference(Span<const Vec3> measured,
                                              Span<const Vec3> reference) noexcept {
    ReferenceComparison comparison;
    const usize count = std::min(measured.size(), reference.size());
    if (count == 0) {
        return comparison;
    }
    f32 total_error = 0.0F;
    f32 total_magnitude = 0.0F;
    for (usize index = 0; index < count; ++index) {
        const f32 error = length(measured[index] - reference[index]);
        total_error += error;
        total_magnitude += length(reference[index]);
        comparison.max_absolute_error = std::max(comparison.max_absolute_error, error);
    }
    comparison.samples = static_cast<u32>(count);
    comparison.mean_absolute_error = total_error / static_cast<f32>(count);
    comparison.mean_reference_magnitude = total_magnitude / static_cast<f32>(count);
    comparison.relative_error =
        comparison.mean_reference_magnitude > 1.0e-5F
            ? comparison.mean_absolute_error / comparison.mean_reference_magnitude
            : 0.0F;
    return comparison;
}

}  // namespace cy::rendering::gi
