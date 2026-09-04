#pragma once
// Entity templates and batch spawning, and live prefab update. Task 3.2.12.
//
// `serialization-and-prefabs` — "Entity templates and batch spawning":
//
//   "Runtime spawning SHALL consume the template directly: allocate chunks, bulk copy component
//    data, fix up intra-template references, apply spawn parameters. Deep prefab inheritance,
//    variant resolution, and override application SHALL NOT occur at runtime."
//
// All four verbs are here and none of the three prohibitions is: an `EntityTemplate` is a cooked
// asset bound to one world's component ids, and spawning it never touches a document, a library, a
// prefab, an override or a value record.
//
// --- WHAT A SPAWN ACTUALLY COSTS, MEASURED RATHER THAN CLAIMED
// ------------------------------------
//
// The M2 spike measured two different paths and they are not the same number, which is worth
// stating here because the specification's "spawns as a copy" reads as one:
//
//   cell activation   copies into fresh chunks, so a whole chunk region is one memcpy.  ~3.7
//   ns/entity template spawn    copies into chunks that are already partly full, so the destination
//   row is
//                     not the source row: one memcpy per column per row.               ~11.2
//                     ns/entity
//
// Batch spawning is what recovers most of the difference, and it is why `spawn_many` builds one
// replicated payload and instantiates once rather than calling `spawn` in a loop: the ECS's bulk
// instantiation resolves the archetype once and acquires whole chunk runs.
//
// --- THE FIXUP, AND THE ONE PLACE THIS IS THINNER THAN THE SPIKE
// ----------------------------------
//
// The reference fixup is driven by the cooked reference-site table, so it is a walk over known
// columns and known offsets rather than a reflection lookup per row. What it is *not*, yet, is the
// strided pass the spike measured: `ecs::World` exposes a row through `get_mut`, which is a table
// lookup and a binary search over the archetype's columns, and does not expose a column span for a
// run of rows. So this pays one lookup per referencing row where the spike paid one per column.
// That is recorded in this module's README as a request against `src/ecs/`, and it changes no
// interface here when it lands — `fix_up_references` below is the only function that would change.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/world.h>
#include <cy/scene/serialization/cook.h>

namespace cy::scene::serialization {

/// A cooked asset bound to one world.
///
/// Binding is where a stable `reflect::TypeId` becomes a per-world `ecs::ComponentTypeId`, and
/// where the build schema identity is checked. Both happen once; spawning afterwards does neither.
class EntityTemplate {
public:
    explicit EntityTemplate(Allocator& allocator) noexcept : blocks_(allocator) {}

    EntityTemplate(const EntityTemplate&) = delete;
    EntityTemplate& operator=(const EntityTemplate&) = delete;
    EntityTemplate(EntityTemplate&&) noexcept = default;
    EntityTemplate& operator=(EntityTemplate&&) noexcept = default;
    ~EntityTemplate() = default;

    /// Bind `asset` to `world`. The asset is borrowed and must outlive the template: a spawn copies
    /// straight out of its payload, which is the whole point of cooking it.
    ///
    /// Fails with `Unsupported` when the asset's build schema does not match the world's — before a
    /// byte is copied, because packed data with the wrong schema means something other than what it
    /// says.
    [[nodiscard]] Status bind(const ecs::World& world, const ComponentLayoutTable& layouts,
                              const CookedAsset& asset) noexcept;

    [[nodiscard]] u32 entity_count() const noexcept { return entity_count_; }
    [[nodiscard]] usize block_count() const noexcept { return blocks_.size(); }

    /// Spawn one instance. `out` receives `entity_count()` entities, indexed by template index —
    /// which is the order the cook assigned, not the order the chunks were filled in.
    [[nodiscard]] Status spawn(ecs::World& world, Array<ecs::Entity>& out) const noexcept;

    /// Spawn `count` instances in one operation. `out` receives `count * entity_count()` entities,
    /// instance-major: instance `i`'s entity for template index `t` is `out[i * entity_count() +
    /// t]`.
    ///
    /// One instantiation per archetype for the whole batch, rather than one per instance: "they
    /// SHALL be created in a batch rather than by a thousand separate spawn calls".
    [[nodiscard]] Status spawn_many(ecs::World& world, u32 count,
                                    Array<ecs::Entity>& out) const noexcept;

