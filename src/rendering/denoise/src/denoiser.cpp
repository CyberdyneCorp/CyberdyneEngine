#include <cy/rendering/denoise/denoiser.h>

#include <cy/core/jobs/types.h>
#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cy::rendering::denoise {
namespace {

/// Rec. 709 luminance for a radiance signal; the value itself for a visibility one.
///
/// The one place `SignalDomain` is read in the filter, and it is read as a FIELD rather than as a
/// signal kind — which is the whole structural rule of this module. Blurring an occlusion term as a
/// colour is what loses contact detail, and this is where that is prevented.
[[nodiscard]] f32 signal_scalar(Vec3 value, SignalDomain domain) noexcept {
    if (domain == SignalDomain::Visibility) {
        return value.x;
    }
    return (0.2126F * value.x) + (0.7152F * value.y) + (0.0722F * value.z);
}

/// The B3-spline row {1, 4, 6, 4, 1} / 16, indexed by |offset|.
[[nodiscard]] f32 spline_weight(i32 offset) noexcept {
    constexpr f32 kRow[3] = {6.0F / 16.0F, 4.0F / 16.0F, 1.0F / 16.0F};
    const i32 magnitude = offset < 0 ? -offset : offset;
    return magnitude <= 2 ? kRow[magnitude] : 0.0F;
}

constexpr f32 kEpsilon = 1.0e-6F;

/// A pixel is converged when it has this many accumulated samples and its standard deviation has
/// fallen below this fraction of its own value. "Filter strength SHALL fall as confidence and
/// sample count rise, so a converged signal is not blurred" is these two numbers.
constexpr f32 kConvergedSamples = 12.0F;
constexpr f32 kConvergedRelativeDeviation = 0.02F;

const QualityPosition kQualityLadder[kQualityPositionCount] = {
    // passes, history, extent, relative cost = passes * taps, normalised to position 0 (4 * 25).
    {4, 32, 2, 1.00F},
    {3, 24, 2, 0.75F},
    {2, 16, 1, 0.18F},
    {1, 8, 1, 0.09F},
};

}  // namespace

const char* signal_name(SignalKind kind) noexcept {
    switch (kind) {
        case SignalKind::IndirectDiffuse:
            return "IndirectDiffuse";
        case SignalKind::IndirectSpecular:
            return "IndirectSpecular";
        case SignalKind::RayTracedShadow:
            return "RayTracedShadow";
        case SignalKind::AmbientOcclusion:
            return "AmbientOcclusion";
        case SignalKind::StochasticDirect:
            return "StochasticDirect";
        case SignalKind::Count:
            break;
    }
    return "Unknown";
}

SignalConfig default_config(SignalKind kind) noexcept {
    SignalConfig config;
    switch (kind) {
        case SignalKind::IndirectDiffuse:
            // Hemispherical and low-frequency: a long history and a wide cascade are free here, and
            // roughness says nothing about a diffuse gather.
            config.domain = SignalDomain::Radiance;
            config.lobe = LobeShape::Hemispherical;
            config.history_length = 32;
            config.max_passes = 4;
            config.roughness_widening = 0.0F;
            break;
        case SignalKind::IndirectSpecular:
            // Lobe-shaped: the filter width follows roughness and nothing else. A near-mirror keeps
            // a one-pixel kernel and a rough surface widens to four times it, which is
            // "a rough surface tolerates a wide filter, a smooth one does not" as a number.
            config.domain = SignalDomain::Radiance;
            config.lobe = LobeShape::Lobe;
            config.history_length = 16;
            config.max_passes = 3;
            config.roughness_widening = 3.0F;
            config.sigma_roughness = 0.05F;
            break;
        case SignalKind::RayTracedShadow:
            // A visibility term. The tight value tolerance is what preserves contact hardening: a
            // penumbra edge is a value edge, and stopping on it is the difference between a shadow
            // and a smear.
            config.domain = SignalDomain::Visibility;
            config.lobe = LobeShape::Hemispherical;
            config.history_length = 16;
            config.max_passes = 3;
            config.sigma_value = 1.5F;
            break;
        case SignalKind::AmbientOcclusion:
            config.domain = SignalDomain::Visibility;
            config.lobe = LobeShape::Hemispherical;
            config.history_length = 24;
            config.max_passes = 3;
            config.sigma_value = 2.0F;
            break;
        case SignalKind::StochasticDirect:
            // Direct lighting sampled stochastically over many lights: high frequency, short
            // history, because a light that moves invalidates it faster than an indirect gather.
            config.domain = SignalDomain::Radiance;
            config.lobe = LobeShape::Hemispherical;
            config.history_length = 12;
            config.max_passes = 3;
            break;
        case SignalKind::Count:
            break;
    }
    return config;
}

