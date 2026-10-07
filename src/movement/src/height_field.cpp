// SPDX-License-Identifier: MIT
// Reading cooked terrain heights. Integer arithmetic only; the cook is height_field_cook.cpp.

#include <cy/movement/height_field.h>

namespace cy::movement {

namespace {

/// A local coordinate is clamped to this many metres before it is scaled to cells, so that the
/// scaling by up to 2^8 cannot overflow. Any field ends long before it.
constexpr i64 kLocalLimitRaw = i64{1} << (22 + Fixed::kFractionBits);

/// `local` in cells: a shift of the raw value by the spacing's exponent.
[[nodiscard]] i64 to_cells(Fixed local, int cell_shift) noexcept {
    i64 raw = local.raw;
    raw = raw > kLocalLimitRaw ? kLocalLimitRaw : raw;
    raw = raw < -kLocalLimitRaw ? -kLocalLimitRaw : raw;
    if (cell_shift >= 0) {
        return raw >> cell_shift;
    }
    return static_cast<i64>(static_cast<u64>(raw) << (-cell_shift));
}

/// The cell and the fraction within it along one axis, clamped to the samples that exist.
struct Axis {
    i64 cell = 0;
    Fixed fraction;
};

[[nodiscard]] Axis axis_of(i64 cells_raw, u32 count) noexcept {
    const i64 cell = cells_raw >> Fixed::kFractionBits;
    if (cell < 0) {
        return Axis{0, Fixed::zero()};
    }
    if (cell >= i64{count} - 1) {
        return Axis{i64{count} - 1, Fixed::zero()};
    }
    return Axis{cell, Fixed::from_raw(cells_raw & (Fixed::kOneRaw - 1))};
}

[[nodiscard]] Fixed lerp(Fixed a, Fixed b, Fixed t) noexcept {
    return a + ((b - a) * t);
}

}  // namespace

Status FixedHeightField::adopt(Span<const Fixed16> samples, u32 width, u32 depth, FixedVec2 origin,
                               int cell_shift) noexcept {
    if (width == 0 || depth == 0 || samples.size() != usize{width} * depth) {
        return fail(ErrorCode::InvalidArgument,
                    "movement: a height field's samples do not match its width and depth");
    }
    if (cell_shift < -8 || cell_shift > 16) {
        return fail(ErrorCode::InvalidArgument,
                    "movement: a height field's spacing must be 2^-8 to 2^16 metres");
    }
    samples_.clear();
    if (Status copied = samples_.append(samples); !copied) {
        return copied;
    }
    origin_ = origin;
    width_ = width;
    depth_ = depth;
    cell_shift_ = cell_shift;
    return ok();
}

Fixed FixedHeightField::sample(i64 x, i64 z) const noexcept {
    const i64 cx = x < i64{width_} ? x : i64{width_} - 1;
    const i64 cz = z < i64{depth_} ? z : i64{depth_} - 1;
    return samples_[static_cast<usize>((cz * width_) + cx)].widen();
}

Fixed FixedHeightField::height_at(FixedVec2 point) const noexcept {
    if (samples_.empty()) {
        return Fixed::zero();
    }
    const Axis x = axis_of(to_cells(point.x - origin_.x, cell_shift_), width_);
    const Axis z = axis_of(to_cells(point.y - origin_.y, cell_shift_), depth_);
    const Fixed near_row = lerp(sample(x.cell, z.cell), sample(x.cell + 1, z.cell), x.fraction);
    const Fixed far_row =
        lerp(sample(x.cell, z.cell + 1), sample(x.cell + 1, z.cell + 1), x.fraction);
    return lerp(near_row, far_row, z.fraction);
}

}  // namespace cy::movement
