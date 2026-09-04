// Behaviours: registration, the batching decision, instances, and the systems that dispatch them.
// Tasks 3.1.6 and 3.1.7.

#include <cy/scene/behaviour.h>

#include <cy/scene/tree.h>

#include <cstdio>
#include <cstring>

namespace cy::scene {

/// One registered behaviour type. Defined here rather than in the header because nothing outside
/// this file needs its shape, and because it holds the arrays the declaration's spans point into.
struct BehaviourRegistry::Type {
    explicit Type(Allocator& allocator) noexcept : reads(allocator), writes(allocator) {}

    Name name;
    BehaviourDesc desc;
    /// The declaration's component lists, copied. `BehaviourDesc::reads`/`writes` are rebound to
    /// these, so a caller may declare them from a stack temporary.
    Array<ComponentTypeId> reads;
    Array<ComponentTypeId> writes;
    BehaviourDispatch dispatch = BehaviourDispatch::None;
    const char* reason = "";
    /// The tag a batched behaviour's generated system queries. Invalid for the other two kinds.
    ComponentTypeId tag = kInvalidComponent;
    u32 instances = 0;
};

/// The state one installed dispatch system holds. Allocated on its own: the ECS schedule keeps the
/// address as the body's `user` pointer, and the registry's arrays grow.
struct BehaviourRegistry::SystemSlot {
    SystemSlot(World& world, ecs::QueryDesc&& desc) noexcept : query(world, std::move(desc)) {}

    BehaviourRegistry* registry = nullptr;
    SceneTree* tree = nullptr;
    BehaviourTypeId type = kInvalidBehaviour;
    bool per_instance = false;
    /// True for the fixed-step callback, false for the per-frame one.
    bool fixed = false;
    /// The lowered function a batched slot calls. Null for a per-instance slot.
    BehaviourBatchFn batch = nullptr;
    ecs::Query query;
};

namespace {

[[nodiscard]] const char* system_name(Name behaviour, bool fixed) noexcept {
    char buffer[Name::kMaxLength + 48];
    const std::string_view text = behaviour.text();
    const int written =
        std::snprintf(buffer, sizeof(buffer), "scene.behaviour.%.*s.%s",
                      static_cast<int>(text.size()), text.data(), fixed ? "fixed" : "frame");
    if (written <= 0) {
        return "scene.behaviour";
    }
    return Name::intern(std::string_view(buffer, static_cast<usize>(written))).c_str();
}

[[nodiscard]] Name behaviour_tag_name(Name behaviour) noexcept {
    char buffer[Name::kMaxLength + 32];
    const std::string_view text = behaviour.text();
    const int written = std::snprintf(buffer, sizeof(buffer), "%s%.*s", kBehaviourComponentPrefix,
                                      static_cast<int>(text.size()), text.data());
    if (written <= 0) {
        return {};
    }
    return Name::intern(std::string_view(buffer, static_cast<usize>(written)));
}

}  // namespace

const char* behaviour_callback_name(BehaviourCallback callback) noexcept {
    switch (callback) {
        case BehaviourCallback::Create:
            return "onCreate";
        case BehaviourCallback::EnterTree:
            return "onEnterTree";
        case BehaviourCallback::Ready:
            return "onReady";
        case BehaviourCallback::Enable:
            return "onEnable";
        case BehaviourCallback::Disable:
            return "onDisable";
        case BehaviourCallback::FixedUpdate:
            return "onFixedUpdate";
        case BehaviourCallback::Update:
            return "onUpdate";
        case BehaviourCallback::ExitTree:
            return "onExitTree";
        case BehaviourCallback::Destroy:
            return "onDestroy";
    }
    return "unknown";
}

const char* behaviour_dispatch_name(BehaviourDispatch dispatch) noexcept {
    switch (dispatch) {
        case BehaviourDispatch::None:
            return "none";
        case BehaviourDispatch::Batched:
            return "batched";
        case BehaviourDispatch::PerInstance:
            return "per-instance";
    }
    return "unknown";
}

DispatchDecision decide_dispatch(const BehaviourDesc& desc) noexcept {
    const bool ticks = desc.on_fixed_update != nullptr || desc.on_update != nullptr ||
                       desc.fixed_update_batch != nullptr || desc.update_batch != nullptr;
    if (!ticks) {
        return {BehaviourDispatch::None, "no per-tick callback: it is in no per-frame list"};
    }
    // The three disqualifiers `scene-graph-and-nodes` names, in the order it names them.
    if (desc.invokes_script) {
        return {BehaviourDispatch::PerInstance, "invokes arbitrary script per entity per tick"};
    }
    if (desc.state_size != 0) {
        return {BehaviourDispatch::PerInstance, "holds per-instance state"};
    }
    if (desc.accesses_undeclared_data) {
        return {BehaviourDispatch::PerInstance, "accesses data outside its declaration"};
    }
    if (desc.reads.empty() && desc.writes.empty()) {
        return {BehaviourDispatch::PerInstance, "declares no component data to iterate"};
    }
    if (desc.on_fixed_update != nullptr && desc.fixed_update_batch == nullptr) {
        return {BehaviourDispatch::PerInstance, "no lowered form for onFixedUpdate"};
    }
    if (desc.on_update != nullptr && desc.update_batch == nullptr) {
        return {BehaviourDispatch::PerInstance, "no lowered form for onUpdate"};
    }
    return {BehaviourDispatch::Batched, ""};
}

BehaviourRegistry::BehaviourRegistry(Allocator& allocator) noexcept
    : allocator_(&allocator),
      types_(allocator),
      by_name_(allocator),
      instances_(allocator),
      free_instances_(allocator),
      dispatch_slots_(allocator) {}

BehaviourRegistry::~BehaviourRegistry() {
    for (Instance& instance : instances_) {
        if (instance.state != nullptr) {
            const Type* type = type_at(instance.type);
            allocator_->deallocate(instance.state, (type == nullptr) ? 0 : type->desc.state_size,
                                   (type == nullptr) ? alignof(void*) : type->desc.state_alignment);
            instance.state = nullptr;
        }
    }
    for (void* slot : dispatch_slots_) {
        auto* typed = static_cast<SystemSlot*>(slot);
        typed->~SystemSlot();
        allocator_->deallocate(slot, sizeof(SystemSlot), alignof(SystemSlot));
    }
    for (Type* type : types_) {
        type->~Type();
        allocator_->deallocate(static_cast<void*>(type), sizeof(Type), alignof(Type));
    }
}

Expected<BehaviourTypeId, Error> BehaviourRegistry::add(World& world,
                                                        const BehaviourDesc& desc) noexcept {
    const Name name = Name::intern(desc.name);
    if (name.is_empty()) {
        return fail(ErrorCode::InvalidArgument, "a behaviour needs a name");
    }
    if (by_name_.find(name.index()) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "a behaviour with this name is registered");
    }

