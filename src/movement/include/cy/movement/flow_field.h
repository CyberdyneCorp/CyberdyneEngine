// SPDX-License-Identifier: MIT
#pragma once
// A flow field over a `Fixed` world: one Dijkstra integration guiding many units to a shared
// destination. Design §9.1 ("Flow-field integration costs become Fixed. Its queue already breaks
// ties by cell index") and task 6.2.
//
// The same shape as `cy::navigation::FlowField` — a grid over a bounded region, each cell holding a
// direction toward the nearest destination — with every quantity integer:
//
//   cells        2^`cell_shift` metres on a side; a cell is walkable when its centre lies on an
//                unblocked polygon of the converted mesh (`FixedNavMesh::locate`), and its step
//                cost is that polygon's cost per metre
//   integration  Dijkstra over the eight neighbours: an axis step costs the cell size, a diagonal
//                the cell size times the correctly rounded square root of two, each times the
//                entered cell's cost. A diagonal past a blocked cell is not taken, so the field
//                never cuts a corner a unit cannot. The queue pops in (cost, cell index) order —
//                total, so every peer integrates in the same order
//   direction    the neighbour with the lowest integrated cost, the first in neighbour order on a
//                tie, stored as one of eight exact unit vectors (the diagonal is the correctly
//                rounded square root of one half on each axis)
//
// What the float field's incremental regeneration and reference counting do is not repeated here:
// a `Fixed` world's dynamic obstacles are polygon flags set by commands, and a field is rebuilt
// when one changes — at a tick boundary, on every peer alike.

#include <cy/core/base/expected.h>
#include <cy/core/detmath/fixed.h>
#include <cy/core/detmath/vec.h>
#include <cy/core/memory/array.h>
#include <cy/movement/nav_mesh.h>

namespace cy::movement {

/// What a build cost and found.
struct FixedFlowFieldReport {
    u32 cells = 0;
    u32 walkable = 0;
    u32 reached = 0;
    u32 popped = 0;
};

/// A direction per cell toward one destination, integrated in `Fixed`, so many units heading to one
/// place follow one field that every peer computes alike.
class FixedFlowField {
public:
    /// No direction: unwalkable, unreachable, or a destination.
    static constexpr u8 kNoDirection = 8;

    explicit FixedFlowField(Allocator& allocator) noexcept;

    /// Build over the mesh's whole extent toward `destination` (snapped onto the mesh). Cells are
    /// 2^`cell_shift` metres, `cell_shift` in [0, 8].
    [[nodiscard]] Expected<FixedFlowFieldReport, Error> build(const FixedNavMesh& mesh,
                                                              FixedVec2 destination,
                                                              int cell_shift) noexcept;

    /// The unit direction to follow from `position`, or zero where the field has none.
    [[nodiscard]] FixedVec2 direction_at(FixedVec2 position) const noexcept;
    /// The integrated cost to the destination from the cell holding `position`, or `Fixed::max()`.
    [[nodiscard]] Fixed cost_at(FixedVec2 position) const noexcept;

    [[nodiscard]] u32 width() const noexcept { return width_; }
    [[nodiscard]] u32 depth() const noexcept { return depth_; }
    /// The cell index holding `position`, or `cell_count()` when it is outside the field.
    [[nodiscard]] u32 cell_of(FixedVec2 position) const noexcept;
    [[nodiscard]] u32 cell_count() const noexcept { return width_ * depth_; }
    /// The direction code of a cell: 0..7 in `neighbour_offsets()` order, or `kNoDirection`.
    [[nodiscard]] u8 direction_code(u32 cell) const noexcept { return direction_[cell]; }

    /// Every cell's integrated cost and direction, folded: equal on peers that built alike.
    [[nodiscard]] u64 digest() const noexcept;

private:
    struct Open {
        Fixed cost;
        u32 cell = 0;
    };

    void push(Open entry) noexcept;
    [[nodiscard]] Open pop() noexcept;
    [[nodiscard]] bool walkable(i64 x, i64 z) const noexcept;

    Array<Fixed> step_cost_;   ///< per cell: cost per metre, or zero when not walkable
    Array<Fixed> integrated_;  ///< per cell: cost to the destination, or `Fixed::max()`
    Array<u8> direction_;      ///< per cell: 0..7, or kNoDirection
    Array<Open> heap_;
    i64 origin_x_ = 0;  ///< the first cell, in cells
    i64 origin_z_ = 0;
    u32 width_ = 0;
    u32 depth_ = 0;
    int cell_shift_ = 0;
};

/// The velocity a flow field gives a unit at `position`: its direction times `speed`.
[[nodiscard]] FixedVec2 follow_field(const FixedFlowField& field, FixedVec2 position,
                                     Fixed speed) noexcept;

}  // namespace cy::movement
