#pragma once
// CyberDenoiser: ONE accumulation and edge-aware filter for every stochastic signal. Task 9.3.
//
// `denoising` — "One denoising framework", "Denoising pipeline", "Denoising is material and
// geometry aware", "Signal-specific configuration", "Denoising quality and budget", "Denoising
// diagnostics".
//
// ================================================================================================
// THE STRUCTURAL RULE, AND HOW IT IS ENFORCED RATHER THAN INTENDED
// ================================================================================================
//
// "One filter for every stochastic signal" is easy to write and easy to lose: the second time a
// signal needs something slightly different, a branch on the signal appears, and six months later
// the branch is a second filter. So the rule here is stronger than the specification's wording and
// it is checked by a test:
//
//     `SignalKind` CARRIES NO BEHAVIOUR. It selects a `SignalConfig` and a history slot, and
//     nothing else in this module reads it.
//
// Everything that differs between indirect diffuse, indirect specular, ray-traced shadows, ambient
// occlusion and stochastic direct lighting is a FIELD OF `SignalConfig`. The consequence is
// testable and `tests/test_one_filter.cpp` tests it: denoising a buffer as `RayTracedShadow` with
// an `IndirectDiffuse` configuration produces the same image, bit for bit, as denoising it as
// `IndirectDiffuse`. A per-signal special case anywhere in denoiser.cpp fails that case.
//
// ================================================================================================
// WHAT THIS MODULE DOES NOT DO: HISTORY
// ================================================================================================
//
// `denoising` requires that temporal accumulation use the framework in `temporal-rendering` for
// jitter, motion vectors, reprojection, disocclusion classification and invalidation, and that
// denoising "SHALL NOT implement its own history handling". That is expressed here as an INPUT: the
// caller hands over `HistoryGuidance`, which is the reprojection's answer — for each pixel, which
// pixel of the previous frame it came from, and how much of that history survived validation. This
// module never computes a motion vector, never reprojects, and never decides what a disocclusion
// is. It consumes the answer.
//
// That is also why the seam is a plain struct of spans rather than a dependency on the temporal
// module: the two are written in the same milestone by different hands, and a struct of spans is
// the interface either can fill.
//
// ================================================================================================
// THE FOUR STAGES, IN ORDER
// ================================================================================================
//
//   1. temporal accumulation over the reprojected history, with the blend weight falling as the
//      accumulated sample count rises and floored by the signal's declared history length;
//   2. variance estimation from the accumulated first and second luminance moments;
//   3. spatial filtering — an a-trous cascade whose pass count and radius are driven by variance
//      and sample count, so a converged region is filtered less and a disoccluded one more;
//   4. history validation, which is applied at stage 1 and REPORTED at stage 4, because "what
//      fraction of pixels rejected their history" is the diagnostic that explains residual noise.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>

