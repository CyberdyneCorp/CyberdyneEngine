// SPDX-License-Identifier: MIT
#include <cy/rendering/gi/screen_resolve.h>

#include <cy/core/math/scalar.h>
#include <cy/rendering/temporal/motion.h>
#include <cy/rendering/temporal/reprojection.h>

#include <algorithm>
#include <cmath>

namespace cy::rendering::gi {
namespace {

/// The world tier `TieredTracer` would select, traced alone: what a final-gather ray is.
///
/// A hit whose surface faces AWAY from the ray answers nothing. A pixel beside a thin wall is
/// within the field's half-voxel hit threshold of the wall's far face, so its ray can start inside
/// the wall and meet that face from behind — and the card there is the other room's. The ray then
/// left the pixel's own space, and the honest answer is none: the pixel takes the cache and the
/// sky instead, as one whose ray found nothing does. The hardware tier turns every triangle to face
/// the ray, so the test never fires there.
///
/// The ray leaves the surface lifted along its normal: by `origin_lift_metres` for the hardware
/// tier, whose triangles are exact, and by at least one finest voxel for the software tier, whose
/// hit threshold is half of one — closer than that, a ray leaving its own surface at any but a
/// steep angle is answered by that surface, the pixel lit by itself.
[[nodiscard]] RadianceSample world_ray_impl(const ScreenResolveSources& sources, Vec3 position,
                                            Vec3 normal, Vec3 direction, f32 lift,
                                            f32 reach) noexcept {
    const Vec3 unit = normalized_or(direction, Vec3{0.0F, 0.0F, -1.0F});
    SceneHit hit;
    if (sources.hardware != nullptr && sources.hardware->available()) {
        (void)sources.hardware->trace(position + (normal * lift), unit, reach, hit);
        if (hit.hit && dot(hit.normal, unit) > 0.0F) {
            return RadianceSample{};
        }
        return sources.hardware->radiance(hit, sources.sky, unit);
    }
    if (sources.software != nullptr) {
        const f32 voxel = sources.field != nullptr ? sources.field->voxel_size() : 0.0F;
        (void)sources.software->trace(position + (normal * std::max(lift, voxel)), unit, reach,
                                      hit);
        if (hit.hit && dot(hit.normal, unit) > 0.0F) {
            return RadianceSample{};
        }
        return sources.software->radiance(hit, sources.sky, unit);
    }
    return RadianceSample{};
}

/// The mirror direction of `view_direction` about `normal`.
/// THE FINAL-GATHER RAY AS ONE SAMPLE OF ITS HEMISPHERE: its weight in the combine is the same
/// whatever it met, and what it met decides only its value.
///
/// A pixel's ray picks one direction at random, so its answer is an estimate of the hemisphere only
/// on average over the directions it picks — and an average whose WEIGHT depends on the direction
/// drawn is not that average. Combined by its own confidence, a ray that escaped to the sky (whose
/// answer is certain) outweighed one that met a card shaded a few frames ago (whose answer is
/// distrusted as stale), so over a few frames the denoiser accumulated an estimate pulled toward
/// the sky: the colonnade's shadowed faces, half lit by sunlit gravel, resolved two thirds of the
/// bounce the path tracer finds there (`samples/12-beauty`'s reference).
///
/// So the ray always counts in full, and its confidence says how much of its value is its own:
/// what it does not trust — a stale card, a hit no card covers, a hit on a back face — the
/// cache's answer for the same pixel (`stand_in`) stands in for, as "stale cache is distrusted"
/// asks. With no stand-in the ray's own answer is all there is; a back face with none is no
/// sample.
[[nodiscard]] RadianceSample gather_sample_impl(const RadianceSample& ray,
                                                const RadianceSample& stand_in,
                                                u32 excluded_sources) noexcept {
    const bool has_stand_in = stand_in.source != RadianceSource::None &&
                              stand_in.confidence > 0.0F &&
                              (excluded_sources & source_bit(stand_in.source)) == 0U;
    RadianceSample sample = ray;
    if (ray.source == RadianceSource::None) {
        if (!has_stand_in) {
            return ray;
        }
        sample.source = stand_in.source;
        sample.radiance = stand_in.radiance;
    } else if (has_stand_in) {
        const f32 trust = math::clamp(ray.confidence, 0.0F, 1.0F);
        sample.radiance = lerp(stand_in.radiance, ray.radiance, trust);
    }
    sample.confidence = 1.0F;
    return sample;
}

/// The radiance cache at a pixel for both signals, every contributing probe traced to through the
/// field — or, with no field, the cache's own query in each direction. None without a cache.
[[nodiscard]] TracedCacheSamples cache_samples(const ScreenResolveSources& sources, Vec3 position,
                                               Vec3 normal, Vec3 reflection) noexcept {
    if (sources.cache == nullptr) {
        return TracedCacheSamples{};
    }
    if (sources.field != nullptr) {
        return sources.cache->sample_traced(position, normal, reflection, sources.cache_frame,
                                            *sources.field);
    }
    return TracedCacheSamples{sources.cache->sample(position, normal, sources.cache_frame),
                              sources.cache->sample(position, reflection, sources.cache_frame)};
}

/// The far-field sky sample: the sky in the normal's direction, seen through ONE of the field's
/// sky visibility rays — which one rotates with the frame — and trusted as far as the pixel is into
/// the far field. Zero confidence in the near field, where the cache and the ray see the sky.
[[nodiscard]] RadianceSample far_sky(u32 pixel, Vec3 position, Vec3 normal, f32 camera_distance,
                                     const ScreenResolveSettings& settings,
                                     const ScreenResolveSources& sources) noexcept {
    RadianceSample sample;
    sample.source = RadianceSource::Sky;
    const f32 far = far_field_weight(camera_distance, settings.error_targets);
    if (far <= 0.0F) {
        return sample;
    }
    f32 openness = 1.0F;
    if (sources.field != nullptr && settings.sky_visibility_rays > 0) {
        const u32 count = settings.sky_visibility_rays;
        const u32 index = static_cast<u32>((sources.frame + pixel) % count);
        openness = sources.field->sky_visibility_ray(position, normal, index, count);
    }
    sample.radiance = sources.sky.radiance(normal) * openness;
    sample.confidence = math::clamp(far * openness, 0.0F, 1.0F);
    return sample;
}

[[nodiscard]] ResolveResult resolve_diffuse(u32 pixel, Vec3 position, Vec3 normal,
                                            f32 camera_distance, const RadianceSample& cached,
                                            const ScreenResolveSettings& settings,
                                            const ScreenResolveSources& sources,
                                            bool& traced) noexcept {
    RadianceSample samples[3];
    usize count = 0;
    if (sources.cache != nullptr) {
        samples[count] = cached;
        count += 1;
    }
    traced = screen_traces(pixel, sources.frame, settings.ray_density);
    if (traced) {
        const u32 hash = screen_hash(pixel, sources.frame, kScreenStreamDirection);
        const Vec3 direction =
            screen_cosine_direction(normal, screen_unit(hash), screen_unit(hash * 0x27220A95U));
        samples[count] = gather_sample_impl(
            world_ray_impl(sources, position, normal, direction, settings.origin_lift_metres,
                           settings.max_ray_distance_metres),
            sources.cache != nullptr ? cached : RadianceSample{},
            exclusion_for(settings.mode, false, false));
        count += 1;
    }
    samples[count] = far_sky(pixel, position, normal, camera_distance, settings, sources);
    count += 1;
    ResolveResult result = combine(Span<const RadianceSample>{samples, count},
                                   exclusion_for(settings.mode, false, false));
    result.radiance = result.radiance * settings.diffuse_intensity;
    return result;
}

[[nodiscard]] ResolveResult resolve_specular(u32 pixel, Vec3 position, Vec3 normal, f32 roughness,
                                             Vec3 reflection, const RadianceSample& cache,
                                             const ScreenResolveSettings& settings,
                                             const ScreenResolveSources& sources,
                                             bool& traced) noexcept {
    RadianceSample samples[2];
    usize count = 0;
    RadianceSample cached{};
    if (sources.cache != nullptr && cache.confidence > 0.0F) {
        cached = cache;
        // `indirect_specular`'s rule: a rough surface trusts the cache as much as anything, a
        // mirror barely at all.
        cached.confidence *= math::clamp(roughness, 0.15F, 1.0F);
    }
    const u32 rays = reflection_ray_count(roughness, settings.reflection_thresholds);
    traced = rays > 0 && screen_traces(pixel, sources.frame, settings.ray_density);
    if (traced) {
        const u32 hash = screen_hash(pixel, sources.frame, kScreenStreamSpecular);
        const Vec3 direction = reflection_lobe_direction(reflection, normal, roughness, hash % rays,
                                                         rays, static_cast<u32>(sources.frame));
        samples[count] = gather_sample_impl(
            world_ray_impl(sources, position, normal, direction, settings.origin_lift_metres,
                           settings.max_ray_distance_metres),
            cached, 0U);
        count += 1;
    }
    if (cached.confidence > 0.0F) {
        samples[count] = cached;
        count += 1;
    }
    ResolveResult result = combine(Span<const RadianceSample>{samples, count}, 0);
    result.radiance = result.radiance * settings.specular_intensity;
    return result;
}

}  // namespace

u32 screen_hash(u32 pixel, u64 frame, u32 stream) noexcept {
    u32 hash = pixel * 0x9E3779B9U;
    hash ^= static_cast<u32>(frame) * 0x85EBCA6BU;
    hash ^= stream * 0xC2B2AE35U;
    hash ^= hash >> 15U;
    hash *= 0x2545F491U;
    hash ^= hash >> 13U;
    return hash;
}

f32 screen_unit(u32 hash) noexcept {
    return static_cast<f32>(hash & 0x00FFFFFFU) / static_cast<f32>(0x01000000U);
}

void screen_basis(Vec3 normal, Vec3& tangent, Vec3& bitangent) noexcept {
    const f32 sign = normal.z >= 0.0F ? 1.0F : -1.0F;
    const f32 a = -1.0F / (sign + normal.z);
    const f32 b = normal.x * normal.y * a;
    tangent = Vec3{1.0F + (sign * normal.x * normal.x * a), sign * b, -sign * normal.x};
    bitangent = Vec3{b, sign + (normal.y * normal.y * a), -normal.y};
}

Vec3 screen_cosine_direction(Vec3 normal, f32 u1, f32 u2) noexcept {
    Vec3 tangent;
    Vec3 bitangent;
    screen_basis(normal, tangent, bitangent);
    const f32 radius = std::sqrt(u1);
    const f32 angle = math::kTwoPi * u2;
    const f32 z = std::sqrt(math::max(0.0F, 1.0F - u1));
    return normalized_or((tangent * (radius * std::cos(angle))) +
                             (bitangent * (radius * std::sin(angle))) + (normal * z),
                         normal);
}

bool screen_traces(u32 pixel, u64 frame, f32 density) noexcept {
    if (density >= 1.0F) {
        return true;
    }
    return screen_unit(screen_hash(pixel, frame, kScreenStreamTrace)) < density;
}

RadianceSample screen_world_ray(const ScreenResolveSources& sources, Vec3 position, Vec3 normal,
                                Vec3 direction, f32 lift, f32 reach) noexcept {
    return world_ray_impl(sources, position, normal, direction, lift, reach);
}

RadianceSample screen_gather_sample(const RadianceSample& ray, const RadianceSample& stand_in,
                                    u32 excluded_sources) noexcept {
    return gather_sample_impl(ray, stand_in, excluded_sources);
}

Vec3 screen_reflect_view(Vec3 view_direction, Vec3 normal) noexcept {
    const Vec3 incident = normalized_or(view_direction, Vec3{0.0F, 0.0F, -1.0F});
    return normalized_or(incident - (normal * (2.0F * dot(incident, normal))), normal);
}

bool screen_invalidated_at(Span<const Aabb> regions, Vec3 point) noexcept {
    const usize count = std::min<usize>(regions.size(), kScreenHistoryRegions);
    for (usize index = 0; index < count; ++index) {
        const Aabb& box = regions[index];
        if (point.x >= box.min.x && point.y >= box.min.y && point.z >= box.min.z &&
            point.x <= box.max.x && point.y <= box.max.y && point.z <= box.max.z) {
            return true;
        }
    }
    return false;
}

bool screen_landing(Vec3 position, const ScreenHistoryInputs& inputs, u32 width, u32 height, u32& x,
                    u32& y) noexcept {
    SurfaceMotionInputs motion_inputs;
    motion_inputs.current_world = position;
    motion_inputs.previous_world = position;
    motion_inputs.current_view_projection = inputs.current_world_to_clip;
    motion_inputs.previous_view_projection = inputs.previous_world_to_clip;
    const SurfaceMotion motion = derive_surface_motion(motion_inputs);
    if (!motion.representable) {
        return false;
    }
    const Vec2 uv{motion.current_uv.x + motion.motion.x, motion.current_uv.y + motion.motion.y};
    const f32 fx = std::floor(uv.x * static_cast<f32>(width));
    const f32 fy = std::floor((1.0F - uv.y) * static_cast<f32>(height));
    if (fx < 0.0F || fy < 0.0F || fx >= static_cast<f32>(width) || fy >= static_cast<f32>(height)) {
        return false;
    }
    x = static_cast<u32>(fx);
    y = static_cast<u32>(fy);
    return true;
}

denoise::SignalConfig screen_signal_config(const denoise::SignalConfig& authored,
                                           const ScreenResolveSettings& settings) noexcept {
    denoise::SignalConfig config = authored;
    if (settings.gather != GiGather::ScreenProbes) {
        return config;
    }
    if (settings.probes.denoiser == ScreenProbeDenoiser::ProbeSpace) {
        // Filtered in probe space already: the pixels are accumulated over time and nothing more,
        // and a history the filtered signal has left behind is cut rather than averaged away.
        config.max_passes = 0;
        config.history_rejection = settings.probes.history_rejection;
        return config;
    }
    config.max_passes = std::min(config.max_passes, settings.probes.filter_passes);
    return config;
}

const char* screen_probe_denoiser_name(ScreenProbeDenoiser denoiser) noexcept {
    switch (denoiser) {
        case ScreenProbeDenoiser::ProbeSpace:
            return "probe space";
        case ScreenProbeDenoiser::Shared:
            return "shared";
    }
    return "unknown";
}

const char* gi_gather_name(GiGather gather) noexcept {
    switch (gather) {
        case GiGather::ScreenProbes:
            return "screen probes";
        case GiGather::PerPixel:
            return "per pixel";
    }
    return "unknown";
}

ScreenPixel resolve_screen_pixel(u32 pixel, Vec3 position, Vec3 normal, f32 roughness,
                                 f32 camera_distance, Vec3 view_direction,
                                 const ScreenResolveSettings& settings,
                                 const ScreenResolveSources& sources) noexcept {
    ScreenPixel answer;
    const Vec3 reflection = screen_reflect_view(view_direction, normal);
    const TracedCacheSamples cached = cache_samples(sources, position, normal, reflection);
    answer.diffuse = resolve_diffuse(pixel, position, normal, camera_distance, cached.diffuse,
                                     settings, sources, answer.traced_diffuse);
    answer.specular = resolve_specular(pixel, position, normal, roughness, reflection,
                                       cached.specular, settings, sources, answer.traced_specular);
    return answer;
}

Status ScreenResolve::resize(u32 width, u32 height) noexcept {
    if (width == 0 || height == 0) {
        return fail(ErrorCode::InvalidArgument, "ScreenResolve::resize: a view has pixels");
    }
    const usize pixels = static_cast<usize>(width) * height;
    if (Status sized = diffuse_.resize(pixels); !sized) {
        return sized;
    }
    if (Status sized = specular_.resize(pixels); !sized) {
        return sized;
    }
    if (Status sized = samples_.resize(pixels); !sized) {
        return sized;
    }
    width_ = width;
    height_ = height;
    return ok();
}

Status ScreenResolve::produce(const ScreenSurfaces& surfaces, const ScreenResolveSettings& settings,
                              const ScreenResolveSources& sources) noexcept {
    const usize pixels = static_cast<usize>(width_) * height_;
    if (surfaces.width != width_ || surfaces.height != height_ || surfaces.depth.size() != pixels ||
        surfaces.position.size() != pixels || surfaces.normal.size() != pixels) {
        return fail(ErrorCode::InvalidArgument,
                    "ScreenResolve::produce: the surfaces do not cover the view it was sized for");
    }
    if (settings.gather == GiGather::ScreenProbes) {
        return produce_probes(surfaces, settings, sources);
    }
    traced_ = 0;
    for (usize index = 0; index < pixels; ++index) {
        diffuse_[index] = Vec3{0.0F, 0.0F, 0.0F};
        specular_[index] = Vec3{0.0F, 0.0F, 0.0F};
        samples_[index] = 0.0F;
        if (surfaces.depth[index] <= 0.0F) {
            continue;
        }
        const Vec3 position = surfaces.position[index];
        const Vec3 normal = normalized_or(surfaces.normal[index], Vec3{0.0F, 1.0F, 0.0F});
        const f32 roughness = surfaces.roughness.empty() ? 0.5F : surfaces.roughness[index];
        const Vec3 offset = position - surfaces.camera;
        const ScreenPixel pixel =
            resolve_screen_pixel(static_cast<u32>(index), position, normal, roughness,
                                 length(offset), offset, settings, sources);
        diffuse_[index] = pixel.diffuse.radiance;
        specular_[index] = pixel.specular.radiance;
        samples_[index] = 1.0F;
        traced_ += pixel.traced_diffuse ? 1U : 0U;
    }
    return ok();
}

Status ScreenResolve::produce_probes(const ScreenSurfaces& surfaces,
                                     const ScreenResolveSettings& settings,
                                     const ScreenResolveSources& sources) noexcept {
    const u32 spacing =
        screen_probe_grid_spacing(settings.probes.spacing_pixels, std::max(surfaces.divisor, 1U));
    if (Status sized = probes_.resize(width_, height_, spacing); !sized) {
        return sized;
    }
    if (Status placed = probes_.place(surfaces, settings.probes); !placed) {
        return placed;
    }
    probes_.trace(surfaces, settings, sources);
    probes_.filter(settings.probes);
    traced_ = 0;
    const usize pixels = static_cast<usize>(width_) * height_;
    for (usize index = 0; index < pixels; ++index) {
        diffuse_[index] = Vec3{0.0F, 0.0F, 0.0F};
        specular_[index] = Vec3{0.0F, 0.0F, 0.0F};
        samples_[index] = 0.0F;
        if (surfaces.depth[index] <= 0.0F) {
            continue;
        }
        const Vec3 position = surfaces.position[index];
        const Vec3 normal = normalized_or(surfaces.normal[index], Vec3{0.0F, 1.0F, 0.0F});
        const f32 roughness = surfaces.roughness.empty() ? 0.5F : surfaces.roughness[index];
        const Vec3 offset = position - surfaces.camera;
        const ScreenPixel pixel =
            probes_.integrate(static_cast<u32>(index), position, normal, roughness,
                              surfaces.depth[index], length(offset), offset, settings, sources);
        diffuse_[index] = pixel.diffuse.radiance;
        specular_[index] = pixel.specular.radiance;
        samples_[index] = settings.probes.pixel_samples;
    }
    traced_ = probes_.stats().rays;
    return ok();
}

denoise::NoisySignal ScreenResolve::noisy(denoise::SignalKind kind) const noexcept {
    denoise::NoisySignal signal;
    signal.values =
        kind == denoise::SignalKind::IndirectSpecular ? specular_.span() : diffuse_.span();
    signal.samples = samples_.span();
    return signal;
}

Status ScreenGrid::build(const ScreenSurfaces& frame, u32 divisor) noexcept {
    const usize pixels = static_cast<usize>(frame.width) * frame.height;
    if (divisor == 0 || frame.width == 0 || frame.height == 0 || frame.depth.size() != pixels ||
        frame.position.size() != pixels || frame.normal.size() != pixels) {
        return fail(ErrorCode::InvalidArgument,
                    "ScreenGrid::build: a frame's surfaces, one entry a pixel, and a divisor");
    }
    divisor_ = divisor;
    width_ = screen_grid_extent(frame.width, divisor);
    height_ = screen_grid_extent(frame.height, divisor);
    camera_ = frame.camera;
    const usize cells = static_cast<usize>(width_) * height_;
    if (Status sized = depth_.resize(cells); !sized) {
        return sized;
    }
    if (Status sized = position_.resize(cells); !sized) {
        return sized;
    }
    if (Status sized = normal_.resize(cells); !sized) {
        return sized;
    }
    if (Status sized = roughness_.resize(cells); !sized) {
        return sized;
    }
    for (u32 y = 0; y < height_; ++y) {
        for (u32 x = 0; x < width_; ++x) {
            const usize cell = (static_cast<usize>(y) * width_) + x;
            const usize pixel =
                (static_cast<usize>(y) * divisor * frame.width) + (static_cast<usize>(x) * divisor);
            depth_[cell] = frame.depth[pixel];
            position_[cell] = frame.position[pixel];
            normal_[cell] = frame.normal[pixel];
            roughness_[cell] = frame.roughness.empty() ? 0.5F : frame.roughness[pixel];
        }
    }
    return ok();
}

ScreenSurfaces ScreenGrid::surfaces() const noexcept {
    ScreenSurfaces out;
    out.width = width_;
    out.height = height_;
    out.depth = depth_.span();
    out.position = position_.span();
    out.normal = normal_.span();
    out.roughness = roughness_.span();
    out.camera = camera_;
    out.divisor = divisor_;
    return out;
}

namespace {

/// One pixel of `screen_upsample`.
[[nodiscard]] Vec3 upsample_pixel(const ScreenSurfaces& frame, const ScreenSurfaces& grid,
                                  u32 divisor, Span<const Vec3> values, u32 x, u32 y) noexcept {
    const usize pixel = (static_cast<usize>(y) * frame.width) + x;
    const f32 depth = frame.depth[pixel];
    if (depth <= 0.0F) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }
    const Vec3 normal = frame.normal[pixel];
    const f32 gx = static_cast<f32>(x) / static_cast<f32>(divisor);
    const f32 gy = static_cast<f32>(y) / static_cast<f32>(divisor);
    const u32 bx = std::min(static_cast<u32>(gx), grid.width - 1U);
    const u32 by = std::min(static_cast<u32>(gy), grid.height - 1U);
    const f32 fx = math::clamp(gx - static_cast<f32>(bx), 0.0F, 1.0F);
    const f32 fy = math::clamp(gy - static_cast<f32>(by), 0.0F, 1.0F);
    Vec3 total{0.0F, 0.0F, 0.0F};
    f32 weight_total = 0.0F;
    Vec3 nearest{0.0F, 0.0F, 0.0F};
    f32 nearest_gap = 1.0e30F;
    for (u32 j = 0; j < 2; ++j) {
        for (u32 i = 0; i < 2; ++i) {
            const u32 tx = std::min(bx + i, grid.width - 1U);
            const u32 ty = std::min(by + j, grid.height - 1U);
            const usize tap = (static_cast<usize>(ty) * grid.width) + tx;
            const f32 tap_depth = grid.depth[tap];
            if (tap_depth <= 0.0F) {
                continue;
            }
            const f32 gap = std::abs(tap_depth - depth);
            if (gap < nearest_gap) {
                nearest_gap = gap;
                nearest = values[tap];
            }
            const f32 bilinear = (i == 0 ? 1.0F - fx : fx) * (j == 0 ? 1.0F - fy : fy);
            f32 facing = std::max(0.0F, dot(grid.normal[tap], normal));
            for (u32 squaring = 0; squaring < kUpsampleNormalSquarings; ++squaring) {
                facing *= facing;
            }
            const f32 weight =
                bilinear * std::exp(-gap / ((kUpsampleDepthTolerance * depth) + 1.0e-6F)) * facing;
            if (weight <= 0.0F) {
                continue;
            }
            total = total + (values[tap] * weight);
            weight_total += weight;
        }
    }
    return weight_total > 1.0e-6F ? total / weight_total : nearest;
}

}  // namespace

