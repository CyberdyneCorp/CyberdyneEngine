// SPDX-License-Identifier: MIT
#include <cy/rendering/upscale/upscale.h>

#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cy::rendering::upscale {
namespace {

/// The scales, in mode order. Quality and Performance are the 1.5x and 2x every vendor upscaler
/// names the same way; Balanced sits between them.
constexpr f32 kModeScales[kUpscaleModeCount] = {1.0F, 0.67F, 0.58F, 0.5F};
constexpr const char* kModeNames[kUpscaleModeCount] = {"native", "quality", "balanced",
                                                       "performance"};

/// The Blackman-Harris-like kernel every temporal resolve in the class uses, `exp(-2.29 d^2)`: one
/// at the sample, a half at 0.55 pixels, and no negative lobe to ring around an edge.
constexpr f32 kKernelFalloff = 2.29F;
/// The kernel this frame's EVIDENCE for an output pixel is weighed with, in output pixels:
/// `exp(-4 d^2)`. Narrower than the spatial one because it decides what each output pixel converges
/// to — the kernel-weighted mean of the samples that landed near it — and measured against a box-
/// filtered supersampled picture this width converges closest (`integration.rendering_upscale`).
constexpr f32 kEvidenceFalloff = 4.0F;
/// The shading-change test: a sample the history cannot explain lies outside the range of the four
/// history texels around it by more than `kShadingMargin` plus `kShadingRelative` of their
/// brightness, in the tone curve's YCoCg. Up to `kShadingKeep` of the nine may be unexplained — a
/// thin feature some frames' samples hit, a silhouette the jitter moved — before the history's
/// trust falls, reaching nothing at `kShadingKeep + kShadingRange`.
constexpr f32 kShadingMargin = 0.02F;
constexpr f32 kShadingRelative = 0.10F;
constexpr f32 kShadingKeep = 0.25F;
constexpr f32 kShadingRange = 0.35F;
/// A thin feature is a ridge of at least this much tone-mapped luma; a lock is made only where the
/// sample that saw it lies within `exp(-4 d^2) >= kLockReach` of the output pixel's centre (0.87
/// output pixels), and it lasts `kLockFrames` frames unless something breaks it.
constexpr f32 kLockContrast = 0.05F;
constexpr f32 kLockReach = 0.05F;
constexpr f32 kLockFrames = 64.0F;
/// A texel that moved, or whose history was discarded, is UNSETTLED for this many still frames
/// after: its history is clipped fully, however little this frame observed it, because what it
/// holds came from somewhere else — the far side of a silhouette that has gone, or a surface that
/// was sliding — and a render-resolution box is the only evidence about it there is.
constexpr f32 kSettleFrames = 4.0F;

/// The view depth a texel no surface covered stands for — far beyond any surface, and finite so
/// two sky texels compare as equal rather than as `inf * (1 - tolerance)`.
constexpr f32 kSkyViewDepth = 1.0e30F;

struct Rgb {
    f32 r = 0.0F;
    f32 g = 0.0F;
    f32 b = 0.0F;
};

[[nodiscard]] Rgb operator+(Rgb a, Rgb b) noexcept {
    return {a.r + b.r, a.g + b.g, a.b + b.b};
}
[[nodiscard]] Rgb operator-(Rgb a, Rgb b) noexcept {
    return {a.r - b.r, a.g - b.g, a.b - b.b};
}
[[nodiscard]] Rgb operator*(Rgb a, f32 s) noexcept {
    return {a.r * s, a.g * s, a.b * s};
}
[[nodiscard]] Rgb operator*(Rgb a, Rgb b) noexcept {
    return {a.r * b.r, a.g * b.g, a.b * b.b};
}
[[nodiscard]] Rgb min3(Rgb a, Rgb b) noexcept {
    return {math::min(a.r, b.r), math::min(a.g, b.g), math::min(a.b, b.b)};
}
[[nodiscard]] Rgb max3(Rgb a, Rgb b) noexcept {
    return {math::max(a.r, b.r), math::max(a.g, b.g), math::max(a.b, b.b)};
}
[[nodiscard]] f32 largest(Rgb a) noexcept {
    return math::max(a.r, math::max(a.g, a.b));
}
[[nodiscard]] Rgb rgb_of(const Vec4& v) noexcept {
    return {v.x, v.y, v.z};
}

/// The reconstruction's tone curve, `x / (1 + max(x))` on the exposed colour: a highlight weighs as
/// much as it will look rather than as much as its radiance, which is what keeps one bright sample
/// from dominating a pixel's mean for sixteen frames. Hue-preserving, and exactly invertible below.
[[nodiscard]] Rgb tonemap(Rgb c, f32 exposure) noexcept {
    const Rgb exposed = c * exposure;
    return exposed * (1.0F / (1.0F + math::max(largest(exposed), 0.0F)));
}

[[nodiscard]] Rgb untonemap(Rgb t, f32 exposure) noexcept {
    const f32 headroom = math::max(1.0F - largest(t), 1.0e-4F);
    return t * (1.0F / (exposure * headroom));
}

[[nodiscard]] Rgb to_ycocg(Rgb c) noexcept {
    return {(0.25F * c.r) + (0.5F * c.g) + (0.25F * c.b), (0.5F * c.r) - (0.5F * c.b),
            (-0.25F * c.r) + (0.5F * c.g) - (0.25F * c.b)};
}

[[nodiscard]] Rgb from_ycocg(Rgb c) noexcept {
    return {c.r + c.g - c.b, c.r + c.b, c.r - c.g - c.b};
}

[[nodiscard]] f32 row_dot(const f32 row[4], f32 x, f32 y, f32 z, f32 w) noexcept {
    return (row[0] * x) + (row[1] * y) + (row[2] * z) + (row[3] * w);
}

[[nodiscard]] u32 clamp_index(i32 value, u32 extent) noexcept {
    if (value < 0) {
        return 0;
    }
    const auto last = static_cast<i32>(extent) - 1;
    return static_cast<u32>(value > last ? last : value);
}

/// Reversed-Z depth to positive view depth in metres. Zero, the cleared far plane, is the sky.
[[nodiscard]] f32 view_depth(const UpscaleConstants& constants, f32 depth) noexcept {
    const f32 denominator = depth + constants.sample[2];
    return denominator > 0.0F ? math::min(constants.sample[3] / denominator, kSkyViewDepth)
                              : kSkyViewDepth;
}

/// Where last frame's clip space put the unjittered point at `uv` and reversed-Z `depth`: x, y in
/// NDC and z the depth, all divided through.
struct PreviousClip {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 z = 0.0F;
};

[[nodiscard]] PreviousClip previous_clip(const UpscaleConstants& constants, Vec2 uv,
                                         f32 depth) noexcept {
    const f32 cx = (uv.x * 2.0F) - 1.0F;
    const f32 cy = 1.0F - (uv.y * 2.0F);
    const f32 px = row_dot(constants.current_to_previous[0], cx, cy, depth, 1.0F);
    const f32 py = row_dot(constants.current_to_previous[1], cx, cy, depth, 1.0F);
    const f32 pz = row_dot(constants.current_to_previous[2], cx, cy, depth, 1.0F);
    const f32 pw = row_dot(constants.current_to_previous[3], cx, cy, depth, 1.0F);
    const f32 w = std::fabs(pw) > 1.0e-8F ? pw : 1.0e-8F;
    return {px / w, py / w, pz / w};
}

/// The motion of the unjittered point at `uv` and `depth` under the camera alone, in the prepass's
/// convention: normalised screen space, pointing at the past.
[[nodiscard]] Vec2 camera_motion(const UpscaleConstants& constants, Vec2 uv, f32 depth) noexcept {
    const PreviousClip previous = previous_clip(constants, uv, depth);
    const Vec2 history{(previous.x * 0.5F) + 0.5F, 0.5F - (previous.y * 0.5F)};
    return Vec2{history.x - uv.x, history.y - uv.y};
}

/// How much of a texel's colour translucency added over the opaque surface: the largest channel's
/// change through the tone curve, four times over so a quarter of the pixel is fully reactive.
[[nodiscard]] f32 reactive_of(Rgb final_colour, Rgb opaque, f32 exposure) noexcept {
    const Rgb difference = tonemap(final_colour, exposure) - tonemap(opaque, exposure);
    const f32 change = math::max(std::fabs(difference.r),
                                 math::max(std::fabs(difference.g), std::fabs(difference.b)));
    return math::saturate(change * 4.0F);
}

/// The Catmull-Rom weights of the four taps around fraction `f`.
void catmull_rom_weights(f32 f, f32 (&w)[4]) noexcept {
    w[0] = f * (-0.5F + (f * (1.0F - (0.5F * f))));
    w[1] = 1.0F + (f * f * (-2.5F + (1.5F * f)));
    w[2] = f * (0.5F + (f * (2.0F - (1.5F * f))));
    w[3] = f * f * (-0.5F + (0.5F * f));
}

/// The history at `uv`, bicubic over the sixteen texels around it with clamped edges. Bicubic
/// rather than bilinear because a history resampled every frame for sixteen frames is blurred
/// sixteen times, and a bilinear fetch is the blurrier of the two by a visible margin.
[[nodiscard]] Vec4 sample_history(Span<const Vec4> history, u32 width, u32 height,
                                  Vec2 uv) noexcept {
    const f32 px = (uv.x * static_cast<f32>(width)) - 0.5F;
    const f32 py = (uv.y * static_cast<f32>(height)) - 0.5F;
    const f32 fx = std::floor(px);
    const f32 fy = std::floor(py);
    f32 wx[4];
    f32 wy[4];
    catmull_rom_weights(px - fx, wx);
    catmull_rom_weights(py - fy, wy);
    const auto ix = static_cast<i32>(fx) - 1;
    const auto iy = static_cast<i32>(fy) - 1;
    Vec4 sum{0.0F, 0.0F, 0.0F, 0.0F};
    for (i32 b = 0; b < 4; ++b) {
        const u32 ty = clamp_index(iy + b, height);
        for (i32 a = 0; a < 4; ++a) {
            const u32 tx = clamp_index(ix + a, width);
            const f32 weight = wx[a] * wy[b];
            const Vec4& texel = history[(static_cast<usize>(ty) * width) + tx];
            sum = Vec4{sum.x + (texel.x * weight), sum.y + (texel.y * weight),
                       sum.z + (texel.z * weight), sum.w + (texel.w * weight)};
        }
    }
    return sum;
}

/// The render texel whose sample is nearest the unjittered point `p` (render pixels): sample(i) is
/// i + 0.5 + offset, so the nearest is floor(p - offset).
[[nodiscard]] i32 nearest_texel(f32 p, f32 offset) noexcept {
    return static_cast<i32>(std::floor(p - offset));
}

}  // namespace

