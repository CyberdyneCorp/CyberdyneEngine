#include <cy/rendering/gi/signals.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering::gi {
namespace {

using denoise::SignalKind;

constexpr u32 kIndex(SignalKind kind) noexcept {
    return static_cast<u32>(kind);
}

/// A deterministic sample index for a pixel in a frame. The whole of this producer's randomness,
/// and it is a permutation rather than a generator: two runs of one frame draw the same directions,
/// which is what lets a stochastic signal be regression tested at all.
[[nodiscard]] u32 sample_index(u32 pixel, u64 frame, u32 stream) noexcept {
    u32 hash = pixel * 0x9E3779B9U;
    hash ^= static_cast<u32>(frame) * 0x85EBCA6BU;
    hash ^= stream * 0xC2B2AE35U;
    hash ^= hash >> 15U;
    hash *= 0x2545F491U;
    hash ^= hash >> 13U;
    return hash;
}

/// A unit float in [0, 1) from that index.
[[nodiscard]] f32 unit_from(u32 hash) noexcept {
    return static_cast<f32>(hash & 0x00FFFFFFU) / static_cast<f32>(0x01000000U);
}

void basis_around(Vec3 normal, Vec3& tangent, Vec3& bitangent) noexcept {
    const f32 sign = normal.z >= 0.0F ? 1.0F : -1.0F;
    const f32 a = -1.0F / (sign + normal.z);
    const f32 b = normal.x * normal.y * a;
    tangent = Vec3{1.0F + (sign * normal.x * normal.x * a), sign * b, -sign * normal.x};
    bitangent = Vec3{b, sign + (normal.y * normal.y * a), -normal.y};
}

/// A cosine-weighted direction around `normal` from two unit floats. Cosine weighted rather than
/// uniform because every consumer of it here multiplies by the cosine anyway, and importance
/// sampling the obvious factor is the difference between one usable sample and one that is mostly
/// zero.
[[nodiscard]] Vec3 cosine_direction(Vec3 normal, f32 u1, f32 u2) noexcept {
    Vec3 tangent;
    Vec3 bitangent;
    basis_around(normal, tangent, bitangent);
    const f32 radius = std::sqrt(u1);
    const f32 angle = math::kTwoPi * u2;
    const f32 z = std::sqrt(math::max(0.0F, 1.0F - u1));
    return normalized_or((tangent * (radius * std::cos(angle))) +
                             (bitangent * (radius * std::sin(angle))) + (normal * z),
                         normal);
}

[[nodiscard]] f32 luminance_of(Vec3 colour) noexcept {
    return (0.2126F * colour.x) + (0.7152F * colour.y) + (0.0722F * colour.z);
}

/// A point on a light, jittered across its own extent. A shadow ray to a light's CENTRE is not a
/// stochastic signal at all — it produces a hard edge with no noise to reconstruct, which is the
/// shape of a producer that looks like it works and exercises nothing.
[[nodiscard]] Vec3 light_sample_point(const GiLight& light, Vec3 from, u32 hash) noexcept {
    const f32 u1 = unit_from(hash);
    const f32 u2 = unit_from(hash * 0x27220A95U);
    const f32 u3 = unit_from(hash * 0x165667B1U);
    if (light.directional) {
        // A directional light's "position" is a point far along the reverse of its travel, and its
        // `radius` is an ANGLE rather than a length. The sun's is about a quarter of a degree, which
        // is the whole of an outdoor penumbra.
        const Vec3 towards = normalized_or(light.direction * -1.0F, Vec3{0.0F, 1.0F, 0.0F});
        const f32 spread_scale = math::max(light.radius, 0.0F);
        const Vec3 spread = cosine_direction(towards, u1 * spread_scale * spread_scale, u2);
        return from + (spread * 1000.0F);
    }
    // A uniform point in the light's own sphere, which is what makes a penumbra a gradient of
    // partially occluded samples rather than a hard step with no noise in it.
    const f32 radius = math::max(light.radius, 0.0F);
    return light.position +
           Vec3{(u1 - 0.5F) * 2.0F * radius, (u2 - 0.5F) * 2.0F * radius,
                (u3 - 0.5F) * 2.0F * radius};
}

}  // namespace

