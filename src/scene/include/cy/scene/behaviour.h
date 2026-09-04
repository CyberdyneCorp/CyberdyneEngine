#pragma once
// Behaviours: the bridge between a node and a system. Tasks 3.1.6 and 3.1.7.
//
// `scene-graph-and-nodes` — "Behaviours bridge nodes and systems": a behaviour is "a script-side or
// native object attached to a node, declaring lifecycle callbacks and the component data it reads
// and writes"; it "SHALL be **compiled** where possible", so that "many instances of one behaviour
// cost one system rather than one call per instance"; the ones that cannot batch "SHALL fall back
// to per-instance dispatch, and the build SHALL **report which behaviours batched, which did not,
// and why**"; and per-instance dispatch is itself a system, so script runs scheduled and ordered
// like everything else.
//
// READ THE BATCHING RULE BEFORE THE API, BECAUSE THE API IS SHAPED BY IT. Three things disqualify a
// behaviour from batching, and the specification names all three: invoking arbitrary script per
// entity per tick, holding unbounded per-instance state, and touching data outside its declaration.
// Each is a field of `BehaviourDesc` rather than an inference, and `decide_dispatch()` is the whole
// decision — twelve lines with a named reason for every outcome. That is deliberate: a batching
// rule a developer cannot predict from the declaration is a rule they cannot design against, and
// the specification's third scenario ("the cost is known at build time") is precisely about not
// discovering the cost at scale.
//
// WHAT M2 DOES AND DOES NOT DECIDE. The decision here is made from the *declaration*, at
// registration, and the lowered form is supplied by the author as `fixed_update_batch` /
// `update_batch`. There is no compiler yet that reads a script body and lowers it — that is the
// script toolchain at M4/M5, and when it arrives it fills in these two pointers and sets
// `invokes_script` from analysis rather than from the author's word. The report, the fallback, the
// system-shaped dispatch and the reasons are all real now; the lowering is hand-written now. Said
// plainly here so nobody reads `batched = true` as evidence that a compiler proved anything.
//
// LIFECYCLE CALLBACKS ARE NEVER BATCHED, AND THAT IS NOT AN OMISSION. `onCreate`, `onEnterTree`,
// `onReady`, `onEnable`, `onDisable`, `onExitTree` and `onDestroy` fire on transitions, not on
// ticks: there is no per-frame cost to remove. Only `onFixedUpdate` and `onUpdate` are per-tick, so
// only they are what "batched" means.
//
// OPT-IN IS THE PRESENCE OF THE POINTER. `scene-graph-and-nodes` requires that a behaviour which
// does not implement `onUpdate` is not added to the per-frame dispatch list. A null function
// pointer is that, checked once at registration rather than once per node per frame.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/core/values/name.h>
#include <cy/ecs/query.h>
#include <cy/ecs/system.h>
#include <cy/scene/node.h>

namespace cy::scene {

class SceneTree;

using BehaviourTypeId = u32;
inline constexpr BehaviourTypeId kInvalidBehaviour = 0xFFFF'FFFFu;
inline constexpr u32 kNoBehaviourInstance = 0xFFFF'FFFFu;

/// The prefix a batched behaviour's tag component is registered under. The tag is what its
/// generated system queries; a per-instance behaviour has none, because its system queries
/// `BehaviourRef` instead.
inline constexpr const char* kBehaviourComponentPrefix = "cy::scene::behaviour/";

/// The callbacks, in the order `scene-graph-and-nodes` tabulates them.
enum class BehaviourCallback : u8 {
    Create = 0,
    EnterTree = 1,
    Ready = 2,
    Enable = 3,
    Disable = 4,
    FixedUpdate = 5,
    Update = 6,
    ExitTree = 7,
    Destroy = 8,
};

const char* behaviour_callback_name(BehaviourCallback callback) noexcept;

/// What a per-instance callback is handed.
struct BehaviourContext {
    SceneTree* tree = nullptr;
    Node node;
    /// The instance's own storage, `state_size` bytes, zeroed at attach. Null when the behaviour
    /// declared none — which is also the condition that lets it batch.
    void* state = nullptr;
    /// Seconds. The fixed step for `onFixedUpdate`, the frame's delta for `onUpdate`, zero for
    /// every transition callback.
    f32 delta = 0.0F;
    /// Where a structural change goes. Null outside a schedule, in which case the callback is
    /// running at a point where the world accepts one directly.
    ecs::CommandBuffer* commands = nullptr;
};

/// What a lowered (batched) callback is handed: a whole chunk, not an entity.
struct BehaviourBatch {
    SceneTree* tree = nullptr;
    ecs::QueryChunk* chunk = nullptr;
    f32 delta = 0.0F;
    ecs::CommandBuffer* commands = nullptr;
};

using BehaviourFn = void (*)(const BehaviourContext& context) noexcept;
using BehaviourBatchFn = void (*)(const BehaviourBatch& batch) noexcept;

/// How a behaviour's per-tick work reaches the frame.
enum class BehaviourDispatch : u8 {
    /// No per-tick callback at all. It costs nothing per frame and is in no dispatch list.
    None = 0,
    /// One generated system iterating chunks, however many instances there are.
    Batched = 1,
    /// A system iterating entities and calling the behaviour once each.
    PerInstance = 2,
};

const char* behaviour_dispatch_name(BehaviourDispatch dispatch) noexcept;

/// A behaviour type, as its author declares it.
struct BehaviourDesc {
    /// A string literal or other storage outliving the registry. Also the tag component's suffix.
    const char* name = "";

