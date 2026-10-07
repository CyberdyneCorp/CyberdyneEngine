// SPDX-License-Identifier: MIT
// The fixed-point kinematic mover. Design §8; the order of a tick is in mover.h.

#include <cy/core/jobs/parallel.h>
#include <cy/core/memory/hash.h>
#include <cy/movement/mover.h>

namespace cy::movement {

namespace {

/// Units per parallel range. A function of nothing but this constant and the unit count, as
/// `jobs::parallel_for` requires for its partitioning — and the result does not depend on it
/// anyway, because every pass writes only its own unit's slot.
constexpr u64 kGrain = 256;

/// The largest grid the mover builds, in cells.
constexpr u64 kMaxGridCells = u64{1} << 22;

/// Run `pass(begin, end)` over every unit, on `jobs` when there is one.
template <class Pass>
[[nodiscard]] Status for_units(jobs::JobSystem* jobs, u32 count, Pass& pass) noexcept {
    if (jobs == nullptr || count <= kGrain) {
        pass(0U, count);
        return ok();
    }
    auto body = [&pass](const jobs::TaskContext&, u64 begin, u64 end) noexcept {
        pass(static_cast<u32>(begin), static_cast<u32>(end));
    };
    return jobs::parallel_for(*jobs, count, kGrain, body, "movement.step");
}

}  // namespace

Angle heading_of(FixedVec2 velocity) noexcept {
    // A yaw of t about +Y takes -Z to (-sin t, 0, -cos t); so t is atan2(-v.x, -v.z).
    return detmath::atan2(-velocity.x, -velocity.y);
}

KinematicMover::KinematicMover(Allocator& allocator, const MoverParams& params) noexcept
    : params_(params),
      dt_(Fixed::one() / Fixed::from_int(params.tick_rate > 0 ? params.tick_rate : 1)),
      kernel_(allocator),
      entities_(allocator),
      heights_(allocator),
      headings_(allocator),
      polys_(allocator),
      contacts_(allocator),
      clamped_(allocator),
      obstacles_(allocator) {}

void KinematicMover::bind(const FixedNavMesh* mesh, const FixedHeightField* heights) noexcept {
    mesh_ = mesh;
    height_field_ = heights;
    for (u32 unit = 0; unit < size(); ++unit) {
        polys_[unit] = kNoPoly;
    }
    settle(0, size());
}

Expected<u32, Error> KinematicMover::add(const UnitDesc& unit) noexcept {
    if (!entities_.empty() && unit.entity <= entities_.back()) {
        return fail(ErrorCode::InvalidArgument,
                    "movement: units are added in ascending entity order, which is the order "
                    "every pass of the mover processes them in");
    }
    const auto index = size();
    if (Status pushed =
            kernel_.push(unit.position, unit.radius, unit.max_speed, unit.max_acceleration);
        !pushed) {
        return make_unexpected(pushed.error());
    }
    if (!entities_.push_back(unit.entity) || !heights_.push_back(Fixed::zero()) ||
        !headings_.push_back(Angle{}) || !polys_.push_back(kNoPoly) ||
        !contacts_.push_back(u32{0}) || !clamped_.push_back(u8{0})) {
        return fail(ErrorCode::OutOfMemory, "movement: could not grow the unit arrays");
    }
    settle(index, index + 1);
    return index;
}

void KinematicMover::deactivate(u32 unit) noexcept {
    if (unit < size()) {
        kernel_.active[unit] = 0;
        kernel_.velocity[unit] = FixedVec2::zero();
        kernel_.desired[unit] = FixedVec2::zero();
    }
}

Status KinematicMover::add_obstacle(const FixedCircle& circle) noexcept {
    return obstacles_.push_back(
        Obstacle{FixedCapsule2D{circle.centre, circle.centre, circle.radius}});
}

Status KinematicMover::add_obstacle(const FixedCapsule2D& capsule) noexcept {
    return obstacles_.push_back(Obstacle{capsule});
}

CrowdGridParams KinematicMover::grid() const noexcept {
    const FixedVec2 low = mesh_ != nullptr ? mesh_->min() : params_.grid_min;
    const FixedVec2 high = mesh_ != nullptr ? mesh_->max() : params_.grid_max;
    CrowdGridParams grid;
    grid.cell_shift = params_.cell_shift;
    u64 cells_x = 0;
    u64 cells_z = 0;
    // A world too large for the grid at the configured spacing gets coarser cells, never fewer of
    // them covering less: a bigger cell still holds every overlapping pair in its 3 x 3 block.
    for (;;) {
        grid.origin_x = FixedPolicy::cell(low.x, grid.cell_shift);
        grid.origin_z = FixedPolicy::cell(low.y, grid.cell_shift);
        cells_x = static_cast<u64>(FixedPolicy::cell(high.x, grid.cell_shift) - grid.origin_x) + 1;
        cells_z = static_cast<u64>(FixedPolicy::cell(high.y, grid.cell_shift) - grid.origin_z) + 1;
        if (cells_x * cells_z <= kMaxGridCells || grid.cell_shift >= 24) {
            break;
        }
        ++grid.cell_shift;
    }
    grid.cells_x = static_cast<u32>(cells_x);
    grid.cells_z = static_cast<u32>(cells_z);
    return grid;
}

void KinematicMover::settle(u32 begin, u32 end) noexcept {
    const WideFixed heading_threshold =
        WideFixed::product(params_.heading_speed, params_.heading_speed);
    for (u32 i = begin; i < end; ++i) {
        contacts_[i] = 0;
        clamped_[i] = 0;
        if (kernel_.active[i] == 0) {
            continue;
        }
        FixedVec2 at = kernel_.position[i];
        const Fixed reach = kernel_.radius[i];
        for (const Obstacle& obstacle : obstacles_) {
            const FixedVec2 nearest = obstacle.shape.closest_point(at);
            const FixedVec2 apart = at - nearest;
            const Fixed limit = obstacle.shape.radius + reach;
            const WideFixed distance_squared = detmath::length_squared(apart);
            if (!(distance_squared < WideFixed::product(limit, limit))) {
                continue;
            }
            ++contacts_[i];
            if (distance_squared == WideFixed{}) {
                // Centred on the obstacle's core: out along +x, the one direction every peer picks.
                at = nearest + FixedVec2{limit, Fixed::zero()};
                continue;
            }
            const Fixed distance = detmath::sqrt(distance_squared);
            at = nearest + FixedVec2{(apart.x / distance) * limit, (apart.y / distance) * limit};
        }
        if (mesh_ != nullptr) {
            const FixedVec2 wanted = at;
            at = mesh_->clamp_move(polys_[i], wanted);
            clamped_[i] = at == wanted ? u8{0} : u8{1};
        }
        kernel_.position[i] = at;
        if (height_field_ != nullptr && !height_field_->empty()) {
            heights_[i] = height_field_->height_at(at);
        } else if (mesh_ != nullptr && polys_[i] != kNoPoly) {
            heights_[i] = mesh_->poly(polys_[i]).height;
        }
        if (heading_threshold < detmath::length_squared(kernel_.velocity[i])) {
            headings_[i] = heading_of(kernel_.velocity[i]);
        }
    }
}

Expected<MoverReport, Error> KinematicMover::step(jobs::JobSystem* jobs) noexcept {
    const u32 count = size();
    auto integrate = [this](u32 begin, u32 end) noexcept { kernel_.integrate(begin, end, dt_); };
    if (Status ran = for_units(jobs, count, integrate); !ran) {
        return make_unexpected(ran.error());
    }
    kernel_.commit();

    MoverReport report;
    const CrowdGridParams cells = grid();
    for (u32 pass = 0; pass < params_.separation_passes; ++pass) {
        if (Status built = kernel_.build_grid(cells); !built) {
            return make_unexpected(built.error());
        }
        auto separate = [this](u32 begin, u32 end) noexcept {
            kernel_.separate(begin, end, params_.separation_share, params_.max_push);
        };
        if (Status ran = for_units(jobs, count, separate); !ran) {
            return make_unexpected(ran.error());
        }
        kernel_.commit();
        report.overlaps += kernel_.counts().overlaps;
    }

    auto settle_pass = [this](u32 begin, u32 end) noexcept { settle(begin, end); };
    if (Status ran = for_units(jobs, count, settle_pass); !ran) {
        return make_unexpected(ran.error());
    }
    for (u32 i = 0; i < count; ++i) {
        report.units += kernel_.active[i];
        report.obstacle_contacts += contacts_[i];
        report.clamped += clamped_[i];
    }
    return report;
}

FixedTransform KinematicMover::transform(u32 unit) const noexcept {
    const FixedVec2 at = kernel_.position[unit];
    return FixedTransform{
        detmath::FixedVec3{at.x, heights_[unit], at.y},
        detmath::FixedQuat::from_axis_angle(detmath::fixed_axis_y(), headings_[unit])};
}

u64 KinematicMover::state_hash() const noexcept {
    u64 fold = hash_combine(0x30'7E50'0001ULL, size());
    for (u32 i = 0; i < size(); ++i) {
        fold = hash_combine(fold, entities_[i]);
        fold = hash_combine(fold, kernel_.active[i]);
        fold = hash_combine(fold, static_cast<u64>(kernel_.position[i].x.raw));
        fold = hash_combine(fold, static_cast<u64>(kernel_.position[i].y.raw));
        fold = hash_combine(fold, static_cast<u64>(heights_[i].raw));
        fold = hash_combine(fold, static_cast<u64>(kernel_.velocity[i].x.raw));
        fold = hash_combine(fold, static_cast<u64>(kernel_.velocity[i].y.raw));
        fold = hash_combine(fold, headings_[i].raw);
        fold = hash_combine(fold, polys_[i]);
    }
    return fold;
}

u64 KinematicMover::params_hash() const noexcept {
    u64 fold = hash_combine(0x30'7E50'0002ULL, static_cast<u64>(params_.tick_rate));
    fold = hash_combine(fold, static_cast<u64>(params_.cell_shift));
    fold = hash_combine(fold, static_cast<u64>(params_.separation_share.raw));
    fold = hash_combine(fold, static_cast<u64>(params_.max_push.raw));
    fold = hash_combine(fold, params_.separation_passes);
    fold = hash_combine(fold, static_cast<u64>(params_.grid_min.x.raw));
    fold = hash_combine(fold, static_cast<u64>(params_.grid_min.y.raw));
    fold = hash_combine(fold, static_cast<u64>(params_.grid_max.x.raw));
    fold = hash_combine(fold, static_cast<u64>(params_.grid_max.y.raw));
    fold = hash_combine(fold, static_cast<u64>(params_.heading_speed.raw));
    for (const Obstacle& obstacle : obstacles_) {
        fold = hash_combine(fold, static_cast<u64>(obstacle.shape.a.x.raw));
        fold = hash_combine(fold, static_cast<u64>(obstacle.shape.a.y.raw));
        fold = hash_combine(fold, static_cast<u64>(obstacle.shape.b.x.raw));
        fold = hash_combine(fold, static_cast<u64>(obstacle.shape.b.y.raw));
        fold = hash_combine(fold, static_cast<u64>(obstacle.shape.radius.raw));
    }
    return fold;
}

}  // namespace cy::movement