const char* upscale_mode_name(UpscaleMode mode) noexcept {
    const auto index = static_cast<u32>(mode);
    return index < kUpscaleModeCount ? kModeNames[index] : "unknown";
}

bool parse_upscale_mode(const char* text, UpscaleMode& out) noexcept {
    if (text == nullptr) {
        return false;
    }
    for (u32 index = 0; index < kUpscaleModeCount; ++index) {
        if (std::strcmp(text, kModeNames[index]) == 0) {
            out = static_cast<UpscaleMode>(index);
            return true;
        }
    }
    return false;
}

f32 upscale_mode_scale(UpscaleMode mode) noexcept {
    const auto index = static_cast<u32>(mode);
    return index < kUpscaleModeCount ? kModeScales[index] : 1.0F;
}

Extent2D render_extent_for(Extent2D output, f32 scale) noexcept {
    const f32 clamped = (scale > 0.0F) ? math::min(scale, 1.0F) : 1.0F;
    const auto axis = [clamped](u32 extent) noexcept {
        const auto rounded =
            static_cast<u32>(std::lround(static_cast<f64>(extent) * static_cast<f64>(clamped)));
        if (extent == 0U) {
            return 0U;
        }
        return rounded == 0U ? 1U : math::min(rounded, extent);
    };
    return Extent2D{axis(output.width), axis(output.height)};
}

