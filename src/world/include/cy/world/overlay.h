#pragma once
// The persistence overlay, and the dynamic index that feeds it. Task 3.7.
//
//     authored cells + persistence overlay = current world
//
// `world-partition-and-streaming` — "Persistence overlay": runtime changes are recorded in an
// overlay and COOKED CELLS REMAIN IMMUTABLE. One overlay mechanism serves save games, dedicated
// server persistence, replays and the editor's play-mode changes. Its ENCODING, journalling,
// atomicity, incremental writing, migration and storage backends belong to `save-and-persistence`;
// this file defines the model the world maintains, and nothing here writes a file.
//
// --- THE PROPERTY THAT DECIDES THE DATA STRUCTURE -----------------------------------------------
//
// "The overlay SHALL be organised so that the persistent state of UNLOADED REGIONS is available
// without loading them, so that saving a world of which most is unloaded requires no additional
// streaming."
//
// That single sentence is why the overlay is keyed by `CellId` at the top level and holds complete
// records underneath, rather than being a diff against a loaded world. `find(cell)` answers for a
// cell that has never been resident, and `cells()` enumerates every cell the overlay has state for
// without touching the streaming system at all. A design that stored overrides as pointers into
// live ECS rows would read beautifully and would make saving require loading the world.
//
// --- AND WHY IT IS APPLIED DURING ACTIVATION ----------------------------------------------------
//
// "Applying an overlay to authored cells SHALL be deterministic, and SHALL occur DURING CELL
// ACTIVATION rather than by instantiating authored content and then correcting it." A destroyed
// building must never briefly exist. `CellActivation::advance()` applies the overlay to its private
// staging before anything is published, which is the only point at which that is possible.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/world/cell.h>
#include <cy/world/layers.h>

namespace cy::world {

/// What happens to an active entity whose runtime cell is deactivated while it is still relevant.
/// "An entity ... SHALL be handled by policy ... and SHALL NOT be silently destroyed."
enum class MigrationPolicy : u8 {
    /// Move it to an active cell and keep it simulating.
    Migrate = 0,
    /// Detach it from cells entirely; gameplay owns its lifetime from here.
    BecomeRuntimeManaged,
    /// Write its state to the overlay and remove it from the ECS world.
    PersistAndRemove,
};

[[nodiscard]] const char* migration_policy_name(MigrationPolicy policy) noexcept;

/// A world state variable. Deliberately narrow: the overlay records gameplay's own bookkeeping, and
/// a general value type here would be a second serialization format beside
/// `save-and-persistence`'s.
struct WorldVariable {
    u64 key = 0;
    i64 integer = 0;
    f64 real = 0.0;
};

/// A component value the overlay overrides on one entity.
struct ComponentOverride {
    PersistentId entity;
    ecs::ComponentTypeId component = 0;
    /// Where the bytes live in the overlay's own value pool.
    u32 first = 0;
    u32 size = 0;
};

/// A persistent position written at a checkpoint — save, deactivation, or explicit request. NOT
/// continuously: "crossing a cell boundary SHALL NOT rewrite persistent ownership".
struct PositionRecord {
    PersistentId entity;
    WorldPosition position;
};

/// Everything the overlay holds for one cell. Available whether or not the cell is loaded.
struct CellOverlay {
    CellId cell;
    Array<PersistentId> removed;
    Array<ComponentOverride> overrides;
    Array<PositionRecord> positions;
    /// Entities created at runtime that persist with this cell, in the same ECS-native form a cook
    /// produces — so activation stages authored blocks and created blocks by one code path.
    CookedCell created;

    explicit CellOverlay(Allocator& allocator) noexcept
        : removed(allocator), overrides(allocator), positions(allocator), created(allocator) {}

    CellOverlay(const CellOverlay&) = delete;
    CellOverlay& operator=(const CellOverlay&) = delete;
    CellOverlay(CellOverlay&&) noexcept = default;
    CellOverlay& operator=(CellOverlay&&) noexcept = default;
    ~CellOverlay() = default;

    [[nodiscard]] bool empty() const noexcept;
};

/// The overlay. Authored cells are immutable; this is everything that has happened since.
class PersistenceOverlay {
public:
    explicit PersistenceOverlay(Allocator& allocator) noexcept;

    /// The content version this overlay was produced against. A mismatch is DETECTED and reported,
    /// never silently applied.
    void set_content_version(u64 version) noexcept { content_version_ = version; }
    [[nodiscard]] u64 content_version() const noexcept { return content_version_; }
    /// `ok()` when the installed content matches; a described failure when it does not.
    [[nodiscard]] Status check_content_version(u64 installed) const noexcept;