f32 luminance_variance(Span<const Vec3> values, Span<const f32> depth) noexcept {
    f32 total = 0.0F;
    f32 square = 0.0F;
    u32 count = 0;
    for (usize pixel = 0; pixel < values.size() && pixel < depth.size(); ++pixel) {
        if (depth[pixel] <= 0.0F) {
            continue;
        }
        const f32 value = luminance_of(values[pixel]);
        total += value;
        square += value * value;
        count += 1;
    }
    if (count == 0) {
        return 0.0F;
    }
    const f32 mean = total / static_cast<f32>(count);
    return math::max(0.0F, (square / static_cast<f32>(count)) - (mean * mean));
}

Status StochasticSignals::resize(u32 width, u32 height) noexcept {
    if (width == 0 || height == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "StochasticSignals::resize: a zero-sized view has no pixels");
    }
    width_ = width;
    height_ = height;
    const usize pixels = static_cast<usize>(width) * static_cast<usize>(height);
    for (SignalBuffer& buffer : buffers_) {
        if (Status sized = buffer.values.resize(pixels); !sized) {
            return sized;
        }
        if (Status sized = buffer.samples.resize(pixels); !sized) {
            return sized;
        }
        buffer.reconstructed = Span<const Vec3>{};
    }
    return ok();
}

Span<const Vec3> StochasticSignals::noisy(SignalKind kind) const noexcept {
    const SignalBuffer& buffer = buffers_[kIndex(kind)];
    return {buffer.values.data(), buffer.values.size()};
}

Span<const Vec3> StochasticSignals::reconstructed(SignalKind kind) const noexcept {
    return buffers_[kIndex(kind)].reconstructed;
}

void StochasticSignals::produce_indirect(const PixelInputs& pixel,
                                         IlluminationSystem& system) noexcept {
    // INDIRECT DIFFUSE. One cosine ray through the tiered tracer, resolved through the surface
    // cache — which is the stochastic estimate the resolve's own confidence-weighted answer is the
    // converged version of. Taking `indirect_diffuse()` here instead would hand the denoiser a
    // signal that is already reconstructed, which is the shape of a producer that denoises nothing.
    const u32 hash = sample_index(pixel.index, pixel.frame, 1);
    const Vec3 direction =
        cosine_direction(pixel.normal, unit_from(hash), unit_from(hash * 0x27220A95U));
    TraceBudget budget;
    budget.world_rays = 1;
    budget.escalations = 1;
    const RadianceSample sample =
        system.tracer().trace(pixel.position + (pixel.normal * 0.01F), direction,
                              system.settings().max_ray_distance_metres, budget);
    SignalBuffer& diffuse = buffers_[kIndex(SignalKind::IndirectDiffuse)];
    diffuse.values[pixel.index] = sample.radiance;
    diffuse.samples[pixel.index] = 1.0F;
    diffuse.pixels += 1;
    diffuse.rays += 1;

    // INDIRECT SPECULAR. The same scene, the same caches, the same resolve; a different ray
    // distribution and a roughness rule, which `reflections.h` says is the whole difference.
    TraceBudget specular_budget;
    specular_budget.world_rays = 1;
    specular_budget.escalations = 1;
    const ResolveResult specular =
        system.indirect_specular(pixel.position + (pixel.normal * 0.01F), pixel.normal, pixel.view,
                                 pixel.roughness, static_cast<u32>(pixel.frame), specular_budget);
    SignalBuffer& reflections = buffers_[kIndex(SignalKind::IndirectSpecular)];
    reflections.values[pixel.index] = specular.radiance;
    reflections.samples[pixel.index] = 1.0F;
    reflections.pixels += 1;
    reflections.rays += 1;
}

