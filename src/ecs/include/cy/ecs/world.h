#pragma once
// The world: entities, their components, and everything that owns them. Tasks 2.1, 2.3, 2.6-2.11.
//
// `ecs-core` — "Multiple worlds": `World` is the ECS runtime container and nothing else is ever
// called one. The spatial and persistence layer at M6 publishes entities *into* a world and is
// addressed by its own names.
//
// A world owns: the entity table, the component registry, the archetype table (and through it every
// chunk), the sparse side tables, the interned shared values, the resources, and the version
// counter change detection is expressed in. Two worlds share nothing — no static registry, no
// process-wide component numbering, no global allocator scope — which is what makes an editor world
// and a play-mode world independent rather than nearly independent.
//
// --- STRUCTURAL CHANGE DEFERRAL IS ENFORCED HERE, AND IT IS CORRECTNESS
// -------------------------
//
// design.md §2, and `ecs-core`'s "Structural change deferral". A system iterating a chunk while
// another moves an entity out of it is iterating freed memory, and the scheduler parallelises
// systems by construction from their access declarations — so immediate structural mutation and
// parallel systems cannot both exist. The resolution is not a convention:
//
//   * every structural entry point below — create, destroy, add, remove, reparent — **refuses**
//     while the world is being iterated, returning `ErrorCode::Unavailable`;
//   * the refusal is a returned error and a counter, not an assertion, because `CY_ASSERT` is
//     compiled out in Profile and Shipping and a rule that only holds in two configurations is not
//     a rule;
//   * the supported way to make a structural change from inside iteration is a `CommandBuffer`
//     (command_buffer.h), which hands back a usable placeholder id immediately and is applied at
//     the stage's flush point.
//
// Setting a component's *value* is not structural and is not refused: it changes bytes in a row
// that already exists, which is what a system is for.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/chunk_allocator.h>
#include <cy/ecs/archetype.h>
#include <cy/ecs/buffer.h>
#include <cy/ecs/component.h>
#include <cy/ecs/entity.h>
#include <cy/ecs/relationships.h>
#include <cy/ecs/resource.h>
#include <cy/ecs/sparse_store.h>

#include <atomic>

namespace cy::ecs {

class CommandBuffer;

struct WorldConfig {
    /// Named in diagnostics and in a snapshot's header. A world that is not identifiable is a
    /// world whose statistics cannot be attributed once there is more than one.
    const char* name = "world";
    /// The chunk size every archetype's storage is laid out for. 16 KiB is what
    /// `core-memory-and-containers` names and what `ecs-core` repeats.
    u32 chunk_bytes = static_cast<u32>(kDefaultChunkBytes);
};

/// What a world is holding, for the diagnostics in task 2.12 and for a test that asserts on shape
/// rather than on timing.
struct WorldStats {
    u32 archetypes = 0;
    u32 chunks = 0;
    u64 entities = 0;
    u64 chunk_capacity = 0;
    /// Rows in use over rows available. The number that says whether the archetype set has
    /// fragmented into chunks holding three entities each.
    f64 fill_ratio = 0.0;
    u64 committed_bytes = 0;
    u64 structural_changes = 0;
    u64 archetype_transitions = 0;
    /// Structural calls refused because the world was being iterated. A non-zero value is a system
    /// that should be recording into a command buffer, and it is counted in every configuration.
    u64 refused_during_iteration = 0;
};

class World {
public:
    explicit World(Allocator& allocator, const WorldConfig& config = {}) noexcept;
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    /// Register the built-in components and validate the world. Called once before use; separate
    /// from the constructor because it allocates and therefore can fail, and a constructor under
    /// -fno-exceptions has no way to say so.
    [[nodiscard]] Status initialize() noexcept;

    [[nodiscard]] const char* name() const noexcept { return config_.name; }
    [[nodiscard]] Allocator& allocator() const noexcept { return *allocator_; }
    [[nodiscard]] ComponentRegistry& components() noexcept { return components_; }
    [[nodiscard]] const ComponentRegistry& components() const noexcept { return components_; }
    /// The world's resources. Named, typed singletons that participate in the scheduler's conflict
    /// detection exactly as components do — see resource.h.
    [[nodiscard]] ResourceRegistry& resources() noexcept { return resources_; }
    [[nodiscard]] const ResourceRegistry& resources() const noexcept { return resources_; }
    [[nodiscard]] ArchetypeTable& archetypes() noexcept { return archetypes_; }
    [[nodiscard]] const ArchetypeTable& archetypes() const noexcept { return archetypes_; }
    [[nodiscard]] const EntityTable& entities() const noexcept { return entities_; }

