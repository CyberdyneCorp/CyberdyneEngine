#include <cy/rendering/gi/scene.h>

#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cy::rendering::gi {
namespace {

/// The margin a surfel's bounds are grown by before insertion. A card is a point with an area, and
/// a zero-volume box in a BVH degenerates the tree's surface-area heuristic.
[[nodiscard]] Aabb surfel_bounds(const Surfel& surfel) noexcept {
    const f32 radius = std::sqrt(std::max(surfel.area, 1.0e-4F)) * 0.5F;
    const Vec3 extent{radius, radius, radius};
    return Aabb{surfel.position - extent, surfel.position + extent};
}

}  // namespace

const char* gi_mode_name(GiMode mode) noexcept {
    switch (mode) {
        case GiMode::None:
            return "None";
        case GiMode::Baked:
            return "Baked";
        case GiMode::Probe:
            return "Probe";
        case GiMode::Dynamic:
            return "Dynamic";
        case GiMode::Hybrid:
            return "Hybrid";
        case GiMode::Count:
            break;
    }
    return "Unknown";
}

const char* radiance_source_name(RadianceSource source) noexcept {
    switch (source) {
        case RadianceSource::None:
            return "None";
        case RadianceSource::Lightmap:
            return "Lightmap";
        case RadianceSource::IrradianceVolume:
            return "IrradianceVolume";
        case RadianceSource::RadianceCache:
            return "RadianceCache";
        case RadianceSource::SurfaceCache:
            return "SurfaceCache";
        case RadianceSource::ScreenTrace:
            return "ScreenTrace";
        case RadianceSource::SoftwareTrace:
            return "SoftwareTrace";
        case RadianceSource::HardwareTrace:
            return "HardwareTrace";
        case RadianceSource::ReflectionProbe:
            return "ReflectionProbe";
        case RadianceSource::Sky:
            return "Sky";
        case RadianceSource::Count:
            break;
    }
    return "Unknown";
}

const char* invalidation_cause_name(InvalidationCause cause) noexcept {
    switch (cause) {
        case InvalidationCause::GeometryMoved:
            return "GeometryMoved";
        case InvalidationCause::LightChanged:
            return "LightChanged";
        case InvalidationCause::MaterialChanged:
            return "MaterialChanged";
        case InvalidationCause::CellIngested:
            return "CellIngested";
        case InvalidationCause::CellEvicted:
            return "CellEvicted";
        case InvalidationCause::Count:
            break;
    }
    return "Unknown";
}

u32 select_detail_level(Span<const DetailLevel> levels, f32 target_error_metres) noexcept {
    if (levels.empty()) {
        return 0;
    }
    // The coarsest level still inside the target. Walking from the coarsest end and stopping at the
    // first acceptable one is the same answer as walking from the finest and remembering the last,
    // and it does not need the array sorted in a particular direction to be right.
    u32 best = levels[0].level;
    f32 best_error = -1.0F;
    for (const DetailLevel& level : levels) {
        if (level.error_metres <= target_error_metres && level.error_metres > best_error) {
            best_error = level.error_metres;
            best = level.level;
        }
    }
    if (best_error >= 0.0F) {
        return best;
    }
    // Even the finest level is coarser than the target. Return it rather than failing: an
    // illumination query that cannot be answered at all is worse than one answered too coarsely,
    // and the caller can compare the level's declared error against its target if it cares.
    u32 finest = levels[0].level;
    f32 finest_error = levels[0].error_metres;
    for (const DetailLevel& level : levels) {
        if (level.error_metres < finest_error) {
            finest_error = level.error_metres;
            finest = level.level;
        }
    }
    return finest;
}

GiScene::GiScene() noexcept = default;

u32 GiScene::find_cell(u64 id) const noexcept {
    for (usize index = 0; index < cells_.size(); ++index) {
        if (cells_[index].resident && cells_[index].id == id) {
            return static_cast<u32>(index);
        }
    }
    return ~0U;
}

Expected<u32, Error> GiScene::allocate_slot(const Surfel& surfel) noexcept {
    u32 slot = ~0U;
    if (!free_slots_.empty()) {
        slot = free_slots_.back();
        free_slots_.pop_back();
        surfels_[slot] = surfel;
        alive_[slot] = 1;
    } else {
        if (Status pushed = surfels_.push_back(surfel); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = alive_.push_back(1); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = proxies_.push_back(0); !pushed) {
            return make_unexpected(pushed.error());
        }
        slot = static_cast<u32>(surfels_.size() - 1);
    }

    Expected<u32, Error> proxy = index_.insert(surfel_bounds(surfel), slot);
    if (!proxy) {
        alive_[slot] = 0;
        (void)free_slots_.push_back(slot);
        return make_unexpected(proxy.error());
    }
    proxies_[slot] = proxy.value();
    live_surfels_ += 1;
    return slot;
}