u32 jitter_phase_count(u32 output_width, u32 render_width) noexcept {
    if (render_width == 0U || output_width <= render_width) {
        return 8U;
    }
    const f64 ratio = static_cast<f64>(output_width) / static_cast<f64>(render_width);
    const auto phases = static_cast<u32>(std::ceil((8.0 * ratio * ratio) - 1.0e-9));
    return math::max(phases, 8U);
}

JitterConfig jitter_config_for(Extent2D output, Extent2D render) noexcept {
    JitterConfig config;
    config.length = jitter_phase_count(output.width, render.width);
    return config;
}

f32 texture_lod_bias(u32 render_width, u32 output_width) noexcept {
    if (render_width == 0U || output_width == 0U) {
        return 0.0F;
    }
    return std::log2(static_cast<f32>(render_width) / static_cast<f32>(output_width));
}

f32 dynamic_render_scale(UpscaleMode mode, f32 arbiter_scale,
                         const DynamicResolutionBounds& bounds) noexcept {
    const f32 allocation = arbiter_scale > 0.0F ? arbiter_scale : 1.0F;
    const f32 lowest = math::clamp(bounds.minimum_scale, 0.05F, 1.0F);
    const f32 highest = math::clamp(bounds.maximum_scale, lowest, 1.0F);
    return math::clamp(upscale_mode_scale(mode) * allocation, lowest, highest);
}

Extent2D dynamic_render_extent(Extent2D output, UpscaleMode mode, f32 arbiter_scale,
                               const DynamicResolutionBounds& bounds) noexcept {
    const f32 scale = dynamic_render_scale(mode, arbiter_scale, bounds);
    Extent2D extent = render_extent_for(output, scale);
    const u32 granule = math::max(bounds.granularity, 1U);
    if (extent.width > granule) {
        extent.width = (extent.width / granule) * granule;
    } else {
        extent.width = math::min(granule, output.width);
    }
    if (output.width > 0U) {
        const f64 aspect = static_cast<f64>(output.height) / static_cast<f64>(output.width);
        const auto height = static_cast<u32>(std::lround(static_cast<f64>(extent.width) * aspect));
        extent.height = math::clamp(height, 1U, output.height);
    }
    return extent;
}

