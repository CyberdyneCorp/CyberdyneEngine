// SPDX-License-Identifier: MIT
#pragma once
// Temporal upscaling: the quality modes, the internal resolution and its dynamic scaling, the
// constants the dispatches read, and the host reference the suites compare against.
// `rendering-post-processing` — "Temporal upscaling".
//
// ================================================================================================
// WHAT THE REQUIREMENT ASKS, AND WHERE EACH CLAUSE LIVES
// ================================================================================================
//
//   "rendering the 3D scene at a reduced       `render_extent_for`: the output extent times the
//    internal resolution"                      mode's scale, and `FrameAssembly`'s render extent
//                                              is where every scene pass is recorded at
//   "reconstructing it at output resolution"   `upscale_reference_at` and
//   `upscale_reconstruct.slang`,
//                                              expression for expression: every output pixel
//                                              gathers the jittered samples near it, reprojects
//                                              its history along the motion vector, rejects it
//                                              where the surface was hidden last frame, clips it
//                                              to this frame's neighbourhood, and accumulates
//   "with UI drawn at native resolution"       the frame's interface stage runs after the post
//                                              chain at the OUTPUT extent; nothing here touches it
//   "one interface taking colour, depth,       `ScreenSpaceStageInputs` at the frame's `Temporal`
//    motion vectors, exposure, and jitter"     stage, plus `UpscaleView` — see `upscaler.h`
//   "a built-in temporal upscaler"             `upscale_pass.h`
//   "Dynamic resolution ... a budget           `dynamic_render_scale`: the arbiter's resolution
//    allocation held by the renderer budget    allocation times the mode's, within configured
//    arbiter"                                  bounds. Nothing here measures a frame time.
//
// ================================================================================================
// THE ONE CONVENTION THE RECONSTRUCTION RESTS ON
// ================================================================================================
//
// The frame jitters its projection by `jitter` render pixels (`pipeline::upload_for`: clip x gains
// `2 * jitter.x / width`, clip y gains `2 * jitter.y / height`). NDC y points up and texture y
// points down, so the surface that lands on render texel `i`'s centre is the one the UNJITTERED
// camera sees at
//
//     sample(i) = i + 0.5 + (-jitter.x, +jitter.y)          render pixels, texture orientation
//
// `UpscaleConstants::sample` stores that offset, and it is the only place the sign is decided. A
// reconstruction that got it backwards still converges — to a picture displaced by twice the
// jitter's spread, which a still frame shows as softness and nothing else. The device suite's
// convergence case is what would see it: its error against the native frame doubles.
//
// ================================================================================================
// THE ACCUMULATION, IN ONE PARAGRAPH
// ================================================================================================
//
// A history texel holds a colour and a WEIGHT — how many samples' worth of evidence it is. Each
// frame an output pixel receives evidence `S` in [0, 1]: one when a jittered sample landed exactly
// on its centre, falling off with the distance to the samples near it. The new colour is the
// weighted mean of the history (weight `n`) and this frame's samples (weight `S`), and the new
// weight is `n + S`, capped. So a pixel converges to the mean of the samples that landed on it —
// which, as the jitter sequence covers the pixel, is a supersampled pixel — and the cap is how
// quickly old evidence is forgotten. Every way history can be wrong reduces `n`: off screen, a
// framework invalidation, a surface that was hidden last frame (disocclusion), a colour outside
// this frame's neighbourhood (rectification), and a reactive pixel — translucency or particles,
// whose motion the velocity target does not carry.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/temporal/jitter.h>

namespace cy::rendering::upscale {

/// The quality modes. Each is a fixed ratio of output to internal resolution; `NativeAntialiasing`
/// renders at the output resolution and uses the same reconstruction as an antialiaser.
enum class UpscaleMode : u8 {
    NativeAntialiasing = 0,
    /// 67 % per axis — 1.5x upscale, 44 % of the pixels.
    Quality,
    /// 58 % per axis — 1.72x upscale, 34 % of the pixels.
    Balanced,
    /// 50 % per axis — 2x upscale, 25 % of the pixels.
    Performance,
    Count,
};

inline constexpr u32 kUpscaleModeCount = static_cast<u32>(UpscaleMode::Count);

[[nodiscard]] const char* upscale_mode_name(UpscaleMode mode) noexcept;
/// "native", "quality", "balanced" or "performance". False for anything else.
[[nodiscard]] bool parse_upscale_mode(const char* text, UpscaleMode& out) noexcept;
/// The per-axis scale a mode renders at: 1, 0.67, 0.58, 0.5.
[[nodiscard]] f32 upscale_mode_scale(UpscaleMode mode) noexcept;

/// An extent in pixels.
struct Extent2D {
    u32 width = 0;
    u32 height = 0;