namespace cy::rendering::denoise {

/// The five stochastic signals `denoising` names. A sixth would be a new enumerator and a new
/// `SignalConfig`, and no other change — which is the requirement "a new noisy signal reuses the
/// framework" as a property of the code rather than a promise.
enum class SignalKind : u8 {
    IndirectDiffuse = 0,
    IndirectSpecular,
    RayTracedShadow,
    AmbientOcclusion,
    StochasticDirect,
    Count,
};

inline constexpr u32 kSignalCount = static_cast<u32>(SignalKind::Count);

/// The enumerator's own spelling, for a diagnostic. Never null.
[[nodiscard]] const char* signal_name(SignalKind kind) noexcept;

/// What kind of quantity the signal is.
///
/// `denoising`: "Visibility terms (shadows, ambient occlusion) SHALL be denoised as occlusion
/// rather than as radiance, since blurring them as colour loses contact detail." The difference is
/// not a different filter — it is that the edge-stopping term compares the OCCLUSION values
/// directly and at a much tighter tolerance, so a penumbra edge stops the filter the way a
/// geometric edge does.
enum class SignalDomain : u8 {
    Radiance = 0,
    Visibility,
};

/// Whether the signal's support is the whole hemisphere or a lobe around a reflection direction.
enum class LobeShape : u8 {
    Hemispherical = 0,
    Lobe,
};

/// Everything that differs between one signal and another. See the header comment: this struct is
/// the ONLY thing that differs.
struct SignalConfig {
    SignalDomain domain = SignalDomain::Radiance;
    LobeShape lobe = LobeShape::Hemispherical;
    /// The longest history a converged pixel keeps, in frames. It is a floor on the temporal blend
    /// weight: 32 frames means a converged pixel still takes 1/32 of each new sample, so a lighting
    /// change is not held out forever by a long history.
    u32 history_length = 32;
    /// The a-trous cascade's length at full variance. Each pass doubles its step, so three passes
    /// reach seven pixels and five reach thirty-one.
    u32 max_passes = 3;
    /// Edge-stopping tolerances. Larger accepts more; the filter's whole quality is here.
    f32 sigma_depth = 0.10F;
    /// The exponent on `dot(n, n')`, so larger is TIGHTER for normals — the conventional form.
    f32 sigma_normal = 32.0F;
    /// Tolerance on the value difference, in units of its own standard deviation.
    f32 sigma_value = 4.0F;
    f32 sigma_roughness = 0.20F;
    /// How much a rough surface widens the filter, for a lobe-shaped signal. Zero means roughness
    /// is not consulted, which is right for a hemispherical one. "A rough surface tolerates a wide
    /// filter, a smooth one does not" is this number and nothing else.
    f32 roughness_widening = 0.0F;
    /// Whether the visibility buffer's instance and material identifiers are a hard boundary.
    /// `denoising` requires that they be used where available, "since they give exact boundaries
    /// rather than inferred ones".
    bool identity_is_hard_boundary = true;
};

/// The configuration each signal declares. The table `denoising`'s "Signal-specific configuration"
/// requirement asks each signal to supply, in one place so the five can be read against each other.
[[nodiscard]] SignalConfig default_config(SignalKind kind) noexcept;

/// The guidance the filter stops on. All spans are `width * height` and indexed `y * width + x`.
///
/// `instance_id` and `material_id` are the visibility buffer's, and may be empty on a renderer that
/// has none — the filter then falls back to depth, normal and roughness alone, and
/// `Diagnostics::identity_available` says which happened.
struct GuidanceBuffers {
    u32 width = 0;
    u32 height = 0;
    /// View-space depth in metres. Zero or negative means "no surface": a sky pixel.
    Span<const f32> depth;
    Span<const Vec3> normal;
    Span<const f32> roughness;
    Span<const u32> instance_id;
    Span<const u32> material_id;
};

/// The reprojection's answer, from `temporal-rendering`. This module consumes it and computes none
/// of it — see the header comment.
///
/// The join, now that `src/rendering/temporal/` exists: `temporal::classify_history()` returns a
/// `ReprojectionResult` per pixel carrying a `HistoryState` and a `history_uv`. A caller fills this
/// struct from it in one loop — `source[p]` is the history pixel `history_uv` lands on, or -1, and
/// `confidence[p]` is `temporal::history_usable(state) ? 1 : 0`, or a softer number where the
/// caller wants a partial rejection. The adapter is at the CALL SITE deliberately: it is where the
/// screen-space conventions live, and putting it here would make this module depend on the shape of
/// the temporal framework's per-pixel record.
struct HistoryGuidance {
    /// For each pixel, the index into the PREVIOUS frame's buffers it reprojected to, or a negative
    /// value where the reprojection failed. Empty means "no history at all", which is what the
    /// first frame after a camera cut hands over.
    Span<const i32> source;
    /// How much of that history survived validation, in [0, 1]. Zero is a full rejection and is
    /// counted as one.
    Span<const f32> confidence;
};

/// The noisy input.
struct NoisySignal {
    /// Radiance, or occlusion replicated across the channels for a `Visibility` signal. The
    /// framework does not care which; `SignalConfig::domain` is what tells it how to stop.
    Span<const Vec3> values;
    /// How many stochastic samples went into each pixel this frame. One sample per pixel per frame
    /// is the ordinary case and an empty span means exactly that.
    Span<const f32> samples;
};

/// The quality ladder the GI budget pulls on.
///
/// `denoising`: "filter passes, filter resolution, and history length SHALL be adjustable, with the
/// trade-off between cost and residual noise reported". Four declared positions, coarsest last, and
/// `relative_cost` is the field the renderer budget arbiter needs in order to price a step — see
/// `design.md` §2.10, which asks every lever to declare what each position costs relative to
/// position 0.
struct QualityPosition {
    /// A cap on the cascade length, not a replacement for it: a converged region still uses fewer.
    u32 max_passes = 4;
    u32 history_length = 32;
    /// Taps per side of the centre. 2 is the 5x5 B3-spline kernel; 1 is 3x3 and costs a third of
    /// it.
    u32 kernel_extent = 2;
    /// Cost relative to position 0, as passes times taps. This is the field `design.md` §2.10 asks
    /// every lever to declare — an arbiter allocating milliseconds over a ladder it cannot price is
    /// choosing blind.
    f32 relative_cost = 1.0F;
};

inline constexpr u32 kQualityPositionCount = 4;

/// The ladder, coarsest last. Position 0 is authored quality.
[[nodiscard]] Span<const QualityPosition> quality_ladder() noexcept;

/// What one signal cost and how well it went. `denoising` — "Denoising diagnostics", field for
/// field, plus the two that answer "why is there still noise".
struct Diagnostics {
    f32 mean_sample_count = 0.0F;
    f32 mean_variance = 0.0F;
    /// The mean a-trous step actually applied, in pixels. Zero where the filter was skipped.
    f32 mean_filter_radius = 0.0F;
    f32 rejected_history_fraction = 0.0F;
    u32 passes_applied = 0;
    u64 cost_ns = 0;
    bool identity_available = false;
    /// The denoiser was off and the raw signal was passed through unchanged.
    bool bypassed = false;
};

/// The one framework.
///
/// Not thread-safe: one instance belongs to one view, and its history is that view's. A second view
/// gets a second instance rather than a second set of buffers inside this one, because a history
/// keyed by view inside a shared object is the shape that produces a reprojection from the wrong
/// camera exactly once, in a reflection view, at four in the morning.
class Denoiser {
public:
    Denoiser() noexcept;