void GiScene::free_slot(u32 slot) noexcept {
    if (slot >= alive_.size() || alive_[slot] == 0) {
        return;
    }
    (void)index_.remove(proxies_[slot]);
    alive_[slot] = 0;
    (void)free_slots_.push_back(slot);
    live_surfels_ -= 1;
}

Status GiScene::ingest_cell(u64 cell, const Aabb& bounds, Span<const Surfel> surfels,
                            u64 frame) noexcept {
    if (find_cell(cell) != ~0U) {
        return fail(ErrorCode::AlreadyExists,
                    "GiScene::ingest_cell: the cell is already resident; evict it first");
    }

    u32 slot = ~0U;
    for (usize index = 0; index < cells_.size(); ++index) {
        if (!cells_[index].resident) {
            slot = static_cast<u32>(index);
            break;
        }
    }
    if (slot == ~0U) {
        Cell fresh;
        if (Status pushed = cells_.push_back(std::move(fresh)); !pushed) {
            return pushed;
        }
        slot = static_cast<u32>(cells_.size() - 1);
    }

    Cell& entry = cells_[slot];
    entry.id = cell;
    entry.bounds = bounds;
    entry.resident = true;
    entry.slots.clear();
    if (Status reserved = entry.slots.reserve(surfels.size()); !reserved) {
        return reserved;
    }
    for (const Surfel& surfel : surfels) {
        Expected<u32, Error> allocated = allocate_slot(surfel);
        if (!allocated) {
            return make_unexpected(allocated.error());
        }
        if (Status pushed = entry.slots.push_back(allocated.value()); !pushed) {
            return pushed;
        }
    }
    last_touched_ = surfels.size();
    invalidate(bounds, InvalidationCause::CellIngested, cell, frame);
    return ok();
}

void GiScene::evict_cell(u64 cell, u64 frame) noexcept {
    const u32 slot = find_cell(cell);
    if (slot == ~0U) {
        return;
    }
    Cell& entry = cells_[slot];
    for (const u32 surfel_slot : entry.slots) {
        free_slot(surfel_slot);
    }
    last_touched_ = entry.slots.size();
    entry.slots.clear();
    entry.resident = false;
    // Only the evicted region is invalidated, and a query into it falls back to the far field —
    // which is `has_coverage` returning false rather than a black answer.
    invalidate(entry.bounds, InvalidationCause::CellEvicted, cell, frame);
}

bool GiScene::cell_resident(u64 cell) const noexcept {
    return find_cell(cell) != ~0U;
}

u32 GiScene::resident_cell_count() const noexcept {
    u32 count = 0;
    for (const Cell& cell : cells_) {
        if (cell.resident) {
            count += 1;
        }
    }
    return count;
}

bool GiScene::has_coverage(Vec3 point) const noexcept {
    return std::ranges::any_of(
        cells_, [point](const Cell& cell) { return cell.resident && cell.bounds.contains(point); });
}

u32 GiScene::nearest_surfel(Vec3 point, Vec3 normal, f32 radius) const noexcept {
    const Vec3 extent{radius, radius, radius};
    const Aabb box{point - extent, point + extent};
    u32 best = kInvalidSurfel;
    f32 best_distance = radius * radius;
    index_.query_aabb(box, [&](u32 /*proxy*/, u64 user_data) {
        const u32 slot = static_cast<u32>(user_data);
        if (slot >= surfels_.size() || alive_[slot] == 0) {
            return true;
        }
        const Surfel& candidate = surfels_[slot];
        // A card facing away from the query is a different surface — the far side of a wall — and
        // taking its radiance is precisely the leak the visibility terms elsewhere exist to stop.
        if (dot(candidate.normal, normal) <= 0.0F) {
            return true;
        }
        const f32 distance = distance_squared(candidate.position, point);
        if (distance < best_distance) {
            best_distance = distance;
            best = slot;
        }
        return true;
    });
    return best;
}

void GiScene::invalidate(const Aabb& region, InvalidationCause cause, u64 source_id,
                         u64 frame) noexcept {
    InvalidationRecord record;
    record.region = region;
    record.cause = cause;
    record.source_id = source_id;
    record.frame = frame;
    (void)invalidations_.push_back(record);
    invalidation_counts_[static_cast<u32>(cause)] += 1;
}

u32 GiScene::invalidation_count(InvalidationCause cause) const noexcept {
    return invalidation_counts_[static_cast<u32>(cause)];
}

}  // namespace cy::rendering::gi
