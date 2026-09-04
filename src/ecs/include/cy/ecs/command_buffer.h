#pragma once
// Deferred structural change. Task 2.6, design.md §2.
//
// `ecs-core` — "Structural change deferral": systems record structural operations into a
// `CommandBuffer` and the buffers are applied at the end of the stage, merged in a deterministic
// order. The supported operations are create (with an immediately usable placeholder id), destroy,
// add component, remove component, set component, and add child.
//
// THIS IS A CORRECTNESS MECHANISM AND NOT AN OPTIMISATION. A system iterating a chunk while another
// moves an entity out of it is iterating freed memory, and the scheduler parallelises systems by
// construction from their access declarations — so there is no build in which immediate structural
// mutation is safe and the deferral is merely faster. `World` refuses a structural call during
// iteration in every configuration; this is the thing to use instead.
//
// THE PLACEHOLDER. `create()` returns an entity id at the moment it is called, before the entity
// exists. It carries `kPlaceholderGeneration`, which no entity table ever issues, so passing one to
// a live-world call fails the same `is_alive` check a stale id fails rather than needing a rule of
// its own. Every later command in the same buffer may name it; at `apply()` each placeholder is
// resolved to the real entity, once, and every reference to it is rewritten.
//
// THE MERGE ORDER IS (system, thread, record). `ecs-core` requires "by system order then by thread
// index, so results are reproducible". Both are declared when the buffer is constructed, not
// discovered from whichever worker happened to run the body — cy::jobs makes the same point about
// its own commit key, and for the same reason: work stealing makes a worker's identity a function
// of timing.
//
// AND `system_order` IS A RANK DERIVED FROM AN IDENTITY, NOT A REGISTRATION SEQUENCE NUMBER. Task
// 1.7. Through M2 it was the index a system happened to be registered at within its stage, which is
// reproducible only for as long as the same code registers the same systems in the same order.
// The moment registration is conditional — a plugin, a feature flag, a game mode that adds one
// system — every later system's key shifts, two conflicting spawns swap places, and the world after
// the flush is a different world. That is not a hypothetical failure mode: it is the same class of
// defect as folding a `ComponentTypeId` into the state hash, which M2 measured and fixed at its
// close for exactly this reason.
//
// `Schedule::build()` now assigns the key as the system's **rank among its stage's systems in name
// order** — `StateProviderRegistry::finalize()`'s shape, and the "deterministic order derived from
// stable identifiers" `simulation-and-determinism` requires of a registry whose contents affect
// simulation. A system's name is unique within a stage (the schedule refuses a duplicate), so the
// rank is a total order; adding a system renumbers the ones after it in the alphabet and nothing
// else; and removing one changes nothing about the rest.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/component.h>
#include <cy/ecs/entity.h>
#include <cy/ecs/relationships.h>

namespace cy::ecs {

class World;

enum class CommandOp : u8 {
    CreateEntity = 0,
    DestroyEntity = 1,
    AddComponent = 2,
    RemoveComponent = 3,
    SetComponent = 4,
    AddChild = 5,
};

const char* command_op_name(CommandOp op) noexcept;

/// One system's, one thread's recorded structural changes.
class CommandBuffer {
public:
    /// `system_order` and `thread_index` are the merge key. A buffer used outside a schedule may
    /// leave both at zero; two buffers with the same key are merged by the order they were attached
    /// to the world, which is itself a registration order and therefore stable.
    ///
    /// A buffer owned by a `Schedule` is constructed here and given its real key by
    /// `Schedule::build()`, which is the only place that knows every system's name and can rank
    /// them — see `set_merge_key` and the header comment.
    CommandBuffer(World& world, u32 system_order = 0, u32 thread_index = 0) noexcept;
    ~CommandBuffer();

    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;
    CommandBuffer(CommandBuffer&&) = delete;
    CommandBuffer& operator=(CommandBuffer&&) = delete;

    /// Record a creation and hand back a placeholder usable immediately. See the header.
    [[nodiscard]] Expected<Entity, Error> create() noexcept;

    [[nodiscard]] Status destroy(Entity entity,
                                 DestroyPolicy policy = DestroyPolicy::CascadeChildren) noexcept;

    /// Add a component, with an optional initial value copied into the buffer. The value is copied
    /// rather than referenced because the caller's storage is very often a stack temporary inside a
    /// system body that has returned by the time the stage flushes.
    [[nodiscard]] Status add(Entity entity, ComponentTypeId component, const void* value = nullptr,
                             u32 size = 0) noexcept;

    template <class T>
    [[nodiscard]] Status add(Entity entity, ComponentTypeId component, const T& value) noexcept {
        return add(entity, component, static_cast<const void*>(&value),
                   static_cast<u32>(sizeof(T)));
    }

    [[nodiscard]] Status remove(Entity entity, ComponentTypeId component) noexcept;

    [[nodiscard]] Status set(Entity entity, ComponentTypeId component, const void* value,
                             u32 size) noexcept;

    template <class T>
    [[nodiscard]] Status set(Entity entity, ComponentTypeId component, const T& value) noexcept {
        return set(entity, component, static_cast<const void*>(&value),
                   static_cast<u32>(sizeof(T)));
    }

    [[nodiscard]] Status add_child(Entity parent, Entity child) noexcept;

    /// Set the merge key from a stable identity, once the identities are all known.
    ///
    /// The one supported way to change it. A registry that orders its members by name cannot know
    /// a member's rank until the last one has joined, so the key is assigned at the point the
    /// registry is finalised rather than at the point a buffer is constructed — which is why this
    /// exists at all, and it is the reason `Schedule::build()` is the only caller in the engine.
    ///
    /// Refused while the buffer holds commands: the key decides where those commands land in the
    /// merge, and changing it underneath them would move recorded work to a different point in the
    /// flush. Refused rather than asserted, because `CY_ASSERT` is compiled out in Profile and
    /// Shipping and this is a condition a caller composing a schedule from configuration can
    /// legitimately reach.
    [[nodiscard]] Status set_merge_key(u32 system_order, u32 thread_index) noexcept;

    /// Apply every recorded command in record order and empty the buffer. Called by `World::flush`,
    /// which is what establishes the order between buffers; calling it directly applies one
    /// buffer's commands and is what a test that wants no merge at all does.
    [[nodiscard]] Expected<u64, Error> apply() noexcept;

    void clear() noexcept;

    [[nodiscard]] u32 pending() const noexcept { return static_cast<u32>(commands_.size()); }
    [[nodiscard]] u32 system_order() const noexcept { return system_order_; }
    [[nodiscard]] u32 thread_index() const noexcept { return thread_index_; }
    /// How many entities the buffer has promised to create but not yet created.
    [[nodiscard]] u32 placeholders() const noexcept { return placeholders_; }

private:
    struct Command {
        CommandOp op = CommandOp::CreateEntity;
        DestroyPolicy policy = DestroyPolicy::CascadeChildren;
        ComponentTypeId component = kInvalidComponent;
        Entity entity;
        Entity other;
        u32 payload_offset = 0;
        u32 payload_size = 0;
    };

    [[nodiscard]] Status record(const Command& command, const void* payload, u32 size) noexcept;
    /// A placeholder becomes the entity it stood for; anything else is itself.
    [[nodiscard]] Entity resolve(Entity entity) const noexcept;

    World* world_;
    u32 system_order_;
    u32 thread_index_;
    Array<Command> commands_;
    Array<u8> payloads_;
    /// Placeholder index to the entity created for it, filled during `apply`.
    Array<Entity> resolved_;
    u32 placeholders_ = 0;
};

}  // namespace cy::ecs