void StochasticSignals::produce_visibility(const PixelInputs& pixel, Span<const GiLight> lights,
                                           const IlluminationSystem& system,
                                           f32 occlusion_radius) noexcept {
    const Vec3 origin = pixel.position + (pixel.normal * 0.01F);

    // RAY-TRACED SHADOWS. One shadow ray to a jittered point on one light, which is what makes a
    // penumbra a noisy edge rather than a hard one. `SignalDomain::Visibility` is why the framework
    // reconstructs this as occlusion and not as colour.
    f32 visible = 1.0F;
    if (!lights.empty()) {
        const u32 hash = sample_index(pixel.index, pixel.frame, 2);
        const GiLight& light = lights[hash % lights.size()];
        const Vec3 target = light_sample_point(light, origin, hash);
        visible = system.world_tracer().occluded(origin, target) ? 0.0F : 1.0F;
    }
    SignalBuffer& shadow = buffers_[kIndex(SignalKind::RayTracedShadow)];
    shadow.values[pixel.index] = Vec3{visible, visible, visible};
    shadow.samples[pixel.index] = 1.0F;
    shadow.pixels += 1;
    shadow.rays += lights.empty() ? 0U : 1U;

    // AMBIENT OCCLUSION. One short cosine ray. Short deliberately: AO is a local term and a ray
    // that reached the whole scene would be a second and worse indirect diffuse.
    const u32 occlusion_hash = sample_index(pixel.index, pixel.frame, 3);
    const Vec3 direction = cosine_direction(pixel.normal, unit_from(occlusion_hash),
                                            unit_from(occlusion_hash * 0x165667B1U));
    SceneHit hit;
    const bool occluded =
        system.world_tracer().trace(origin, direction, occlusion_radius, hit) && hit.hit;
    const f32 openness = occluded ? 0.0F : 1.0F;
    SignalBuffer& occlusion = buffers_[kIndex(SignalKind::AmbientOcclusion)];
    occlusion.values[pixel.index] = Vec3{openness, openness, openness};
    occlusion.samples[pixel.index] = 1.0F;
    occlusion.pixels += 1;
    occlusion.rays += 1;
}

void StochasticSignals::produce_direct(const PixelInputs& pixel, Span<const GiLight> lights,
                                       const IlluminationSystem& system) noexcept {
    // STOCHASTIC DIRECT LIGHTING. ONE light per pixel per frame, chosen uniformly and divided by
    // its own probability — which is what makes the estimate unbiased and what makes a hundred
    // lights cost what one costs. A producer that summed every light would be the deterministic
    // direct term with a denoiser bolted to it.
    SignalBuffer& direct = buffers_[kIndex(SignalKind::StochasticDirect)];
    direct.samples[pixel.index] = 1.0F;
    direct.pixels += 1;
    if (lights.empty()) {
        direct.values[pixel.index] = Vec3{0.0F, 0.0F, 0.0F};
        return;
    }
    const u32 hash = sample_index(pixel.index, pixel.frame, 4);
    const usize chosen = hash % lights.size();
    const GiLight one[] = {lights[chosen]};
    const Vec3 shaded = shaded_direct({one, 1}, pixel.position, pixel.normal,
                                      &system.world_tracer());
    direct.values[pixel.index] = shaded * static_cast<f32>(lights.size());
    direct.rays += 1;
}

Status StochasticSignals::route(SignalKind kind, const SignalSurfaces& surfaces,
                                const denoise::HistoryGuidance& history,
                                denoise::Denoiser& denoiser, SignalProduction& report) noexcept {
    denoise::GuidanceBuffers guidance;
    guidance.width = surfaces.width;
    guidance.height = surfaces.height;
    guidance.depth = surfaces.depth;
    guidance.normal = surfaces.normal;
    guidance.roughness = surfaces.roughness;
    guidance.instance_id = surfaces.instance_id;
    guidance.material_id = surfaces.material_id;

    SignalBuffer& buffer = buffers_[kIndex(kind)];
    denoise::NoisySignal noisy;
    noisy.values = {buffer.values.data(), buffer.values.size()};
    noisy.samples = {buffer.samples.data(), buffer.samples.size()};

    auto filtered = denoiser.denoise(kind, noisy, guidance, history);
    if (!filtered) {
        return fail(filtered.error().code, filtered.error().message);
    }
    buffer.reconstructed = filtered.value();

    const u32 index = kIndex(kind);
    report.produced[index] = true;
    report.pixels[index] = buffer.pixels;
    report.rays[index] = buffer.rays;
    report.noisy_variance[index] = luminance_variance(noisy.values, surfaces.depth);
    report.reconstructed_variance[index] = luminance_variance(buffer.reconstructed, surfaces.depth);
    return ok();
}