Status screen_upsample(const ScreenSurfaces& frame, const ScreenSurfaces& grid, u32 divisor,
                       Span<const Vec3> grid_values, Span<Vec3> out) noexcept {
    const usize pixels = static_cast<usize>(frame.width) * frame.height;
    const usize cells = static_cast<usize>(grid.width) * grid.height;
    if (divisor == 0 || out.size() != pixels || grid_values.size() != cells ||
        frame.depth.size() != pixels || grid.depth.size() != cells ||
        grid.width != screen_grid_extent(frame.width, divisor) ||
        grid.height != screen_grid_extent(frame.height, divisor)) {
        return fail(ErrorCode::InvalidArgument,
                    "screen_upsample: the frame, its grid at this divisor and the grid's values");
    }
    if (divisor == 1) {
        // Every pixel is its own sample: the grid's values, exactly.
        for (usize pixel = 0; pixel < pixels; ++pixel) {
            out[pixel] = frame.depth[pixel] > 0.0F ? grid_values[pixel] : Vec3{0.0F, 0.0F, 0.0F};
        }
        return ok();
    }
    for (u32 y = 0; y < frame.height; ++y) {
        for (u32 x = 0; x < frame.width; ++x) {
            out[(static_cast<usize>(y) * frame.width) + x] =
                upsample_pixel(frame, grid, divisor, grid_values, x, y);
        }
    }
    return ok();
}