    /// Register a reflected type as a component of this world.
    template <class T>
    [[nodiscard]] Expected<ComponentTypeId, Error> register_component(
        const ComponentOptions& options = {}) noexcept {
        return components_.register_reflected(reflect::type_of<T>(), options);
    }

    /// Register a buffer component whose elements are `T`, with `inline_capacity` of them held in
    /// the chunk before the buffer spills to the heap.
    template <class T>
    [[nodiscard]] Expected<ComponentTypeId, Error> register_buffer_component(
        const reflect::TypeInfo& type, u32 inline_capacity) noexcept {
        ComponentOptions options;
        options.kind = ComponentKind::Buffer;
        options.element_size = static_cast<u32>(sizeof(T));
        options.element_alignment = static_cast<u32>(alignof(T));
        options.inline_capacity = inline_capacity;
        options.release = &release_buffer<T>;
        return components_.register_reflected(type, options);
    }

    // --- Entities -----------------------------------------------------------------------------

    [[nodiscard]] Expected<Entity, Error> create() noexcept;
    [[nodiscard]] Expected<Entity, Error> create(Span<const ComponentTypeId> components) noexcept;

    /// Create `count` entities of one component set. Resolves the archetype once and allocates
    /// whole chunk runs, which is what `ecs-core`'s "bulk creation is cheap" asks for: no
    /// per-entity archetype lookup, and one chunk-store call per chunk rather than per entity.
    [[nodiscard]] Status create_many(u32 count, Span<const ComponentTypeId> components,
                                     Array<Entity>& out) noexcept;

    [[nodiscard]] Status destroy(Entity entity,
                                 DestroyPolicy policy = DestroyPolicy::CascadeChildren) noexcept;
    [[nodiscard]] Status destroy_many(
        Span<const Entity> entities,
        DestroyPolicy policy = DestroyPolicy::CascadeChildren) noexcept;

    [[nodiscard]] bool is_alive(Entity entity) const noexcept { return entities_.is_alive(entity); }
    [[nodiscard]] u32 entity_count() const noexcept { return entities_.live_count(); }
    [[nodiscard]] const EntityLocation* location(Entity entity) const noexcept {
        return entities_.location(entity);
    }

    // --- Components on entities ---------------------------------------------------------------

    [[nodiscard]] bool has(Entity entity, ComponentTypeId component) const noexcept;

    /// Add a component, optionally with an initial value. Structural for every kind but sparse.
    [[nodiscard]] Status add(Entity entity, ComponentTypeId component,
                             const void* value = nullptr) noexcept;
    [[nodiscard]] Status remove(Entity entity, ComponentTypeId component) noexcept;

    /// A pointer to the entity's component value, or null when it does not have one or the id is
    /// stale. **Slower than iteration and inappropriate for bulk work** — it is a table lookup and
    /// a binary search over the archetype's columns, where iteration is a span walk. `ecs-core`
    /// asks for it to be documented as such, so it is documented here and again at every call site
    /// in the tests.
    [[nodiscard]] const void* get(Entity entity, ComponentTypeId component) const noexcept;

    /// The same, for writing: it stamps the chunk's version for this component, so a downstream
    /// change filter fires. Read through `get()` when nothing is being written — that is the whole
    /// distinction `ecs-core`'s "read does not dirty" scenario is about.
    [[nodiscard]] void* get_mut(Entity entity, ComponentTypeId component) noexcept;

    template <class T>
    [[nodiscard]] const T* get(Entity entity, ComponentTypeId component) const noexcept {
        return static_cast<const T*>(get(entity, component));
    }
    template <class T>
    [[nodiscard]] T* get_mut(Entity entity, ComponentTypeId component) noexcept {
        return static_cast<T*>(get_mut(entity, component));
    }

