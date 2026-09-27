#pragma once
// SPDX-License-Identifier: MIT
// The display-referred grading lookup: `.cube` files, the encoding the runtime table is indexed in,
// and the bake that folds a parametric grade and a `.cube` look into one table. Task 8.4.
//
// `rendering-post-processing` — "Colour grading" asks for a 3D LUT (`.cube`) "applied in log space"
// and a grade "bakeable into a single 3D LUT … so the runtime cost is one texture lookup"; "Chain
// order and colour space" puts that lookup at step 12, AFTER tonemapping, on display-referred
// colour.
//
// ================================================================================================
// WHY A SECOND ENCODING BESIDE `grading.h`'s
// ================================================================================================
//
// `log_encode()` spans eighteen stops of SCENE-referred light, -10 to +8 around middle grey. After
// the tone curve every value is in [0, 1], so eight of those stops would index nothing and a 33³
// table would spend fourteen of its thirty-three samples a side on colours that cannot reach it.
// `display_log_encode()` is the same idea over the range that exists at step 12: a base-two log
// with a toe, `log2(1 + x / ε)` normalised so that 0 → 0 and 1 → 1 exactly, with ε = 2⁻¹². Above
// the toe it is logarithmic — thirty-two intervals over twelve stops, 0.375 stop a cell, which is
// where the shadows get their resolution — and below it it is linear, so black is a lattice point
// rather than a value the table can only approach.
//
// ================================================================================================
// A `.cube` IS READ IN ITS OWN ENCODING
// ================================================================================================
//
// A `.cube` a colourist exports is indexed by DISPLAY-ENCODED values — the sRGB transfer function —
// over `DOMAIN_MIN`..`DOMAIN_MAX`, red fastest. The engine's table is indexed by the encoding
// above, so the bake does not copy the file: at every lattice point of the engine's table it
// decodes to display-linear, applies the parametric grade, re-encodes into the file's input
// encoding, samples the file trilinearly, and decodes the result. Any size of `.cube` becomes one
// engine table of the size the runtime binds, and the runtime still does exactly one lookup.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/post/grading.h>

#include <string_view>

namespace cy::rendering {

/// The edge of the table the runtime binds. 33 is the `.cube` standard, and odd puts a lattice
/// point on the neutral axis's midpoint.
inline constexpr u32 kDisplayLutSize = 33;

/// The largest `.cube` edge `parse_cube_lut` accepts. 65³ is the largest a grading tool exports by
/// default; beyond it a file is almost certainly not a 3D LUT.
inline constexpr u32 kMaxCubeLutSize = 129;

/// How a `.cube`'s input axis and output values are encoded.
enum class CubeEncoding : u8 {
    /// The sRGB transfer function. What an exported creative LUT expects, and the default.
    Srgb = 0,
    /// Display-linear. A LUT generated for the engine rather than exported from a tool.
    Linear,
    Count,
};

[[nodiscard]] const char* cube_encoding_name(CubeEncoding encoding) noexcept;

/// A parsed `.cube` 3D LUT.
struct CubeLut {
    explicit CubeLut(Allocator& allocator) noexcept : entries(allocator) {}

    u32 size = 0;
    Vec3 domain_min{0.0F, 0.0F, 0.0F};
    Vec3 domain_max{1.0F, 1.0F, 1.0F};
    /// `size³` output colours, red fastest — the file's own order.
    Array<Vec3> entries;

    /// Trilinear lookup at a point of the file's input domain. Outside the domain clamps.
    [[nodiscard]] Vec3 sample(Vec3 input) const noexcept;
};

/// Parse a `.cube` file's text. Accepts `TITLE`, `LUT_3D_SIZE`, `DOMAIN_MIN`, `DOMAIN_MAX`,
/// comments and blank lines; REFUSES a 1D LUT, a missing or out-of-range size, an entry count that
/// is not the size cubed, and a token that is not a number. A LUT that parsed wrongly is a grade
/// that looks wrong with no error, so every one of those is an error instead.
[[nodiscard]] Status parse_cube_lut(std::string_view text, CubeLut& out) noexcept;

/// The sRGB transfer function and its inverse, per channel, clamped to [0, 1].
[[nodiscard]] f32 srgb_encode(f32 linear) noexcept;
[[nodiscard]] f32 srgb_decode(f32 encoded) noexcept;

/// The display-referred log encoding the runtime table is indexed in. See the header comment.
/// Exact at 0 and 1; clamps outside [0, 1].
[[nodiscard]] f32 display_log_encode(f32 display_linear) noexcept;
[[nodiscard]] f32 display_log_decode(f32 encoded) noexcept;
[[nodiscard]] Vec3 display_log_encode(Vec3 display_linear) noexcept;
[[nodiscard]] Vec3 display_log_decode(Vec3 encoded) noexcept;

/// What one engine table is baked from: a parametric grade, then optionally a `.cube` look.
struct DisplayGrade {
    GradingSettings settings;
    /// Null for none.
    const CubeLut* cube = nullptr;
    CubeEncoding cube_encoding = CubeEncoding::Srgb;
};

/// The reference: the grade evaluated directly on one display-linear colour. The parametric grade
/// first, clamped to the displayable range, then the `.cube` look in the file's encoding.
[[nodiscard]] Vec3 apply_display_grade(Vec3 display_linear, const DisplayGrade& grade) noexcept;

/// Bake the grade into a `size`³ table indexed by `display_log_encode`, x fastest — the layout a 3D
/// texture upload wants. Each entry is the graded colour ENCODED the same way, so the runtime
/// interpolates in the encoding and decodes once: that is exact for any grade linear in the
/// encoding (the identity, a channel permutation, a gain above the toe), where interpolating
/// display-linear outputs across a 0.375-stop cell is wrong by up to 0.8 %. False when the array
/// is too small or the size is below 2.
[[nodiscard]] bool bake_display_lut(const DisplayGrade& grade, u32 size, Vec3* out,
                                    usize capacity) noexcept;

/// Sample a baked table the way the runtime does: encode, trilinear, decode. The host twin of the
/// graded resolve's lookup.
[[nodiscard]] Vec3 sample_display_lut(const Vec3* lut, u32 size, Vec3 display_linear) noexcept;

/// True when every entry of a baked table is its own lattice point to within `tolerance`, in the
/// encoding (1e-5 is 0.008 % of a display-linear value). An identity table is a stage that does
/// nothing, and the chain drops it rather than paying a lookup that can only round.
[[nodiscard]] bool display_lut_is_identity(const Vec3* lut, u32 size,
                                           f32 tolerance = 1e-5F) noexcept;

}  // namespace cy::rendering