    /// Set the working resolution. Every history buffer is dropped, which is correct: a resolution
    /// change is a disocclusion of the whole frame.
    [[nodiscard]] Status resize(u32 width, u32 height) noexcept;

    /// Override one signal's declared configuration. Rarely needed — `default_config` is the
    /// table — but a GI volume that wants a longer history in an interior sets it here.
    void configure(SignalKind kind, const SignalConfig& config) noexcept;
    [[nodiscard]] const SignalConfig& config(SignalKind kind) const noexcept;

    /// Move the whole framework to a position on the declared ladder. Out-of-range clamps to the
    /// coarsest, because a budget that asked for more than the ladder has should get the bottom of
    /// it rather than an error in a frame.
    void set_quality_position(u32 position) noexcept;
    [[nodiscard]] u32 quality_position() const noexcept { return quality_position_; }

    /// `denoising`: "Denoising SHALL be disableable for reference comparison and validation, so the
    /// raw stochastic signal can be inspected." Off means the input is returned unchanged and
    /// `Diagnostics::bypassed` says so — it does not mean a cheaper filter.
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    /// Reconstruct one signal. The returned span is valid until the next call for the same signal.
    [[nodiscard]] Expected<Span<const Vec3>, Error> denoise(
        SignalKind kind, const NoisySignal& noisy, const GuidanceBuffers& guidance,
        const HistoryGuidance& history) noexcept;

    [[nodiscard]] const Diagnostics& diagnostics(SignalKind kind) const noexcept;

    /// Drop every history buffer. A camera cut, a level load, or a test that wants frame one twice.
    void reset_history() noexcept;

private:
    struct SignalState {
        SignalConfig config{};
        Diagnostics diagnostics{};
        /// The accumulated signal, and the two luminance moments variance is estimated from.
        Array<Vec3> accumulated;
        Array<f32> moment1;
        Array<f32> moment2;
        Array<f32> samples;
        Array<f32> variance;
        /// The two spatial-filter buffers, ping-ponged across the cascade.
        Array<Vec3> filtered;
        Array<Vec3> scratch;
        /// Last frame's accumulation, read through the reprojection. Separate buffers rather than
        /// in-place: a pixel reads its history from wherever the reprojection points, which is not
        /// itself, so writing this frame's answer over last frame's would let one pixel read
        /// another pixel's already-updated value.
        Array<Vec3> history_colour;
        Array<f32> history_moment1;
        Array<f32> history_moment2;
        Array<f32> history_samples;
        bool has_history = false;
    };

    // Const because each stage mutates the SIGNAL STATE it is handed and nothing else on the
    // denoiser: the four are pure functions of a state and its inputs, and saying so is what keeps
    // a stage from quietly reaching for another signal's history.
    /// One pixel's filtered value, and whether the filter actually ran on it.
    struct PixelResult {
        Vec3 value{0.0F, 0.0F, 0.0F};
        bool filtered = false;
        u32 step = 0;
    };
    struct TapContext;

    [[nodiscard]] static f32 tap_weight(const TapContext& context, usize tap, i32 dx,
                                        i32 dy) noexcept;
    [[nodiscard]] PixelResult filter_pixel(const SignalState& state,
                                           const GuidanceBuffers& guidance, u32 x, u32 y, u32 step,
                                           i32 extent, bool identity) const noexcept;

    [[nodiscard]] Status resize_state(SignalState& state) const noexcept;
    void accumulate(SignalState& state, const NoisySignal& noisy, const HistoryGuidance& history,
                    u32 pixels) const noexcept;
    void estimate_variance(SignalState& state, u32 pixels) const noexcept;
    void filter(SignalState& state, const GuidanceBuffers& guidance) const noexcept;

    u32 width_ = 0;
    u32 height_ = 0;
    u32 quality_position_ = 0;
    bool enabled_ = true;
    SignalState states_[kSignalCount];
};

}  // namespace cy::rendering::denoise
