#include <cy/rendering/gi/distance_field.h>

#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace cy::rendering::gi {
namespace {

/// Pack a signed brick coordinate into one key. 21 bits per axis covers +/-1,048,576 bricks, which
/// at a metre a brick is a world a thousand kilometres across.
[[nodiscard]] u64 brick_key(i32 x, i32 y, i32 z) noexcept {
    const auto fold = [](i32 value) {
        return static_cast<u64>(static_cast<u32>(value + (1 << 20)) & 0x1FFFFFU);
    };
    return (fold(x) << 42U) | (fold(y) << 21U) | fold(z);
}

[[nodiscard]] i32 floor_div(f32 value, f32 divisor) noexcept {
    return static_cast<i32>(std::floor(value / divisor));
}

/// Trilinear read out of one brick's 4x4x4 samples, with the voxel coordinate already local.
[[nodiscard]] f32 trilinear(const f32* voxels, f32 fx, f32 fy, f32 fz) noexcept {
    const auto clampi = [](i32 value) {
        return std::min(std::max(value, 0), static_cast<i32>(kBrickEdge) - 1);
    };
    const i32 x0 = clampi(static_cast<i32>(std::floor(fx)));
    const i32 y0 = clampi(static_cast<i32>(std::floor(fy)));
    const i32 z0 = clampi(static_cast<i32>(std::floor(fz)));
    const i32 x1 = clampi(x0 + 1);
    const i32 y1 = clampi(y0 + 1);
    const i32 z1 = clampi(z0 + 1);
    const f32 tx = math::clamp(fx - static_cast<f32>(x0), 0.0F, 1.0F);
    const f32 ty = math::clamp(fy - static_cast<f32>(y0), 0.0F, 1.0F);
    const f32 tz = math::clamp(fz - static_cast<f32>(z0), 0.0F, 1.0F);

    const auto at = [voxels](i32 x, i32 y, i32 z) {
        return voxels[(((static_cast<usize>(z) * kBrickEdge) + static_cast<usize>(y)) *
                       kBrickEdge) +
                      static_cast<usize>(x)];
    };
    const f32 c00 = math::lerp(at(x0, y0, z0), at(x1, y0, z0), tx);
    const f32 c10 = math::lerp(at(x0, y1, z0), at(x1, y1, z0), tx);
    const f32 c01 = math::lerp(at(x0, y0, z1), at(x1, y0, z1), tx);
    const f32 c11 = math::lerp(at(x0, y1, z1), at(x1, y1, z1), tx);
    return math::lerp(math::lerp(c00, c10, ty), math::lerp(c01, c11, ty), tz);
}

/// A deterministic cosine-weighted hemisphere sequence. Deterministic because a sky visibility term
/// that changes when nothing changed makes a golden image a coin toss.
[[nodiscard]] Vec3 hemisphere_direction(Vec3 normal, u32 index, u32 count) noexcept {
    // The golden-ratio spiral: uniform in the projected disc, and stable under any count.
    constexpr f32 kGolden = 2.39996323F;
    const f32 offset = (static_cast<f32>(index) + 0.5F) / static_cast<f32>(count);
    const f32 radius = std::sqrt(offset);
    const f32 angle = kGolden * static_cast<f32>(index);
    const f32 x = radius * std::cos(angle);
    const f32 y = radius * std::sin(angle);
    const f32 z = std::sqrt(std::max(0.0F, 1.0F - (x * x) - (y * y)));

    Vec3 tangent = std::abs(normal.y) < 0.99F ? cross(Vec3{0.0F, 1.0F, 0.0F}, normal)
                                              : cross(Vec3{1.0F, 0.0F, 0.0F}, normal);
    tangent = normalized_or(tangent, Vec3{1.0F, 0.0F, 0.0F});
    const Vec3 bitangent = cross(normal, tangent);
    return normalized_or((tangent * x) + (bitangent * y) + (normal * z), normal);
}

}  // namespace

f32 box_distance(Vec3 point, Vec3 half_extents) noexcept {
    const Vec3 q{std::abs(point.x) - half_extents.x, std::abs(point.y) - half_extents.y,
                 std::abs(point.z) - half_extents.z};
    const Vec3 outside{std::max(q.x, 0.0F), std::max(q.y, 0.0F), std::max(q.z, 0.0F)};
    const f32 inside = std::min(std::max({q.x, q.y, q.z}), 0.0F);
    return length(outside) + inside;
}

