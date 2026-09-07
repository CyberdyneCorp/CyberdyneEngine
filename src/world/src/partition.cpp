#include <cy/world/partition.h>

#include <algorithm>
#include <cmath>

namespace cy::world {
namespace {

/// The largest edge of a box. `level_for()` compares this against a level's cell size: a box that
/// fits within one cell's edge fits within some cell at that level, once the grid is walked.
[[nodiscard]] f64 longest_edge(const Aabb& bounds) noexcept {
    const Vec3 size = bounds.size();
    return static_cast<f64>(std::max({size.x, size.y, size.z}));
}

}  // namespace

const char* streaming_policy_name(StreamingPolicy policy) noexcept {
    switch (policy) {
        case StreamingPolicy::Spatial:
            return "Spatial";
        case StreamingPolicy::AlwaysLoaded:
            return "AlwaysLoaded";
        case StreamingPolicy::RuntimeManaged:
            return "RuntimeManaged";
        case StreamingPolicy::OwnerManaged:
            return "OwnerManaged";
        case StreamingPolicy::Transient:
            return "Transient";
    }
    return "unknown";
}

Assignment Partitioner::assign(const EntityPlacement& placement) const noexcept {
    Assignment assignment;
    if (placement.policy != StreamingPolicy::Spatial) {
        // A game-rules entity has no spatial cell, and giving it one is the defect the "Global
        // rules are not spatial" scenario names. The coordinate stays zeroed and `cell` invalid, so
        // a consumer that ignores `spatial` gets an invalid identifier rather than a plausible one.
        assignment.spatial = false;
        return assignment;
    }

    bool forced = false;
    const u8 level = level_for(placement.bounds, forced);
    assignment.forced_to_coarsest = forced;

    // The centre of the BOUNDS, not the pivot: an entity whose pivot sits at one corner of a long
    // wall belongs to the cell the wall occupies, and at a level whose cells contain it.
    const Vec3 centre = placement.bounds.center();
    assignment.coord = coord_of(WorldVec3d{static_cast<f64>(centre.x), static_cast<f64>(centre.y),
                                           static_cast<f64>(centre.z)},
                                level);
    assignment.cell = id_of(assignment.coord);
    return assignment;
}

HierarchicalGrid::HierarchicalGrid(const PartitionConfig& config) noexcept : config_(config) {
    if (config_.levels == 0) {
        config_.levels = 1;
    }
    config_.levels = std::min(config_.levels, kMaxPartitionLevels);
    config_.level_ratio = std::max<u32>(config_.level_ratio, 2);
    if (!(config_.base_cell_size > 0.0f)) {
        config_.base_cell_size = 1.0f;
    }
}

CellCoord HierarchicalGrid::coord_of(const WorldVec3d& point, u8 level) const noexcept {
    const u8 capped = (level < config_.levels) ? level : static_cast<u8>(config_.levels - 1);
    return from_absolute(config_, point, capped).cell;
}

u8 HierarchicalGrid::level_for(const Aabb& bounds, bool& forced) const noexcept {
    forced = false;
    const f64 edge = longest_edge(bounds);
    for (u8 level = 0; level < config_.levels; ++level) {
        if (edge <= config_.cell_size(level)) {
            return level;
        }
    }
    // Too large for every level. It lands on the coarsest and is reported, because one entity in
    // this state keeps a large region resident and the cook report has to name it.
    forced = true;
    return static_cast<u8>(config_.levels - 1);
}

Status HierarchicalGrid::cells_overlapping(const Aabb& region, u8 level,
                                           Array<CellCoord>& out) const noexcept {
    if (region.is_empty()) {
        return ok();
    }
    const u8 capped = (level < config_.levels) ? level : static_cast<u8>(config_.levels - 1);
    const WorldPosition low =
        from_absolute(config_,
                      WorldVec3d{static_cast<f64>(region.min.x), static_cast<f64>(region.min.y),
                                 static_cast<f64>(region.min.z)},
                      capped);
    const WorldPosition high =
        from_absolute(config_,
                      WorldVec3d{static_cast<f64>(region.max.x), static_cast<f64>(region.max.y),
                                 static_cast<f64>(region.max.z)},
                      capped);

    // Ascending z, then y, then x: a deterministic order, so two machines planning the same frame
    // request the same cells in the same sequence.
    for (i32 z = low.cell.z; z <= high.cell.z; ++z) {
        for (i32 y = low.cell.y; y <= high.cell.y; ++y) {
            for (i32 x = low.cell.x; x <= high.cell.x; ++x) {
                if (Status pushed = out.push_back(CellCoord{x, y, z, capped}); !pushed) {
                    return pushed;
                }
            }
        }
    }
    return ok();
}

PartitionConfig uniform_grid_config(f32 cell_size, u32 partition) noexcept {
    PartitionConfig config;
    config.partition = partition;
    config.base_cell_size = cell_size;
    config.levels = 1;
    config.level_ratio = 2;
    return config;
}

PartitionChange compare_partitions(const PartitionConfig& previous,
                                   const PartitionConfig& current) noexcept {
    PartitionChange change;
    change.previous_signature = previous.signature();
    change.current_signature = current.signature();
    change.identity_changed = change.previous_signature != change.current_signature;
    if (!change.identity_changed) {
        change.reason = "partition settings are unchanged; cell identity is stable";
        return change;
    }

    // Naming WHICH field moved, because "the identifiers changed" is not actionable and the whole
    // point of reporting this is that saves, patches and caches all key on cell identity.
    if (previous.base_cell_size != current.base_cell_size) {
        change.reason =
            "cell size changed: every cell identifier changes, and saves, patches, "
            "streaming caches and build caches all key on them";
    } else if (previous.levels != current.levels || previous.level_ratio != current.level_ratio) {
        change.reason =
            "the level hierarchy changed: entities are reassigned to different levels "
            "and every cell identifier changes";
    } else if (previous.origin.x != current.origin.x || previous.origin.y != current.origin.y ||
               previous.origin.z != current.origin.z) {
        change.reason =
            "the grid origin moved: every cell covers a different region and every "
            "cell identifier changes";
    } else {
        change.reason =
            "the partition identifier changed: this is a different partition of the "
            "same world and shares no cell identity with the previous one";
    }
    return change;
}

}  // namespace cy::world
