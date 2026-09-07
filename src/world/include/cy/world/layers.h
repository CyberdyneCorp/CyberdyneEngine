#pragma once
// World layers: named groupings of content orthogonal to spatial partitioning. Task 3.6.
//
// `world-partition-and-streaming` — "World layers": a cell may contain entities belonging to
// several layers, and a layer may span many cells. Layers are identified by STABLE IDENTIFIERS, not
// by name or path, so a layer can be renamed without rewriting the entities that belong to it.
// Editor-only layers are not cooked into runtime data.
//
// --- WHY A LAYER SWITCH IS ONE OPERATION --------------------------------------------------------
//
// "WHEN a mission destroys a city, THEN the destroyed-city layer SHALL be activated and the intact
// layer deactivated as one operation, not twenty thousand property changes."
//
// The mechanism is in cell.h rather than here: a cooked cell's rows are grouped into blocks by
// (archetype, LAYER), so activating a layer publishes whole blocks and deactivating one withdraws
// whole blocks. There is no per-entity path to be slow. That grouping is the only reason this file
// can promise the scenario above, and it is why `CookedBlock` carries a `LayerId` at all.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/world/coordinates.h>

namespace cy::world {

/// A stable layer identifier. Renaming a layer does not change it, which is the whole point.
struct LayerId {
    u64 value = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }

    friend constexpr bool operator==(LayerId, LayerId) noexcept = default;
    friend constexpr bool operator<(LayerId a, LayerId b) noexcept { return a.value < b.value; }
};

/// The layer every entity belongs to when a cook says nothing else. Always present, always
/// `Runtime`, always activated — so a world that has no use for layers pays no attention to them.
inline constexpr LayerId kDefaultLayer{1};

enum class LayerKind : u8 {
    /// Never cooked into runtime data.
    EditorOnly = 0,
    Runtime,
    Scenario,
    Variant,
    System,
};

[[nodiscard]] const char* layer_kind_name(LayerKind kind) noexcept;

/// A runtime layer's state, controllable independently of cell streaming — so a scenario layer can
/// be made resident before it is activated and then switched instantly when the event occurs.
enum class LayerState : u8 { Unloaded = 0, Loaded, Activated };

[[nodiscard]] const char* layer_state_name(LayerState state) noexcept;

struct Layer {
    LayerId id;
    LayerKind kind = LayerKind::Runtime;
    LayerState state = LayerState::Unloaded;
    /// For diagnostics only. Membership never keys on it.
    const char* name = "";
};

/// The world's layers. Small and ordered: a world has tens of layers, not thousands, and iteration
/// order has to be the same on two machines because it decides publication order.
class LayerTable {
public:
    explicit LayerTable(Allocator& allocator) noexcept;

    /// Declare a layer. Re-declaring one updates its kind and name and leaves its state alone,
    /// because a cook reloading its layer definitions must not reset a running scenario.
    [[nodiscard]] Status declare(LayerId id, LayerKind kind, const char* name = "") noexcept;

    [[nodiscard]] const Layer* find(LayerId id) const noexcept;

    /// Set a layer's state. One operation for every entity in it — see the header comment.
    [[nodiscard]] Status set_state(LayerId id, LayerState state) noexcept;

    [[nodiscard]] LayerState state_of(LayerId id) const noexcept;

    /// True when a block belonging to this layer should be published into the ECS world.
    [[nodiscard]] bool is_activated(LayerId id) const noexcept {
        return state_of(id) == LayerState::Activated;
    }

    /// True when this layer's content is cooked at all. `EditorOnly` is not.
    [[nodiscard]] bool is_cooked(LayerId id) const noexcept;

    [[nodiscard]] Span<const Layer> layers() const noexcept { return layers_.span(); }
    [[nodiscard]] usize size() const noexcept { return layers_.size(); }

    /// Replication is an identifier plus a state, never per-entity messages. This is that pair.
    struct StateChange {
        LayerId layer;
        LayerState state = LayerState::Unloaded;
    };
    /// The changes since `drain_changes()` was last called, in the order they were made. What a
    /// server replicates and what the persistence overlay records.
    [[nodiscard]] Status drain_changes(Array<StateChange>& out) noexcept;

private:
    [[nodiscard]] Layer* mutable_find(LayerId id) noexcept;

    Array<Layer> layers_;
    Array<StateChange> changes_;
};

}  // namespace cy::world

namespace cy {

template <>
struct Hash<world::LayerId> {
    [[nodiscard]] u64 operator()(world::LayerId id) const noexcept {
        return hash_integer(id.value, hash_seed());
    }
};

}  // namespace cy