Expected<UpscaleConstants, Error> make_upscale_constants(const UpscaleSettings& settings,
                                                         const UpscaleView& view) noexcept {
    if (view.render.width == 0 || view.render.height == 0 || view.output.width == 0 ||
        view.output.height == 0) {
        return fail(ErrorCode::InvalidArgument, "upscale: an extent has no pixels");
    }
    if (view.render.width > view.output.width || view.render.height > view.output.height) {
        return fail(ErrorCode::InvalidArgument,
                    "upscale: the render extent is larger than the output it is reconstructed to");
    }
    if (!(settings.max_history_weight >= 1.0F) || !(settings.moving_history_weight >= 1.0F) ||
        settings.moving_history_weight > settings.max_history_weight ||
        !(settings.motion_falloff_pixels > 0.0F) || !(settings.still_clip_gamma > 0.0F) ||
        !(settings.moving_clip_gamma > 0.0F) || !(settings.depth_tolerance >= 0.0F) ||
        !(settings.exposure > 0.0F) || settings.sharpness < 0.0F || settings.sharpness > 1.0F ||
        settings.reactive_strength < 0.0F || settings.reactive_strength > 1.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "upscale: a history cap is below one sample or the moving one above the still "
                    "one, a gamma, falloff or exposure is not positive, or the sharpness, reactive "
                    "strength or depth tolerance is out of range");
    }
    Expected<Mat4, Error> inverse_clip = inverse(view.relative_to_clip);
    if (!inverse_clip.has_value()) {
        return fail(ErrorCode::InvalidArgument,
                    "upscale: the camera-relative to clip transform is not invertible");
    }

    UpscaleConstants constants;
    const Mat4 current_to_previous = view.previous_relative_to_clip * *inverse_clip;
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            constants.current_to_previous[row][column] = current_to_previous.columns[column][row];
        }
    }
    if (view.output.width > 0xFFFFU || view.output.height > 0xFFFFU) {
        return fail(ErrorCode::InvalidArgument,
                    "upscale: an extent is wider than the constants' sixteen bits an axis");
    }
    constants.extents[0] = pack_extent(view.render);
    constants.extents[1] = pack_extent(view.output);
    // THE SIGN, decided once. See the header comment: texel i's sample is at i + 0.5 + this.
    constants.sample[0] = -view.jitter.x;
    constants.sample[1] = view.jitter.y;
    constants.sample[2] = view.projection.columns[2].z;
    constants.sample[3] = view.projection.columns[3].z;
    constants.history[0] = settings.max_history_weight;
    constants.history[1] = settings.moving_history_weight;
    constants.history[2] = settings.motion_falloff_pixels;
    constants.history[3] = settings.depth_tolerance;
    constants.control[0] = settings.still_clip_gamma;
    constants.control[1] = settings.moving_clip_gamma;
    constants.control[2] = settings.exposure;
    constants.control[3] = settings.reactive_strength;
    u32 flags = 0;
    flags |= view.history_valid ? kUpscaleHistoryValid : 0U;
    flags |= view.velocity ? kUpscaleVelocity : 0U;
    flags |= view.reactive ? kUpscaleReactive : 0U;
    flags |= settings.reject_disocclusion ? kUpscaleRejectDisocclusion : 0U;
    flags |= settings.detect_shading_change ? kUpscaleShadingChange : 0U;
    flags |= settings.lock_thin_features ? kUpscaleLocks : 0U;
    constants.extents[2] = 0U;
    constants.extents[3] = flags;
    return constants;
}

SharpenConstants make_sharpen_constants(const UpscaleSettings& settings, Extent2D output) noexcept {
    SharpenConstants constants;
    constants.extent[0] = output.width;
    constants.extent[1] = output.height;
    constants.control[0] = math::saturate(settings.sharpness);
    constants.control[1] = settings.exposure > 0.0F ? settings.exposure : 1.0F;
    return constants;
}

namespace {

/// What the 3x3 render texels around an output pixel's nearest sample say about it.
struct Neighbourhood {
    /// The spatial reconstruction: a kernel in RENDER pixels, what a pixel with no history shows.
    Rgb spatial;
    /// This frame's evidence for the output pixel: each sample's linear colour by the kernel in
    /// OUTPUT pixels, unnormalised, and the sum of those weights.
    Rgb evidence;
    f32 evidence_weight = 0.0F;
    /// The moments of the tone-mapped YCoCg colours, which the history is clipped to.
    Rgb mean;
    Rgb deviation;
    /// The nearest surface of the nine (reversed-Z: the largest depth), whose motion the pixel
    /// takes.
    f32 closest_depth = -1.0F;
    usize closest = 0;
    /// The nearest of the 5x5 around the same sample: the disocclusion test's tolerance for which
    /// of a silhouette's samples this frame's jitter happened to pick.
    f32 near_depth = -1.0F;
    f32 reactive = 0.0F;
    /// Each tap's sample position (normalised) and tone-mapped luma, row by row; tap 4 is the
    /// sample nearest the pixel's centre, and `centre_weight` its output-pixel kernel weight.
    Vec2 sample_uvs[9];
    f32 luma[9] = {};
    Rgb ycocg[9];
    f32 centre_weight = 0.0F;
};

[[nodiscard]] Neighbourhood gather(const UpscaleInputs& inputs, const UpscaleConstants& constants,
                                   Vec2 p, Vec2 ratio, f32 exposure) noexcept {
    const u32 render_width = constants.render().width;
    const u32 render_height = constants.render().height;
    const i32 base_x = nearest_texel(p.x, constants.sample[0]);
    const i32 base_y = nearest_texel(p.y, constants.sample[1]);
    const bool has_reactive = (constants.flags() & kUpscaleReactive) != 0U;
    Neighbourhood n;
    f32 spatial_weight = 0.0F;
    Rgb first_moment;
    Rgb second_moment;
    u32 tap = 0;
    for (i32 dy = -1; dy <= 1; ++dy) {
        const u32 ty = clamp_index(base_y + dy, render_height);
        for (i32 dx = -1; dx <= 1; ++dx, ++tap) {
            const u32 tx = clamp_index(base_x + dx, render_width);
            const usize at = (static_cast<usize>(ty) * render_width) + tx;
            const Rgb colour = rgb_of(inputs.color[at]);
            const Vec2 sample{static_cast<f32>(tx) + 0.5F + constants.sample[0],
                              static_cast<f32>(ty) + 0.5F + constants.sample[1]};
            n.sample_uvs[tap] = Vec2{sample.x / static_cast<f32>(render_width),
                                     sample.y / static_cast<f32>(render_height)};
            const Vec2 render_distance{p.x - sample.x, p.y - sample.y};
            const Vec2 output_distance{render_distance.x * ratio.x, render_distance.y * ratio.y};
            const f32 spatial_tap =
                std::exp(-kKernelFalloff * ((render_distance.x * render_distance.x) +
                                            (render_distance.y * render_distance.y)));
            const f32 evidence_tap =
                std::exp(-kEvidenceFalloff * ((output_distance.x * output_distance.x) +
                                              (output_distance.y * output_distance.y)));
            n.spatial = n.spatial + (colour * spatial_tap);
            spatial_weight += spatial_tap;
            n.evidence = n.evidence + (colour * evidence_tap);
            n.evidence_weight += evidence_tap;
            const Rgb ycocg = to_ycocg(tonemap(colour, exposure));
            first_moment = first_moment + ycocg;
            second_moment = second_moment + (ycocg * ycocg);
            n.luma[tap] = ycocg.r;
            n.ycocg[tap] = ycocg;
            if (tap == 4U) {
                n.centre_weight = evidence_tap;
            }
            // An edge pixel moves with the foreground, which is the surface a viewer tracks; the
            // background it reveals is the disocclusion test's to reject.
            const f32 depth = inputs.depth[at];
            if (depth > n.closest_depth) {
                n.closest_depth = depth;
                n.closest = at;
            }
            if (has_reactive) {
                n.reactive =
                    math::max(n.reactive, reactive_of(colour, rgb_of(inputs.opaque[at]), exposure));
            }
        }
    }
    n.near_depth = n.closest_depth;
    for (i32 dy = -2; dy <= 2; ++dy) {
        for (i32 dx = -2; dx <= 2; ++dx) {
            if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1) {
                continue;
            }
            const usize at =
                (static_cast<usize>(clamp_index(base_y + dy, render_height)) * render_width) +
                clamp_index(base_x + dx, render_width);
            n.near_depth = math::max(n.near_depth, inputs.depth[at]);
        }
    }
    n.spatial = n.spatial * (1.0F / spatial_weight);
    n.mean = first_moment * (1.0F / 9.0F);
    const Rgb variance = (second_moment * (1.0F / 9.0F)) - (n.mean * n.mean);
    n.deviation =
        Rgb{std::sqrt(math::max(variance.r, 0.0F)), std::sqrt(math::max(variance.g, 0.0F)),
            std::sqrt(math::max(variance.b, 0.0F))};
    return n;
}

