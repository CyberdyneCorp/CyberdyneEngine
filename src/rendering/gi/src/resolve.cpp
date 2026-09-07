#include <cy/rendering/gi/resolve.h>

#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>

namespace cy::rendering::gi {
namespace {

/// The sources that carry dynamic indirect diffuse. A surface taking a bounce from a lightmap
/// excludes exactly these, and nothing else — the sky and the reflection probes are not a second
/// count of the same bounce.
constexpr u32 kDynamicDiffuseSources =
    source_bit(RadianceSource::RadianceCache) | source_bit(RadianceSource::SurfaceCache) |
    source_bit(RadianceSource::ScreenTrace) | source_bit(RadianceSource::SoftwareTrace) |
    source_bit(RadianceSource::HardwareTrace);

}  // namespace

ResolveResult combine(Span<const RadianceSample> samples, u32 excluded_sources) noexcept {
    ResolveResult result;
    result.sources_excluded = excluded_sources;

    Vec3 total{0.0F, 0.0F, 0.0F};
    f32 total_weight = 0.0F;
    f32 best_weight = 0.0F;
    for (const RadianceSample& sample : samples) {
        if (sample.source == RadianceSource::None) {
            continue;
        }
        const u32 bit = source_bit(sample.source);
        if ((excluded_sources & bit) != 0) {
            continue;
        }
        const f32 weight = math::clamp(sample.confidence, 0.0F, 1.0F);
        if (weight <= 0.0F) {
            continue;
        }
        total = total + (sample.radiance * weight);
        total_weight += weight;
        result.sources_used |= bit;
        result.confidence = std::max(result.confidence, weight);
        if (weight > best_weight) {
            best_weight = weight;
            result.dominant = sample.source;
        }
    }

    if (total_weight > 0.0F) {
        result.radiance = total / total_weight;
    }
    return result;
}

u32 exclusion_for(GiMode mode, bool surface_has_lightmap,
                  bool surface_has_irradiance_volume) noexcept {
    switch (mode) {
        case GiMode::None:
            // Ambient and sky only. Everything that is a bounce is excluded, which is the mode
            // rather than a failure of it.
            return kDynamicDiffuseSources | source_bit(RadianceSource::Lightmap) |
                   source_bit(RadianceSource::IrradianceVolume);
        case GiMode::Baked:
            return kDynamicDiffuseSources;
        case GiMode::Probe:
            // Baked probes for dynamic objects, no lightmaps.
            return kDynamicDiffuseSources | source_bit(RadianceSource::Lightmap);
        case GiMode::Dynamic:
            return source_bit(RadianceSource::Lightmap) |
                   source_bit(RadianceSource::IrradianceVolume);
        case GiMode::Hybrid: {
            // The only interesting row. A lightmapped surface takes the lightmap and nothing
            // dynamic; a surface with only an irradiance volume takes that and nothing dynamic; a
            // surface with neither takes the dynamic sources.
            if (surface_has_lightmap) {
                return kDynamicDiffuseSources | source_bit(RadianceSource::IrradianceVolume);
            }
            if (surface_has_irradiance_volume) {
                return kDynamicDiffuseSources | source_bit(RadianceSource::Lightmap);
            }
            return source_bit(RadianceSource::Lightmap) |
                   source_bit(RadianceSource::IrradianceVolume);
        }
        case GiMode::Count:
            break;
    }
    return 0;
}

f32 far_field_weight(f32 distance_metres, const ErrorTargets& targets) noexcept {
    const f32 begin = targets.far_field_begins_metres;
    if (begin <= 0.0F) {
        return 1.0F;
    }
    // The ramp's width is the ratio of the two error targets, so a system whose far field is only
    // slightly coarser transitions over a shorter distance. Clamped so the ramp is never
    // zero-width, which would be the visible boundary the requirement forbids.
    const f32 ratio = targets.near_field_metres > 0.0F
                          ? targets.far_field_metres / targets.near_field_metres
                          : 8.0F;
    const f32 width = begin * math::clamp(ratio / 100.0F, 0.15F, 1.0F);
    if (distance_metres <= begin) {
        return 0.0F;
    }
    const f32 t = math::clamp((distance_metres - begin) / width, 0.0F, 1.0F);
    // Smoothstep: the ramp has no derivative discontinuity at either end, which is what stops a
    // slowly moving camera from showing a band.
    return t * t * (3.0F - (2.0F * t));
}

ConvergenceTracker::ConvergenceTracker() noexcept = default;

void ConvergenceTracker::configure(f32 region_size_metres, f32 smoothing) noexcept {
    region_size_ = std::max(region_size_metres, 0.01F);
    smoothing_ = math::clamp(smoothing, 0.01F, 1.0F);
}

u64 ConvergenceTracker::key_of(Vec3 position) const noexcept {
    const auto fold = [this](f32 value) {
        const auto cell = static_cast<i32>(std::floor(value / region_size_));
        return static_cast<u64>(static_cast<u32>(cell + (1 << 20)) & 0x1FFFFFU);
    };
    return (fold(position.x) << 42U) | (fold(position.y) << 21U) | fold(position.z);
}

void ConvergenceTracker::observe(Vec3 position, f32 relative_error) noexcept {
    const u64 key = key_of(position);
    const f32 error = math::clamp(relative_error, 0.0F, 1.0F);
    if (f32* existing = regions_.find(key); existing != nullptr) {
        *existing = math::lerp(*existing, error, smoothing_);
        return;
    }
    (void)regions_.insert(key, error);
}

f32 ConvergenceTracker::convergence(Vec3 position) const noexcept {
    const f32* existing = regions_.find(key_of(position));
    if (existing == nullptr) {
        // Unknown is not converged. Reporting 1 here would let a capture proceed on a region
        // nothing has looked at yet, which is the failure this class exists to prevent.
        return 0.0F;
    }
    return math::clamp(1.0F - *existing, 0.0F, 1.0F);
}

f32 ConvergenceTracker::worst_convergence() const noexcept {
    f32 worst = 1.0F;
    for (const auto& region : regions_) {
        worst = std::min(worst, math::clamp(1.0F - region.value, 0.0F, 1.0F));
    }
    return regions_.empty() ? 0.0F : worst;
}

bool ConvergenceTracker::converged(f32 threshold) const noexcept {
    return !regions_.empty() && worst_convergence() >= threshold;
}

u32 ConvergenceTracker::unconverged_regions(f32 threshold) const noexcept {
    u32 count = 0;
    for (const auto& region : regions_) {
        if (math::clamp(1.0F - region.value, 0.0F, 1.0F) < threshold) {
            count += 1;
        }
    }
    return count;
}

u32 ConvergenceTracker::region_count() const noexcept {
    return static_cast<u32>(regions_.size());
}

void ConvergenceTracker::reset() noexcept {
    regions_ = HashMap<u64, f32>{};
}

}  // namespace cy::rendering::gi
