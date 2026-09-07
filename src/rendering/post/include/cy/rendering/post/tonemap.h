#pragma once
// Tonemapping: six operators, AgX by default, parameterised for the output transfer function.
// Task 8.4.
//
// `rendering-post-processing` — "Tonemapping". The operator list is the specification's, and **AgX
// is the default "for its hue stability and gentle highlight desaturation"** — which is a decision
// about what a bright saturated light should look like, not a preference. A saturated red light
// under the ACES fit shifts toward orange as it clips; under AgX it desaturates toward white and
// keeps its hue. `tests/test_tonemap.cpp` measures both statements rather than asserting the
// default and moving on.
//
// THE TRANSFER FUNCTION IS PART OF TONEMAPPING, NOT AFTER IT. "Tonemapping SHALL be parameterised
// for the output transfer function: sRGB for SDR, PQ or scRGB for HDR displays, with the display's
// peak luminance taken into account." An operator that mapped to SDR white and then had a PQ encode
// bolted on afterwards would compress the highlights twice, which is why `OutputTarget` is an input
// to the operator here rather than a separate stage's setting.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>

namespace cy::rendering {

/// The operators `rendering-post-processing` lists, in its order.
enum class TonemapOperator : u8 {
    /// Clamp. Present because a debug view of raw radiance needs it.
    None = 0,
    Reinhard,
    ReinhardExtended,
    /// The fitted RRT+ODT approximation.
    Aces,
    /// The default.
    AgX,
    /// A user-supplied curve or LUT, applied by the caller.
    Custom,
    Count,
};

[[nodiscard]] const char* tonemap_operator_name(TonemapOperator op) noexcept;

/// The default, stated as a function so that the reason travels with the value.
[[nodiscard]] constexpr TonemapOperator default_tonemap_operator() noexcept {
    return TonemapOperator::AgX;
}

enum class TransferFunction : u8 {
    /// SDR.
    Srgb = 0,
    /// HDR10.
    Pq,
    /// HDR, linear-ish extended range.
    ScRgb,
    Count,
};

[[nodiscard]] const char* transfer_function_name(TransferFunction function) noexcept;

struct OutputTarget {
    TransferFunction transfer = TransferFunction::Srgb;
    /// The display's peak luminance in nits. 100 for SDR; 1000 and above for HDR.
    f32 peak_nits = 100.0F;
};

struct TonemapSettings {
    TonemapOperator op = default_tonemap_operator();
    OutputTarget output;
    /// `ReinhardExtended`'s white point, in the same units as the input.
    f32 white_point = 4.0F;
};

/// Scene-referred, exposed linear colour to display-referred colour in [0,1] for SDR, or to the
/// display's range for HDR. One channel at a time is deliberately NOT offered: AgX and ACES are
/// three-channel transforms and applying them per channel is exactly the mistake that produces the
/// hue shift AgX exists to avoid.
[[nodiscard]] Vec3 tonemap(Vec3 colour, const TonemapSettings& settings) noexcept;

/// The output transfer function, applied after the operator. Separate because a golden-image test
/// wants the display-referred value before encoding.
[[nodiscard]] Vec3 encode_output(Vec3 display_referred, const OutputTarget& target) noexcept;

/// Saturation as `(max - min) / max`, in [0,1]. Used by the tests to measure "highlights desaturate
/// rather than hue-shift" as a number, and exposed because a diagnostic view wants the same figure.
[[nodiscard]] f32 colour_saturation(Vec3 colour) noexcept;

/// Hue angle in radians, from the same hexcone model. Zero for an achromatic colour.
[[nodiscard]] f32 colour_hue(Vec3 colour) noexcept;

}  // namespace cy::rendering
