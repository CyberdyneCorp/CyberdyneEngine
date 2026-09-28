// SPDX-License-Identifier: MIT
// After the trace: denoising, border dilation, seam reconciliation, and the planes' encoding.

#include "internal.h"

#include <cy/rendering/denoise/denoiser.h>
#include <cy/rendering/lightmap_bake/asset.h>

#include <algorithm>
#include <cmath>
#include <utility>

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
    static constexpr Channel kShL1[] = {Channel::Mean, Channel::Red, Channel::Green, Channel::Blue};
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
            total += static_cast<f64>(luminance(canvas.moments[index].mean));
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

[[nodiscard]] f32 inner(const TexelMoments& a, const TexelMoments& b) noexcept {
    f32 sum = dot(a.mean, b.mean) + dot(a.luminance_direction, b.luminance_direction) +
              (a.luminance * b.luminance);
    for (u32 channel = 0; channel < 3U; ++channel) {
        sum += dot(a.channel_direction[channel], b.channel_direction[channel]);
    }
    return sum;
}

/// The ridge added to C C^T in the seam solve: a hundred-thousandth of a footprint's squared norm,
/// which is between a half and two.
constexpr f32 kSeamRidge = 1.0e-5F;

/// The seam constraints as a sparse matrix over the texels they touch: row s is sample s's
/// footprint on side a minus its footprint on side b, so `C x = 0` is "every seam sample reads the
/// same value from both sides".
class SeamSystem {
public:
    [[nodiscard]] Status build(Span<const SeamSample> samples) noexcept {
        samples_ = samples;
        for (const SeamSample& sample : samples) {
            for (u32 corner = 0; corner < 4U; ++corner) {
                if (Status added = add_texel(sample.a.texels[corner]); !added) {
                    return added;
                }
                if (Status added = add_texel(sample.b.texels[corner]); !added) {
                    return added;
                }
            }
        }
        std::ranges::sort(texels_);
        const auto kept =
            static_cast<usize>(std::ranges::unique(texels_).begin() - texels_.begin());
        while (texels_.size() > kept) {
            texels_.pop_back();
        }
        return ok();
    }

    [[nodiscard]] usize rows() const noexcept { return samples_.size(); }
    [[nodiscard]] usize columns() const noexcept { return texels_.size(); }
    [[nodiscard]] usize texel(usize column) const noexcept { return texels_[column]; }

    /// out = C x, x over the touched texels.
    void apply(Span<const TexelMoments> x, Span<TexelMoments> out) const noexcept {
        for (usize row = 0; row < samples_.size(); ++row) {
            TexelMoments value;
            for (u32 corner = 0; corner < 4U; ++corner) {
                add_moments(value, x[column_of(samples_[row].a.texels[corner])],
                            samples_[row].a.weights[corner]);
                add_moments(value, x[column_of(samples_[row].b.texels[corner])],
                            -samples_[row].b.weights[corner]);
            }
            out[row] = value;
        }
    }

    /// out = C^T y.
    void apply_transpose(Span<const TexelMoments> y, Span<TexelMoments> out) const noexcept {
        for (TexelMoments& value : out) {
            value = TexelMoments{};
        }
        for (usize row = 0; row < samples_.size(); ++row) {
            for (u32 corner = 0; corner < 4U; ++corner) {
                add_moments(out[column_of(samples_[row].a.texels[corner])], y[row],
                            samples_[row].a.weights[corner]);
                add_moments(out[column_of(samples_[row].b.texels[corner])], y[row],
                            -samples_[row].b.weights[corner]);
            }
        }
    }

private:
    [[nodiscard]] Status add_texel(usize texel) noexcept { return texels_.push_back(texel); }
    [[nodiscard]] usize column_of(usize texel) const noexcept {
        return static_cast<usize>(std::ranges::lower_bound(texels_, texel) - texels_.begin());
    }

    Span<const SeamSample> samples_;
    Array<usize> texels_;
};

