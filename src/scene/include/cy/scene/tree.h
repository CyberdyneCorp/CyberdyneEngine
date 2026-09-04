#pragma once
// `SceneTree` — the node layer of one world. Tasks 3.1.1, 3.1.2, 3.1.6, 3.1.9.
//
// One world, one tree. The tree owns nothing an entity owns: it holds the world reference, this
// world's scene component ids, the four registries (templates, groups, behaviours, scenes) and the
// alias index — and every one of those is either a table of *definitions* or an index over the ECS,
// never a second copy of per-entity data. That is the line design.md §3 draws, and it is the line
// `check_coherence()` in coherence.h checks.
//
// WHY A `Node` HOLDS A TREE POINTER AND NOT A WORLD POINTER. A component id is per world
// (`ecs-core`), so `node.local_transform()` needs both the world and this world's id for
// `LocalTransform`. Bundling them is what keeps a `Node` two words and keeps the ids out of a
// static, which would make them per process and wrong the moment an editor world and a play-mode
// world exist at once.
//
// THE ROOT. `initialize()` creates one node named `/`, and "in the tree" means "reachable from it".
// A node created with no parent is a valid, simulated, fully live node that is simply not attached
// — which is how a prefab under construction, a pooled object and a detached subtree are all
// represented without a second concept. Attachment is what `onEnterTree` and `onExitTree` are
// transitions of.
//
// THE PUMP. Lifecycle callbacks that depend on *tree shape* — `onEnterTree`, `onReady`,
// `onExitTree` — cannot fire at the moment the shape changes, because a subtree is attached one
// edge at a time and `onReady` is defined as running after all children are ready. They are queued
// and dispatched by `pump()`, which the frame calls once, after the stage flush and before script
// observes the frame. `onCreate` and `onDestroy` are not queued: the specification places them at
// the moments components appear and are about to be released, and those are calls, not shapes.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/core/values/name.h>
#include <cy/ecs/system.h>
#include <cy/ecs/world.h>
#include <cy/scene/behaviour.h>
#include <cy/scene/components.h>
#include <cy/scene/group.h>
#include <cy/scene/node.h>
#include <cy/scene/node_template.h>
#include <cy/scene/propagation.h>
#include <cy/scene/scene.h>

#include <string_view>

namespace cy::scene {

/// How a scene load treats what is already loaded.
enum class LoadMode : u8 {
    /// Added beside the scenes already in the world. The default and the common case.
    Additive = 0,
    /// Everything already loaded is unloaded first. "Loadable ... as a replacement".
    Replace = 1,
};

/// What a tree is holding, for diagnostics and for tests that assert on shape.
struct SceneTreeStats {
    u32 nodes = 0;
    u32 roots = 0;
    u32 scenes_loaded = 0;
    u32 scenes_active = 0;
    u32 aliases = 0;
    u32 groups = 0;
    u32 behaviour_types = 0;
    u32 behaviour_instances = 0;
    u64 pending_lifecycle_events = 0;
};

class SceneTree {
public:
    /// Binds to an already-initialised world. The world outlives the tree; two trees over one world
    /// bind to the same component ids, because registration is idempotent by name.
    explicit SceneTree(World& world) noexcept;
    ~SceneTree();

    SceneTree(const SceneTree&) = delete;
    SceneTree& operator=(const SceneTree&) = delete;
    SceneTree(SceneTree&&) = delete;
    SceneTree& operator=(SceneTree&&) = delete;

    /// Register the scene components and the shipped template catalogue, and create the root.
    /// Separate from the constructor for the reason `World::initialize` is: it allocates, and a
    /// constructor under -fno-exceptions cannot report that it could not.
    [[nodiscard]] Status initialize() noexcept;

    [[nodiscard]] World& world() const noexcept { return *world_; }
    [[nodiscard]] Allocator& allocator() const noexcept { return world_->allocator(); }
    [[nodiscard]] const SceneComponents& components() const noexcept { return ids_; }
    [[nodiscard]] NodeTemplateRegistry& templates() noexcept { return templates_; }
    [[nodiscard]] const NodeTemplateRegistry& templates() const noexcept { return templates_; }
    [[nodiscard]] GroupRegistry& groups() noexcept { return groups_; }
    [[nodiscard]] const GroupRegistry& groups() const noexcept { return groups_; }
    [[nodiscard]] BehaviourRegistry& behaviours() noexcept { return behaviours_; }
    [[nodiscard]] const BehaviourRegistry& behaviours() const noexcept { return behaviours_; }

    /// The tree root. Every attached node is a descendant of it.
    [[nodiscard]] Node root() noexcept { return {*this, root_}; }