DistanceField::DistanceField() noexcept = default;

Status DistanceField::configure(const ClipmapSettings& settings) noexcept {
    if (settings.levels == 0 || settings.resolution < kBrickEdge ||
        (settings.resolution % kBrickEdge) != 0 || settings.base_extent_metres <= 0.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "DistanceField::configure: levels must be non-zero and the resolution a "
                    "non-zero multiple of the brick edge");
    }
    settings_ = settings;
    levels_.clear();
    brick_pool_.clear();
    free_bricks_.clear();
    if (Status reserved = levels_.reserve(settings.levels); !reserved) {
        return reserved;
    }
    for (u32 level = 0; level < settings.levels; ++level) {
        Level fresh;
        const f32 extent = settings.base_extent_metres * static_cast<f32>(1U << level);
        fresh.voxel_size = extent / static_cast<f32>(settings.resolution);
        fresh.brick_size = fresh.voxel_size * static_cast<f32>(kBrickEdge);
        // What a level answers with outside an allocated brick, and it is a CONSERVATIVE step for
        // a sphere trace rather than a true distance. One brick edge, against a sparsity test one
        // brick DIAGONAL plus a voxel: a brick is only freed when every sample in it is further
        // than that, and any point in the brick is within a voxel's half-diagonal of a sample, so
        // the true distance at any point of a freed brick exceeds what this answers. A far value
        // equal to the diagonal would be the tunnelling bug: the step would be up to one voxel
        // longer than the guarantee, and a wall in the next brick would be passed through.
        fresh.far_distance = fresh.brick_size;
        if (Status pushed = levels_.push_back(std::move(fresh)); !pushed) {
            return pushed;
        }
    }
    diagnostics_ = FieldDiagnostics{};
    diagnostics_.levels = settings.levels;
    return ok();
}

u32 DistanceField::find_placement(u64 id) const noexcept {
    for (usize index = 0; index < placements_.size(); ++index) {
        if (placements_[index].live && placements_[index].id == id) {
            return static_cast<u32>(index);
        }
    }
    return ~0U;
}

Status DistanceField::place(u64 id, const AssetDistanceField& field,
                            const Mat4& transform) noexcept {
    if (field.dimension < 2 || field.distances.size() != static_cast<usize>(field.dimension) *
                                                             field.dimension * field.dimension) {
        return fail(ErrorCode::InvalidArgument,
                    "DistanceField::place: the sample count does not match dimension^3");
    }
    Expected<Mat4, Error> inverse = inverse_affine(transform);
    if (!inverse) {
        return make_unexpected(inverse.error());
    }

    u32 slot = find_placement(id);
    if (slot == ~0U) {
        for (usize index = 0; index < placements_.size(); ++index) {
            if (!placements_[index].live) {
                slot = static_cast<u32>(index);
                break;
            }
        }
    }
    if (slot == ~0U) {
        Placement fresh;
        if (Status pushed = placements_.push_back(std::move(fresh)); !pushed) {
            return pushed;
        }
        slot = static_cast<u32>(placements_.size() - 1);
    }

    Placement& placement = placements_[slot];
    placement.id = id;
    placement.live = true;
    placement.transform = transform;
    placement.inverse = inverse.value();
    placement.dimension = field.dimension;
    placement.local_bounds = field.bounds;
    placement.world_bounds = transformed(field.bounds, transform);
    placement.distances.clear();
    if (Status appended = placement.distances.append(field.distances); !appended) {
        return appended;
    }

    placement.boundary_min = 1.0e6F;
    const u32 last = field.dimension - 1;
    for (u32 z = 0; z < field.dimension; ++z) {
        for (u32 y = 0; y < field.dimension; ++y) {
            for (u32 x = 0; x < field.dimension; ++x) {
                if (x != 0 && y != 0 && z != 0 && x != last && y != last && z != last) {
                    continue;
                }
                const usize index =
                    (((static_cast<usize>(z) * field.dimension) + y) * field.dimension) + x;
                placement.boundary_min = std::min(placement.boundary_min, field.distances[index]);
            }
        }
    }
    placement.boundary_min = std::max(placement.boundary_min, 0.0F);

    (void)invalidate(placement.world_bounds);
    return ok();
}

