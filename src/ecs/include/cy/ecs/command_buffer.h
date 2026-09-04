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
