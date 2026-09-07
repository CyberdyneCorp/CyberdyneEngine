#pragma once
// Colour grading, and the bake that turns it into one texture lookup. Task 8.4.
//
// `rendering-post-processing` — "Colour grading": white balance, per-channel lift/gamma/gain,
// shadows/midtones/highlights wheels with range boundaries, saturation, contrast, hue shift,
// channel mixer, and a 3D LUT applied in LOG SPACE. And: "Grading parameters SHALL be bakeable into
// a single 3D LUT at build time so the runtime cost is one texture lookup."
//
// ================================================================================================
// LOG SPACE IS NOT A DETAIL
// ================================================================================================
//
// "WHEN a LUT is applied THEN the input SHALL be converted to a log encoding first, so the LUT has
// adequate precision in shadows." A 33³ LUT indexed by a linear value spends most of its resolution
// on the top stop and almost none below 0.05, which is where skin shadows and night interiors live;
// the visible result is banding in exactly the region a colourist is working in. `log_encode()` and
// `log_decode()` are that encoding, and `bake_grading_lut()` samples in it.
//
// THE BAKE IS THE POINT. `apply_grading()` is the reference — every parameter, evaluated directly —
// and `bake_grading_lut()` is what ships. `tests/test_grading.cpp` asserts they agree, because a
// bake that drifts from its reference is a grade that looks different in the editor and in the
// build, which is the worst kind of difference to debug.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>

namespace cy::rendering {

/// One of the three tonal ranges the wheels act on.
struct ColourWheel {
    /// Multiplied in. Neutral is white.
    Vec3 gain{1.0F, 1.0F, 1.0F};
    /// Added. Neutral is black.
    Vec3 offset{0.0F, 0.0F, 0.0F};
};

struct GradingSettings {
    /// Kelvin. 6500 is neutral; lower is warmer.
    f32 temperature = 6500.0F;
    /// Green/magenta, in [-1, 1]. Zero is neutral.
    f32 tint = 0.0F;

    /// The classic three, per channel. Lift raises the black point, gain the white, gamma bends
    /// between them.
    Vec3 lift{0.0F, 0.0F, 0.0F};
    Vec3 gamma{1.0F, 1.0F, 1.0F};
    Vec3 gain{1.0F, 1.0F, 1.0F};

    ColourWheel shadows;
    ColourWheel midtones;
    ColourWheel highlights;
    /// Luminance below which a pixel is entirely shadows, and above which it is entirely
    /// highlights. The "range boundaries" the requirement names.
    f32 shadow_boundary = 0.25F;
    f32 highlight_boundary = 0.6F;

    f32 saturation = 1.0F;
    f32 contrast = 1.0F;
    /// Radians.
    f32 hue_shift = 0.0F;

    /// Rows are the output channels: `channel_mixer[0]` is how much of R, G and B goes into R.
    Vec3 channel_mixer[3] = {Vec3{1.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F},
                             Vec3{0.0F, 0.0F, 1.0F}};

    /// True when nothing has been touched. A neutral grade should bake to an identity LUT and the
    /// chain should be able to drop the stage entirely.
    [[nodiscard]] bool neutral() const noexcept;
};

/// The log encoding a LUT is indexed in. A shifted log2 over roughly 18 stops, which puts the
/// shadows where a 33³ LUT has resolution to spend on them.
[[nodiscard]] f32 log_encode(f32 linear) noexcept;
[[nodiscard]] f32 log_decode(f32 encoded) noexcept;
[[nodiscard]] Vec3 log_encode(Vec3 linear) noexcept;
[[nodiscard]] Vec3 log_decode(Vec3 encoded) noexcept;

/// The reference evaluation: every parameter, applied directly, in the order a colourist expects —
/// white balance, then the mixer, then lift/gamma/gain, then the wheels, then contrast, saturation
/// and hue.
[[nodiscard]] Vec3 apply_grading(Vec3 colour, const GradingSettings& settings) noexcept;

/// Bake the grade into a `size`³ lookup table, indexed in log space. `out` receives `size³` RGB
/// triples in x-fastest order, which is the layout a 3D texture upload wants.
///
/// Returns false when the array is too small or the size is degenerate. `size` is conventionally
/// 33: it is the `.cube` standard, and an odd size puts a sample exactly on the neutral axis.
[[nodiscard]] bool bake_grading_lut(const GradingSettings& settings, u32 size, Vec3* out,
                                    usize capacity) noexcept;

/// Sample a baked table with trilinear interpolation, in log space. The runtime's whole grading
/// cost, and the function `tests/test_grading.cpp` compares against `apply_grading()`.
[[nodiscard]] Vec3 sample_grading_lut(const Vec3* lut, u32 size, Vec3 colour) noexcept;

}  // namespace cy::rendering