    /// Wrap an entity as a node. The handle is valid only if the entity is alive and has a
    /// `NodeName`, which is what `Node::valid()` answers — this does not check, because a handle to
    /// something that has died is exactly what the specification wants a script to be able to hold.
    [[nodiscard]] Node node(Entity entity) noexcept { return {*this, entity}; }

    // --- Creating and destroying nodes ----------------------------------------------------------

    /// Create a node from a template. `parent` may be a null node, which leaves it detached.
    ///
    /// The name is made unique among its siblings by suffixing. `onCreate` fires before this
    /// returns; `onEnterTree` and `onReady` are queued for the next `pump()`.
    [[nodiscard]] Expected<Node, Error> create_node(Name name, Node parent,
                                                    Name node_template = Name()) noexcept;

    /// Destroy a node and its subtree, firing `onExitTree` and `onDestroy` child-first while the
    /// components still exist.
    [[nodiscard]] Status destroy_node(Node node) noexcept;

    // --- Addressing -----------------------------------------------------------------------------

    /// Resolve an absolute path (`/Level/Player`). The first segment names a root.
    [[nodiscard]] Node find(std::string_view path) noexcept;

    /// The node holding this alias, or a null node. Verified against the entity's own `NodeAlias`
    /// before it is returned, so a stale index cannot resolve to the wrong node.
    [[nodiscard]] Node find_alias(Name alias) noexcept;
    [[nodiscard]] Status set_alias(Node node, Name alias) noexcept;
    [[nodiscard]] Status clear_alias(Node node) noexcept;
    [[nodiscard]] u32 alias_count() const noexcept;

    /// Every node with no parent, in entity-index order. Appends to `out`. Includes the root.
    [[nodiscard]] Status roots(Array<Node>& out) noexcept;

    // --- Scenes (task 3.1.9) --------------------------------------------------------------------

    /// Instantiate a whole description now. The scene is `Active` when this returns.
    [[nodiscard]] Expected<SceneId, Error> load(const SceneDescription& description,
                                                LoadMode mode = LoadMode::Additive) noexcept;

    /// Begin a budgeted load. `description` must outlive the load; `pump_loads()` advances it, and
    /// the scene is `Loading` — not active — until every node exists.
    [[nodiscard]] Expected<SceneId, Error> begin_load(const SceneDescription& description,
                                                      LoadMode mode = LoadMode::Additive) noexcept;

    /// Instantiate at most `budget` more nodes across the loading scenes, oldest first. Returns how
    /// many were created, so a caller can spend a time budget rather than a count.
    [[nodiscard]] Expected<u32, Error> pump_loads(u32 budget) noexcept;

    /// Destroy exactly the entities this scene created, and retire the id.
    [[nodiscard]] Status unload(SceneId scene) noexcept;

    [[nodiscard]] Expected<SceneInfo, Error> scene(SceneId scene) const noexcept;
    [[nodiscard]] Node scene_root(SceneId scene) noexcept;
    /// Every scene ever loaded into this tree that has not been unloaded. Appends to `out`.
    [[nodiscard]] Status scenes(Array<SceneInfo>& out) const noexcept;

    // --- The frame ------------------------------------------------------------------------------

    /// Dispatch queued tree-shape callbacks and enable/disable transitions. Called once per frame,
    /// after the stage flush.
    [[nodiscard]] Status pump() noexcept;

    /// The time steps the dispatch systems hand to `onFixedUpdate` and `onUpdate`.
    ///
    /// The clock itself is `simulation-and-determinism`' and lives in src/runtime/; the tree is
    /// told its step rather than reading a wall clock, because a system reading the wall clock is
    /// the first thing M9's determinism work has to remove. Defaults to a 60 Hz fixed step and a
    /// zero frame delta, so a tree driven by nothing reports honestly rather than plausibly.
    void set_time_step(f32 fixed_delta, f32 frame_delta) noexcept {
        fixed_delta_ = fixed_delta;
        frame_delta_ = frame_delta;
    }
    [[nodiscard]] f32 fixed_delta() const noexcept { return fixed_delta_; }
    [[nodiscard]] f32 frame_delta() const noexcept { return frame_delta_; }

    /// Run one propagation directly, outside a schedule. What a test or a tool calls; a frame gets
    /// it from the systems `install_systems()` registers.
    [[nodiscard]] Status propagate(PropagationPhase phase = PropagationPhase::Simulation,
                                   PropagationStats* out = nullptr) noexcept;

    /// Register the propagation systems and the behaviour dispatch systems into `schedule`.
    [[nodiscard]] Status install_systems(ecs::Schedule& schedule) noexcept;