    template <class T>
    [[nodiscard]] Status set(Entity entity, ComponentTypeId component, const T& value) noexcept {
        T* slot = get_mut<T>(entity, component);
        if (slot == nullptr) {
            return fail(ErrorCode::NotFound, "this entity does not have that component");
        }
        *slot = value;
        return ok();
    }

    // --- Shared components ---------------------------------------------------------------------

    /// Intern a shared value, returning the index that identifies it. Interning is what makes an
    /// archetype's identity an integer comparison instead of a memcmp of the payload, and it is
    /// what lets every entity with one material land in the same chunks.
    [[nodiscard]] Expected<u32, Error> intern_shared(ComponentTypeId component,
                                                     const void* value) noexcept;
    [[nodiscard]] const void* shared_value(ComponentTypeId component, u32 index) const noexcept;
    /// The value index this entity's archetype carries for a shared component, or nothing.
    [[nodiscard]] Expected<u32, Error> shared_of(Entity entity,
                                                 ComponentTypeId component) const noexcept;
    /// Add or change a shared component's value on an entity. Structural: it changes the archetype.
    [[nodiscard]] Status set_shared(Entity entity, ComponentTypeId component, u32 value) noexcept;

    // --- Sparse components ---------------------------------------------------------------------

    /// Add or overwrite a sparse component. **Not structural**: no archetype changes and no row
    /// moves, which is why it is the kind to declare for data that toggles every frame. It is
    /// therefore also permitted during iteration.
    [[nodiscard]] Status set_sparse(Entity entity, ComponentTypeId component,
                                    const void* value) noexcept;
    [[nodiscard]] void* get_sparse(Entity entity, ComponentTypeId component) noexcept;
    [[nodiscard]] const void* get_sparse(Entity entity, ComponentTypeId component) const noexcept;
    [[nodiscard]] Status remove_sparse(Entity entity, ComponentTypeId component) noexcept;

    // --- Buffer components -----------------------------------------------------------------------

    template <class T>
    [[nodiscard]] Expected<BufferView<T>, Error> buffer(Entity entity,
                                                        ComponentTypeId component) noexcept {
        void* slot = get_mut(entity, component);
        if (slot == nullptr) {
            return fail(ErrorCode::NotFound, "this entity does not have that buffer component");
        }
        const ComponentInfo& info = components_.info(component);
        return BufferView<T>(static_cast<BufferHeader*>(slot), info.inline_capacity, *allocator_);
    }

    // --- Relationships (task 2.9) ----------------------------------------------------------------

    [[nodiscard]] ComponentTypeId parent_component() const noexcept { return parent_component_; }
    [[nodiscard]] ComponentTypeId children_component() const noexcept {
        return children_component_;
    }

    /// Reparent. Both the old and the new parent's `Children` and the entity's `Parent` are updated
    /// in this one call, which is what "atomically at the flush point" means when the call *is* the
    /// flush point: a command buffer's `add_child` becomes exactly this call during `flush()`.
    /// Passing `kNoEntity` detaches.
    [[nodiscard]] Status set_parent(Entity child, Entity parent) noexcept;
    [[nodiscard]] Entity parent_of(Entity entity) const noexcept;
    [[nodiscard]] Span<const Entity> children_of(Entity entity) const noexcept;

    // --- Change detection (task 2.8)
    // --------------------------------------------------------------

    /// The world's global version. Monotonic, and advanced once per stage.
    [[nodiscard]] u64 version() const noexcept { return version_; }
    /// Advance it. What the schedule calls at a stage boundary.
    u64 advance_version() noexcept { return ++version_; }

    // --- Deferral (task 2.6)
    // -----------------------------------------------------------------------

    [[nodiscard]] bool iterating() const noexcept {
        return iterating_.load(std::memory_order_acquire) != 0;
    }

    /// Held for the duration of a query's iteration. While one is alive every structural entry
    /// point refuses, in every configuration.
    ///
    /// THE COUNTER IS ATOMIC BECAUSE THE ITERATIONS ARE CONCURRENT. Two systems of one batch run in
    /// parallel by construction, and each iterates its own query over this one world; a plain
    /// counter loses an increment or a decrement under that and the world is then either
    /// permanently "iterating" — which refuses the stage's own flush — or momentarily not, which is
    /// worse. Found by the first stage that actually ran two systems at once.
    class IterationGuard {
    public:
        explicit IterationGuard(World& world) noexcept : world_(&world) {
            world_->iterating_.fetch_add(1, std::memory_order_acq_rel);
        }
        ~IterationGuard() { world_->iterating_.fetch_sub(1, std::memory_order_acq_rel); }

