// SPDX-License-Identifier: MIT
// A flow field over a `Fixed` world. See include/cy/movement/flow_field.h.

#include <cy/core/detmath/functions.h>
#include <cy/core/memory/hash.h>
#include <cy/movement/flow_field.h>

#include <initializer_list>

namespace cy::movement {

namespace {

/// The eight neighbours, axes first, then diagonals, each as (dx, dz). The order is the tie-break.
constexpr i64 kOffsets[8][2] = {{1, 0}, {0, 1},  {-1, 0},  {0, -1},
                                {1, 1}, {-1, 1}, {-1, -1}, {1, -1}};

/// The most cells a field may have: 2^22, a 4 km square at 2 m.
constexpr u64 kMaxCells = u64{1} << 22;

[[nodiscard]] bool before(Fixed cost_a, u32 cell_a, Fixed cost_b, u32 cell_b) noexcept {
    return cost_a < cost_b || (cost_a == cost_b && cell_a < cell_b);
}

}  // namespace

FixedFlowField::FixedFlowField(Allocator& allocator) noexcept
    : step_cost_(allocator), integrated_(allocator), direction_(allocator), heap_(allocator) {}

void FixedFlowField::push(Open entry) noexcept {
    if (!heap_.push_back(entry)) {
        return;
    }
    usize child = heap_.size() - 1;
    while (child > 0) {
        const usize parent = (child - 1) / 2;
        if (!before(heap_[child].cost, heap_[child].cell, heap_[parent].cost, heap_[parent].cell)) {
            break;
        }
        const Open swap = heap_[child];
        heap_[child] = heap_[parent];
        heap_[parent] = swap;
        child = parent;
    }
}

FixedFlowField::Open FixedFlowField::pop() noexcept {
    const Open top = heap_[0];
    heap_[0] = heap_.back();
    heap_.pop_back();
    usize parent = 0;
    for (;;) {
        usize smallest = parent;
        for (const usize child : {(2 * parent) + 1, (2 * parent) + 2}) {
            if (child < heap_.size() && before(heap_[child].cost, heap_[child].cell,
                                               heap_[smallest].cost, heap_[smallest].cell)) {
                smallest = child;
            }
        }
        if (smallest == parent) {
            break;
        }
        const Open swap = heap_[parent];
        heap_[parent] = heap_[smallest];
        heap_[smallest] = swap;
        parent = smallest;
    }
    return top;
}

bool FixedFlowField::walkable(i64 x, i64 z) const noexcept {
    return x >= 0 && z >= 0 && x < i64{width_} && z < i64{depth_} &&
           step_cost_[static_cast<usize>((z * width_) + x)].raw != 0;
}

u32 FixedFlowField::cell_of(FixedVec2 position) const noexcept {
    const i64 x = (position.x.raw >> (Fixed::kFractionBits + cell_shift_)) - origin_x_;
    const i64 z = (position.y.raw >> (Fixed::kFractionBits + cell_shift_)) - origin_z_;
    if (x < 0 || z < 0 || x >= i64{width_} || z >= i64{depth_}) {
        return cell_count();
    }
    return static_cast<u32>((z * width_) + x);
}

Expected<FixedFlowFieldReport, Error> FixedFlowField::build(const FixedNavMesh& mesh,
                                                            FixedVec2 destination,
                                                            int cell_shift) noexcept {
    if (cell_shift < 0 || cell_shift > 8 || mesh.poly_count() == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "movement: a flow field needs a mesh and cells of 1 to 256 metres");
    }
    cell_shift_ = cell_shift;
    const int shift = Fixed::kFractionBits + cell_shift;
    origin_x_ = mesh.min().x.raw >> shift;
    origin_z_ = mesh.min().y.raw >> shift;
    // The last cell is the one holding the bound's last representable point, so a mesh ending on a
    // cell boundary gets no empty row beyond it.
    const u64 width = static_cast<u64>(((mesh.max().x.raw - 1) >> shift) - origin_x_) + 1;
    const u64 depth = static_cast<u64>(((mesh.max().y.raw - 1) >> shift) - origin_z_) + 1;
    if (width * depth > kMaxCells) {
        return fail(ErrorCode::OutOfRange, "movement: a flow field over this mesh is too large");
    }
    width_ = static_cast<u32>(width);
    depth_ = static_cast<u32>(depth);

    FixedFlowFieldReport report;
    report.cells = cell_count();
    step_cost_.clear();
    integrated_.clear();
    direction_.clear();
    heap_.clear();
    if (!step_cost_.resize(report.cells) || !integrated_.resize(report.cells) ||
        !direction_.resize(report.cells)) {
        return fail(ErrorCode::OutOfMemory, "movement: could not allocate a flow field");
    }