    void* block = allocator_->allocate(sizeof(Type), alignof(Type));
    if (block == nullptr) {
        return fail(ErrorCode::OutOfMemory, "could not allocate a behaviour type");
    }
    auto* type = ::new (block) Type(*allocator_);
    type->name = name;
    type->desc = desc;
    const DispatchDecision decision = decide_dispatch(desc);
    type->dispatch = decision.dispatch;
    type->reason = decision.reason;

    Status prepared = type->reads.append(desc.reads);
    if (prepared) {
        prepared = type->writes.append(desc.writes);
    }
    if (prepared && type->dispatch == BehaviourDispatch::Batched) {
        // Only a batched behaviour needs a tag: its generated system is a query, and a query needs
        // something to match on. A per-instance one is found through `BehaviourRef` instead.
        ecs::ComponentOptions options;
        options.kind = ecs::ComponentKind::Tag;
        Expected<ComponentTypeId, Error> tag =
            world.components().register_builtin(behaviour_tag_name(name).c_str(), 0, 1, options);
        if (!tag) {
            prepared = make_unexpected(tag.error());
        } else {
            type->tag = *tag;
        }
    }
    if (prepared) {
        prepared = types_.push_back(type);
    }
    if (!prepared) {
        type->~Type();
        allocator_->deallocate(block, sizeof(Type), alignof(Type));
        return make_unexpected(prepared.error());
    }
    // Rebound onto the registry's copies, so the declaration may have named a stack temporary.
    type->desc.reads = type->reads.span();
    type->desc.writes = type->writes.span();