        IterationGuard(const IterationGuard&) = delete;
        IterationGuard& operator=(const IterationGuard&) = delete;
        IterationGuard(IterationGuard&&) = delete;
        IterationGuard& operator=(IterationGuard&&) = delete;

    private:
        World* world_;
    };

    /// Register a command buffer with this world's flush. Buffers are applied in ascending
    /// (system order, thread index) and, within one buffer, in record order — the deterministic
    /// merge `ecs-core` requires.
    [[nodiscard]] Status attach(CommandBuffer& buffer) noexcept;
    void detach(CommandBuffer& buffer) noexcept;

    /// Apply every attached command buffer and empty it. Returns how many commands were applied.
    /// Refuses while iterating: a flush is the structural change it defers.
    [[nodiscard]] Expected<u64, Error> flush() noexcept;

    // --- Bulk instantiation (task 2.11)
    // ------------------------------------------------------------

    /// One archetype's worth of prepared component columns, as a cooked cell or an entity template
    /// hands them over.
    ///
    /// `ecs-core`: "allocating chunks and copying component columns without per-entity
    /// construction, since that is how cooked cells and entity templates are instantiated". The
    /// columns are parallel to `components`, each `count` rows of that component's size.
    struct ArchetypeBlock {
        Span<const ComponentTypeId> components;
        Span<const void* const> columns;
        Span<const SharedValue> shared;
        u32 count = 0;
    };

    /// Instantiate a block. Appends the created entities to `out`, in block row order, so the
    /// caller can fix up the references its cook recorded by row index.
    [[nodiscard]] Status instantiate(const ArchetypeBlock& block, Array<Entity>& out) noexcept;

    // --- Statistics
    // ---------------------------------------------------------------------------------

    [[nodiscard]] WorldStats stats() const noexcept;

    /// Give every empty chunk back to M1's allocator. The pressure response.
    usize trim() noexcept;

    /// Structural bookkeeping the diagnostics module reads. Counted in every configuration.
    [[nodiscard]] u64 structural_changes() const noexcept {
        return structural_changes_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] u64 archetype_transitions() const noexcept { return archetype_transitions_; }
    [[nodiscard]] u64 refused_during_iteration() const noexcept {
        return refused_.load(std::memory_order_relaxed);
    }
    /// The component whose addition or removal caused the most recent archetype transition, and the
    /// entity it happened to. What the archetype-thrash diagnostic names.
    [[nodiscard]] ComponentTypeId last_transition_component() const noexcept {
        return last_transition_component_;
    }
    [[nodiscard]] Entity last_transition_entity() const noexcept { return last_transition_entity_; }

    /// The busiest entity since the counters were last reset, and the component that moved it.
    ///
    /// `ecs-core` asks for "detection of archetype thrash (an entity changing archetype more than a
    /// threshold per second)". A rate needs a window, and a window needs a reset — so the world
    /// counts and the diagnostics module (diagnostics.h) decides how long a second is and what to
    /// do about it. Counting is one increment per transition, which is already the cheapest part of
    /// a transition.
    struct TransitionSample {
        Entity entity;
        ComponentTypeId component = kInvalidComponent;
        u32 transitions = 0;
    };

    [[nodiscard]] TransitionSample busiest_entity() const noexcept;
    void reset_transition_counters() noexcept;

private:
    friend class CommandBuffer;
    friend class Snapshot;

    /// The one place a structural entry point asks whether it may proceed.
    [[nodiscard]] Status admit_structural_change() noexcept;

    /// Move an entity to the archetype for `mask`/`components`/`shared`, carrying every column the
    /// two share. The single implementation behind add, remove, set_shared and create-with-a-set.
    [[nodiscard]] Status move_to(Entity entity, const ComponentMask& mask,
                                 Span<const ComponentTypeId> components,
                                 Span<const SharedValue> shared) noexcept;

