// The persistent, advected, decaying, bounded foam field. M10 task 2.3.

#include <cy/water/foam.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::water {

namespace {

[[nodiscard]] i64 floor_cell(f64 value, f32 cell_metres) noexcept {
    return static_cast<i64>(std::floor(value / static_cast<f64>(cell_metres)));
}

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    return (value < 0.0F) ? 0.0F : ((value > 1.0F) ? 1.0F : value);
}

}  // namespace

FoamField::FoamField(Allocator& allocator) noexcept
    : allocator_(&allocator), coverage_(allocator), scratch_(allocator) {}

Status FoamField::configure(const FoamParams& params) noexcept {
    if (params.resolution == 0 || params.cell_metres <= 0.0F || params.lifetime_seconds <= 0.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "water: a foam field needs a non-zero resolution, a positive cell size and a "
                    "positive lifetime");
    }
    params_ = params;
    const usize cells = static_cast<usize>(params.resolution) * params.resolution;
    if (Status sized = coverage_.resize(cells); !sized) {
        return sized;
    }
    if (Status sized = scratch_.resize(cells); !sized) {
        return sized;
    }
    for (usize index = 0; index < cells; ++index) {
        coverage_[index] = 0.0F;
        scratch_[index] = 0.0F;
    }
    // Re-configuring clears: the cells would otherwise mean a different amount of world than the
    // coverage in them was deposited over.
    centre_ = world::WorldVec3d{};
    origin_cell_x_ = -static_cast<i64>(params.resolution / 2);
    origin_cell_z_ = -static_cast<i64>(params.resolution / 2);
    configured_ = true;
    return ok();
}

f32* FoamField::cell(i32 x, i32 z) noexcept {
    if (x < 0 || z < 0 || static_cast<u32>(x) >= params_.resolution ||
        static_cast<u32>(z) >= params_.resolution) {
        return nullptr;
    }
    return &coverage_[(static_cast<usize>(z) * params_.resolution) + static_cast<usize>(x)];
}

const f32* FoamField::cell(i32 x, i32 z) const noexcept {
    if (x < 0 || z < 0 || static_cast<u32>(x) >= params_.resolution ||
        static_cast<u32>(z) >= params_.resolution) {
        return nullptr;
    }
    return &coverage_[(static_cast<usize>(z) * params_.resolution) + static_cast<usize>(x)];
}

void FoamField::grid_of(const world::WorldVec3d& at, f32& x, f32& z) const noexcept {
    const auto cell_metres = static_cast<f64>(params_.cell_metres);
    x = static_cast<f32>((at.x / cell_metres) - static_cast<f64>(origin_cell_x_) - 0.5);
    z = static_cast<f32>((at.z / cell_metres) - static_cast<f64>(origin_cell_z_) - 0.5);
}

Status FoamField::recentre(const world::WorldVec3d& focus) noexcept {
    if (!configured_) {
        return fail(ErrorCode::Unavailable, "water: the foam field has not been configured");
    }
    const i64 half = static_cast<i64>(params_.resolution / 2);
    const i64 wanted_x = floor_cell(focus.x, params_.cell_metres) - half;
    const i64 wanted_z = floor_cell(focus.z, params_.cell_metres) - half;
    centre_ = focus;
    const i64 shift_x = wanted_x - origin_cell_x_;
    const i64 shift_z = wanted_z - origin_cell_z_;
    if (shift_x == 0 && shift_z == 0) {
        // A sub-cell move does nothing. Shifting by a fraction every frame would smear the field
        // in the direction of travel, which is the artefact this integer grid exists to avoid.
        return ok();
    }

    const auto side = static_cast<i64>(params_.resolution);
    for (i64 z = 0; z < side; ++z) {
        for (i64 x = 0; x < side; ++x) {
            const i64 source_x = x + shift_x;
            const i64 source_z = z + shift_z;
            const bool inside =
                source_x >= 0 && source_z >= 0 && source_x < side && source_z < side;
            // What falls off the edge is dropped, which is what "scoped to regions near streaming
            // sources" means for a field of fixed size.
            scratch_[(static_cast<usize>(z) * params_.resolution) + static_cast<usize>(x)] =
                inside ? coverage_[(static_cast<usize>(source_z) * params_.resolution) +
                                   static_cast<usize>(source_x)]
                       : 0.0F;
        }
    }
    for (usize index = 0; index < coverage_.size(); ++index) {
        coverage_[index] = scratch_[index];
    }
    origin_cell_x_ = wanted_x;
    origin_cell_z_ = wanted_z;
    return ok();
}