    const auto id = static_cast<BehaviourTypeId>(types_.size() - 1);
    if (Expected<BehaviourTypeId*, Error> slot = by_name_.insert(name.index(), id); !slot) {
        types_.pop_back();
        type->~Type();
        allocator_->deallocate(block, sizeof(Type), alignof(Type));
        return make_unexpected(slot.error());
    }
    return id;
}

const BehaviourRegistry::Type* BehaviourRegistry::type_at(BehaviourTypeId type) const noexcept {
    return (type < types_.size()) ? types_[type] : nullptr;
}

BehaviourTypeId BehaviourRegistry::find(Name name) const noexcept {
    const BehaviourTypeId* found = by_name_.find(name.index());
    return (found == nullptr) ? kInvalidBehaviour : *found;
}

BehaviourDispatch BehaviourRegistry::dispatch_of(BehaviourTypeId type) const noexcept {
    const Type* entry = type_at(type);
    return (entry == nullptr) ? BehaviourDispatch::None : entry->dispatch;
}

const char* BehaviourRegistry::reason_of(BehaviourTypeId type) const noexcept {
    const Type* entry = type_at(type);
    return (entry == nullptr) ? "" : entry->reason;
}

ComponentTypeId BehaviourRegistry::tag_of(BehaviourTypeId type) const noexcept {
    const Type* entry = type_at(type);
    return (entry == nullptr) ? kInvalidComponent : entry->tag;
}

Expected<u32, Error> BehaviourRegistry::acquire_instance(BehaviourTypeId type,
                                                         Entity entity) noexcept {
    const Type* entry = type_at(type);
    if (entry == nullptr) {
        return fail(ErrorCode::NotFound, "no behaviour type with this id");
    }
    void* state = nullptr;
    if (entry->desc.state_size != 0) {
        state = allocator_->allocate(entry->desc.state_size, entry->desc.state_alignment);
        if (state == nullptr) {
            return fail(ErrorCode::OutOfMemory, "could not allocate behaviour state");
        }
        std::memset(state, 0, entry->desc.state_size);
    }

    u32 index = 0;
    if (!free_instances_.empty()) {
        index = free_instances_.back();
        free_instances_.pop_back();
    } else {
        if (Status pushed = instances_.push_back(Instance{}); !pushed) {
            if (state != nullptr) {
                allocator_->deallocate(state, entry->desc.state_size, entry->desc.state_alignment);
            }
            return make_unexpected(pushed.error());
        }
        index = static_cast<u32>(instances_.size() - 1);
    }
    Instance& instance = instances_[index];
    instance = Instance{};
    instance.type = type;
    instance.entity = entity;
    instance.state = state;
    instance.live = true;
    return index;
}

void BehaviourRegistry::release_instance(u32 instance) noexcept {
    if (instance >= instances_.size() || !instances_[instance].live) {
        return;
    }
    Instance& held = instances_[instance];
    if (Type* entry = (held.type < types_.size()) ? types_[held.type] : nullptr; entry != nullptr) {
        if (held.state != nullptr) {
            allocator_->deallocate(held.state, entry->desc.state_size, entry->desc.state_alignment);
        }
        if (entry->instances != 0) {
            --entry->instances;
        }
    }
    held = Instance{};
    (void)free_instances_.push_back(instance);
}

u32 BehaviourRegistry::instance_of(const SceneTree& tree, Entity entity) const noexcept {
    const auto* ref = tree.world().get<BehaviourRef>(entity, tree.components().behaviour_ref);
    // Validated against the pool rather than trusted: the component is data in a chunk, and a
    // snapshot restored from another run could carry an index this registry never handed out.
    if (ref == nullptr || ref->instance >= instances_.size() || !instances_[ref->instance].live) {
        return kNoBehaviourInstance;
    }
    return ref->instance;
}

void* BehaviourRegistry::state_of(u32 instance) noexcept {
    return (instance < instances_.size()) ? instances_[instance].state : nullptr;
}

u32 BehaviourRegistry::instance_count() const noexcept {
    u32 live = 0;
    for (const Instance& instance : instances_) {
        live += instance.live ? 1U : 0U;
    }
    return live;
}

BehaviourTypeId BehaviourRegistry::type_of(u32 instance) const noexcept {
    return (instance < instances_.size()) ? instances_[instance].type : kInvalidBehaviour;
}