    [[nodiscard]] friend bool operator==(Extent2D a, Extent2D b) noexcept {
        return a.width == b.width && a.height == b.height;
    }
};

/// The internal extent for an output extent at a per-axis scale: each axis rounded to the nearest
/// pixel, at least one and never more than the output. A scale outside (0, 1] is clamped into it.
[[nodiscard]] Extent2D render_extent_for(Extent2D output, f32 scale) noexcept;

/// How many jitter phases cover an output pixel: `ceil(8 * (output / render)^2)`, at least 8. Eight
/// samples antialias a native pixel; an upscale spreads the same samples over `(output / render)^2`
/// output pixels, so the sequence must be that much longer before it repeats.
[[nodiscard]] u32 jitter_phase_count(u32 output_width, u32 render_width) noexcept;

/// The temporal framework's jitter configuration for an upscale: the phase count above, Halton 2,3.
[[nodiscard]] JitterConfig jitter_config_for(Extent2D output, Extent2D render) noexcept;

/// The texture LOD bias a material sampler takes at this ratio: `log2(render / output)`. A texture
/// sampled at the internal resolution picks the mip the internal pixel footprint asks for, which is
/// blurrier than the output pixel the reconstruction resolves; the bias selects the output's.
[[nodiscard]] f32 texture_lod_bias(u32 render_width, u32 output_width) noexcept;

/// The bounds dynamic resolution moves within.
struct DynamicResolutionBounds {
    /// The smallest per-axis scale the frame may render at, whatever the arbiter asks.
    f32 minimum_scale = 0.5F;
    /// The largest. 1 is native.
    f32 maximum_scale = 1.0F;
    /// The render width is a multiple of this many pixels, so the arbiter's ladder maps to a few
    /// distinct extents rather than a new allocation for every hundredth of a scale. The height
    /// follows from the width at the output's aspect ratio.
    u32 granularity = 8;
};

/// The per-axis scale a frame renders at: the mode's own scale times the arbiter's resolution
/// allocation (`BudgetArbiter::resolution_scale()`, 1 at authored quality), within the bounds.
///
/// THE ARBITER DECIDES, THIS ONLY COMPOSES. `rendering-post-processing`: internal resolution SHALL
/// be "a budget allocation held by the renderer budget arbiter ... not an independent controller
/// measuring frame time". Nothing in this module reads a frame time.
[[nodiscard]] f32 dynamic_render_scale(UpscaleMode mode, f32 arbiter_scale,
                                       const DynamicResolutionBounds& bounds) noexcept;

/// `render_extent_for` at `dynamic_render_scale`, with the width snapped down to the bounds'
/// granularity (never below one granule, never above the output).
[[nodiscard]] Extent2D dynamic_render_extent(Extent2D output, UpscaleMode mode, f32 arbiter_scale,
                                             const DynamicResolutionBounds& bounds) noexcept;

/// The reconstruction's controls.
struct UpscaleSettings {
    UpscaleMode mode = UpscaleMode::Quality;
    /// Contrast-adaptive sharpening of the reconstructed colour, 0 (none: the reconstruction is
    /// copied through) to 1. The history is never sharpened: sharpening a signal that feeds back
    /// into itself compounds every frame.
    f32 sharpness = 0.3F;
    /// The most samples' worth of evidence a history texel holds — how slowly old evidence is
    /// forgotten on a still pixel. See the header comment.
    f32 max_history_weight = 16.0F;
    /// The cap on a pixel that moves: the cap falls from `max_history_weight` to this as the
    /// motion reaches `motion_falloff_pixels` output pixels a frame, because every reprojection
    /// resamples the history and a long accumulation of resampled history is a blur.
    f32 moving_history_weight = 6.0F;
    f32 motion_falloff_pixels = 4.0F;
    /// Neighbourhood clipping: the history is clipped into this frame's 3x3 neighbourhood mean plus
    /// or minus this many standard deviations. Wider on a still pixel, where a thin feature only
    /// some frames' samples hit must survive the frames whose samples miss it; tighter on a moving
    /// one, where a wide box is a ghost.
    f32 still_clip_gamma = 2.5F;
    f32 moving_clip_gamma = 1.0F;
    /// How strongly a reactive pixel (translucency, particles) discards its history, 0 to 1.
    f32 reactive_strength = 1.0F;
    /// A history texel is from a surface that was HIDDEN last frame — disoccluded — when the depth
    /// it holds is nearer, by more than this fraction, than every surface within two render pixels
    /// of this frame's sample, reprojected to last frame.
    f32 depth_tolerance = 0.05F;
    /// The exposure the colour will be shown at, applied before the reconstruction's tone curve
    /// (`x / (1 + x)`) so that a highlight is weighted as bright as it will look. One is unexposed.
    f32 exposure = 1.0F;
    /// Discount the history where the neighbourhood's brightness changed against the history at
    /// the same sample positions — a light switched, a shadow moved. Off leaves only the clip.
    bool detect_shading_change = true;
    /// Lock thin features — a sample brighter or darker than both neighbours across it — so that
    /// the frames whose samples miss them do not clip their history away.
    bool lock_thin_features = true;
    /// Discard the history of a pixel whose surface was hidden last frame. Off leaves only the clip
    /// and the shading-change test between a revealed background and its occluder's history.
    bool reject_disocclusion = true;
};

/// What one frame's view contributes.
struct UpscaleView {
    Extent2D render;
    Extent2D output;
    /// The temporal framework's jitter this frame, in RENDER pixels — `JitterSequence::current()`.
    Vec2 jitter{0.0F, 0.0F};
    /// The UNJITTERED projection, reversed-Z: its (2,2) and (3,2) terms give a depth in metres.
    Mat4 projection = Mat4::identity();
    /// This frame's camera-relative to clip transform, unjittered, and last frame's expressed about
    /// this frame's camera — what a pixel with no motion vector is reprojected through, and what a
    /// surface's expected depth last frame is computed with.
    Mat4 relative_to_clip = Mat4::identity();
    Mat4 previous_relative_to_clip = Mat4::identity();
    /// The temporal framework's answer: false on a cut, a teleport, a projection or output change,
    /// and before the first frame. False discards every history texel.
    bool history_valid = false;
    /// Whether the prepass wrote motion vectors. Without them every pixel is reprojected by the
    /// camera's motion through its own depth, which is right for everything that does not move.
    bool velocity = true;
    /// Whether a pre-translucency copy of the colour was made, from which the reactive mask is
    /// derived.
    bool reactive = false;
};

/// Bits of `UpscaleConstants::flags`.
inline constexpr u32 kUpscaleHistoryValid = 1U;
inline constexpr u32 kUpscaleVelocity = 2U;
inline constexpr u32 kUpscaleReactive = 4U;
inline constexpr u32 kUpscaleRejectDisocclusion = 8U;
inline constexpr u32 kUpscaleShadingChange = 16U;
inline constexpr u32 kUpscaleLocks = 32U;

/// An extent packed into one word, width in the low sixteen bits: `UpscaleConstants::extents`.
[[nodiscard]] constexpr u32 pack_extent(Extent2D extent) noexcept {
    return (extent.width & 0xFFFFU) | ((extent.height & 0xFFFFU) << 16U);
}
[[nodiscard]] constexpr Extent2D unpack_extent(u32 packed) noexcept {
    return Extent2D{packed & 0xFFFFU, packed >> 16U};
}

/// `CyUpscaleConstants` in upscale_common.slang, word for word: 128 bytes, the push-constant limit.
/// The extents are packed two to a word so the whole block fits it.
struct alignas(16) UpscaleConstants {
    /// This frame's clip space (unjittered NDC and depth) to last frame's clip space, as four rows.
    f32 current_to_previous[4][4] = {};
    /// x: the render extent, y: the output extent, each packed by `pack_extent`; z: unused, zero;
    /// w: the `kUpscale*` flags.
    u32 extents[4] = {};
    /// x, y: the sample offset — the unjittered position of render texel i is i + 0.5 + (x, y).
    /// z: m22, w: m32 of the projection, for a depth in metres.
    f32 sample[4] = {};
    /// x: the still history cap, y: the moving history cap, z: the motion (output pixels) at which
    /// the cap reaches the moving one, w: the depth tolerance.
    f32 history[4] = {};
    /// x: the clip gamma when still, y: when moving, z: exposure, w: reactive strength.
    f32 control[4] = {};