    [[nodiscard]] Status record_removed(CellId cell, PersistentId entity) noexcept;
    [[nodiscard]] Status record_component(CellId cell, PersistentId entity,
                                          ecs::ComponentTypeId component,
                                          Span<const u8> value) noexcept;
    [[nodiscard]] Status record_created(CellId cell, PersistentId entity, LayerId layer,
                                        Span<const ecs::ComponentTypeId> components,
                                        Span<const void* const> values,
                                        Span<const u32> sizes) noexcept;
    [[nodiscard]] Status record_position(CellId cell, PersistentId entity,
                                         const WorldPosition& position) noexcept;
    [[nodiscard]] Status record_layer_state(LayerId layer, LayerState state) noexcept;
    [[nodiscard]] Status set_variable(u64 key, i64 value) noexcept;
    [[nodiscard]] Status set_real_variable(u64 key, f64 value) noexcept;

    [[nodiscard]] const CellOverlay* find(CellId cell) const noexcept;
    [[nodiscard]] bool is_removed(CellId cell, PersistentId entity) const noexcept;
    /// The overriding bytes, or an empty span. Reading this does not load the cell.
    [[nodiscard]] Span<const u8> component_override(CellId cell, PersistentId entity,
                                                    ecs::ComponentTypeId component) const noexcept;
    [[nodiscard]] const WorldVariable* variable(u64 key) const noexcept;

    /// Every cell the overlay holds state for. The save's iteration, and it streams nothing.
    [[nodiscard]] Status cells(Array<CellId>& out) const noexcept;

    /// Layer states, as identifier-and-state pairs. Replicated as such, never as per-entity
    /// messages.
    [[nodiscard]] Span<const LayerTable::StateChange> layer_states() const noexcept {
        return layer_states_.span();
    }

    [[nodiscard]] usize cell_count() const noexcept { return cells_.size(); }

private:
    [[nodiscard]] Expected<CellOverlay*, Error> entry_for(CellId cell) noexcept;

    Allocator* allocator_;
    /// In the order cells first acquired state, and within a cell in the order records were made.
    /// Applying one cell's overlay is therefore deterministic for a given sequence of runtime
    /// events, which is what determinism can mean here; `cells()` sorts, because a SAVE must be the
    /// same bytes for the same state however that state was reached.
    Array<CellOverlay> cells_;
    Array<u8> values_;
    Array<LayerTable::StateChange> layer_states_;
    Array<WorldVariable> variables_;
    u64 content_version_ = 0;
};

/// Where moving entities currently are, as distinct from where they are PERSISTED.
///
/// "An entity's HOME CELL — where it is persisted — SHALL be distinct from the RUNTIME SPATIAL CELL
/// it currently occupies. Moving entities SHALL be tracked in a dynamic spatial index while active,
/// and crossing a cell boundary SHALL NOT rewrite persistent ownership."
class DynamicIndex {
public:
    explicit DynamicIndex(Allocator& allocator) noexcept;

    /// Begin tracking an entity, with the home cell it is persisted in.
    [[nodiscard]] Status track(PersistentId entity, CellId home, const WorldPosition& position,
                               MigrationPolicy policy = MigrationPolicy::Migrate) noexcept;
    [[nodiscard]] Status forget(PersistentId entity) noexcept;

    /// The per-frame call. Cheap, and it does not touch the overlay: a vehicle crossing a hundred
    /// cells rewrites nothing persistent.
    [[nodiscard]] Status moved(PersistentId entity, const WorldPosition& position) noexcept;

    struct Tracked {
        PersistentId entity;
        CellId home;
        CellCoord runtime_cell;
        WorldPosition position;
        MigrationPolicy policy = MigrationPolicy::Migrate;
        /// How many times this entity has crossed a runtime cell boundary. A counter rather than a
        /// write, so a test can show the persistent record was NOT rewritten.
        u32 crossings = 0;
    };

    [[nodiscard]] const Tracked* find(PersistentId entity) const noexcept;
    [[nodiscard]] Span<const Tracked> tracked() const noexcept { return entities_.span(); }

    /// Write every tracked entity's position into the overlay. A CHECKPOINT — save, deactivation,
    /// or explicit request — and the only thing that makes a persistent position current.
    [[nodiscard]] Status checkpoint(const PartitionConfig& config,
                                    PersistenceOverlay& overlay) noexcept;

    /// Entities whose runtime cell is `cell`, so a deactivation can apply their migration policy
    /// rather than destroying them silently.
    [[nodiscard]] Status occupants_of(const PartitionConfig& config, CellId cell,
                                      Array<PersistentId>& out) const noexcept;

private:
    [[nodiscard]] Tracked* mutable_find(PersistentId entity) noexcept;

    Array<Tracked> entities_;
};

}  // namespace cy::world