    /// The same, placing each instance with its own transform.
    ///
    /// The transform is composed onto every entity the template roots — the ones whose relationship
    /// the cook did not retain are all roots, so a flattened template is placed by transforming
    /// each of its entities, and a template that kept its hierarchy is placed by transforming its
    /// root.
    [[nodiscard]] Status spawn_many(ecs::World& world, Span<const cy::Transform> placements,
                                    const TransformBinding& binding,
                                    Array<ecs::Entity>& out) const noexcept;

private:
    /// One cooked block, bound to this world's component ids.
    struct BoundBlock {
        Array<ecs::ComponentTypeId> components;
        Array<const void*> columns;
        Array<u32> element_sizes;
        Array<serialize::ReferenceSite> sites;
        u32 rows = 0;
    };

    [[nodiscard]] Status instantiate_all(ecs::World& world, u32 count, Array<u8>& scratch,
                                         Array<ecs::Entity>& raw) const noexcept;
    [[nodiscard]] Status fix_up_references(ecs::World& world, u32 count,
                                           Span<const ecs::Entity> raw,
                                           Array<ecs::Entity>& out) const noexcept;
    [[nodiscard]] Status attach_relationships(ecs::World& world, u32 count,
                                              Span<const ecs::Entity> out) const noexcept;

    Array<BoundBlock> blocks_;
    const CookedAsset* asset_ = nullptr;
    u32 entity_count_ = 0;
};

/// One field a live prefab edit changes on every existing instance.
struct LiveFieldUpdate {
    /// Template index of the entity, in the *new* cook's ordering.
    u32 entity = 0;
    ecs::ComponentTypeId component = ecs::kInvalidComponent;
    u32 offset = 0;
    u32 size = 0;
    /// The new bytes, in the component's native layout.
    Array<u8> bytes;
};

/// What a live prefab edit does to the instances that already exist.
///
/// `live-editing`'s contract, instantiated: "the live edit compiler produces a runtime delta, the
/// applicable live edit policy determines how it is applied, and field classification determines
/// what is preserved — `Authoring` fields updated, `RuntimeState` and `PersistentState` preserved,
/// `Derived` recomputed."
class LiveUpdatePlan {
public:
    explicit LiveUpdatePlan(Allocator& allocator) noexcept : updates_(allocator) {}

    LiveUpdatePlan(const LiveUpdatePlan&) = delete;
    LiveUpdatePlan& operator=(const LiveUpdatePlan&) = delete;
    LiveUpdatePlan(LiveUpdatePlan&&) noexcept = default;
    LiveUpdatePlan& operator=(LiveUpdatePlan&&) noexcept = default;
    ~LiveUpdatePlan() = default;

    [[nodiscard]] Array<LiveFieldUpdate>& updates() noexcept { return updates_; }
    [[nodiscard]] const Array<LiveFieldUpdate>& updates() const noexcept { return updates_; }

    /// True when the edit changed structure — an entity or a component added or removed — so
    /// instances cannot be updated in place.
    ///
    /// "Instances whose state cannot be reconciled SHALL be reported rather than silently reset",
    /// and "a structural change announces its policy": this is the flag a caller checks *before*
    /// applying anything, and `apply_live_update` refuses while it is set.
    [[nodiscard]] bool requires_recreation() const noexcept { return requires_recreation_; }
    void set_requires_recreation(bool value) noexcept { requires_recreation_ = value; }

    /// Fields skipped because their classification says the running simulation owns them. Counted,
    /// because "preserved" is a claim a report should be able to substantiate.
    u32 preserved_runtime_fields = 0;
    u32 recomputed_derived_fields = 0;

private:
    Array<LiveFieldUpdate> updates_;
    bool requires_recreation_ = false;
};

/// Build the delta between two resolved graphs of one prefab.
///
/// `layouts` supplies each component's reflected descriptor, which is where the field
/// classification comes from — so what is updated and what is preserved is decided by the same
/// declaration serialization reads, and not by a second table.
[[nodiscard]] Status plan_live_update(const ResolvedGraph& before, const ResolvedGraph& after,
                                      const ecs::World& world, const ComponentLayoutTable& layouts,
                                      LiveUpdatePlan& out) noexcept;

/// Apply a plan to one spawned instance, whose entities are indexed by template index.
///
/// Refuses with `Unavailable` when the plan requires recreation, rather than applying the half of
/// it that happens to fit.
[[nodiscard]] Status apply_live_update(ecs::World& world, const LiveUpdatePlan& plan,
                                       Span<const ecs::Entity> instance) noexcept;

}  // namespace cy::scene::serialization
