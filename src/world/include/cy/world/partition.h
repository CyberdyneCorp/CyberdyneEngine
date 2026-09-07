#pragma once
// The partitioner, and the spatial binding that decides which cell an entity belongs to. Tasks 3.1
// and 3.2.
//
// `world-partition-and-streaming` — "Partitioner interface": spatial partitioning is a REPLACEABLE
// component, not a fixed uniform grid baked into the world model. The engine ships a hierarchical
// grid as the default and admits uniform grid, octree and project-supplied partitioners. A custom
// partitioner integrates "without changes to cell identity, streaming or cooking", which is why
// every one of those three is written against `Partitioner` and against `CellId` and never against
// `HierarchicalGrid`.
//
// A UNIFORM GRID IS A HIERARCHY OF ONE LEVEL. There is no second class here, and adding one would
// be two code paths for the same arithmetic. `uniform_grid_config(size)` is the spelling.
//
// --- BOUNDS DECIDE, NOT THE PIVOT ---------------------------------------------------------------
//
// `world-partition-and-streaming` — "Assignment to a cell SHALL be derived from bounds, mobility,
// streaming policy and importance — NOT from pivot position alone, since a pivot says nothing about
// what an entity occupies." And: "An entity whose bounds span many cells at one hierarchy level
// SHALL be assigned to a COARSER level whose cells contain it, rather than being duplicated into
// every overlapping cell." Both are `assign_cell()` below, and the second is why the return value
// carries `forced_to_coarsest` — one such entity pins a large region resident, so the cooker has to
// be able to name it.

#include <cy/core/base/expected.h>
#include <cy/core/math/shapes.h>
#include <cy/core/memory/array.h>
#include <cy/world/coordinates.h>

namespace cy::world {

/// How an entity is streamed. `world-partition-and-streaming`, "Entity spatial binding and
/// streaming policy".
enum class StreamingPolicy : u8 {
    /// Exists when its cell is activated.
    Spatial = 0,
    /// Global; exists for the world's lifetime. Never assigned to a spatial cell.
    AlwaysLoaded,
    /// Created at runtime; lifetime owned by gameplay.
    RuntimeManaged,
    /// Exists with its owner, not with a cell.
    OwnerManaged,
    /// Not persisted; never written to the world.
    Transient,
};

[[nodiscard]] const char* streaming_policy_name(StreamingPolicy policy) noexcept;

/// A persistent entity identifier: stable across editing, cooking, saving, networking and world
/// reload, independent of the runtime `ecs::Entity` and of which file the entity is stored in.
///
/// Assigned at AUTHORING time and never derived from position, file, index or path — moving an
/// entity must not change its identity. Duplicate identifiers are a cook error, which
/// `CellCooker::validate()` is what reports.
struct PersistentId {
    u64 value = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }

    friend constexpr bool operator==(PersistentId, PersistentId) noexcept = default;
    friend constexpr bool operator<(PersistentId a, PersistentId b) noexcept {
        return a.value < b.value;
    }
};

/// What is known about an entity before it has a cell: everything assignment is allowed to consult.
struct EntityPlacement {
    PersistentId id;
    /// The entity's bounds in absolute space. The pivot alone is never enough.
    Aabb bounds;
    StreamingPolicy policy = StreamingPolicy::Spatial;
    /// Set for an entity that moves. A mobile entity is assigned by its bounds like any other, but
    /// its home cell is where it is PERSISTED, and the dynamic index tracks where it currently is —
    /// see overlay.h.
    bool mobile = false;
    /// Gameplay importance, 0..1. Feeds the streaming priority, not the assignment.
    f32 importance = 0.5f;
};

/// Where an entity was placed, and why the answer might need reporting.
struct Assignment {
    CellCoord coord;
    CellId cell;
    /// True when the entity has no spatial home: `AlwaysLoaded`, `RuntimeManaged`, `OwnerManaged`
    /// and `Transient` are all non-spatial, and a global rules entity in a cell is the bug the
    /// specification's "Global rules are not spatial" scenario names.
    bool spatial = true;
    /// The entity was too large for every level and landed on the coarsest. The cook report names
    /// these, because one of them keeps a large region resident.
    bool forced_to_coarsest = false;
};