    [[nodiscard]] SceneTreeStats stats() const noexcept;

private:
    friend class Node;

    /// A queued tree-shape transition. `attached` says which way the subtree went.
    struct LifecycleEvent {
        Entity subtree_root;
        bool attached = false;
    };

    struct SceneRecord {
        SceneId id = kNoScene;
        Name name;
        SceneStatus status = SceneStatus::Loading;
        Entity root;
        /// Borrowed for the life of a budgeted load; empty once the scene is active.
        Span<const NodeDesc> pending;
        u32 next = 0;
        u32 total = 0;
        /// The entity each already-instantiated description index became, so a later node can name
        /// its parent. Released when the scene becomes active.
        Array<Entity> created;

        explicit SceneRecord(Allocator& allocator) noexcept : created(allocator) {}
    };

    [[nodiscard]] Status queue_lifecycle(Entity subtree_root, bool attached) noexcept;
    [[nodiscard]] Status dispatch_attached(Entity subtree_root) noexcept;
    [[nodiscard]] Status dispatch_detached(Entity subtree_root) noexcept;
    /// Renumber a parent's children 0..n-1 in their authored order, closing the gap a removal left.
    /// Keeping the numbering dense is what makes `Node::child(index)` an equality test.
    [[nodiscard]] Status renumber_children(Entity parent) noexcept;
    [[nodiscard]] Status collect_scene_entities(SceneId scene, Array<Entity>& out) noexcept;
    /// The subtree of `root`, parents before children, into `out`. Cleared first.
    [[nodiscard]] Status collect_subtree(Entity root, Array<Entity>& out) const noexcept;

    [[nodiscard]] Expected<Entity, Error> instantiate(Name name, Entity parent, Name node_template,
                                                      SceneId scene) noexcept;
    /// The component set a node of this template needs: the base components every node has, the
    /// template's own, and `SceneRef` when the node belongs to a scene. `bindings` is left pointing
    /// at the template's bound component list, which the defaults are then written from.
    [[nodiscard]] Status collect_component_set(
        Name node_template, SceneId scene, Array<ComponentTypeId>& set,
        Span<const NodeTemplateRegistry::Binding>& bindings) noexcept;
    /// Write the values a fresh node starts from. Explicit rather than relying on a zeroed chunk:
    /// a node starts fully dirty, visible, enabled and at the identity, and none of those is what
    /// an all-zero row would hold.
    void write_node_defaults(Entity entity, Name name, SceneId scene) noexcept;
    void apply_template_defaults(Entity entity,
                                 Span<const NodeTemplateRegistry::Binding> bindings) noexcept;
    /// Attach the behaviour a template names, if it names one and the tree knows it.
    [[nodiscard]] Status attach_template_behaviour(Entity entity, Name node_template) noexcept;
    /// Unload every scene that is still loaded. What `LoadMode::Replace` does first.
    [[nodiscard]] Status unload_all() noexcept;
    /// Instantiate one described node under the scene it belongs to.
    [[nodiscard]] Status instantiate_scene_node(SceneRecord& record, const NodeDesc& desc) noexcept;
    [[nodiscard]] Status apply_node_desc(const NodeDesc& desc, Entity entity) noexcept;
    [[nodiscard]] Status advance_load(SceneRecord& record, u32 budget, u32& created) noexcept;
    [[nodiscard]] SceneRecord* find_scene(SceneId scene) noexcept;
    [[nodiscard]] const SceneRecord* find_scene(SceneId scene) const noexcept;

    World* world_;
    SceneComponents ids_;
    NodeTemplateRegistry templates_;
    GroupRegistry groups_;
    BehaviourRegistry behaviours_;

    Entity root_;
    /// Alias name index to the entity holding it. An *index*, not a truth: the entity's own
    /// `NodeAlias` is the truth, and every lookup verifies against it before returning.
    HashMap<u32, Entity> aliases_;
    Array<LifecycleEvent> pending_;
    /// The events being dispatched right now, moved out of `pending_` so a callback that reparents
    /// something queues into the next pump rather than extending this one.
    Array<LifecycleEvent> dispatching_;
    Array<SceneRecord*> scenes_;
    SceneId next_scene_ = kNoScene + 1;
    f32 fixed_delta_ = 1.0F / 60.0F;
    f32 frame_delta_ = 0.0F;

    /// Scratch reused by the walks, so a pump or an unload does not allocate per call.
    Array<Entity> scratch_;
    Array<Entity> scratch_children_;
    bool initialized_ = false;
};

}  // namespace cy::scene