/// A THIN FEATURE: the centre sample is a ridge — brighter, or darker, than both of its neighbours
/// across it — in one of the four directions through it. A line one sample wide is a ridge across
/// its own direction; an edge or a flat region is a ridge in none.
[[nodiscard]] bool thin_feature(const Neighbourhood& n) noexcept {
    constexpr u32 kOpposite[4][2] = {{3, 5}, {1, 7}, {0, 8}, {2, 6}};
    const f32 centre = n.luma[4];
    return std::ranges::any_of(kOpposite, [&n, centre](const auto& pair) {
        const f32 a = n.luma[pair[0]];
        const f32 b = n.luma[pair[1]];
        return centre - math::max(a, b) > kLockContrast || math::min(a, b) - centre > kLockContrast;
    });
}

/// The previous state at `uv`: the nearest texel's lock and depth, and the nearest depth any of the
/// 3x3 texels around it kept.
struct PreviousState {
    f32 lock = 0.0F;
    f32 depth = 0.0F;
    f32 near_depth = 0.0F;
};

[[nodiscard]] PreviousState previous_state_at(const UpscaleInputs& inputs, u32 width, u32 height,
                                              Vec2 uv) noexcept {
    const auto cx = static_cast<i32>(std::floor(uv.x * static_cast<f32>(width)));
    const auto cy = static_cast<i32>(std::floor(uv.y * static_cast<f32>(height)));
    PreviousState out;
    const Vec2 centre =
        inputs.previous_state[(static_cast<usize>(clamp_index(cy, height)) * width) +
                              clamp_index(cx, width)];
    out.lock = centre.x;
    out.depth = centre.y;
    out.near_depth = centre.y;
    for (i32 dy = -1; dy <= 1; ++dy) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            const Vec2 texel =
                inputs.previous_state[(static_cast<usize>(clamp_index(cy + dy, height)) * width) +
                                      clamp_index(cx + dx, width)];
            out.near_depth = math::max(out.near_depth, texel.y);
        }
    }
    return out;
}