    // Each cell's centre is located on the mesh once: its polygon's cost is the cell's.
    const i64 half_cell = i64{1} << (shift - 1);
    for (u32 z = 0; z < depth_; ++z) {
        for (u32 x = 0; x < width_; ++x) {
            // Shifted as unsigned: the origin may be negative, and the bits are what is wanted.
            const auto corner_x = static_cast<i64>(static_cast<u64>(origin_x_ + x) << shift);
            const auto corner_z = static_cast<i64>(static_cast<u64>(origin_z_ + z) << shift);
            const FixedVec2 centre{Fixed::from_raw(corner_x + half_cell),
                                   Fixed::from_raw(corner_z + half_cell)};
            const FixedPolyIndex poly = mesh.locate(centre);
            const usize cell = (usize{z} * width_) + x;
            const bool open = poly != kNoPoly && !mesh.blocked(poly);
            step_cost_[cell] = open ? mesh.poly(poly).cost : Fixed::zero();
            integrated_[cell] = Fixed::max();
            direction_[cell] = kNoDirection;
            report.walkable += open ? 1U : 0U;
        }
    }

    FixedVec2 on_mesh = destination;
    (void)mesh.nearest(destination, on_mesh);
    const u32 seed = cell_of(on_mesh);
    if (seed == cell_count()) {
        return report;
    }
    integrated_[seed] = Fixed::zero();
    push(Open{Fixed::zero(), seed});

    const Fixed axis = Fixed::from_int(1 << cell_shift);
    const Fixed diagonal = axis * detmath::sqrt(Fixed::from_int(2));
    while (!heap_.empty()) {
        const Open open = pop();
        if (open.cost != integrated_[open.cell]) {
            continue;  // a stale entry: the cell was reached more cheaply since it was pushed
        }
        ++report.popped;
        const i64 x = open.cell % width_;
        const i64 z = open.cell / width_;
        for (int k = 0; k < 8; ++k) {
            const i64 nx = x + kOffsets[k][0];
            const i64 nz = z + kOffsets[k][1];
            const bool diagonal_step = k >= 4;
            if (!walkable(nx, nz) || (diagonal_step && (!walkable(nx, z) || !walkable(x, nz)))) {
                continue;
            }
            const auto next = static_cast<u32>((nz * width_) + nx);
            const Fixed cost = open.cost + ((diagonal_step ? diagonal : axis) * step_cost_[next]);
            if (cost < integrated_[next]) {
                integrated_[next] = cost;
                push(Open{cost, next});
            }
        }
    }

    for (u32 cell = 0; cell < cell_count(); ++cell) {
        if (integrated_[cell] == Fixed::max()) {
            continue;
        }
        ++report.reached;
        const i64 x = cell % width_;
        const i64 z = cell / width_;
        Fixed best = integrated_[cell];
        for (int k = 0; k < 8; ++k) {
            const i64 nx = x + kOffsets[k][0];
            const i64 nz = z + kOffsets[k][1];
            if (!walkable(nx, nz) || (k >= 4 && (!walkable(nx, z) || !walkable(x, nz)))) {
                continue;
            }
            const Fixed there = integrated_[static_cast<usize>((nz * width_) + nx)];
            if (there < best) {
                best = there;
                direction_[cell] = static_cast<u8>(k);
            }
        }
    }
    return report;
}

FixedVec2 FixedFlowField::direction_at(FixedVec2 position) const noexcept {
    const u32 cell = cell_of(position);
    if (cell == cell_count() || direction_[cell] == kNoDirection) {
        return FixedVec2::zero();
    }
    const int k = direction_[cell];
    if (k < 4) {
        return FixedVec2{Fixed::from_int(static_cast<i32>(kOffsets[k][0])),
                         Fixed::from_int(static_cast<i32>(kOffsets[k][1]))};
    }
    // The square root of one half, correctly rounded, on each axis.
    const Fixed half_root = detmath::sqrt(Fixed::half());
    return FixedVec2{kOffsets[k][0] > 0 ? half_root : -half_root,
                     kOffsets[k][1] > 0 ? half_root : -half_root};
}

Fixed FixedFlowField::cost_at(FixedVec2 position) const noexcept {
    const u32 cell = cell_of(position);
    return cell == cell_count() ? Fixed::max() : integrated_[cell];
}

u64 FixedFlowField::digest() const noexcept {
    u64 fold = hash_combine(0xF10'3F1E'1DULL, (u64{width_} << 32U) | depth_);
    for (u32 cell = 0; cell < cell_count(); ++cell) {
        fold = hash_combine(fold, static_cast<u64>(integrated_[cell].raw));
        fold = hash_combine(fold, direction_[cell]);
    }
    return fold;
}

FixedVec2 follow_field(const FixedFlowField& field, FixedVec2 position, Fixed speed) noexcept {
    return field.direction_at(position) * speed;
}

}  // namespace cy::movement
