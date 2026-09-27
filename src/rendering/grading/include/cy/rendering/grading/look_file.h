// SPDX-License-Identifier: MIT
#pragma once
// A look from disk: the `.cygrade` file, the `.cube` it names, and the table they bake into.
//
// The parsing and the bake are cy::rendering-post's (`look.h`, `lut.h`) and touch no file; this is
// the one place that reads them, so a sample and a test load a committed look the same way. A
// `.cube` path in a look is relative to the look file's own directory, which is what lets a look
// and its cube be moved together.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/post/look.h>
#include <cy/rendering/post/lut.h>

namespace cy::rendering::grading {

/// Read and parse a `.cube` file.
[[nodiscard]] Status load_cube(const char* path, CubeLut& out) noexcept;

/// Read a `.cygrade` file, the `.cube` it names if any, and bake the table the graded resolve
/// binds: `look.lut_size`³ entries into `table`, `bake_display_lut`'s layout.
[[nodiscard]] Status load_look(const char* path, Allocator& allocator, Look& look,
                               Array<Vec3>& table) noexcept;

}  // namespace cy::rendering::grading