/// The replaceable partitioner. Implement it for a planetary or non-Euclidean world; nothing in
/// cell identity, streaming or cooking changes.
class Partitioner {
public:
    Partitioner() = default;
    virtual ~Partitioner() = default;

    Partitioner(const Partitioner&) = delete;
    Partitioner& operator=(const Partitioner&) = delete;
    Partitioner(Partitioner&&) = delete;
    Partitioner& operator=(Partitioner&&) = delete;

    /// The configuration every cell identifier is derived from. Its `signature()` is what a world
    /// asset records so that a partition change is reported rather than silently incompatible.
    [[nodiscard]] virtual const PartitionConfig& config() const noexcept = 0;

    [[nodiscard]] virtual u8 level_count() const noexcept = 0;

    /// The cell at `level` containing an absolute point.
    [[nodiscard]] virtual CellCoord coord_of(const WorldVec3d& point, u8 level) const noexcept = 0;

    /// The finest level whose cell size contains `bounds`, and whether that answer was forced.
    [[nodiscard]] virtual u8 level_for(const Aabb& bounds, bool& forced) const noexcept = 0;

    /// Every cell at `level` whose bounds intersect `region`, appended to `out` in a deterministic
    /// order (ascending z, then y, then x). Determinism matters: this drives which cells a
    /// streaming source requires, and a set that came out in a different order on two machines is
    /// a plan that does.
    [[nodiscard]] virtual Status cells_overlapping(const Aabb& region, u8 level,
                                                   Array<CellCoord>& out) const noexcept = 0;

    /// Convenience, and never overridden: identity is the configuration's, not the partitioner's.
    [[nodiscard]] CellId id_of(CellCoord coord) const noexcept {
        return cell_id_of(config(), coord);
    }
    [[nodiscard]] Aabb bounds_of(CellCoord coord) const noexcept {
        return cell_bounds(config(), coord);
    }

    /// Bounds, mobility, streaming policy and importance decide; the pivot does not.
    [[nodiscard]] Assignment assign(const EntityPlacement& placement) const noexcept;
};

/// The default partitioner: levels of decreasing cell size, configurable per project, so that
/// half-metre props and kilometre landforms are not forced into one granularity.
class HierarchicalGrid final : public Partitioner {
public:
    explicit HierarchicalGrid(const PartitionConfig& config) noexcept;

    [[nodiscard]] const PartitionConfig& config() const noexcept override { return config_; }
    [[nodiscard]] u8 level_count() const noexcept override { return config_.levels; }
    [[nodiscard]] CellCoord coord_of(const WorldVec3d& point, u8 level) const noexcept override;
    [[nodiscard]] u8 level_for(const Aabb& bounds, bool& forced) const noexcept override;
    [[nodiscard]] Status cells_overlapping(const Aabb& region, u8 level,
                                           Array<CellCoord>& out) const noexcept override;

private:
    PartitionConfig config_;
};

/// A uniform grid is a hierarchy of one level, and there is no second implementation of it.
[[nodiscard]] PartitionConfig uniform_grid_config(f32 cell_size, u32 partition = 0) noexcept;

/// Why two partition configurations are not interchangeable. `world-partition-and-streaming`:
/// "Changing partition settings SHALL be recognised as a change that invalidates cell identity, and
/// SHALL be reported as such rather than silently producing incompatible data."
struct PartitionChange {
    bool identity_changed = false;
    u64 previous_signature = 0;
    u64 current_signature = 0;
    /// What a build reports. Never null.
    const char* reason = "";
};

[[nodiscard]] PartitionChange compare_partitions(const PartitionConfig& previous,
                                                 const PartitionConfig& current) noexcept;

}  // namespace cy::world