    [[nodiscard]] Extent2D render() const noexcept { return unpack_extent(extents[0]); }
    [[nodiscard]] Extent2D output() const noexcept { return unpack_extent(extents[1]); }
    [[nodiscard]] u32 flags() const noexcept { return extents[3]; }
};
static_assert(sizeof(UpscaleConstants) == 128, "CyUpscaleConstants is eight 16-byte rows");

/// `CySharpenConstants` in upscale_sharpen.slang.
struct alignas(16) SharpenConstants {
    /// x, y: the output extent.
    u32 extent[4] = {};
    /// x: sharpness in [0, 1], y: exposure.
    f32 control[4] = {};
};
static_assert(sizeof(SharpenConstants) == 32, "CySharpenConstants is two 16-byte rows");

/// The dispatches' constants. Refuses an empty extent, a render extent larger than the output, a
/// non-invertible camera-relative to clip transform, and settings out of range.
[[nodiscard]] Expected<UpscaleConstants, Error> make_upscale_constants(
    const UpscaleSettings& settings, const UpscaleView& view) noexcept;

[[nodiscard]] SharpenConstants make_sharpen_constants(const UpscaleSettings& settings,
                                                      Extent2D output) noexcept;

// --- The host reference -------------------------------------------------------------------------
//
// Every function below is a shader's, written expression for expression in the shader's order, so
// a suite with no device can ask what the reconstruction DOES, and the device suite can ask whether
// the device did it.

/// What the reconstruction reads, as the device reads it: row-major from the top-left.
struct UpscaleInputs {
    /// `constants.extents.xy` long: this frame's linear HDR colour, depth (reversed-Z, 0 is the
    /// cleared far plane), motion (normalised screen space, pointing at the past) — empty without
    /// `kUpscaleVelocity` — and the colour before translucency, empty without `kUpscaleReactive`.
    Span<const Vec4> color;
    Span<const f32> depth;
    Span<const Vec2> velocity;
    Span<const Vec4> opaque;
    /// The output extent long: last frame's history, colour and weight, and its state — x the
    /// lock, y the depth of the surface the history texel holds; read only with
    /// `kUpscaleHistoryValid`.
    Span<const Vec4> history;
    Span<const Vec2> previous_state;
};

/// One reconstructed output texel: the new history, colour and weight, and its state.
struct UpscaleTexel {
    Vec4 history{0.0F, 0.0F, 0.0F, 0.0F};
    /// x: the lock, y: the reversed-Z depth of the surface this texel now holds — the nearest of
    /// the nine around its sample, the one whose motion it took.
    Vec2 state{0.0F, 0.0F};
};

/// `cyUpscaleReconstruct` at one output pixel.
[[nodiscard]] UpscaleTexel upscale_reference_at(const UpscaleInputs& inputs,
                                                const UpscaleConstants& constants, u32 x,
                                                u32 y) noexcept;

/// The whole output. `out` is the output extent long, and so is `state` unless it is empty.
[[nodiscard]] Status upscale_reference(const UpscaleInputs& inputs,
                                       const UpscaleConstants& constants, Span<Vec4> out,
                                       Span<Vec2> state = {}) noexcept;

/// `cyUpscaleSharpen` over a reconstructed history. Alpha (the weight) is replaced by one. `out`
/// and `history` are the output extent long.
[[nodiscard]] Status sharpen_reference(Span<const Vec4> history, const SharpenConstants& constants,
                                       Span<Vec4> out) noexcept;

/// `cyUpscaleResample` at one output pixel: the depth and motion of the render texel whose sample
/// is nearest its centre — what the stages after the upscaler (depth of field, motion blur, the
/// selection outlines) read at the output resolution. `velocity` may be empty.
struct ResampledTexel {
    f32 depth = 0.0F;
    Vec2 velocity{0.0F, 0.0F};
};
[[nodiscard]] ResampledTexel resample_reference_at(const UpscaleInputs& inputs,
                                                   const UpscaleConstants& constants, u32 x,
                                                   u32 y) noexcept;

}  // namespace cy::rendering::upscale
