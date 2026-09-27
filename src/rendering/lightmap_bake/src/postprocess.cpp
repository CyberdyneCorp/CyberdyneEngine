// SPDX-License-Identifier: MIT
// After the trace: denoising, border dilation, seam reconciliation, and the planes' encoding.

#include "internal.h"

#include <cy/rendering/denoise/denoiser.h>
#include <cy/rendering/lightmap_bake/asset.h>

#include <algorithm>
#include <cmath>

namespace cy::rendering::lightmap_bake::detail {
namespace {

/// The four channels of the moments a mode reads, as the denoiser's Vec3 signals.
enum class Channel : u8 { Mean, LuminanceDirection, Red, Green, Blue };

[[nodiscard]] Vec3& channel_of(TexelMoments& moments, Channel channel) noexcept {
    switch (channel) {
        case Channel::LuminanceDirection:
            return moments.luminance_direction;
        case Channel::Red:
            return moments.channel_direction[0];
        case Channel::Green:
            return moments.channel_direction[1];
        case Channel::Blue:
            return moments.channel_direction[2];
        case Channel::Mean:
            break;
    }
    return moments.mean;
}

[[nodiscard]] Span<const Channel> channels_of(LightmapMode mode) noexcept {
    static constexpr Channel kIrradiance[] = {Channel::Mean};
    static constexpr Channel kDirectional[] = {Channel::Mean, Channel::LuminanceDirection};
    static constexpr Channel kShL1[] = {Channel::Mean, Channel::Red, Channel::Green,
                                        Channel::Blue};
    switch (mode) {
        case LightmapMode::Directional:
            return {kDirectional, 2};
        case LightmapMode::ShL1:
            return {kShL1, 4};
        case LightmapMode::Irradiance:
        case LightmapMode::Count:
            break;
    }
    return {kIrradiance, 1};
}

[[nodiscard]] bool has_surface(const TexelSurface& texel) noexcept {
    return texel.state == TexelState::Surface;
}

void add_moments(TexelMoments& into, const TexelMoments& from, f32 weight) noexcept {
    into.mean = into.mean + (from.mean * weight);
    into.luminance_direction = into.luminance_direction + (from.luminance_direction * weight);
    for (u32 channel = 0; channel < 3U; ++channel) {
        into.channel_direction[channel] =
            into.channel_direction[channel] + (from.channel_direction[channel] * weight);
    }
    into.luminance += from.luminance * weight;
}

/// A bilinear footprint: four texels and their weights, at a coordinate in texels.
struct Footprint {
    usize texels[4] = {};
    f32 weights[4] = {};
};

[[nodiscard]] Footprint footprint_at(const Canvas& canvas, Vec2 coordinate) noexcept {
    const f32 x = coordinate.x - 0.5F;
    const f32 y = coordinate.y - 0.5F;
    const f32 fx = std::floor(x);
    const f32 fy = std::floor(y);
    const f32 tx = x - fx;
    const f32 ty = y - fy;
    const auto clamp_x = [&](f32 value) {
        return static_cast<u32>(std::clamp(value, 0.0F, static_cast<f32>(canvas.width - 1U)));
    };
    const auto clamp_y = [&](f32 value) {
        return static_cast<u32>(std::clamp(value, 0.0F, static_cast<f32>(canvas.height - 1U)));
    };
    const u32 x0 = clamp_x(fx);
    const u32 x1 = clamp_x(fx + 1.0F);
    const u32 y0 = clamp_y(fy);
    const u32 y1 = clamp_y(fy + 1.0F);
    Footprint footprint;
    footprint.texels[0] = canvas.index(x0, y0);
    footprint.texels[1] = canvas.index(x1, y0);
    footprint.texels[2] = canvas.index(x0, y1);
    footprint.texels[3] = canvas.index(x1, y1);
    footprint.weights[0] = (1.0F - tx) * (1.0F - ty);
    footprint.weights[1] = tx * (1.0F - ty);
    footprint.weights[2] = (1.0F - tx) * ty;
    footprint.weights[3] = tx * ty;
    return footprint;
}

[[nodiscard]] TexelMoments sample_footprint(const Canvas& canvas,
                                            const Footprint& footprint) noexcept {
    TexelMoments value;
    for (u32 corner = 0; corner < 4U; ++corner) {
        add_moments(value, canvas.moments[footprint.texels[corner]], footprint.weights[corner]);
    }
    return value;
}

[[nodiscard]] f32 mean_covered_luminance(const Canvas& canvas) noexcept {
    f64 total = 0.0;
    u64 count = 0;
    for (usize index = 0; index < canvas.surfaces.size(); ++index) {
        if (has_surface(canvas.surfaces[index])) {
            total += luminance(canvas.moments[index].mean);
            count += 1;
        }
    }
    return count > 0 ? static_cast<f32>(total / static_cast<f64>(count)) : 0.0F;
}

/// The seam samples: pairs of footprints that must read one value.
struct SeamSample {
    Footprint a;
    Footprint b;
};

[[nodiscard]] Status seam_samples(const Canvas& canvas, Span<const SeamEdge> seams,
                                  Array<SeamSample>& out) noexcept {
    for (const SeamEdge& seam : seams) {
        const f32 span = std::max(length(seam.a1 - seam.a0), length(seam.b1 - seam.b0));
        // Four samples per texel of the longer side, ends included: dense enough that the two
        // sides' piecewise-bilinear readings agree between the samples as well as at them.
        const u32 count = std::max(2U, static_cast<u32>(std::ceil(span * 4.0F)) + 1U);
        for (u32 index = 0; index < count; ++index) {
            const f32 t = static_cast<f32>(index) / static_cast<f32>(count - 1U);
            SeamSample sample;
            sample.a = footprint_at(canvas, lerp(seam.a0, seam.a1, t));
            sample.b = footprint_at(canvas, lerp(seam.b0, seam.b1, t));
            if (Status pushed = out.push_back(sample); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

[[nodiscard]] f32 worst_disagreement(const Canvas& canvas,
                                     Span<const SeamSample> samples) noexcept {
    f32 worst = 0.0F;
    for (const SeamSample& sample : samples) {
        const TexelMoments a = sample_footprint(canvas, sample.a);
        const TexelMoments b = sample_footprint(canvas, sample.b);
        worst = std::max(worst, std::fabs(luminance(a.mean) - luminance(b.mean)));
    }
    return worst;
}

/// One minimum-norm step moving both footprints to the same bilinear value.
void solve_sample(Canvas& canvas, const SeamSample& sample) noexcept {
    const TexelMoments a = sample_footprint(canvas, sample.a);
    const TexelMoments b = sample_footprint(canvas, sample.b);
    f32 norm = 0.0F;
    for (u32 corner = 0; corner < 4U; ++corner) {
        norm += (sample.a.weights[corner] * sample.a.weights[corner]) +
                (sample.b.weights[corner] * sample.b.weights[corner]);
    }
    if (norm <= 1.0e-8F) {
        return;
    }
    TexelMoments difference = a;
    add_moments(difference, b, -1.0F);
    for (u32 corner = 0; corner < 4U; ++corner) {
        add_moments(canvas.moments[sample.a.texels[corner]], difference,
                    -sample.a.weights[corner] / norm);
        add_moments(canvas.moments[sample.b.texels[corner]], difference,
                    sample.b.weights[corner] / norm);
    }
}

[[nodiscard]] Vec4 encode_plane(const TexelMoments& moments, Vec3 normal, LightmapMode mode,
                                u32 plane) noexcept {
    const Vec3 mean = moments.mean;
    if (mode == LightmapMode::Irradiance || (mode == LightmapMode::Directional && plane == 0U)) {
        return Vec4{mean.x, mean.y, mean.z, 1.0F};
    }
    if (mode == LightmapMode::Directional) {
        // The luminance-weighted mean direction scaled by its directionality, and the geometric
        // normal's own factor, so `1 + v . n` over `w` is one at the geometric normal.
        const f32 lum = luminance(mean);
        const Vec3 v = lum > 1.0e-8F ? moments.luminance_direction / lum : Vec3{};
        return Vec4{v.x, v.y, v.z, std::max(1.0F + dot(v, normal), 1.0e-3F)};
    }
    // ShL1: per channel `a + b . n`, fitted so it is the channel's value at the geometric normal.
    const f32 channel_mean[3] = {mean.x, mean.y, mean.z};
    const f32 value = channel_mean[plane];
    const Vec3 v = value > 1.0e-8F ? moments.channel_direction[plane] / value : Vec3{};
    const f32 w = std::max(1.0F + dot(v, normal), 1.0e-3F);
    const Vec3 b = v * (value / w);
    return Vec4{b.x, b.y, b.z, value / w};
}

[[nodiscard]] f32 rounded(f32 value) noexcept {
    return float_from_half(half_from_float(value));
}

}  // namespace

Status denoise_moments(Canvas& canvas, LightmapMode mode, u32 /*samples*/) noexcept {
    const usize texels = canvas.surfaces.size();
    Array<f32> depth;
    Array<Vec3> normals;
    Array<f32> roughness;
    Array<u32> charts;
    Array<Vec3> values;
    if (Status sized = depth.resize(texels); !sized) {
        return sized;
    }
    if (Status sized = normals.resize(texels); !sized) {
        return sized;
    }
    if (Status sized = roughness.resize(texels); !sized) {
        return sized;
    }
    if (Status sized = charts.resize(texels); !sized) {
        return sized;
    }
    if (Status sized = values.resize(texels); !sized) {
        return sized;
    }
    for (usize index = 0; index < texels; ++index) {
        const TexelSurface& texel = canvas.surfaces[index];
        // Depth is only the "is there a surface" test here: an atlas has no view, and a texel with
        // none is the denoiser's sky, which it neither filters nor borrows from.
        depth[index] = has_surface(texel) ? 1.0F : 0.0F;
        normals[index] = texel.normal;
        roughness[index] = 1.0F;
        charts[index] = has_surface(texel) ? texel.chart + 1U : 0U;
    }

    denoise::Denoiser denoiser;
    if (Status sized = denoiser.resize(canvas.width, canvas.height); !sized) {
        return sized;
    }
    denoise::GuidanceBuffers guidance;
    guidance.width = canvas.width;
    guidance.height = canvas.height;
    guidance.depth = depth.span();
    guidance.normal = normals.span();
    guidance.roughness = roughness.span();
    // THE CHART IS THE HARD BOUNDARY. Two charts adjacent in the atlas are strangers in the world,
    // and a filter that crossed from one into the other would move light across the level.
    guidance.instance_id = charts.span();

    for (const Channel channel : channels_of(mode)) {
        for (usize index = 0; index < texels; ++index) {
            values[index] = channel_of(canvas.moments[index], channel);
        }
        // One estimate per texel, and no history: an atlas is one frame. The empty sample span
        // makes the denoiser take each texel's variance from its neighbourhood, which is the only
        // variance a single frame has.
        denoiser.reset_history();
        denoise::NoisySignal noisy;
        noisy.values = values.span();
        Expected<Span<const Vec3>, Error> filtered = denoiser.denoise(
            denoise::SignalKind::IndirectDiffuse, noisy, guidance, denoise::HistoryGuidance{});
        if (!filtered.has_value()) {
            return make_unexpected(filtered.error());
        }
        for (usize index = 0; index < texels; ++index) {
            if (has_surface(canvas.surfaces[index])) {
                channel_of(canvas.moments[index], channel) = filtered.value()[index];
            }
        }
    }
    for (usize index = 0; index < texels; ++index) {
        canvas.moments[index].luminance = luminance(canvas.moments[index].mean);
    }
    return ok();
}

u32 dilate(Canvas& canvas, u32 passes) noexcept {
    // A texel filled in this pass must not feed its neighbour in the same pass, or the fill would
    // run in scan order; `known` is updated only between passes.
    Array<u8> known;
    if (!known.resize(canvas.surfaces.size()).has_value()) {
        return 0;
    }
    for (usize index = 0; index < canvas.surfaces.size(); ++index) {
        known[index] = has_surface(canvas.surfaces[index]) ? 1U : 0U;
    }
    Array<usize> frontier;
    Array<TexelMoments> fills;
    Array<Vec3> fill_normals;
    u32 filled = 0;
    for (u32 pass = 0; pass < passes; ++pass) {
        frontier.clear();
        fills.clear();
        fill_normals.clear();
        for (u32 y = 0; y < canvas.height; ++y) {
            for (u32 x = 0; x < canvas.width; ++x) {
                const usize index = canvas.index(x, y);
                const TexelSurface& texel = canvas.surfaces[index];
                if (known[index] != 0U || texel.owner == kNoOwner) {
                    continue;
                }
                TexelMoments sum;
                Vec3 normal_sum{0.0F, 0.0F, 0.0F};
                f32 weight = 0.0F;
                for (i32 dy = -1; dy <= 1; ++dy) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        const i32 nx = static_cast<i32>(x) + dx;
                        const i32 ny = static_cast<i32>(y) + dy;
                        if (nx < 0 || ny < 0 || nx >= static_cast<i32>(canvas.width) ||
                            ny >= static_cast<i32>(canvas.height)) {
                            continue;
                        }
                        const usize neighbour =
                            canvas.index(static_cast<u32>(nx), static_cast<u32>(ny));
                        // Only the texel's own rectangle: a gutter is filled from its object.
                        if (known[neighbour] == 0U ||
                            canvas.surfaces[neighbour].owner != texel.owner) {
                            continue;
                        }
                        add_moments(sum, canvas.moments[neighbour], 1.0F);
                        normal_sum = normal_sum + canvas.surfaces[neighbour].normal;
                        weight += 1.0F;
                    }
                }
                if (weight <= 0.0F) {
                    continue;
                }
                TexelMoments average;
                add_moments(average, sum, 1.0F / weight);
                // The normal travels with the light: the directional planes are encoded against it.
                if (!frontier.push_back(index).has_value() ||
                    !fills.push_back(average).has_value() ||
                    !fill_normals.push_back(normalized_or(normal_sum, Vec3{0.0F, 1.0F, 0.0F}))
                         .has_value()) {
                    return filled;
                }
            }
        }
        if (frontier.empty()) {
            break;
        }
        for (usize at = 0; at < frontier.size(); ++at) {
            canvas.moments[frontier[at]] = fills[at];
            canvas.surfaces[frontier[at]].normal = fill_normals[at];
            known[frontier[at]] = 1U;
        }
        filled += static_cast<u32>(frontier.size());
    }
    return filled;
}

u32 reconcile_seams(Canvas& canvas, Span<const SeamEdge> seams, u32 iterations, bool apply,
                    f32& error_before, f32& error_after) noexcept {
    Array<SeamSample> samples;
    if (!seam_samples(canvas, seams, samples).has_value()) {
        return 0;
    }
    const f32 scale = std::max(mean_covered_luminance(canvas), 1.0e-6F);
    error_before = worst_disagreement(canvas, samples.span()) / scale;
    if (apply) {
        for (u32 iteration = 0; iteration < iterations; ++iteration) {
            for (const SeamSample& sample : samples) {
                solve_sample(canvas, sample);
            }
        }
    }
    error_after = worst_disagreement(canvas, samples.span()) / scale;
    return static_cast<u32>(samples.size());
}

void encode_planes(const Canvas& canvas, LightmapMode mode, LightmapTexels& texels) noexcept {
    for (u32 plane = 0; plane < texels.planes; ++plane) {
        for (u32 y = 0; y < canvas.height; ++y) {
            for (u32 x = 0; x < canvas.width; ++x) {
                const usize index = canvas.index(x, y);
                const Vec4 value =
                    encode_plane(canvas.moments[index], canvas.surfaces[index].normal, mode, plane);
                texels.texels[texels.index(plane, x, y)] =
                    Vec4{rounded(value.x), rounded(value.y), rounded(value.z), rounded(value.w)};
            }
        }
    }
}

}  // namespace cy::rendering::lightmap_bake::detail