denoise::GuidanceBuffers screen_guidance(const ScreenSurfaces& surfaces) noexcept {
    denoise::GuidanceBuffers guidance;
    guidance.width = surfaces.width;
    guidance.height = surfaces.height;
    guidance.depth = surfaces.depth;
    guidance.normal = surfaces.normal;
    guidance.roughness = surfaces.roughness;
    return guidance;
}

Status screen_history(const ScreenSurfaces& surfaces, const ScreenHistoryInputs& inputs,
                      Span<i32> source, Span<f32> confidence) noexcept {
    const usize pixels = static_cast<usize>(surfaces.width) * surfaces.height;
    if (source.size() != pixels || confidence.size() != pixels ||
        surfaces.position.size() != pixels || surfaces.depth.size() != pixels) {
        return fail(ErrorCode::InvalidArgument,
                    "screen_history: every span covers the view, one entry a pixel");
    }
    const bool history = inputs.previous_depth.size() == pixels;
    const auto width = static_cast<f32>(surfaces.width);
    const auto height = static_cast<f32>(surfaces.height);
    for (usize index = 0; index < pixels; ++index) {
        source[index] = -1;
        confidence[index] = 0.0F;
        if (!history || surfaces.depth[index] <= 0.0F) {
            continue;
        }
        SurfaceMotionInputs motion_inputs;
        motion_inputs.current_world = surfaces.position[index];
        motion_inputs.previous_world = surfaces.position[index];
        motion_inputs.current_view_projection = inputs.current_world_to_clip;
        motion_inputs.previous_view_projection = inputs.previous_world_to_clip;
        const SurfaceMotion motion = derive_surface_motion(motion_inputs);
        ReprojectionInputs reprojection;
        reprojection.current_uv = motion.current_uv;
        reprojection.motion = motion.motion;
        reprojection.representable = motion.representable;
        reprojection.current_depth = surfaces.depth[index];
        reprojection.depth_tolerance = inputs.depth_tolerance;
        // The history depth is the one the reprojection LANDS on, so the landing pixel is found
        // first; a landing off the screen is classified out of frame before the depth is read.
        const Vec2 uv{motion.current_uv.x + motion.motion.x, motion.current_uv.y + motion.motion.y};
        const f32 fx = std::floor(uv.x * width);
        const f32 fy = std::floor((1.0F - uv.y) * height);
        const bool on_screen = fx >= 0.0F && fy >= 0.0F && fx < width && fy < height;
        const usize landing =
            on_screen ? (static_cast<usize>(fy) * surfaces.width) + static_cast<usize>(fx) : 0;
        reprojection.history_depth = on_screen ? inputs.previous_depth[landing] : 0.0F;
        const ReprojectionResult result = classify_history(reprojection);
        if (!history_usable(result.state) || !on_screen || inputs.previous_depth[landing] <= 0.0F ||
            screen_invalidated_at(inputs.invalidated, surfaces.position[index])) {
            continue;
        }
        source[index] = static_cast<i32>(landing);
        confidence[index] = 1.0F;
    }
    return ok();
}

}  // namespace cy::rendering::gi