/// The smallest change to the seam texels that makes every constraint hold: x = x0 - C^T y with
/// (C C^T) y = C x0, solved by conjugate gradients. Every moment channel is solved at once — the
/// matrix is the same for all of them, so the stacked system is still symmetric positive
/// semi-definite and the inner product is the sum over channels. A row-by-row projection (Kaczmarz)
/// was the first version, and on a seam whose footprints overlap it converged too slowly to
/// matter: 8 sweeps left 4.6% of the mean, 512 still 0.5%.
[[nodiscard]] Status solve_seams(Canvas& canvas, Span<const SeamSample> samples,
                                 u32 iterations) noexcept {
    SeamSystem system;
    if (Status built = system.build(samples); !built) {
        return built;
    }
    Array<TexelMoments> x;
    Array<TexelMoments> z;
    Array<TexelMoments> y;
    Array<TexelMoments> r;
    Array<TexelMoments> p;
    Array<TexelMoments> mp;
    for (Array<TexelMoments>* vector : {&x, &z}) {
        if (Status sized = vector->resize(system.columns()); !sized) {
            return sized;
        }
    }
    for (Array<TexelMoments>* vector : {&y, &r, &p, &mp}) {
        if (Status sized = vector->resize(system.rows()); !sized) {
            return sized;
        }
    }
    for (usize column = 0; column < system.columns(); ++column) {
        x[column] = canvas.moments[system.texel(column)];
    }
    system.apply(x.span(), r.span());  // r = C x0 - (C C^T) 0
    f32 residual = 0.0F;
    for (usize row = 0; row < system.rows(); ++row) {
        p[row] = r[row];
        residual += inner(r[row], r[row]);
    }
    // Converged is relative to where it started. Overlapping footprints make rows dependent, so
    // C C^T is singular, and iterating past convergence divides round-off by round-off; the small
    // ridge keeps the step finite on the way there.
    const f32 converged = residual * 1.0e-12F;
    for (u32 iteration = 0; iteration < iterations && residual > converged; ++iteration) {
        system.apply_transpose(p.span(), z.span());
        system.apply(z.span(), mp.span());
        for (usize row = 0; row < system.rows(); ++row) {
            add_moments(mp[row], p[row], kSeamRidge);
        }
        f32 curvature = 0.0F;
        for (usize row = 0; row < system.rows(); ++row) {
            curvature += inner(p[row], mp[row]);
        }
        if (curvature <= 1.0e-20F) {
            break;
        }
        const f32 step = residual / curvature;
        f32 next = 0.0F;
        for (usize row = 0; row < system.rows(); ++row) {
            add_moments(y[row], p[row], step);
            add_moments(r[row], mp[row], -step);
            next += inner(r[row], r[row]);
        }
        const f32 ratio = next / residual;
        for (usize row = 0; row < system.rows(); ++row) {
            TexelMoments direction = r[row];
            add_moments(direction, p[row], ratio);
            p[row] = direction;
        }
        residual = next;
    }
    system.apply_transpose(y.span(), z.span());
    for (usize column = 0; column < system.columns(); ++column) {
        add_moments(canvas.moments[system.texel(column)], z[column], -1.0F);
    }
    return ok();
}

/// One texel the dilation fills in a pass.
struct Fill {
    usize texel = 0;
    TexelMoments moments;
    Vec3 normal{0.0F, 1.0F, 0.0F};
    u32 chart = kNoOwner;
};

/// The texel `(dx, dy)` from `(x, y)`, or false off the canvas.
[[nodiscard]] bool neighbour_of(const Canvas& canvas, u32 x, u32 y, i32 dx, i32 dy,
                                usize& out) noexcept {
    const i32 nx = static_cast<i32>(x) + dx;
    const i32 ny = static_cast<i32>(y) + dy;
    if (nx < 0 || ny < 0 || std::cmp_greater_equal(nx, canvas.width) ||
        std::cmp_greater_equal(ny, canvas.height)) {
        return false;
    }
    out = canvas.index(static_cast<u32>(nx), static_cast<u32>(ny));
    return true;
}

/// Counts of the charts among at most eight neighbours.
class ChartTally {
public:
    void add(u32 chart) noexcept {
        u32 slot = 0;
        while (slot < distinct_ && charts_[slot] != chart) {
            ++slot;
        }
        if (slot == distinct_) {
            charts_[distinct_++] = chart;
        }
        counts_[slot] += 1U;
    }

    /// The most frequent chart, the lowest id on a tie; `kNoOwner` when none was added.
    [[nodiscard]] u32 majority() const noexcept {
        u32 best = kNoOwner;
        u32 best_count = 0;
        for (u32 slot = 0; slot < distinct_; ++slot) {
            const bool more = counts_[slot] > best_count;
            const bool tie_lower = counts_[slot] == best_count && charts_[slot] < best;
            if (more || tie_lower) {
                best = charts_[slot];
                best_count = counts_[slot];
            }
        }
        return best;
    }

private:
    u32 charts_[8] = {};
    u32 counts_[8] = {};
    u32 distinct_ = 0;
};

/// The chart most of a texel's known neighbours in its own rectangle belong to, the lowest id on a
/// tie so the fill is reproducible; `kNoOwner` when it has none.
[[nodiscard]] u32 majority_chart(const Canvas& canvas, const Array<u8>& known, u32 x, u32 y,
                                 u32 owner) noexcept {
    ChartTally tally;
    for (i32 dy = -1; dy <= 1; ++dy) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            usize neighbour = 0;
            if ((dx == 0 && dy == 0) || !neighbour_of(canvas, x, y, dx, dy, neighbour)) {
                continue;
            }
            if (known[neighbour] != 0U && canvas.surfaces[neighbour].owner == owner) {
                tally.add(canvas.surfaces[neighbour].chart);
            }
        }
    }
    return tally.majority();
}