Span<const QualityPosition> quality_ladder() noexcept {
    return {kQualityLadder, kQualityPositionCount};
}

Denoiser::Denoiser() noexcept {
    for (u32 index = 0; index < kSignalCount; ++index) {
        states_[index].config = default_config(static_cast<SignalKind>(index));
    }
}

Status Denoiser::resize_state(SignalState& state) const noexcept {
    const usize pixels = static_cast<usize>(width_) * static_cast<usize>(height_);
    if (Status sized = state.accumulated.resize(pixels); !sized) {
        return sized;
    }
    if (Status sized = state.moment1.resize(pixels); !sized) {
        return sized;
    }
    if (Status sized = state.moment2.resize(pixels); !sized) {
        return sized;
    }
    if (Status sized = state.samples.resize(pixels); !sized) {
        return sized;
    }
    if (Status sized = state.variance.resize(pixels); !sized) {
        return sized;
    }
    if (Status sized = state.filtered.resize(pixels); !sized) {
        return sized;
    }
    if (Status sized = state.scratch.resize(pixels); !sized) {
        return sized;
    }
    if (Status sized = state.history_colour.resize(pixels); !sized) {
        return sized;
    }
    if (Status sized = state.history_moment1.resize(pixels); !sized) {
        return sized;
    }
    if (Status sized = state.history_moment2.resize(pixels); !sized) {
        return sized;
    }
    if (Status sized = state.history_samples.resize(pixels); !sized) {
        return sized;
    }
    state.has_history = false;
    return ok();
}

Status Denoiser::resize(u32 width, u32 height) noexcept {
    if (width == 0 || height == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "Denoiser::resize: a zero-sized view has no pixels");
    }
    width_ = width;
    height_ = height;
    for (SignalState& state : states_) {
        if (Status sized = resize_state(state); !sized) {
            return sized;
        }
    }
    return ok();
}

void Denoiser::configure(SignalKind kind, const SignalConfig& config) noexcept {
    states_[static_cast<u32>(kind)].config = config;
}

const SignalConfig& Denoiser::config(SignalKind kind) const noexcept {
    return states_[static_cast<u32>(kind)].config;
}

void Denoiser::set_quality_position(u32 position) noexcept {
    quality_position_ = std::min(position, kQualityPositionCount - 1);
}

const Diagnostics& Denoiser::diagnostics(SignalKind kind) const noexcept {
    return states_[static_cast<u32>(kind)].diagnostics;
}

void Denoiser::reset_history() noexcept {
    for (SignalState& state : states_) {
        state.has_history = false;
    }
}