Status DistanceField::move(u64 id, const Mat4& transform) noexcept {
    const u32 slot = find_placement(id);
    if (slot == ~0U) {
        return fail(ErrorCode::NotFound, "DistanceField::move: no such placement");
    }
    Expected<Mat4, Error> inverse = inverse_affine(transform);
    if (!inverse) {
        return make_unexpected(inverse.error());
    }
    Placement& placement = placements_[slot];
    // The bricks it left and the bricks it enters. Nothing else.
    (void)invalidate(placement.world_bounds);
    placement.transform = transform;
    placement.inverse = inverse.value();
    placement.world_bounds = transformed(placement.local_bounds, transform);
    (void)invalidate(placement.world_bounds);
    return ok();
}

void DistanceField::remove(u64 id) noexcept {
    const u32 slot = find_placement(id);
    if (slot == ~0U) {
        return;
    }
    (void)invalidate(placements_[slot].world_bounds);
    placements_[slot].live = false;
    placements_[slot].distances.clear();
}

u32 DistanceField::placement_count() const noexcept {
    u32 count = 0;
    for (const Placement& placement : placements_) {
        if (placement.live) {
            count += 1;
        }
    }
    return count;
}

f32 DistanceField::sample_placements(Vec3 point) const noexcept {
    f32 nearest = 1.0e6F;
    for (const Placement& placement : placements_) {
        if (!placement.live) {
            continue;
        }
        const Vec3 local = transform_point(placement.inverse, point);
        const Vec3 size = placement.local_bounds.size();
        const f32 spacing =
            std::max({size.x, size.y, size.z}) / static_cast<f32>(placement.dimension - 1);
        if (!placement.local_bounds.expanded(spacing).contains(local)) {
            // Outside the asset's own grid. The bound used here is
            //
            //     d(p) >= dist(p, bbox) + min over the grid boundary of d
            //
            // and it is valid because the surface lies inside the box: the segment from p to its
            // nearest surface point crosses the boundary at some x, so |p - s| = |p - x| + |x - s|,
            // the first term is at least dist(p, bbox) and the second at least the boundary
            // minimum. The bbox distance alone is also valid and is what this used to be — it is
            // just so loose that a brick a clear four metres from a small object never reached the
            // sparsity threshold, and the field allocated storage for empty space.
            const Vec3 closest = placement.local_bounds.closest_point(local);
            nearest = std::min(nearest, cy::distance(closest, local) + placement.boundary_min);
            continue;
        }
        const Vec3 relative = local - placement.local_bounds.min;
        const f32 grid = static_cast<f32>(placement.dimension - 1);
        const auto axis = [&](f32 value, f32 extent) {
            return extent > 0.0F ? math::clamp((value / extent) * grid, 0.0F, grid) : 0.0F;
        };
        const f32 fx = axis(relative.x, size.x);
        const f32 fy = axis(relative.y, size.y);
        const f32 fz = axis(relative.z, size.z);
        const i32 x0 = static_cast<i32>(fx);
        const i32 y0 = static_cast<i32>(fy);
        const i32 z0 = static_cast<i32>(fz);
        const u32 dimension = placement.dimension;
        const auto at = [&](i32 x, i32 y, i32 z) {
            const auto cx = static_cast<usize>(std::min<i32>(x, static_cast<i32>(dimension) - 1));
            const auto cy_index =
                static_cast<usize>(std::min<i32>(y, static_cast<i32>(dimension) - 1));
            const auto cz = static_cast<usize>(std::min<i32>(z, static_cast<i32>(dimension) - 1));
            return placement.distances[(((cz * dimension) + cy_index) * dimension) + cx];
        };
        const f32 tx = fx - static_cast<f32>(x0);
        const f32 ty = fy - static_cast<f32>(y0);
        const f32 tz = fz - static_cast<f32>(z0);
        const f32 c00 = math::lerp(at(x0, y0, z0), at(x0 + 1, y0, z0), tx);
        const f32 c10 = math::lerp(at(x0, y0 + 1, z0), at(x0 + 1, y0 + 1, z0), tx);
        const f32 c01 = math::lerp(at(x0, y0, z0 + 1), at(x0 + 1, y0, z0 + 1), tx);
        const f32 c11 = math::lerp(at(x0, y0 + 1, z0 + 1), at(x0 + 1, y0 + 1, z0 + 1), tx);
        nearest =
            std::min(nearest, math::lerp(math::lerp(c00, c10, ty), math::lerp(c01, c11, ty), tz));
    }
    return nearest;
}