    // The lifecycle callbacks. A null pointer is "not implemented", and an unimplemented callback
    // is in no list and costs nothing.
    BehaviourFn on_create = nullptr;
    BehaviourFn on_enter_tree = nullptr;
    BehaviourFn on_ready = nullptr;
    BehaviourFn on_enable = nullptr;
    BehaviourFn on_disable = nullptr;
    BehaviourFn on_exit_tree = nullptr;
    BehaviourFn on_destroy = nullptr;

    // The per-tick callbacks, in both forms. When a lowered form is present and nothing
    // disqualifies the behaviour, the lowered form is what runs and the per-instance one is never
    // called.
    BehaviourFn on_fixed_update = nullptr;
    BehaviourFn on_update = nullptr;
    BehaviourBatchFn fixed_update_batch = nullptr;
    BehaviourBatchFn update_batch = nullptr;

    /// The component data the behaviour declares. Copied at registration; these become the
    /// generated system's query and therefore its `jobs::AccessSet`, which is what the scheduler
    /// orders it by. A behaviour that reads a component it did not declare is the third
    /// disqualifier, and `accesses_undeclared_data` is how it says so.
    Span<const ComponentTypeId> reads;
    Span<const ComponentTypeId> writes;

    /// Bytes of per-instance state. Non-zero means per-instance dispatch: there is nowhere in a
    /// chunk to put it, and putting it in a side table indexed per row is the per-instance call the
    /// batching exists to avoid.
    u32 state_size = 0;
    u32 state_alignment = alignof(void*);

    /// True when a tick invokes arbitrary script for one entity. Set by the script toolchain when
    /// it arrives; declared by the author until then.
    bool invokes_script = false;
    /// True when the body reaches data its `reads`/`writes` do not name.
    bool accesses_undeclared_data = false;
};

/// One line of the batching report the specification requires the build to produce.
struct BehaviourReport {
    const char* name = "";
    BehaviourDispatch dispatch = BehaviourDispatch::None;
    /// Why it is not batched. A literal, empty when it is.
    const char* reason = "";
    u32 instances = 0;
    /// The callbacks it implements, as a bitmask over `BehaviourCallback`.
    u16 callbacks = 0;
};

/// The batching decision and its reason, derived from a declaration alone.
///
/// A free function, and public, so a project can ask the question before registering — and so the
/// rule can be tested directly rather than through a registry.
struct DispatchDecision {
    BehaviourDispatch dispatch = BehaviourDispatch::None;
    const char* reason = "";
};

[[nodiscard]] DispatchDecision decide_dispatch(const BehaviourDesc& desc) noexcept;

/// One world's behaviour types and their instances.
class BehaviourRegistry {
public:
    explicit BehaviourRegistry(Allocator& allocator) noexcept;
    ~BehaviourRegistry();

    BehaviourRegistry(const BehaviourRegistry&) = delete;
    BehaviourRegistry& operator=(const BehaviourRegistry&) = delete;
    BehaviourRegistry(BehaviourRegistry&&) = delete;
    BehaviourRegistry& operator=(BehaviourRegistry&&) = delete;

    /// Register a behaviour type and decide its dispatch. Refuses a duplicate name.
    [[nodiscard]] Expected<BehaviourTypeId, Error> add(World& world,
                                                       const BehaviourDesc& desc) noexcept;

    [[nodiscard]] BehaviourTypeId find(Name name) const noexcept;
    [[nodiscard]] BehaviourDispatch dispatch_of(BehaviourTypeId type) const noexcept;
    [[nodiscard]] const char* reason_of(BehaviourTypeId type) const noexcept;
    [[nodiscard]] ComponentTypeId tag_of(BehaviourTypeId type) const noexcept;
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(types_.size()); }
    /// Live instances across every type. `instances_.size()` counts the free list as well, which is
    /// why this is a method rather than that.
    [[nodiscard]] u32 instance_count() const noexcept;

    /// Attach an instance to a node: adds `BehaviourRef` (and the batched tag), allocates the
    /// instance's state, and fires `onCreate`. Structural, so it is refused during iteration.
    [[nodiscard]] Status attach(SceneTree& tree, Node node, BehaviourTypeId type) noexcept;

    /// Fire `onDestroy`, release the instance and remove the components. Called by node
    /// destruction; safe to call on a node with no behaviour.
    [[nodiscard]] Status detach(SceneTree& tree, Node node) noexcept;