Status FoamField::deposit(const FoamDeposit& source, f32 seconds) noexcept {
    if (!configured_) {
        return fail(ErrorCode::Unavailable, "water: the foam field has not been configured");
    }
    if (seconds <= 0.0F || source.rate <= 0.0F) {
        return ok();
    }
    f32 grid_x = 0.0F;
    f32 grid_z = 0.0F;
    grid_of(source.position, grid_x, grid_z);
    const f32 radius_cells = source.radius / params_.cell_metres;
    const auto span = static_cast<i32>(std::ceil(radius_cells));

    const auto centre_x = static_cast<i32>(std::lround(grid_x));
    const auto centre_z = static_cast<i32>(std::lround(grid_z));
    for (i32 dz = -span; dz <= span; ++dz) {
        for (i32 dx = -span; dx <= span; ++dx) {
            const i32 x = centre_x + dx;
            const i32 z = centre_z + dz;
            f32* target = cell(x, z);
            if (target == nullptr) {
                // Outside the grid is not an error: a source at the far end of a river is simply
                // not near the focus.
                continue;
            }
            const f32 offset_x = static_cast<f32>(x) - grid_x;
            const f32 offset_z = static_cast<f32>(z) - grid_z;
            const f32 distance = std::sqrt((offset_x * offset_x) + (offset_z * offset_z));
            // The cell the source is IN always takes the full rate, whatever the radius. A source
            // narrower than a cell is the ordinary case — a boat's wake at a coarse resolution —
            // and a falloff that reached zero at half a cell would deposit nothing at all for it.
            f32 falloff = 1.0F;
            if (dx != 0 || dz != 0) {
                if (radius_cells <= 0.0F || distance > radius_cells) {
                    continue;
                }
                falloff = clamp01(1.0F - (distance / radius_cells));
            }
            *target = clamp01(*target + (source.rate * seconds * falloff));
        }
    }
    return ok();
}

Status FoamField::advect(f32 seconds, VelocitySource velocity_at, void* user) noexcept {
    if (!configured_) {
        return fail(ErrorCode::Unavailable, "water: the foam field has not been configured");
    }
    if (seconds <= 0.0F) {
        return ok();
    }
    // Exponential decay with the declared lifetime as its time constant: a cell is at 1/e after one
    // lifetime. "Decaying over a declared lifetime" for a quantity that fades rather than expires.
    const f32 decay = std::exp(-seconds / params_.lifetime_seconds);
    const auto side = static_cast<i32>(params_.resolution);
    const auto cell_metres = static_cast<f64>(params_.cell_metres);

    for (i32 z = 0; z < side; ++z) {
        for (i32 x = 0; x < side; ++x) {
            const world::WorldVec3d here{
                (static_cast<f64>(origin_cell_x_ + x) + 0.5) * cell_metres, centre_.y,
                (static_cast<f64>(origin_cell_z_ + z) + 0.5) * cell_metres};
            const Vec3 velocity =
                (velocity_at == nullptr) ? Vec3{0.0F, 0.0F, 0.0F} : velocity_at(user, here);
            // Semi-Lagrangian: what is HERE next is what was upstream of here a step ago. Stable at
            // any step size, which matters because foam is advected at the frame's rate and a frame
            // is not a fixed length.
            const auto step_x = static_cast<f64>(velocity.x) * static_cast<f64>(seconds);
            const auto step_z = static_cast<f64>(velocity.z) * static_cast<f64>(seconds);
            const world::WorldVec3d from{here.x - step_x, here.y, here.z - step_z};
            scratch_[(static_cast<usize>(z) * params_.resolution) + static_cast<usize>(x)] =
                sample(from) * decay;
        }
    }
    for (usize index = 0; index < coverage_.size(); ++index) {
        coverage_[index] = scratch_[index];
    }
    return ok();
}

f32 FoamField::sample(const world::WorldVec3d& at) const noexcept {
    if (!configured_) {
        return 0.0F;
    }
    f32 grid_x = 0.0F;
    f32 grid_z = 0.0F;
    grid_of(at, grid_x, grid_z);
    const auto x0 = static_cast<i32>(std::floor(grid_x));
    const auto z0 = static_cast<i32>(std::floor(grid_z));
    const f32 fx = grid_x - static_cast<f32>(x0);
    const f32 fz = grid_z - static_cast<f32>(z0);

    const auto read = [this](i32 x, i32 z) noexcept {
        const f32* value = cell(x, z);
        return (value == nullptr) ? 0.0F : *value;
    };
    const f32 top = math::lerp(read(x0, z0), read(x0 + 1, z0), fx);
    const f32 bottom = math::lerp(read(x0, z0 + 1), read(x0 + 1, z0 + 1), fx);
    return math::lerp(top, bottom, fz);
}

u64 FoamField::bytes() const noexcept {
    // The grid and its scratch, and nothing else. A function of the resolution alone — not of how
    // long the session has run, and not of how large the world is.
    return static_cast<u64>((coverage_.size() + scratch_.size()) * sizeof(f32));
}

f32 FoamField::total_coverage() const noexcept {
    f32 total = 0.0F;
    for (const f32 value : coverage_) {
        total += value;
    }
    return total;
}

}  // namespace cy::water