void DistanceField::free_brick(Brick& brick) noexcept {
    if (!brick.empty && brick.slot != kEmptySlot) {
        (void)free_bricks_.push_back(brick.slot);
    }
    brick.slot = kEmptySlot;
    brick.empty = true;
}

Status DistanceField::solve_brick(Level& level, i32 bx, i32 by, i32 bz, Brick& brick) noexcept {
    f32 voxels[kBrickVoxels];
    const Vec3 origin{static_cast<f32>(bx) * level.brick_size,
                      static_cast<f32>(by) * level.brick_size,
                      static_cast<f32>(bz) * level.brick_size};
    f32 nearest = 1.0e6F;
    for (u32 z = 0; z < kBrickEdge; ++z) {
        for (u32 y = 0; y < kBrickEdge; ++y) {
            for (u32 x = 0; x < kBrickEdge; ++x) {
                const Vec3 point = origin + Vec3{static_cast<f32>(x) * level.voxel_size,
                                                 static_cast<f32>(y) * level.voxel_size,
                                                 static_cast<f32>(z) * level.voxel_size};
                const f32 value = sample_placements(point);
                voxels[(((z * kBrickEdge) + y) * kBrickEdge) + x] = value;
                nearest = std::min(nearest, value);
            }
        }
    }

    // The sparsity rule, with the margin the far value's conservatism depends on: a brick every
    // one of whose samples is beyond a brick diagonal plus a voxel carries no information a
    // constant could not, and is not stored.
    if (nearest >= (level.brick_size * std::numbers::sqrt3_v<f32>)+level.voxel_size) {
        free_brick(brick);
        brick.stale = false;
        return ok();
    }

    if (brick.empty || brick.slot == kEmptySlot) {
        if (!free_bricks_.empty()) {
            brick.slot = free_bricks_.back();
            free_bricks_.pop_back();
        } else {
            const usize base = brick_pool_.size();
            if (Status sized = brick_pool_.resize(base + kBrickVoxels); !sized) {
                return sized;
            }
            brick.slot = static_cast<u32>(base / kBrickVoxels);
        }
        brick.empty = false;
    }
    f32* destination = brick_pool_.data() + (static_cast<usize>(brick.slot) * kBrickVoxels);
    for (u32 index = 0; index < kBrickVoxels; ++index) {
        destination[index] = voxels[index];
    }
    brick.stale = false;
    return ok();
}

u32 DistanceField::invalidate(const Aabb& region) noexcept {
    (void)pending_invalidations_.push_back(region);
    u32 marked = 0;
    for (Level& level : levels_) {
        const i32 min_x = floor_div(region.min.x, level.brick_size) - 1;
        const i32 min_y = floor_div(region.min.y, level.brick_size) - 1;
        const i32 min_z = floor_div(region.min.z, level.brick_size) - 1;
        const i32 max_x = floor_div(region.max.x, level.brick_size) + 1;
        const i32 max_y = floor_div(region.max.y, level.brick_size) + 1;
        const i32 max_z = floor_div(region.max.z, level.brick_size) + 1;
        for (i32 z = min_z; z <= max_z; ++z) {
            for (i32 y = min_y; y <= max_y; ++y) {
                for (i32 x = min_x; x <= max_x; ++x) {
                    if (Brick* brick = level.bricks.find(brick_key(x, y, z)); brick != nullptr) {
                        brick->stale = true;
                        marked += 1;
                    }
                }
            }
        }
    }
    return marked;
}

/// Free the bricks a level no longer covers. A key that has left the window is storage the level is
/// holding for a region it can no longer answer about.
void DistanceField::retire_departed(Level& level, i32 half, ScrollReport& report) noexcept {
    Array<u64> departed;
    for (const auto& entry : level.bricks) {
        const u64 key = entry.key;
        const i32 kx = static_cast<i32>((key >> 42U) & 0x1FFFFFU) - (1 << 20);
        const i32 ky = static_cast<i32>((key >> 21U) & 0x1FFFFFU) - (1 << 20);
        const i32 kz = static_cast<i32>(key & 0x1FFFFFU) - (1 << 20);
        const bool inside =
            kx >= level.origin_brick[0] && kx < level.origin_brick[0] + (half * 2) &&
            ky >= level.origin_brick[1] && ky < level.origin_brick[1] + (half * 2) &&
            kz >= level.origin_brick[2] && kz < level.origin_brick[2] + (half * 2);
        if (!inside) {
            (void)departed.push_back(key);
        }
    }
    for (const u64 key : departed) {
        if (Brick* brick = level.bricks.find(key); brick != nullptr) {
            free_brick(*brick);
            (void)level.bricks.remove(key);
            report.bricks_freed += 1;
        }
    }
}