    /// Release an instance whose entity has already been destroyed elsewhere — a system's
    /// `CommandBuffer::destroy`, say. `onDestroy` cannot run with the components gone, so it is not
    /// fired; the return value counts what was reclaimed so a caller can tell that it happened.
    [[nodiscard]] u32 reclaim_dead(SceneTree& tree) noexcept;

    /// The instance attached to `node`, or `kNoBehaviourInstance`.
    [[nodiscard]] u32 instance_of(const SceneTree& tree, Entity entity) const noexcept;
    [[nodiscard]] void* state_of(u32 instance) noexcept;
    [[nodiscard]] BehaviourTypeId type_of(u32 instance) const noexcept;

    /// Invoke one callback on one node, if the behaviour implements it. Returns true when it ran.
    bool invoke(SceneTree& tree, Node node, BehaviourCallback callback, f32 delta,
                ecs::CommandBuffer* commands) noexcept;

    /// Fire `onEnable`/`onDisable` for every instance whose effective enablement has changed since
    /// the last call. The transition, like the tree-shape ones, is observed at the pump rather than
    /// at the write, because effective enablement is published by propagation at the stage flush.
    [[nodiscard]] Status sync_enablement(SceneTree& tree) noexcept;

    /// `onReady` fires "once per attachment unless re-requested". This is the re-request.
    [[nodiscard]] Status request_ready(SceneTree& tree, Node node) noexcept;
    [[nodiscard]] bool ready(u32 instance) const noexcept;
    void set_ready(u32 instance, bool ready) noexcept;
    [[nodiscard]] bool enabled_state(u32 instance) const noexcept;
    void set_enabled_state(u32 instance, bool enabled) noexcept;

    /// The report the specification requires. Appends one entry per registered behaviour, in
    /// registration order.
    [[nodiscard]] Status report(Array<BehaviourReport>& out) const noexcept;
    /// The same, as the text a build log carries. NUL-terminated; `out` is cleared first.
    [[nodiscard]] Status write_report(Array<char>& out) const noexcept;

    /// The two shared per-instance dispatch systems' names, for a caller ordering around them.
    static constexpr const char* kFixedDispatchSystem = "scene.behaviour.dispatch.fixed";
    static constexpr const char* kFrameDispatchSystem = "scene.behaviour.dispatch.frame";

    /// Register the dispatch systems into `schedule`: one generated system per batched behaviour
    /// per per-tick callback, plus one per-instance dispatch system per stage for the rest.
    ///
    /// `scene-graph-and-nodes`' "behaviour dispatch is a system" scenario is this function: after
    /// it, nothing in the frame calls a behaviour except a registered system.
    [[nodiscard]] Status install(SceneTree& tree, ecs::Schedule& schedule) noexcept;

private:
    /// One behaviour type. Allocated on its own so its address is stable: a generated system holds
    /// it as the body's `user` pointer, and the array of types grows.
    struct Type;
    /// One installed dispatch system's state: its cached query and what it dispatches. Also
    /// allocated on its own, and for the same reason.
    struct SystemSlot;
    /// One attached behaviour.
    struct Instance {
        BehaviourTypeId type = kInvalidBehaviour;
        Entity entity;
        void* state = nullptr;
        bool live = false;
        bool ready = false;
        bool enabled = true;
    };

    static void batched_body(const ecs::SystemContext& context) noexcept;
    static void per_instance_body(const ecs::SystemContext& context) noexcept;
    [[nodiscard]] Status install_batched(SceneTree& tree, ecs::Schedule& schedule,
                                         BehaviourTypeId id, bool fixed) noexcept;
    [[nodiscard]] Status install_per_instance(SceneTree& tree, ecs::Schedule& schedule,
                                              bool fixed) noexcept;
    /// Add every per-instance behaviour's declared components to the shared dispatch system's
    /// query, and report how many behaviours it will dispatch. Zero means no system is needed.
    [[nodiscard]] Expected<u32, Error> declare_per_instance_terms(ecs::QueryDesc& desc,
                                                                  ecs::ComponentMask& seen,
                                                                  bool fixed) const noexcept;
    [[nodiscard]] Expected<SystemSlot*, Error> acquire_slot(SceneTree& tree,
                                                            ecs::QueryDesc&& desc) noexcept;

    [[nodiscard]] Expected<u32, Error> acquire_instance(BehaviourTypeId type,
                                                        Entity entity) noexcept;
    void release_instance(u32 instance) noexcept;
    [[nodiscard]] const Type* type_at(BehaviourTypeId type) const noexcept;

    Allocator* allocator_;
    Array<Type*> types_;
    HashMap<u32, BehaviourTypeId> by_name_;
    Array<Instance> instances_;
    Array<u32> free_instances_;
    /// The per-instance dispatch systems' state, one per stage they run in. Allocated for the same
    /// stability reason as a `Type`.
    Array<void*> dispatch_slots_;
};

}  // namespace cy::scene