    /// Place an entity into an archetype it is not yet in, with no source row to carry over.
    [[nodiscard]] Status place(Entity entity, Archetype& archetype) noexcept;

    /// The component mask a list of component ids denotes, refusing an unregistered one. The shared
    /// kind is refused here too, because a shared component carries a value and a bare id is not
    /// one — `set_shared` is how it is spelled.
    [[nodiscard]] Expected<ComponentMask, Error> mask_of(Span<const ComponentTypeId> components,
                                                         bool allow_shared) const noexcept;

    /// Copy one archetype block's columns into a run of freshly added rows. Static: everything it
    /// needs is in the block and the range it is given.
    static void copy_block_columns(Archetype& archetype, const ArchetypeBlock& block,
                                   const Archetype::RowRange& range, u32 consumed) noexcept;

    /// Create an entity per row of a run, writing its key and its location record.
    [[nodiscard]] Status populate_rows(Archetype& archetype, const Archetype::RowRange& range,
                                       Array<Entity>& out) noexcept;

    /// Undo a row's occupancy: release its buffers, remove the row, and repair the location of
    /// whichever entity was moved into the gap.
    void vacate(Archetype& archetype, u32 chunk, u32 row, const ComponentMask& keep) noexcept;

    void count_transition(Entity entity) noexcept;

    [[nodiscard]] Status ensure_sparse_store(ComponentTypeId component) noexcept;
    [[nodiscard]] SparseStore* sparse_store(ComponentTypeId component) noexcept;
    [[nodiscard]] const SparseStore* sparse_store(ComponentTypeId component) const noexcept;

    [[nodiscard]] Status detach_from_parent(Entity child) noexcept;
    [[nodiscard]] Status collect_subtree(Entity root, Array<Entity>& out) const noexcept;
    [[nodiscard]] Status destroy_one(Entity entity) noexcept;

    /// The component set an entity currently has, as a mask and as a list. Read out of its
    /// archetype, which is the only place it is recorded.
    [[nodiscard]] Status current_set(Entity entity, ComponentMask& mask,
                                     Array<ComponentTypeId>& components,
                                     Array<SharedValue>& shared) const noexcept;

    struct SharedTable {
        u32 value_size = 0;
        u32 count = 0;
        Array<u8> bytes;

        explicit SharedTable(Allocator& allocator) noexcept : bytes(allocator) {}
    };

    struct SparseSlot {
        ComponentTypeId component = kInvalidComponent;
        SparseStore store;

        SparseSlot(Allocator& allocator, ComponentTypeId id, u32 value_size) noexcept
            : component(id), store(allocator, value_size) {}
    };

    Allocator* allocator_;
    WorldConfig config_;
    ComponentRegistry components_;
    ResourceRegistry resources_;
    EntityTable entities_;
    ArchetypeTable archetypes_;
    Array<SharedTable> shared_tables_;
    Array<SparseSlot> sparse_;
    Array<CommandBuffer*> command_buffers_;

    /// Scratch reused by every structural operation, so add/remove does not allocate per call.
    Array<ComponentTypeId> scratch_components_;
    Array<SharedValue> scratch_shared_;
    Array<Entity> scratch_entities_;
    Array<Archetype::RowRange> scratch_ranges_;

    ComponentTypeId parent_component_ = kInvalidComponent;
    ComponentTypeId children_component_ = kInvalidComponent;

    u64 version_ = 1;
    /// Atomic: see IterationGuard. The three counters below it are atomic for the same reason —
    /// every one of them is written from a refusal, and a refusal is exactly what happens on a
    /// worker thread inside a parallel stage.
    std::atomic<u32> iterating_{0};
    std::atomic<u64> structural_changes_{0};
    std::atomic<u64> refused_{0};
    u64 archetype_transitions_ = 0;
    ComponentTypeId last_transition_component_ = kInvalidComponent;
    Entity last_transition_entity_;
    /// Transitions per entity index within the current window, and what caused the last one. Grown
    /// to the entity table's extent on first use; the window is the diagnostics module's.
    struct TransitionCounter {
        u32 count = 0;
        ComponentTypeId component = kInvalidComponent;
    };
    Array<TransitionCounter> transitions_;
    bool initialized_ = false;
};

}  // namespace cy::ecs