/// The fill for one unknown texel: the mean of its known neighbours of ONE CHART in its own
/// rectangle. One chart, because a rectangle holds every chart of its object and the padding
/// between two of them is read by both sides' bilinear taps: a fill that mixed a floor's moments
/// with a wall's, and their normals, would be encoded against a normal neither chart has, and a
/// directional texel read at the floor's own normal would no longer be the floor's light.
[[nodiscard]] bool fill_from_neighbours(const Canvas& canvas, const Array<u8>& known, u32 x, u32 y,
                                        Fill& out) noexcept {
    const usize index = canvas.index(x, y);
    const u32 owner = canvas.surfaces[index].owner;
    if (known[index] != 0U || owner == kNoOwner) {
        return false;
    }
    // A BURIED texel is its own chart's surface and is filled from that chart alone: the strip of a
    // floor under a wall standing on it borders the floor's padding, which the first passes fill
    // from whatever chart the unwrap put beside it.
    const u32 own = canvas.surfaces[index].chart;
    const u32 chart = own != kNoOwner ? own : majority_chart(canvas, known, x, y, owner);
    if (chart == kNoOwner) {
        return false;
    }
    TexelMoments sum;
    Vec3 normal_sum{0.0F, 0.0F, 0.0F};
    f32 weight = 0.0F;
    for (i32 dy = -1; dy <= 1; ++dy) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            usize neighbour = 0;
            if (!neighbour_of(canvas, x, y, dx, dy, neighbour)) {
                continue;
            }
            const TexelSurface& texel = canvas.surfaces[neighbour];
            if (known[neighbour] == 0U || texel.owner != owner || texel.chart != chart) {
                continue;
            }
            add_moments(sum, canvas.moments[neighbour], 1.0F);
            normal_sum = normal_sum + texel.normal;
            weight += 1.0F;
        }
    }
    if (weight <= 0.0F) {
        return false;
    }
    out.texel = index;
    out.chart = chart;
    add_moments(out.moments, sum, 1.0F / weight);
    // The normal travels with the light: the directional planes are encoded against it.
    out.normal = normalized_or(normal_sum, Vec3{0.0F, 1.0F, 0.0F});
    return true;
}

[[nodiscard]] Vec4 encode_plane(const TexelMoments& moments, Vec3 normal, LightmapMode mode,
                                u32 plane) noexcept {
    const Vec3 mean = moments.mean;
    if (mode == LightmapMode::Irradiance || (mode == LightmapMode::Directional && plane == 0U)) {
        return Vec4{mean.x, mean.y, mean.z, 1.0F};
    }
    if (mode == LightmapMode::Directional) {
        // The luminance's tilt gradient relative to the luminance, and the geometric normal's own
        // factor, so `1 + v . n` over `w` is one at the geometric normal.
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

Status denoise_moments(Canvas& canvas, LightmapMode mode, u32 passes) noexcept {
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
    // A SHORT CASCADE. With no history the denoiser takes each texel's variance from its 3x3
    // neighbourhood, and across a lightmap chart that neighbourhood holds the light's real gradient
    // as well as the sampling noise: the default five-pass reach flattened a floor lit from one
    // side into its mean. Every texel already carries many samples, so the filter's job is the
    // residual grain, which a reach of a few texels removes.
    denoise::SignalConfig config = denoise::default_config(denoise::SignalKind::IndirectDiffuse);
    config.max_passes = std::max(1U, passes);
    denoiser.configure(denoise::SignalKind::IndirectDiffuse, config);
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
    Array<Fill> fills;
    u32 filled = 0;
    for (u32 pass = 0; pass < passes; ++pass) {
        fills.clear();
        for (u32 y = 0; y < canvas.height; ++y) {
            for (u32 x = 0; x < canvas.width; ++x) {
                Fill fill;
                if (fill_from_neighbours(canvas, known, x, y, fill) &&
                    !fills.push_back(fill).has_value()) {
                    return filled;
                }
            }
        }
        if (fills.empty()) {
            break;
        }
        for (const Fill& fill : fills) {
            canvas.moments[fill.texel] = fill.moments;
            canvas.surfaces[fill.texel].normal = fill.normal;
            canvas.surfaces[fill.texel].chart = fill.chart;
            known[fill.texel] = 1U;
        }
        filled += static_cast<u32>(fills.size());
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
    if (apply && !samples.empty()) {
        if (!solve_seams(canvas, samples.span(), iterations).has_value()) {
            error_after = error_before;
            return 0;
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