/// Solve one brick of the window, or reuse it. The reuse is the scroll.
void DistanceField::visit_brick(Level& level, i32 bx, i32 by, i32 bz,
                                ScrollReport& report) noexcept {
    const u64 key = brick_key(bx, by, bz);
    Brick* existing = level.bricks.find(key);
    if (existing != nullptr && !existing->stale) {
        // Already solved and still valid. A rebuild would have re-solved it.
        report.bricks_reused += 1;
        if (existing->empty) {
            report.bricks_empty += 1;
        }
        return;
    }

    Brick brick;
    if (existing != nullptr) {
        brick = *existing;
    } else {
        brick.slot = kEmptySlot;
        brick.empty = true;
    }
    if (Status solved = solve_brick(level, bx, by, bz, brick); !solved) {
        return;
    }
    if (Expected<Brick*, Error> stored = level.bricks.insert(key, brick); !stored) {
        return;
    }
    report.bricks_solved += 1;
    if (brick.empty) {
        report.bricks_empty += 1;
    }
}

ScrollReport DistanceField::scroll_to(Vec3 camera) noexcept {
    ScrollReport report;
    const i32 half = static_cast<i32>(settings_.resolution / kBrickEdge) / 2;

    for (Level& level : levels_) {
        level.origin_brick[0] = floor_div(camera.x, level.brick_size) - half;
        level.origin_brick[1] = floor_div(camera.y, level.brick_size) - half;
        level.origin_brick[2] = floor_div(camera.z, level.brick_size) - half;
        level.centred = true;

        retire_departed(level, half, report);
        for (i32 z = 0; z < half * 2; ++z) {
            for (i32 y = 0; y < half * 2; ++y) {
                for (i32 x = 0; x < half * 2; ++x) {
                    visit_brick(level, level.origin_brick[0] + x, level.origin_brick[1] + y,
                                level.origin_brick[2] + z, report);
                }
            }
        }
    }

    pending_invalidations_.clear();
    diagnostics_.last_scroll = report;
    account();
    return report;
}

void DistanceField::account() noexcept {
    u32 allocated = 0;
    u32 empty = 0;
    for (const Level& level : levels_) {
        for (const auto& entry : level.bricks) {
            if (entry.value.empty) {
                empty += 1;
            } else {
                allocated += 1;
            }
        }
    }
    diagnostics_.allocated_bricks = allocated;
    diagnostics_.empty_bricks = empty;
    diagnostics_.bytes = static_cast<u64>(allocated) * kBrickVoxels * sizeof(f32);
}

f32 DistanceField::sample_level(const Level& level, Vec3 point) const noexcept {
    const i32 bx = floor_div(point.x, level.brick_size);
    const i32 by = floor_div(point.y, level.brick_size);
    const i32 bz = floor_div(point.z, level.brick_size);
    const Brick* brick = level.bricks.find(brick_key(bx, by, bz));
    if (brick == nullptr) {
        return level.far_distance;
    }
    if (brick->empty || brick->slot == kEmptySlot) {
        return level.far_distance;
    }
    const Vec3 origin{static_cast<f32>(bx) * level.brick_size,
                      static_cast<f32>(by) * level.brick_size,
                      static_cast<f32>(bz) * level.brick_size};
    const Vec3 local = (point - origin) / level.voxel_size;
    const f32* voxels = brick_pool_.data() + (static_cast<usize>(brick->slot) * kBrickVoxels);
    return trilinear(voxels, local.x, local.y, local.z);
}

f32 DistanceField::distance(Vec3 point) const noexcept {
    // The finest level that has an allocated brick answers; a level with none says "far", and the
    // next level out is asked. That is the clipmap read, and it is why level 0's far value is
    // small: an empty fine brick must not claim more emptiness than it covers.
    f32 answer = 1.0e6F;
    for (const Level& level : levels_) {
        const f32 sampled = sample_level(level, point);
        answer = std::min(answer, sampled);
        if (sampled < level.far_distance) {
            return sampled;
        }
    }
    return answer;
}