void Denoiser::accumulate(SignalState& state, const NoisySignal& noisy,
                          const HistoryGuidance& history, u32 pixels) const noexcept {
    const QualityPosition& quality = kQualityLadder[quality_position_];
    const f32 history_length =
        static_cast<f32>(std::min(state.config.history_length, quality.history_length));
    const f32 alpha_floor = history_length > 0.0F ? 1.0F / history_length : 1.0F;
    const bool have_history = state.has_history && !history.source.empty();

    u32 rejected = 0;
    f32 total_samples = 0.0F;
    for (u32 pixel = 0; pixel < pixels; ++pixel) {
        const Vec3 value = noisy.values[pixel];
        const f32 scalar = signal_scalar(value, state.config.domain);
        const f32 fresh = noisy.samples.empty() ? 1.0F : noisy.samples[pixel];

        i32 source = -1;
        f32 confidence = 0.0F;
        if (have_history) {
            source = history.source[pixel];
            if (source >= 0 && std::cmp_less(source, pixels)) {
                confidence =
                    history.confidence.empty()
                        ? 1.0F
                        : math::clamp(history.confidence[static_cast<usize>(pixel)], 0.0F, 1.0F);
            } else {
                source = -1;
            }
        }

        if (source < 0 || confidence <= 0.0F) {
            rejected += 1;
            state.accumulated[pixel] = value;
            state.moment1[pixel] = scalar;
            state.moment2[pixel] = scalar * scalar;
            state.samples[pixel] = fresh;
            total_samples += fresh;
            continue;
        }

        const auto from = static_cast<usize>(source);
        const f32 carried = state.history_samples[from] * confidence;
        const f32 count = carried + fresh;
        // The blend weight falls as the accumulated count rises, and never below the floor the
        // declared history length sets — a converged pixel still takes 1/history_length of each new
        // sample, so a lighting change is not held out forever.
        const f32 alpha = std::max(count > 0.0F ? fresh / count : 1.0F, alpha_floor);
        state.accumulated[pixel] = lerp(state.history_colour[from], value, alpha);
        state.moment1[pixel] = math::lerp(state.history_moment1[from], scalar, alpha);
        state.moment2[pixel] = math::lerp(state.history_moment2[from], scalar * scalar, alpha);
        state.samples[pixel] = std::min(count, history_length);
        total_samples += state.samples[pixel];
    }

    state.diagnostics.rejected_history_fraction =
        pixels == 0 ? 0.0F : static_cast<f32>(rejected) / static_cast<f32>(pixels);
    state.diagnostics.mean_sample_count =
        pixels == 0 ? 0.0F : total_samples / static_cast<f32>(pixels);
}

void Denoiser::estimate_variance(SignalState& state, u32 pixels) const noexcept {
    f32 total = 0.0F;
    for (u32 pixel = 0; pixel < pixels; ++pixel) {
        const f32 first = state.moment1[pixel];
        const f32 second = state.moment2[pixel];
        state.variance[pixel] = std::max(0.0F, second - (first * first));
    }

    // A pixel with almost no temporal history has no meaningful temporal variance either, so its
    // estimate is taken spatially from the 3x3 around it. Without this the disoccluded region — the
    // one that most needs a wide filter — reports zero variance and gets none.
    for (u32 y = 0; y < height_; ++y) {
        for (u32 x = 0; x < width_; ++x) {
            const u32 pixel = (y * width_) + x;
            if (state.samples[pixel] >= 4.0F) {
                total += state.variance[pixel];
                continue;
            }
            f32 sum = 0.0F;
            f32 sum_squared = 0.0F;
            f32 count = 0.0F;
            for (i32 dy = -1; dy <= 1; ++dy) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    const i32 sx = static_cast<i32>(x) + dx;
                    const i32 sy = static_cast<i32>(y) + dy;
                    if (sx < 0 || sy < 0 || std::cmp_greater_equal(sx, width_) ||
                        std::cmp_greater_equal(sy, height_)) {
                        continue;
                    }
                    const usize neighbour =
                        (static_cast<usize>(sy) * width_) + static_cast<usize>(sx);
                    const f32 value = state.moment1[neighbour];
                    sum += value;
                    sum_squared += value * value;
                    count += 1.0F;
                }
            }
            const f32 mean = sum / count;
            state.variance[pixel] =
                std::max({state.variance[pixel], 0.0F, (sum_squared / count) - (mean * mean)});
            total += state.variance[pixel];
        }
    }
    state.diagnostics.mean_variance = pixels == 0 ? 0.0F : total / static_cast<f32>(pixels);
}

/// Everything one pixel of one pass needs, gathered once so the tap loop reads a struct rather than
/// six captures. Splitting the filter into "what the centre decided" and "what one tap is worth" is
/// what keeps either half readable: the whole thing in one function measures 81 on the cognitive
/// complexity scale, and no amount of comment makes a five-deep nest of continues followable.
struct Denoiser::TapContext {
    const GuidanceBuffers* guidance = nullptr;
    const SignalConfig* config = nullptr;
    const Array<Vec3>* image = nullptr;
    usize centre = 0;
    f32 depth = 0.0F;
    f32 centre_value = 0.0F;
    f32 deviation = 0.0F;
    f32 roughness = 0.0F;
    Vec3 normal{0.0F, 1.0F, 0.0F};
    bool identity = false;
};