/// How many of this frame's nine samples the history cannot explain: each sample against the
/// range of the four history texels around its own position, displaced by the motion, in the tone
/// curve's YCoCg. A light that came on, a shadow that moved or a surface replaced by another at the
/// same depth leaves most of them outside it; an edge the jitter moved, or a thin feature most
/// frames miss, leaves a few — the history's own anti-aliased texels span both sides of an edge.
[[nodiscard]] f32 unexplained_fraction(const UpscaleInputs& inputs, const Neighbourhood& n,
                                       u32 width, u32 height, Vec2 motion, f32 exposure) noexcept {
    u32 unexplained = 0;
    for (u32 tap = 0; tap < 9U; ++tap) {
        const f32 px = ((n.sample_uvs[tap].x + motion.x) * static_cast<f32>(width)) - 0.5F;
        const f32 py = ((n.sample_uvs[tap].y + motion.y) * static_cast<f32>(height)) - 0.5F;
        const auto ix = static_cast<i32>(std::floor(px));
        const auto iy = static_cast<i32>(std::floor(py));
        Rgb low{1.0e9F, 1.0e9F, 1.0e9F};
        Rgb high{-1.0e9F, -1.0e9F, -1.0e9F};
        for (i32 b = 0; b < 2; ++b) {
            for (i32 a = 0; a < 2; ++a) {
                const Vec4& texel =
                    inputs.history[(static_cast<usize>(clamp_index(iy + b, height)) * width) +
                                   clamp_index(ix + a, width)];
                const Rgb value = to_ycocg(tonemap(max3(rgb_of(texel), Rgb{}), exposure));
                low = min3(low, value);
                high = max3(high, value);
            }
        }
        const f32 margin = kShadingMargin + (kShadingRelative * math::max(high.r, 0.0F));
        const Rgb sample = n.ycocg[tap];
        const bool outside = sample.r < low.r - margin || sample.r > high.r + margin ||
                             sample.g < low.g - margin || sample.g > high.g + margin ||
                             sample.b < low.b - margin || sample.b > high.b + margin;
        unexplained += outside ? 1U : 0U;
    }
    return static_cast<f32>(unexplained) * (1.0F / 9.0F);
}

/// How far, in [0, 1], `history` is pulled toward the neighbourhood's box: the fraction of the
/// way from it to the box's surface, scaled by how much this frame observed the pixel.
[[nodiscard]] Rgb rectify(const Neighbourhood& n, Rgb history, f32 gamma, f32 observed,
                          f32 exposure) noexcept {
    const Rgb extent = n.deviation * gamma;
    const Rgb offset = to_ycocg(tonemap(history, exposure)) - n.mean;
    const f32 outside = math::max(std::fabs(offset.r) / math::max(extent.r, 1.0e-5F),
                                  math::max(std::fabs(offset.g) / math::max(extent.g, 1.0e-5F),
                                            std::fabs(offset.b) / math::max(extent.b, 1.0e-5F)));
    const f32 pull = outside > 1.0F ? observed * (1.0F - (1.0F / outside)) : 0.0F;
    if (pull <= 0.0F) {
        return history;
    }
    return untonemap(from_ycocg(n.mean + (offset * (1.0F - pull))), exposure);
}

}  // namespace

