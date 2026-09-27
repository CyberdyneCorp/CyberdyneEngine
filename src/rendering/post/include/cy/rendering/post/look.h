#pragma once
// SPDX-License-Identifier: MIT
// A look: the content a colourist commits for one grade — the parametric controls, an optional
// `.cube`, and the size of the table the two are baked into. Task 8.4.
//
// `rendering-post-processing` — "Colour grading" names the controls and the `.cube`; "Grade baked
// to a LUT" says static parameters are baked. A `.cygrade` file is those parameters as text, so a
// grade is changed by editing content rather than by recompiling a sample:
//
//     cygrade 1
//     name Warm evening
//     temperature 5200            # Kelvin, 6500 neutral
//     tint 0.02                   # green/magenta, [-1, 1]
//     lift 0.01 0.0 -0.01         # per channel
//     gamma 1.0 1.0 1.0
//     gain 1.06 1.0 0.92
//     contrast 1.08
//     saturation 1.1
//     hue-shift 0                 # degrees
//     shadows-gain 1 1 1          # and -offset, midtones-*, highlights-*
//     shadow-boundary 0.25
//     highlight-boundary 0.6
//     mixer-red 1 0 0             # and mixer-green, mixer-blue: the output channel's row
//     cube looks/teal.cube        # relative to the look file; optional
//     cube-encoding srgb          # or linear
//     lut-size 33
//
// Unknown keys are REFUSED rather than skipped: a misspelt `saturaton 1.2` that parsed would be a
// grade that silently lost a control.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/post/grading.h>
#include <cy/rendering/post/lut.h>

#include <string_view>

namespace cy::rendering {

struct Look {
    char name[64] = "";
    GradingSettings settings;
    /// The `.cube` path as written — relative to the look file. Empty for none.
    char cube[256] = "";
    CubeEncoding cube_encoding = CubeEncoding::Srgb;
    u32 lut_size = kDisplayLutSize;
};

/// Parse a `.cygrade` file's text. The first non-comment line must be `cygrade 1`.
[[nodiscard]] Status parse_look(std::string_view text, Look& out) noexcept;

}  // namespace cy::rendering