bool BehaviourRegistry::ready(u32 instance) const noexcept {
    return instance < instances_.size() && instances_[instance].ready;
}

void BehaviourRegistry::set_ready(u32 instance, bool ready) noexcept {
    if (instance < instances_.size()) {
        instances_[instance].ready = ready;
    }
}

bool BehaviourRegistry::enabled_state(u32 instance) const noexcept {
    return instance < instances_.size() && instances_[instance].enabled;
}

void BehaviourRegistry::set_enabled_state(u32 instance, bool enabled) noexcept {
    if (instance < instances_.size()) {
        instances_[instance].enabled = enabled;
    }
}

Status BehaviourRegistry::attach(SceneTree& tree, Node node, BehaviourTypeId type) noexcept {
    if (!node.valid()) {
        return fail(ErrorCode::NotFound, "attach() on a node that is not valid");
    }
    Type* entry = (type < types_.size()) ? types_[type] : nullptr;
    if (entry == nullptr) {
        return fail(ErrorCode::NotFound, "no behaviour type with this id");
    }
    World& world = tree.world();
    if (world.has(node.entity(), tree.components().behaviour_ref)) {
        return fail(ErrorCode::AlreadyExists, "this node already has a behaviour");
    }

    Expected<u32, Error> instance = acquire_instance(type, node.entity());
    if (!instance) {
        return make_unexpected(instance.error());
    }
    const BehaviourRef ref{*instance};
    if (Status added = world.add(node.entity(), tree.components().behaviour_ref, &ref); !added) {
        release_instance(*instance);
        return added;
    }
    if (entry->tag != kInvalidComponent) {
        if (Status tagged = world.add(node.entity(), entry->tag); !tagged) {
            release_instance(*instance);
            return tagged;
        }
    }
    ++entry->instances;
    instances_[*instance].enabled = node.effective_enabled();
    // `onCreate`: "Entity and components exist; parent may not be set." Both are true here, and
    // this is the last moment at which they are true only of this node.
    (void)invoke(tree, node, BehaviourCallback::Create, 0.0F, nullptr);
    return ok();
}

Status BehaviourRegistry::detach(SceneTree& tree, Node node) noexcept {
    if (!node.valid()) {
        return ok();
    }
    const u32 instance = instance_of(tree, node.entity());
    if (instance == kNoBehaviourInstance || instance >= instances_.size()) {
        return ok();
    }
    // `onDestroy` is specified as running "before components are released", so it is fired while
    // the node is still whole and the components are removed afterwards.
    (void)invoke(tree, node, BehaviourCallback::Destroy, 0.0F, nullptr);

    const BehaviourTypeId type = instances_[instance].type;
    const Type* entry = type_at(type);
    World& world = tree.world();
    if (entry != nullptr && entry->tag != kInvalidComponent &&
        world.has(node.entity(), entry->tag)) {
        if (Status removed = world.remove(node.entity(), entry->tag); !removed) {
            return removed;
        }
    }
    if (world.has(node.entity(), tree.components().behaviour_ref)) {
        if (Status removed = world.remove(node.entity(), tree.components().behaviour_ref);
            !removed) {
            return removed;
        }
    }
    release_instance(instance);
    return ok();
}

u32 BehaviourRegistry::reclaim_dead(SceneTree& tree) noexcept {
    u32 reclaimed = 0;
    for (usize index = 0; index < instances_.size(); ++index) {
        const Instance& instance = instances_[index];
        if (!instance.live || tree.world().is_alive(instance.entity)) {
            continue;
        }
        // The components are already gone, so `onDestroy` cannot run: it is specified to run before
        // they are released, and they were released by whoever destroyed the entity. Destroying
        // through the node API gives the full ordering; this path only stops the instance leaking.
        release_instance(static_cast<u32>(index));
        ++reclaimed;
    }
    return reclaimed;
}