UpscaleTexel upscale_reference_at(const UpscaleInputs& inputs, const UpscaleConstants& constants,
                                  u32 x, u32 y) noexcept {
    const u32 output_columns = constants.output().width;
    const u32 output_rows = constants.output().height;
    const auto output_width = static_cast<f32>(output_columns);
    const auto output_height = static_cast<f32>(output_rows);
    const f32 exposure = constants.control[2];
    const u32 flags = constants.flags();
    const Vec2 ratio{output_width / static_cast<f32>(constants.render().width),
                     output_height / static_cast<f32>(constants.render().height)};
    const Vec2 uv{(static_cast<f32>(x) + 0.5F) / output_width,
                  (static_cast<f32>(y) + 0.5F) / output_height};
    const Vec2 p{uv.x * static_cast<f32>(constants.render().width),
                 uv.y * static_cast<f32>(constants.render().height)};
    const Neighbourhood n = gather(inputs, constants, p, ratio, exposure);

    // THE MOTION. The prepass's vector where a surface wrote one; where none did — the sky, or a
    // frame whose prepass writes no velocity — the camera's own motion through the pixel's depth.
    const Vec2 motion = ((flags & kUpscaleVelocity) != 0U && n.closest_depth > 0.0F)
                            ? inputs.velocity[n.closest]
                            : camera_motion(constants, uv, n.closest_depth);
    const Vec2 history_uv{uv.x + motion.x, uv.y + motion.y};
    const f32 motion_pixels = std::sqrt((motion.x * output_width * motion.x * output_width) +
                                        (motion.y * output_height * motion.y * output_height));
    const f32 moving = math::saturate(motion_pixels / constants.history[2]);
    const f32 cap = math::lerp(constants.history[0], constants.history[1], moving);

    f32 weight = 0.0F;
    f32 lock = 0.0F;
    f32 kept_depth = n.closest_depth;
    Rgb history_linear;
    const bool on_screen = history_uv.x >= 0.0F && history_uv.x <= 1.0F && history_uv.y >= 0.0F &&
                           history_uv.y <= 1.0F;
    if ((flags & kUpscaleHistoryValid) != 0U && on_screen) {
        const Vec4 history =
            sample_history(inputs.history, output_columns, output_rows, history_uv);
        const PreviousState previous =
            previous_state_at(inputs, output_columns, output_rows, history_uv);
        weight = math::min(math::max(history.w, 0.0F), cap);
        f32 trust = 1.0F;
        bool disoccluded = false;

        // DISOCCLUSION: the history around this texel holds a surface — the depths the 3x3 history
        // texels kept beside their colours — nearer than anything within two render pixels of this
        // frame's sample would have been last frame. Whatever stood there has gone, and its colour
        // is not this surface's. The depth is the history's OWN, kept at the output extent with the
        // same nearest-of-nine rule this frame applies, so a silhouette's anti-aliased fringe
        // carries its occluder's depth and is rejected with it when the occluder leaves; and the
        // texels AROUND are asked, so the pixel where an object stood on the floor, whose own depth
        // was the floor's, goes with the object.
        //
        // The test is against the nearest of the 5x5 rather than the 3x3 the depth was kept from,
        // so the jitter moving a silhouette in and out of a pixel's nine does not reject it. A
        // history that passes ONLY because of that margin — its occluder has left the nine but not
        // yet the twenty-five — keeps the occluder's depth rather than this frame's, so that the
        // frame it leaves the twenty-five rejects the colour it left behind. Kept one frame longer
        // across a moving camera, it is compared a frame later than it was written: the cost is a
        // fringe pixel's history judged against a slightly stale depth.
        if ((flags & kUpscaleRejectDisocclusion) != 0U) {
            const f32 tolerance = 1.0F - constants.history[3];
            const PreviousClip near = previous_clip(constants, uv, n.near_depth);
            const PreviousClip closest = previous_clip(constants, uv, n.closest_depth);
            if (view_depth(constants, previous.near_depth) <
                view_depth(constants, near.z) * tolerance) {
                trust = 0.0F;
                disoccluded = true;
            } else if (view_depth(constants, previous.depth) <
                       view_depth(constants, closest.z) * tolerance) {
                kept_depth = previous.depth;
            }
        }
        // A CHANGE OF SHADING: a light that came on, a shadow that moved, or a surface replaced by
        // another at the same depth leaves most of this frame's samples outside what the history
        // around them holds; a thin feature most samples miss, or an edge the jitter moved, leaves
        // a few.
        if ((flags & kUpscaleShadingChange) != 0U) {
            const f32 change =
                unexplained_fraction(inputs, n, output_columns, output_rows, motion, exposure);
            trust =
                math::min(trust, math::saturate(1.0F - ((change - kShadingKeep) / kShadingRange)));
        }
        // REACTIVE: translucency and particles move without a motion vector of their own.
        trust *= 1.0F - (n.reactive * constants.control[3]);
        weight *= trust;

        // THE STATE'S FIRST CHANNEL: a lock in (0, 1], or an unsettled count in [-1, 0). A lock
        // protects a thin feature's history from a neighbourhood whose samples missed it; it
        // follows the history, fades over `kLockFrames`, and is broken by anything that took the
        // history's trust away. Motion and disocclusion break it by UNSETTLING the texel: what it
        // holds came from somewhere else, and it is clipped fully until it has been still for
        // `kSettleFrames`.
        if (disoccluded || moving > 0.5F) {
            lock = -1.0F;
        } else if (previous.lock < 0.0F) {
            lock = math::min(previous.lock + (1.0F / kSettleFrames), 0.0F);
        } else if (trust < 0.5F) {
            lock = 0.0F;
        } else if ((flags & kUpscaleLocks) != 0U) {
            lock = math::max(previous.lock - (1.0F / kLockFrames), 0.0F);
        }
        const bool unsettled = previous.lock < 0.0F || lock < 0.0F;

        // RECTIFICATION: clip toward this frame's neighbourhood, into a box of `gamma` standard
        // deviations — wide on a still pixel, tight on a moving one — as far as this frame observed
        // the pixel: a sample on its centre clips fully, a pixel no sample came near not at all,
        // because a render-resolution box knows nothing of sub-render-pixel detail. A moving pixel
        // always clips; a locked one does not.
        const f32 gamma = math::lerp(constants.control[0], constants.control[1], moving);
        const f32 observed =
            unsettled ? 1.0F : math::max(moving, math::saturate(n.evidence_weight)) * (1.0F - lock);
        history_linear = rectify(n, max3(rgb_of(history), Rgb{}), gamma, observed, exposure);
    }
    if ((flags & kUpscaleLocks) != 0U && lock >= 0.0F && n.centre_weight >= kLockReach &&
        thin_feature(n) && moving <= 0.5F) {
        lock = 1.0F;
    }

    UpscaleTexel texel;
    texel.state = Vec2{lock, kept_depth};
    if (weight <= 0.0F) {
        // NO HISTORY: the spatial reconstruction, holding this frame's evidence as its weight.
        texel.history =
            Vec4{n.spatial.r, n.spatial.g, n.spatial.b, math::min(n.evidence_weight, cap)};
        return texel;
    }
    // THE ACCUMULATION, in linear light: the history's colour as `weight` samples' worth and this
    // frame's samples each by its own kernel weight — so a pixel converges to the kernel-weighted
    // mean of every sample that landed near it, which is a supersampled pixel.
    const Rgb blended =
        ((history_linear * weight) + n.evidence) * (1.0F / (weight + n.evidence_weight));
    texel.history =
        Vec4{blended.r, blended.g, blended.b, math::min(weight + n.evidence_weight, cap)};
    return texel;
}