f32 DistanceField::voxel_size() const noexcept {
    return levels_.empty() ? 0.0F : levels_[0].voxel_size;
}

SphereTraceHit DistanceField::sphere_trace(const Ray& ray, f32 max_distance,
                                           f32 t_min) const noexcept {
    SphereTraceHit hit;
    if (levels_.empty()) {
        return hit;
    }
    const f32 surface = levels_[0].voxel_size * 0.5F;
    f32 t = std::max(t_min, surface);
    // The closest approach is measured over every step EXCEPT the last few, and a ray that hits
    // within those few never got a chance to approach anything, so it starts at "nothing was near".
    //
    // The delay is the whole subtlety. A march that ends in a hit ends by definition with distances
    // below the surface threshold, and the two or three steps before it are the descent onto that
    // surface — folding them in reports every straight-on hit as a graze, which is what the first
    // form did and which put every software hit in the engine at the confidence floor. The delayed
    // window measures whether the ray came near something OTHER than what it hit.
    constexpr u32 kApproachDelay = 3;
    f32 recent[kApproachDelay] = {1.0e6F, 1.0e6F, 1.0e6F};
    u32 recent_slot = 0;
    f32 closest = 1.0e6F;
    constexpr u32 kMaxSteps = 192;
    for (u32 step = 0; step < kMaxSteps; ++step) {
        if (t > max_distance) {
            // A clean miss: every step counts, including the last few, because there was no
            // surface to descend onto.
            for (const f32 value : recent) {
                closest = std::min(closest, value);
            }
            hit.closest_approach_metres = closest;
            return hit;
        }
        const Vec3 position = ray.at(t);
        const f32 d = distance(position);
        if (d < surface) {
            hit.hit = true;
            hit.t = t;
            hit.position = position;
            // The gradient of the field is the surface normal, and central differences over one
            // voxel is the cheapest estimate that is not biased along an axis.
            const f32 h = levels_[0].voxel_size;
            hit.normal = normalized_or(Vec3{distance(position + Vec3{h, 0.0F, 0.0F}) -
                                                distance(position - Vec3{h, 0.0F, 0.0F}),
                                            distance(position + Vec3{0.0F, h, 0.0F}) -
                                                distance(position - Vec3{0.0F, h, 0.0F}),
                                            distance(position + Vec3{0.0F, 0.0F, h}) -
                                                distance(position - Vec3{0.0F, 0.0F, h})},
                                       -ray.direction);
            // The closest approach BEFORE the terminating step. Including it would report every hit
            // as a graze — a ray that hits ends by definition with a distance below the surface
            // threshold — and the confidence built on it would be at its floor for every software
            // hit in the engine. That was the first form and it was wrong.
            hit.closest_approach_metres = closest;
            return hit;
        }
        closest = std::min(closest, recent[recent_slot]);
        recent[recent_slot] = d;
        recent_slot = (recent_slot + 1) % kApproachDelay;
        t += std::max(d, surface);
    }
    hit.exhausted = true;
    for (const f32 value : recent) {
        closest = std::min(closest, value);
    }
    hit.closest_approach_metres = closest;
    return hit;
}

f32 DistanceField::sky_visibility(Vec3 position, Vec3 normal, u32 rays) const noexcept {
    if (rays == 0 || levels_.empty()) {
        return 1.0F;
    }
    const f32 reach = settings_.base_extent_metres * static_cast<f32>(1U << (settings_.levels - 1));
    f32 open = 0.0F;
    for (u32 index = 0; index < rays; ++index) {
        Ray ray;
        ray.origin = position + (normal * (levels_[0].voxel_size * 2.0F));
        ray.direction = hemisphere_direction(normal, index, rays);
        const SphereTraceHit hit = sphere_trace(ray, reach);
        if (!hit.hit) {
            // A cone that grazed a surface is partly occluded, which is what makes this a soft
            // answer rather than a stair-stepped one. The width the closest approach is measured
            // against is four voxels of the finest level: closer than that and the ray was inside
            // the cone a surface subtends at this resolution.
            open += math::clamp(hit.closest_approach_metres / (levels_[0].voxel_size * 4.0F), 0.0F,
                                1.0F);
        }
    }
    return open / static_cast<f32>(rays);
}

}  // namespace cy::rendering::gi