bool BehaviourRegistry::invoke(SceneTree& tree, Node node, BehaviourCallback callback, f32 delta,
                               ecs::CommandBuffer* commands) noexcept {
    if (!node.valid()) {
        return false;
    }
    const u32 instance = instance_of(tree, node.entity());
    if (instance == kNoBehaviourInstance || instance >= instances_.size() ||
        !instances_[instance].live) {
        return false;
    }
    const Type* entry = type_at(instances_[instance].type);
    if (entry == nullptr) {
        return false;
    }

    BehaviourFn function = nullptr;
    switch (callback) {
        case BehaviourCallback::Create:
            function = entry->desc.on_create;
            break;
        case BehaviourCallback::EnterTree:
            function = entry->desc.on_enter_tree;
            break;
        case BehaviourCallback::Ready:
            function = entry->desc.on_ready;
            break;
        case BehaviourCallback::Enable:
            function = entry->desc.on_enable;
            break;
        case BehaviourCallback::Disable:
            function = entry->desc.on_disable;
            break;
        case BehaviourCallback::FixedUpdate:
            function = entry->desc.on_fixed_update;
            break;
        case BehaviourCallback::Update:
            function = entry->desc.on_update;
            break;
        case BehaviourCallback::ExitTree:
            function = entry->desc.on_exit_tree;
            break;
        case BehaviourCallback::Destroy:
            function = entry->desc.on_destroy;
            break;
    }
    if (function == nullptr) {
        // Opt-in, checked here rather than at every call site: an unimplemented callback costs one
        // null test and never reaches a dispatch list.
        return false;
    }

    BehaviourContext context;
    context.tree = &tree;
    context.node = node;
    context.state = instances_[instance].state;
    context.delta = delta;
    context.commands = commands;
    function(context);
    return true;
}

Status BehaviourRegistry::sync_enablement(SceneTree& tree) noexcept {
    for (Instance& instance : instances_) {
        if (!instance.live) {
            continue;
        }
        const Node node = tree.node(instance.entity);
        if (!node.valid()) {
            continue;
        }
        const bool now = node.effective_enabled();
        if (now == instance.enabled) {
            continue;
        }
        instance.enabled = now;
        (void)invoke(tree, node, now ? BehaviourCallback::Enable : BehaviourCallback::Disable, 0.0F,
                     nullptr);
    }
    return ok();
}

Status BehaviourRegistry::request_ready(SceneTree& tree, Node node) noexcept {
    const u32 instance = instance_of(tree, node.entity());
    if (instance == kNoBehaviourInstance || instance >= instances_.size()) {
        return fail(ErrorCode::NotFound, "this node has no behaviour");
    }
    instances_[instance].ready = false;
    return ok();
}