Status upscale_reference(const UpscaleInputs& inputs, const UpscaleConstants& constants,
                         Span<Vec4> out, Span<Vec2> state) noexcept {
    const Extent2D render_extent = constants.render();
    const Extent2D output_extent = constants.output();
    const usize render = static_cast<usize>(render_extent.width) * render_extent.height;
    const usize output = static_cast<usize>(output_extent.width) * output_extent.height;
    if (render == 0 || output == 0 || inputs.color.size() != render ||
        inputs.depth.size() != render || out.size() != output ||
        (!state.empty() && state.size() != output)) {
        return fail(ErrorCode::InvalidArgument,
                    "upscale: the colour and depth are not the render extent or an output span is "
                    "not the output extent");
    }
    const u32 flags = constants.flags();
    if (((flags & kUpscaleVelocity) != 0U && inputs.velocity.size() != render) ||
        ((flags & kUpscaleReactive) != 0U && inputs.opaque.size() != render) ||
        ((flags & kUpscaleHistoryValid) != 0U &&
         (inputs.history.size() != output || inputs.previous_state.size() != output))) {
        return fail(ErrorCode::InvalidArgument,
                    "upscale: a flagged input is missing or is not its extent");
    }
    for (u32 y = 0; y < output_extent.height; ++y) {
        for (u32 x = 0; x < output_extent.width; ++x) {
            const usize at = (static_cast<usize>(y) * output_extent.width) + x;
            const UpscaleTexel texel = upscale_reference_at(inputs, constants, x, y);
            out[at] = texel.history;
            if (!state.empty()) {
                state[at] = texel.state;
            }
        }
    }
    return ok();
}

Status sharpen_reference(Span<const Vec4> history, const SharpenConstants& constants,
                         Span<Vec4> out) noexcept {
    const u32 width = constants.extent[0];
    const u32 height = constants.extent[1];
    const usize pixels = static_cast<usize>(width) * height;
    if (pixels == 0 || history.size() != pixels || out.size() != pixels) {
        return fail(ErrorCode::InvalidArgument, "upscale: the sharpen spans are not the output");
    }
    const f32 sharpness = constants.control[0];
    const f32 exposure = constants.control[1];
    const auto at = [&](i32 px, i32 py) noexcept {
        return rgb_of(history[(static_cast<usize>(clamp_index(py, height)) * width) +
                              clamp_index(px, width)]);
    };
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const auto ix = static_cast<i32>(x);
            const auto iy = static_cast<i32>(y);
            const Rgb centre_linear = at(ix, iy);
            Rgb result = centre_linear;
            if (sharpness > 0.0F) {
                // AMD's robust contrast-adaptive sharpening (FidelityFX RCAS): the largest negative
                // lobe the cross's own minimum and maximum allow without clipping, scaled by the
                // sharpness — in the tone curve's space, where the bounds are 0 and 1.
                const Rgb b = tonemap(at(ix, iy - 1), exposure);
                const Rgb d = tonemap(at(ix - 1, iy), exposure);
                const Rgb e = tonemap(centre_linear, exposure);
                const Rgb f = tonemap(at(ix + 1, iy), exposure);
                const Rgb h = tonemap(at(ix, iy + 1), exposure);
                const Rgb low = min3(min3(min3(b, d), min3(e, f)), h);
                const Rgb high = max3(max3(max3(b, d), max3(e, f)), h);
                const auto lobe_of = [](f32 mn, f32 mx) noexcept {
                    const f32 hit_low = mn / math::max(4.0F * mx, 1.0e-6F);
                    const f32 hit_high = (1.0F - mx) / math::min((4.0F * mn) - 4.0F, -1.0e-6F);
                    return math::max(-hit_low, hit_high);
                };
                const f32 lobe_channels =
                    math::max(lobe_of(low.r, high.r),
                              math::max(lobe_of(low.g, high.g), lobe_of(low.b, high.b)));
                const f32 lobe = math::max(-0.1875F, math::min(lobe_channels, 0.0F)) * sharpness;
                const Rgb cross = b + d + f + h;
                const Rgb sharpened = ((cross * lobe) + e) * (1.0F / ((4.0F * lobe) + 1.0F));
                result = untonemap(max3(sharpened, Rgb{}), exposure);
            }
            out[(static_cast<usize>(y) * width) + x] = Vec4{result.r, result.g, result.b, 1.0F};
        }
    }
    return ok();
}

ResampledTexel resample_reference_at(const UpscaleInputs& inputs, const UpscaleConstants& constants,
                                     u32 x, u32 y) noexcept {
    const u32 render_width = constants.render().width;
    const u32 render_height = constants.render().height;
    const Vec2 uv{(static_cast<f32>(x) + 0.5F) / static_cast<f32>(constants.output().width),
                  (static_cast<f32>(y) + 0.5F) / static_cast<f32>(constants.output().height)};
    const u32 tx = clamp_index(
        nearest_texel(uv.x * static_cast<f32>(render_width), constants.sample[0]), render_width);
    const u32 ty = clamp_index(
        nearest_texel(uv.y * static_cast<f32>(render_height), constants.sample[1]), render_height);
    const usize at = (static_cast<usize>(ty) * render_width) + tx;
    ResampledTexel texel;
    texel.depth = inputs.depth[at];
    const bool has_velocity = (constants.flags() & kUpscaleVelocity) != 0U;
    texel.velocity = (has_velocity && texel.depth > 0.0F)
                         ? inputs.velocity[at]
                         : camera_motion(constants, uv, texel.depth);
    return texel;
}

}  // namespace cy::rendering::upscale