Expected<SignalProduction, Error> StochasticSignals::produce(
    const SignalSurfaces& surfaces, Span<const GiLight> lights, IlluminationSystem& system,
    const denoise::HistoryGuidance& history, u64 frame) noexcept {
    if (surfaces.width != width_ || surfaces.height != height_) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument,
                  "StochasticSignals::produce: the surfaces are a different size from the view"});
    }
    const usize pixels = static_cast<usize>(width_) * static_cast<usize>(height_);
    if (surfaces.depth.size() != pixels || surfaces.position.size() != pixels ||
        surfaces.normal.size() != pixels) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument,
                  "StochasticSignals::produce: a surface buffer does not cover every pixel"});
    }

    for (SignalBuffer& buffer : buffers_) {
        buffer.pixels = 0;
        buffer.rays = 0;
        for (usize pixel = 0; pixel < pixels; ++pixel) {
            buffer.values[pixel] = Vec3{0.0F, 0.0F, 0.0F};
            buffer.samples[pixel] = 0.0F;
        }
    }

    for (usize pixel = 0; pixel < pixels; ++pixel) {
        if (surfaces.depth[pixel] <= 0.0F) {
            // A SKY PIXEL PRODUCES NO SAMPLE. Not a black one: a zero handed to a denoiser is a
            // measurement of nothing that the filter would then spread into its neighbours, which
            // is how a sky edge acquires a dark fringe.
            continue;
        }
        PixelInputs inputs;
        inputs.position = surfaces.position[pixel];
        inputs.normal = normalized_or(surfaces.normal[pixel], Vec3{0.0F, 1.0F, 0.0F});
        inputs.view = normalized_or(inputs.position - surfaces.camera, Vec3{0.0F, 0.0F, -1.0F});
        inputs.roughness = pixel < surfaces.roughness.size() ? surfaces.roughness[pixel] : 0.5F;
        inputs.index = static_cast<u32>(pixel);
        inputs.frame = frame;
        produce_indirect(inputs, system);
        produce_visibility(inputs, lights, system, surfaces.occlusion_radius_metres);
        produce_direct(inputs, lights, system);
    }

    SignalProduction report;
    denoise::Denoiser& denoiser = system.denoiser();
    if (Status sized = denoiser.resize(width_, height_); !sized) {
        return make_unexpected(sized.error());
    }
    // EVERY SIGNAL THE FRAMEWORK DECLARES, ONE LINE EACH. The loop over `kSignalCount` that would
    // read more tidily is exactly what this must not be: a signal is routed because a producer for
    // it was written, so deleting one of these lines has to leave a tree that compiles and a
    // framework with one signal nobody drives — which is the defect
    // `m11c:denoiser-signals-have-producers` is proven against.
    if (Status routed = route(SignalKind::IndirectDiffuse, surfaces, history, denoiser, report);
        !routed) {
        return make_unexpected(routed.error());
    }
    if (Status routed = route(SignalKind::IndirectSpecular, surfaces, history, denoiser, report);
        !routed) {
        return make_unexpected(routed.error());
    }
    if (Status routed = route(SignalKind::RayTracedShadow, surfaces, history, denoiser, report);
        !routed) {
        return make_unexpected(routed.error());
    }
    if (Status routed = route(SignalKind::AmbientOcclusion, surfaces, history, denoiser, report);
        !routed) {
        return make_unexpected(routed.error());
    }
    if (Status routed = route(SignalKind::StochasticDirect, surfaces, history, denoiser, report);
        !routed) {
        return make_unexpected(routed.error());
    }
    return report;
}

}  // namespace cy::rendering::gi