/// The edge-stopping weight of one tap. Zero means the tap is on the other side of something.
f32 Denoiser::tap_weight(const TapContext& context, usize tap, i32 dx, i32 dy) noexcept {
    const GuidanceBuffers& guidance = *context.guidance;
    const SignalConfig& config = *context.config;

    const f32 tap_depth = guidance.depth[tap];
    if (tap_depth <= 0.0F) {
        return 0.0F;
    }
    // Identity beats inference: two surfaces with the same depth and normal and different materials
    // are not filtered together, and the visibility buffer's identifiers say so exactly rather than
    // approximately.
    if (context.identity && (guidance.instance_id[tap] != guidance.instance_id[context.centre] ||
                             (!guidance.material_id.empty() &&
                              guidance.material_id[tap] != guidance.material_id[context.centre]))) {
        return 0.0F;
    }

    f32 weight = spline_weight(dx) * spline_weight(dy);
    const f32 depth_difference = std::abs(tap_depth - context.depth);
    weight *=
        std::exp(-depth_difference / ((config.sigma_depth * std::abs(context.depth)) + kEpsilon));
    if (!guidance.normal.empty()) {
        const f32 alignment = std::max(0.0F, dot(context.normal, guidance.normal[tap]));
        weight *= std::pow(alignment, config.sigma_normal);
    }
    if (!guidance.roughness.empty()) {
        weight *= std::exp(-std::abs(guidance.roughness[tap] - context.roughness) /
                           (config.sigma_roughness + kEpsilon));
    }
    const f32 tap_value = signal_scalar((*context.image)[tap], config.domain);
    weight *= std::exp(-std::abs(tap_value - context.centre_value) /
                       ((config.sigma_value * context.deviation) + kEpsilon));
    return std::max(weight, 0.0F);
}

Denoiser::PixelResult Denoiser::filter_pixel(const SignalState& state,
                                             const GuidanceBuffers& guidance, u32 x, u32 y,
                                             u32 step, i32 extent, bool identity) const noexcept {
    PixelResult result;
    const usize centre = (static_cast<usize>(y) * width_) + x;
    result.value = state.filtered[centre];

    const f32 depth = guidance.depth[centre];
    if (depth <= 0.0F) {
        return result;  // a sky pixel has no surface to stop on
    }

    const f32 centre_value = signal_scalar(state.filtered[centre], state.config.domain);
    const f32 deviation = std::sqrt(state.variance[centre]);
    // Filter strength falls as confidence and sample count rise.
    if (state.samples[centre] >= kConvergedSamples &&
        deviation <= kConvergedRelativeDeviation * (std::abs(centre_value) + kEpsilon)) {
        return result;
    }

    const f32 roughness = guidance.roughness.empty() ? 0.0F : guidance.roughness[centre];
    // The lobe widening, and the only place roughness changes the KERNEL rather than a weight.
    // Hemispherical signals declare zero and are unaffected.
    const f32 widened =
        static_cast<f32>(step) * (1.0F + (state.config.roughness_widening * roughness));
    const auto effective_step = std::max<u32>(1U, static_cast<u32>(std::lround(widened)));

    TapContext context;
    context.guidance = &guidance;
    context.config = &state.config;
    context.image = &state.filtered;
    context.centre = centre;
    context.depth = depth;
    context.centre_value = centre_value;
    context.deviation = deviation;
    context.roughness = roughness;
    context.normal = guidance.normal.empty() ? Vec3{0.0F, 1.0F, 0.0F} : guidance.normal[centre];
    context.identity = identity;

    Vec3 sum{0.0F, 0.0F, 0.0F};
    f32 weight_sum = 0.0F;
    for (i32 dy = -extent; dy <= extent; ++dy) {
        for (i32 dx = -extent; dx <= extent; ++dx) {
            const i32 sx = static_cast<i32>(x) + (dx * static_cast<i32>(effective_step));
            const i32 sy = static_cast<i32>(y) + (dy * static_cast<i32>(effective_step));
            if (sx < 0 || sy < 0 || std::cmp_greater_equal(sx, width_) ||
                std::cmp_greater_equal(sy, height_)) {
                continue;
            }
            const auto tap = (static_cast<usize>(sy) * width_) + static_cast<usize>(sx);
            const f32 weight = tap_weight(context, tap, dx, dy);
            if (weight <= 0.0F) {
                continue;
            }
            sum = sum + (state.filtered[tap] * weight);
            weight_sum += weight;
        }
    }

    if (weight_sum > 0.0F) {
        result.value = sum / weight_sum;
        result.filtered = true;
        result.step = effective_step;
    }
    return result;
}

