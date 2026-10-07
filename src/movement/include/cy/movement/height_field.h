// SPDX-License-Identifier: MIT
#pragma once
// Terrain height for authoritative units, from samples cooked to `Fixed16`. Design §8.
//
// The terrain's float heights are never read in a tick. `cook()` converts them once — through
// `detmath::from_f32_cooked`, then narrowed to Q16.16, which holds ±32 768 m at 1.5e-5 m — and a
// tick reads only the cooked samples. This is the dense per-element storage `Fixed16` exists for
// (design §4.2): a 1024 x 1024 field is 4 MB as `Fixed16` and 8 MB as `Fixed`.
//
// The sample spacing is a power of two metres, 2^`cell_shift`, so a position's cell and its
// fraction within the cell are a shift and a mask of the raw value — exact, and with no division in
// the per-unit path. The height between samples is bilinear, each lerp rounded as `Fixed *` rounds.
// Outside the field the nearest edge sample is used.

#include <cy/core/base/expected.h>
#include <cy/core/detmath/fixed.h>
#include <cy/core/detmath/vec.h>
#include <cy/core/memory/array.h>

namespace cy::movement {

using detmath::Fixed;
using detmath::Fixed16;
using detmath::FixedVec2;

class FixedHeightField {
public:
    explicit FixedHeightField(Allocator& allocator) noexcept : samples_(allocator) {}

    /// Convert `heights` (`width` x `depth`, row-major, rows along +Z) into cooked samples spaced
    /// 2^`cell_shift` metres from `origin`. THE FLOAT STEP, and it runs at cook or load, never in a
    /// tick. `cell_shift` is in [-8, 16]; a height beyond ±32 768 m is refused.
    [[nodiscard]] Status cook(Span<const f32> heights, u32 width, u32 depth, FixedVec2 origin,
                              int cell_shift) noexcept;

    /// Adopt samples that were cooked elsewhere: the raw Q16.16 values a cooked asset stores.
    [[nodiscard]] Status adopt(Span<const Fixed16> samples, u32 width, u32 depth, FixedVec2 origin,
                               int cell_shift) noexcept;

    /// The bilinear height at `point` (world x, z). Zero for an empty field.
    [[nodiscard]] Fixed height_at(FixedVec2 point) const noexcept;

    [[nodiscard]] bool empty() const noexcept { return samples_.empty(); }
    [[nodiscard]] u32 width() const noexcept { return width_; }
    [[nodiscard]] u32 depth() const noexcept { return depth_; }
    [[nodiscard]] Span<const Fixed16> samples() const noexcept { return samples_.span(); }

private:
    [[nodiscard]] Fixed sample(i64 x, i64 z) const noexcept;

    Array<Fixed16> samples_;
    FixedVec2 origin_;
    u32 width_ = 0;
    u32 depth_ = 0;
    int cell_shift_ = 0;
};

}  // namespace cy::movement
