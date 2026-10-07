// SPDX-License-Identifier: MIT
#pragma once
// The mover's crowd kernel — integration, the neighbour grid and pairwise separation — written once
// over a scalar policy. Design §8, §9.1 and §11.
//
// ONE ALGORITHM TEXT, TWO KINDS OF ARITHMETIC. `KinematicMover` instantiates it over `FixedPolicy`,
// which is the authoritative path. The movement-step benchmark instantiates it a second time over
// an `f32` policy of its own (benchmarks/movement/), because design §11's budget is a RATIO —
// "≤ 2.5× the same kernel in f32" — and a ratio against a different algorithm would measure
// nothing. The `f32` policy lives with the benchmark, so this module compiles no float into the
// authoritative path.
//
// A policy supplies:
//
//   Scalar, Vec, Wide          the value, a plane vector of two, and the exact square of a value
//   length_squared(Vec) -> Wide, square(Scalar) -> Wide, sqrt(Wide) -> Scalar
//   clamp_length(Vec, Scalar) -> Vec
//   cell(Scalar, int shift)    floor(value / 2^shift), as an integer
//   zero(), wide_zero()        constants
//   Vec has x and y, and +, -, and * by a Scalar; Scalar has + - * / and <; Wide has < and ==.
//
// DETERMINISM, INDEPENDENT OF THE WORKER COUNT. Separation is a Jacobi step: every unit reads the
// positions of the previous pass and writes only its own new position, and sums its neighbours'
// pushes in a fixed order — grid cell by grid cell, row-major over the 3 x 3 block, and within a
// cell in unit order, which is the counting sort's stable order. So a range of units can run on any
// worker, in any order, and the result is the same bits as the serial loop. The grid is built
// serially (a counting sort, O(units + cells)).
//
// Coincident units — an exact zero distance — are pushed apart along x, the lower unit index toward
// -x, so even that case has one answer.

#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::movement {

/// What the kernel is told about the world it works in.
struct CrowdGridParams {
    /// Grid cells are 2^`cell_shift` metres on a side. Choose at least the largest unit diameter,
    /// so every overlapping pair is in the 3 x 3 block.
    int cell_shift = 1;
    /// The grid's extent, in cells, and its origin cell. Units outside are binned into the border
    /// cells, which keeps the grid bounded without dropping anyone.
    i64 origin_x = 0;
    i64 origin_z = 0;
    u32 cells_x = 1;
    u32 cells_z = 1;
};

/// What one separation pass found, summed in unit order.
struct CrowdKernelCounts {
    /// Overlapping pairs, counted once from each side.
    u64 overlaps = 0;
};

/// The units' arrays and the kernel over them: what moves a crowd one tick, in whichever arithmetic
/// `Policy` supplies. Structure-of-arrays, one entry per unit, all the same length; an inactive
/// unit has `active` zero and is neither moved nor pushes anyone.
template <class Policy>
class CrowdKernel {
public:
    using Scalar = Policy::Scalar;
    using Vec = Policy::Vec;
    using Wide = Policy::Wide;

    explicit CrowdKernel(Allocator& allocator) noexcept
        : position(allocator),
          velocity(allocator),
          desired(allocator),
          radius(allocator),
          max_speed(allocator),
          max_acceleration(allocator),
          active(allocator),
          next_position(allocator),
          overlaps(allocator),
          unit_cell_(allocator),
          cell_start_(allocator),
          cell_units_(allocator),
          cell_fill_(allocator) {}

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(position.size()); }

    /// Append a unit. Every array grows together.
    [[nodiscard]] Status push(Vec at, Scalar unit_radius, Scalar speed,
                              Scalar acceleration) noexcept {
        if (!position.push_back(at) || !velocity.push_back(Vec{}) || !desired.push_back(Vec{}) ||
            !radius.push_back(unit_radius) || !max_speed.push_back(speed) ||
            !max_acceleration.push_back(acceleration) || !active.push_back(u8{1}) ||
            !next_position.push_back(at) || !overlaps.push_back(u32{0}) ||
            !unit_cell_.push_back(u32{0})) {
            return fail(ErrorCode::OutOfMemory, "movement: could not grow the unit arrays");
        }
        return ok();
    }

    /// Steer toward the desired velocity within the acceleration limit, cap at the unit's speed,
    /// and move. Units [begin, end). Writes `next_position`.
    void integrate(u32 begin, u32 end, Scalar dt) noexcept {
        for (u32 i = begin; i < end; ++i) {
            if (active[i] == 0) {
                next_position[i] = position[i];
                continue;
            }
            const Vec change =
                Policy::clamp_length(desired[i] - velocity[i], max_acceleration[i] * dt);
            velocity[i] = Policy::clamp_length(velocity[i] + change, max_speed[i]);
            next_position[i] = position[i] + (velocity[i] * dt);
        }
    }

    /// Adopt `next_position` as the position. Serial: the separation pass reads one array while it
    /// writes the other, so the swap is between passes.
    void commit() noexcept {
        for (u32 i = 0; i < size(); ++i) {
            position[i] = next_position[i];
        }
    }

    /// Bin every active unit into the grid by its position: a stable counting sort, so a cell's
    /// units are in unit order.
    [[nodiscard]] Status build_grid(const CrowdGridParams& grid) noexcept {
        grid_ = grid;
        const usize cells = usize{grid.cells_x} * grid.cells_z;
        cell_start_.clear();
        if (Status sized = cell_start_.resize(cells + 1); !sized) {
            return sized;
        }
        for (u32 i = 0; i < size(); ++i) {
            unit_cell_[i] = cell_index(position[i]);
            if (active[i] != 0) {
                ++cell_start_[unit_cell_[i] + 1];
            }
        }
        for (usize cell = 0; cell < cells; ++cell) {
            cell_start_[cell + 1] += cell_start_[cell];
        }
        cell_units_.clear();
        cell_fill_.clear();
        if (Status sized = cell_units_.resize(cell_start_[cells]); !sized) {
            return sized;
        }
        if (Status copied = cell_fill_.append(cell_start_.span()); !copied) {
            return copied;
        }
        for (u32 i = 0; i < size(); ++i) {
            if (active[i] != 0) {
                cell_units_[cell_fill_[unit_cell_[i]]++] = i;
            }
        }
        return ok();
    }

    /// One Jacobi separation pass over units [begin, end): each takes `share` of every overlap it
    /// has, along the line between the centres, and moves by at most `max_push`. Reads `position`,
    /// writes `next_position` and `overlaps`.
    void separate(u32 begin, u32 end, Scalar share, Scalar max_push) noexcept {
        for (u32 i = begin; i < end; ++i) {
            next_position[i] = position[i];
            overlaps[i] = 0;
            if (active[i] == 0) {
                continue;
            }
            const Vec push = neighbourhood_push(i, share);
            next_position[i] = position[i] + Policy::clamp_length(push, max_push);
        }
    }

    /// The pairs and overlaps the last separation pass counted, summed in unit order.
    [[nodiscard]] CrowdKernelCounts counts() const noexcept {
        CrowdKernelCounts total;
        for (u32 i = 0; i < size(); ++i) {
            total.overlaps += overlaps[i];
        }
        return total;
    }

    Array<Vec> position;
    Array<Vec> velocity;
    Array<Vec> desired;
    Array<Scalar> radius;
    Array<Scalar> max_speed;
    Array<Scalar> max_acceleration;
    Array<u8> active;
    Array<Vec> next_position;
    Array<u32> overlaps;