void Denoiser::filter(SignalState& state, const GuidanceBuffers& guidance) const noexcept {
    const QualityPosition& quality = kQualityLadder[quality_position_];
    const u32 passes = std::min(state.config.max_passes, quality.max_passes);
    const auto extent = static_cast<i32>(quality.kernel_extent);
    const bool identity = state.config.identity_is_hard_boundary && !guidance.instance_id.empty();
    state.diagnostics.identity_available = !guidance.instance_id.empty();

    const u32 pixels = width_ * height_;
    for (u32 pixel = 0; pixel < pixels; ++pixel) {
        state.filtered[pixel] = state.accumulated[pixel];
    }

    f32 radius_total = 0.0F;
    f32 radius_count = 0.0F;
    u32 applied = 0;
    for (u32 pass = 0; pass < passes; ++pass) {
        const u32 step = 1U << pass;
        bool touched = false;
        for (u32 y = 0; y < height_; ++y) {
            for (u32 x = 0; x < width_; ++x) {
                const PixelResult result =
                    filter_pixel(state, guidance, x, y, step, extent, identity);
                state.scratch[(static_cast<usize>(y) * width_) + x] = result.value;
                if (result.filtered) {
                    touched = true;
                    radius_total += static_cast<f32>(result.step);
                    radius_count += 1.0F;
                }
            }
        }
        std::swap(state.filtered, state.scratch);
        if (touched) {
            applied = pass + 1;
        }
    }

    state.diagnostics.passes_applied = applied;
    state.diagnostics.mean_filter_radius = radius_count > 0.0F ? radius_total / radius_count : 0.0F;
}

Expected<Span<const Vec3>, Error> Denoiser::denoise(SignalKind kind, const NoisySignal& noisy,
                                                    const GuidanceBuffers& guidance,
                                                    const HistoryGuidance& history) noexcept {
    if (width_ == 0 || height_ == 0) {
        return fail(ErrorCode::Unavailable, "Denoiser::denoise: resize() has not been called");
    }
    if (guidance.width != width_ || guidance.height != height_) {
        return fail(ErrorCode::InvalidArgument,
                    "Denoiser::denoise: the guidance buffers are a different size from the view");
    }
    const usize pixels = static_cast<usize>(width_) * static_cast<usize>(height_);
    if (noisy.values.size() != pixels || guidance.depth.size() != pixels) {
        return fail(ErrorCode::InvalidArgument,
                    "Denoiser::denoise: a buffer does not cover every pixel of the view");
    }

    SignalState& state = states_[static_cast<u32>(kind)];
    const i64 started = jobs::monotonic_now_ns();
    state.diagnostics = Diagnostics{};

    if (!enabled_) {
        // The raw signal, unchanged. Not a cheaper filter — the point is to see what the filter is
        // being asked to reconstruct, against a path-traced reference.
        for (usize pixel = 0; pixel < pixels; ++pixel) {
            state.filtered[pixel] = noisy.values[pixel];
        }
        state.diagnostics.bypassed = true;
        state.diagnostics.cost_ns = static_cast<u64>(jobs::monotonic_now_ns() - started);
        state.has_history = false;
        return Span<const Vec3>{state.filtered.data(), pixels};
    }

    accumulate(state, noisy, history, static_cast<u32>(pixels));
    estimate_variance(state, static_cast<u32>(pixels));
    filter(state, guidance);

    // The history for the next frame is the ACCUMULATED signal rather than the filtered one. Both
    // are defensible and this one is the honest one: feeding the spatial filter's output back makes
    // the accumulated variance an estimate of the filter's residual rather than of the signal's
    // noise, and the variance is what drives the filter. A converged region would then report the
    // variance it has after filtering, and widen no filter it needed.
    for (usize pixel = 0; pixel < pixels; ++pixel) {
        state.history_colour[pixel] = state.accumulated[pixel];
        state.history_moment1[pixel] = state.moment1[pixel];
        state.history_moment2[pixel] = state.moment2[pixel];
        state.history_samples[pixel] = state.samples[pixel];
    }
    state.has_history = true;
    state.diagnostics.cost_ns = static_cast<u64>(jobs::monotonic_now_ns() - started);
    return Span<const Vec3>{state.filtered.data(), pixels};
}

}  // namespace cy::rendering::denoise