Status BehaviourRegistry::report(Array<BehaviourReport>& out) const noexcept {
    for (const Type* type : types_) {
        BehaviourReport entry;
        entry.name = type->name.c_str();
        entry.dispatch = type->dispatch;
        entry.reason = type->reason;
        entry.instances = type->instances;
        entry.callbacks = 0;
        const BehaviourFn callbacks[] = {
            type->desc.on_create, type->desc.on_enter_tree, type->desc.on_ready,
            type->desc.on_enable, type->desc.on_disable,    type->desc.on_fixed_update,
            type->desc.on_update, type->desc.on_exit_tree,  type->desc.on_destroy};
        for (u32 index = 0; index < sizeof(callbacks) / sizeof(callbacks[0]); ++index) {
            if (callbacks[index] != nullptr) {
                entry.callbacks = static_cast<u16>(entry.callbacks | (1U << index));
            }
        }
        if (Status pushed = out.push_back(entry); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status BehaviourRegistry::write_report(Array<char>& out) const noexcept {
    out.clear();
    Array<BehaviourReport> entries(*allocator_);
    if (Status collected = report(entries); !collected) {
        return collected;
    }
    char line[512];
    for (const BehaviourReport& entry : entries) {
        const int written =
            std::snprintf(line, sizeof(line), "%-32s %-12s instances=%u%s%s\n", entry.name,
                          behaviour_dispatch_name(entry.dispatch), entry.instances,
                          (entry.reason[0] == '\0') ? "" : "  reason: ", entry.reason);
        if (written <= 0) {
            continue;
        }
        if (Status appended = out.append(Span<const char>(line, static_cast<usize>(written)));
            !appended) {
            return appended;
        }
    }
    return out.push_back('\0');
}

// --- Installing the dispatch systems --------------------------------------------------------
//
// After `install()`, nothing in the frame calls a behaviour except a registered system: a batched
// behaviour is one generated system iterating chunks, and every per-instance behaviour of one stage
// shares a system that iterates `BehaviourRef`. That is `scene-graph-and-nodes`' "behaviour
// dispatch is a system" scenario, and it is why script execution is "scheduled and ordered like any
// other system" rather than being called from a tree walk.

namespace {

/// Declare a behaviour's component terms on a query, skipping anything already declared.
///
/// `jobs::AccessSet::declare` refuses an identical duplicate — correctly, since a system declaring
/// one component twice is a copy-paste — so the shared per-instance system, whose declaration is
/// the union of several behaviours', has to dedupe before it declares. Writes go first, so a
/// component that one behaviour reads and another writes is declared as a write.
[[nodiscard]] Status declare_terms(ecs::QueryDesc& desc, Span<const ComponentTypeId> reads,
                                   Span<const ComponentTypeId> writes,
                                   ecs::ComponentMask& seen) noexcept {
    for (const ComponentTypeId component : writes) {
        if (component == kInvalidComponent || seen.test(component)) {
            continue;
        }
        seen.set(component);
        if (Status declared = desc.write(component); !declared) {
            return declared;
        }
    }
    for (const ComponentTypeId component : reads) {
        if (component == kInvalidComponent || seen.test(component)) {
            continue;
        }
        seen.set(component);
        if (Status declared = desc.read(component); !declared) {
            return declared;
        }
    }
    return ok();
}

}  // namespace

void BehaviourRegistry::batched_body(const ecs::SystemContext& context) noexcept {
    auto* slot = static_cast<SystemSlot*>(context.user);
    if (slot == nullptr || slot->batch == nullptr) {
        return;
    }
    const f32 delta = slot->fixed ? slot->tree->fixed_delta() : slot->tree->frame_delta();
    (void)slot->query.for_each_chunk([slot, &context, delta](ecs::QueryChunk& chunk) noexcept {
        BehaviourBatch batch;
        batch.tree = slot->tree;
        batch.chunk = &chunk;
        batch.delta = delta;
        batch.commands = context.commands;
        slot->batch(batch);
    });
}

void BehaviourRegistry::per_instance_body(const ecs::SystemContext& context) noexcept {
    auto* slot = static_cast<SystemSlot*>(context.user);
    if (slot == nullptr) {
        return;
    }
    SceneTree& tree = *slot->tree;
    BehaviourRegistry& registry = *slot->registry;
    const BehaviourCallback callback =
        slot->fixed ? BehaviourCallback::FixedUpdate : BehaviourCallback::Update;
    const f32 delta = slot->fixed ? tree.fixed_delta() : tree.frame_delta();
    (void)slot->query.for_each_chunk(
        [&tree, &registry, callback, delta, &context](ecs::QueryChunk& chunk) noexcept {
            for (const Entity entity : chunk.entities()) {
                // The body runs inside the iteration, so a structural change from it goes to the
                // system's command buffer — which is exactly the deal every other system has.
                (void)registry.invoke(tree, tree.node(entity), callback, delta, context.commands);
            }
        });
}

Status BehaviourRegistry::install(SceneTree& tree, ecs::Schedule& schedule) noexcept {
    for (usize index = 0; index < types_.size(); ++index) {
        Type& type = *types_[index];
        if (type.dispatch != BehaviourDispatch::Batched) {
            continue;
        }
        if (type.desc.fixed_update_batch != nullptr) {
            if (Status added =
                    install_batched(tree, schedule, static_cast<BehaviourTypeId>(index), true);
                !added) {
                return added;
            }
        }
        if (type.desc.update_batch != nullptr) {
            if (Status added =
                    install_batched(tree, schedule, static_cast<BehaviourTypeId>(index), false);
                !added) {
                return added;
            }
        }
    }
    if (Status added = install_per_instance(tree, schedule, true); !added) {
        return added;
    }
    return install_per_instance(tree, schedule, false);
}

Status BehaviourRegistry::install_batched(SceneTree& tree, ecs::Schedule& schedule,
                                          BehaviourTypeId id, bool fixed) noexcept {
    Type& type = *types_[id];
    ecs::QueryDesc desc(tree.allocator());
    if (Status with = desc.with(type.tag); !with) {
        return with;
    }
    // A disabled subtree is excluded before its chunks are touched, which is the whole reason
    // effective enablement is a tag — see components.h.
    if (Status without = desc.without(tree.components().disabled); !without) {
        return without;
    }
    ecs::ComponentMask seen;
    if (Status declared = declare_terms(desc, type.desc.reads, type.desc.writes, seen); !declared) {
        return declared;
    }
    const jobs::AccessSet access = desc.access();

    Expected<SystemSlot*, Error> slot = acquire_slot(tree, std::move(desc));
    if (!slot) {
        return make_unexpected(slot.error());
    }
    (*slot)->fixed = fixed;
    (*slot)->type = id;
    (*slot)->batch = fixed ? type.desc.fixed_update_batch : type.desc.update_batch;

    ecs::SystemDesc system;
    system.name = system_name(type.name, fixed);
    system.body = &batched_body;
    system.user = *slot;
    system.access = access;
    Expected<ecs::SystemId, Error> added =
        schedule.add(fixed ? ecs::Stage::Simulation : ecs::Stage::Frame, system);
    if (!added) {
        return make_unexpected(added.error());
    }
    return ok();
}

Status BehaviourRegistry::install_per_instance(SceneTree& tree, ecs::Schedule& schedule,
                                               bool fixed) noexcept {
    ecs::QueryDesc desc(tree.allocator());
    if (Status read = desc.read(tree.components().behaviour_ref); !read) {
        return read;
    }
    if (Status without = desc.without(tree.components().disabled); !without) {
        return without;
    }

    ecs::ComponentMask seen;
    seen.set(tree.components().behaviour_ref);
    Expected<u32, Error> dispatched = declare_per_instance_terms(desc, seen, fixed);
    if (!dispatched) {
        return make_unexpected(dispatched.error());
    }
    if (*dispatched == 0) {
        // No system at all rather than a system that iterates and calls nothing: an unimplemented
        // callback has to cost nothing, and a registered system costs a graph node and a batch.
        return ok();
    }
    const jobs::AccessSet access = desc.access();

    Expected<SystemSlot*, Error> slot = acquire_slot(tree, std::move(desc));
    if (!slot) {
        return make_unexpected(slot.error());
    }
    (*slot)->fixed = fixed;
    (*slot)->per_instance = true;

    ecs::SystemDesc system;
    system.name = fixed ? kFixedDispatchSystem : kFrameDispatchSystem;
    system.body = &per_instance_body;
    system.user = *slot;
    system.access = access;
    Expected<ecs::SystemId, Error> added =
        schedule.add(fixed ? ecs::Stage::Simulation : ecs::Stage::Frame, system);
    if (!added) {
        return make_unexpected(added.error());
    }
    return ok();
}

Expected<u32, Error> BehaviourRegistry::declare_per_instance_terms(ecs::QueryDesc& desc,
                                                                   ecs::ComponentMask& seen,
                                                                   bool fixed) const noexcept {
    u32 dispatched = 0;
    for (const Type* type : types_) {
        if (type->dispatch != BehaviourDispatch::PerInstance) {
            continue;
        }
        if ((fixed ? type->desc.on_fixed_update : type->desc.on_update) == nullptr) {
            continue;
        }
        ++dispatched;
        // The union is a declaration this system can only honour for behaviours that keep to it.
        // One that does not is precisely the behaviour the report names as accessing undeclared
        // data, which is the same reason it is not batched.
        if (Status declared = declare_terms(desc, type->desc.reads, type->desc.writes, seen);
            !declared) {
            return make_unexpected(declared.error());
        }
    }
    return dispatched;
}

Expected<BehaviourRegistry::SystemSlot*, Error> BehaviourRegistry::acquire_slot(
    SceneTree& tree, ecs::QueryDesc&& desc) noexcept {
    void* block = allocator_->allocate(sizeof(SystemSlot), alignof(SystemSlot));
    if (block == nullptr) {
        return fail(ErrorCode::OutOfMemory, "could not allocate a behaviour dispatch slot");
    }
    auto* slot = ::new (block) SystemSlot(tree.world(), std::move(desc));
    slot->registry = this;
    slot->tree = &tree;
    if (Status pushed = dispatch_slots_.push_back(block); !pushed) {
        slot->~SystemSlot();
        allocator_->deallocate(block, sizeof(SystemSlot), alignof(SystemSlot));
        return make_unexpected(pushed.error());
    }
    return slot;
}

}  // namespace cy::scene