private:
    [[nodiscard]] u32 cell_index(Vec at) const noexcept {
        const i64 x =
            clamp_cell(Policy::cell(at.x, grid_.cell_shift) - grid_.origin_x, grid_.cells_x);
        const i64 z =
            clamp_cell(Policy::cell(at.y, grid_.cell_shift) - grid_.origin_z, grid_.cells_z);
        return static_cast<u32>((z * grid_.cells_x) + x);
    }

    /// `cell` clamped into [0, `count`): a unit outside the grid is binned into its border.
    [[nodiscard]] static i64 clamp_cell(i64 cell, u32 count) noexcept {
        if (cell < 0) {
            return 0;
        }
        return cell >= i64{count} ? i64{count} - 1 : cell;
    }

    /// The sum of the pushes unit `i` gets from the 3 x 3 block of cells around its own, in
    /// row-major cell order and unit order within a cell: the fixed order every worker sums in.
    [[nodiscard]] Vec neighbourhood_push(u32 i, Scalar share) noexcept {
        Vec push{};
        const u32 home = unit_cell_[i];
        const i64 home_x = static_cast<i64>(home % grid_.cells_x);
        const i64 home_z = static_cast<i64>(home / grid_.cells_x);
        for (i64 z = home_z - 1; z <= home_z + 1; ++z) {
            for (i64 x = home_x - 1; x <= home_x + 1; ++x) {
                push = push + cell_push(i, x, z, share);
            }
        }
        return push;
    }

    /// The pushes unit `i` gets from the units of cell (`x`, `z`); zero outside the grid.
    [[nodiscard]] Vec cell_push(u32 i, i64 x, i64 z, Scalar share) noexcept {
        Vec push{};
        if (x < 0 || z < 0 || x >= i64{grid_.cells_x} || z >= i64{grid_.cells_z}) {
            return push;
        }
        const auto cell = static_cast<usize>((z * grid_.cells_x) + x);
        for (u32 slot = cell_start_[cell]; slot < cell_start_[cell + 1]; ++slot) {
            const u32 j = cell_units_[slot];
            if (j != i) {
                push = push + pushed_by(i, j, share);
            }
        }
        return push;
    }

    /// The push unit `j` gives unit `i`: zero unless they overlap, decided on the exact squares.
    [[nodiscard]] Vec pushed_by(u32 i, u32 j, Scalar share) noexcept {
        const Scalar reach = radius[i] + radius[j];
        const Vec apart = position[i] - position[j];
        const Wide distance_squared = Policy::length_squared(apart);
        if (!(distance_squared < Policy::square(reach))) {
            return Vec{};
        }
        ++overlaps[i];
        if (distance_squared == Policy::wide_zero()) {
            const Scalar along = reach * share;
            return i < j ? Vec{Policy::zero() - along, Policy::zero()} : Vec{along, Policy::zero()};
        }
        const Scalar distance = Policy::sqrt(distance_squared);
        const Vec direction{apart.x / distance, apart.y / distance};
        return direction * ((reach - distance) * share);
    }

    CrowdGridParams grid_;
    Array<u32> unit_cell_;
    Array<u32> cell_start_;
    Array<u32> cell_units_;
    Array<u32> cell_fill_;
};

}  // namespace cy::movement
