// Deferred structural change, and the world's flush. Task 2.6.

#include <cy/ecs/command_buffer.h>

#include <cy/ecs/world.h>

#include <cstring>

namespace cy::ecs {

const char* command_op_name(CommandOp op) noexcept {
    switch (op) {
        case CommandOp::CreateEntity:
            return "create-entity";
        case CommandOp::DestroyEntity:
            return "destroy-entity";
        case CommandOp::AddComponent:
            return "add-component";
        case CommandOp::RemoveComponent:
            return "remove-component";
        case CommandOp::SetComponent:
            return "set-component";
        case CommandOp::AddChild:
            return "add-child";
    }
    return "unknown";
}

CommandBuffer::CommandBuffer(World& world, u32 system_order, u32 thread_index) noexcept
    : world_(&world),
      system_order_(system_order),
      thread_index_(thread_index),
      commands_(world.allocator()),
      payloads_(world.allocator()),
      resolved_(world.allocator()) {}

CommandBuffer::~CommandBuffer() {
    world_->detach(*this);
}

Status CommandBuffer::record(const Command& command, const void* payload, u32 size) noexcept {
    Command stored = command;
    if (size != 0 && payload != nullptr) {
        stored.payload_offset = static_cast<u32>(payloads_.size());
        stored.payload_size = size;
        const auto* bytes = static_cast<const u8*>(payload);
        if (Status appended = payloads_.append(Span<const u8>(bytes, size)); !appended) {
            return appended;
        }
    }
    return commands_.push_back(stored);
}

Expected<Entity, Error> CommandBuffer::create() noexcept {
    const Entity placeholder = Entity::make(placeholders_, kPlaceholderGeneration);
    Command command;
    command.op = CommandOp::CreateEntity;
    command.entity = placeholder;
    if (Status recorded = record(command, nullptr, 0); !recorded) {
        return make_unexpected(recorded.error());
    }
    ++placeholders_;
    return placeholder;
}

Status CommandBuffer::destroy(Entity entity, DestroyPolicy policy) noexcept {
    Command command;
    command.op = CommandOp::DestroyEntity;
    command.entity = entity;
    command.policy = policy;
    return record(command, nullptr, 0);
}

Status CommandBuffer::add(Entity entity, ComponentTypeId component, const void* value,
                          u32 size) noexcept {
    Command command;
    command.op = CommandOp::AddComponent;
    command.entity = entity;
    command.component = component;
    return record(command, value, size);
}

Status CommandBuffer::remove(Entity entity, ComponentTypeId component) noexcept {
    Command command;
    command.op = CommandOp::RemoveComponent;
    command.entity = entity;
    command.component = component;
    return record(command, nullptr, 0);
}

Status CommandBuffer::set(Entity entity, ComponentTypeId component, const void* value,
                          u32 size) noexcept {
    if (value == nullptr || size == 0) {
        return fail(ErrorCode::InvalidArgument, "set() needs a value");
    }
    Command command;
    command.op = CommandOp::SetComponent;
    command.entity = entity;
    command.component = component;
    return record(command, value, size);
}

Status CommandBuffer::add_child(Entity parent, Entity child) noexcept {
    Command command;
    command.op = CommandOp::AddChild;
    command.entity = child;
    command.other = parent;
    return record(command, nullptr, 0);
}

Entity CommandBuffer::resolve(Entity entity) const noexcept {
    if (!entity.placeholder()) {
        return entity;
    }
    const u32 index = entity.index();
    return (index < resolved_.size()) ? resolved_[index] : kNoEntity;
}

Expected<u64, Error> CommandBuffer::apply() noexcept {
    if (commands_.empty()) {
        return u64{0};
    }
    resolved_.clear();
    if (Status resized = resolved_.resize(placeholders_); !resized) {
        return make_unexpected(resized.error());
    }

    u64 applied = 0;
    for (const Command& command : commands_) {
        const Entity entity = resolve(command.entity);
        const u8* payload =
            (command.payload_size == 0) ? nullptr : (payloads_.data() + command.payload_offset);

        Status outcome = ok();
        switch (command.op) {
            case CommandOp::CreateEntity: {
                Expected<Entity, Error> created = world_->create();
                if (!created) {
                    return make_unexpected(created.error());
                }
                resolved_[command.entity.index()] = *created;
                break;
            }
            case CommandOp::DestroyEntity:
                outcome = world_->destroy(entity, command.policy);
                break;
            case CommandOp::AddComponent:
                outcome = world_->add(entity, command.component, static_cast<const void*>(payload));
                break;
            case CommandOp::RemoveComponent:
                outcome = world_->remove(entity, command.component);
                break;
            case CommandOp::SetComponent: {
                void* slot = world_->get_mut(entity, command.component);
                if (payload == nullptr) {
                    // `set()` refuses an empty value, so this is unreachable through the public
                    // interface; it is here because a record with no payload must not reach memcpy.
                    outcome = fail(ErrorCode::InvalidArgument, "a set command carries no value");
                } else if (slot == nullptr) {
                    outcome = fail(ErrorCode::NotFound,
                                   "set() names a component the entity does not have at flush");
                } else {
                    const u32 size = world_->components().info(command.component).size;
                    std::memcpy(slot, static_cast<const void*>(payload),
                                (size < command.payload_size) ? size : command.payload_size);
                }
                break;
            }
            case CommandOp::AddChild:
                outcome = world_->set_parent(entity, resolve(command.other));
                break;
        }
        if (!outcome) {
            return make_unexpected(outcome.error());
        }
        ++applied;
    }

    clear();
    return applied;
}

void CommandBuffer::clear() noexcept {
    commands_.clear();
    payloads_.clear();
    resolved_.clear();
    placeholders_ = 0;
}

Status CommandBuffer::set_merge_key(u32 system_order, u32 thread_index) noexcept {
    if (!commands_.empty()) {
        return fail(ErrorCode::Unavailable,
                    "the merge key decides where this buffer's commands land in the flush; it "
                    "cannot be changed while it holds any. Flush or clear first.");
    }
    system_order_ = system_order;
    thread_index_ = thread_index;
    return ok();
}

// --- The world's half ---------------------------------------------------------------------------

Status World::attach(CommandBuffer& buffer) noexcept {
    for (const CommandBuffer* existing : command_buffers_) {
        if (existing == &buffer) {
            return ok();
        }
    }
    return command_buffers_.push_back(&buffer);
}

void World::detach(CommandBuffer& buffer) noexcept {
    for (usize index = 0; index < command_buffers_.size(); ++index) {
        if (command_buffers_[index] == &buffer) {
            // erase(), not remove_unordered(): attachment order is the tie-break between two
            // buffers with the same (system, thread) key, so it has to survive a detach.
            command_buffers_.erase(index);
            return;
        }
    }
}

Expected<u64, Error> World::flush() noexcept {
    if (iterating()) {
        refused_.fetch_add(1, std::memory_order_relaxed);
        return fail(ErrorCode::Unavailable,
                    "a flush is a structural change; it cannot run inside "
                    "iteration");
    }

    // The merge order: system, then thread, then attachment. Selection over a small list rather
    // than a sort, because the list is a handful of buffers and a stable order stated as a loop is
    // easier to be sure of than a comparator.
    Array<CommandBuffer*> order(*allocator_);
    if (Status reserved = order.reserve(command_buffers_.size()); !reserved) {
        return make_unexpected(reserved.error());
    }
    Array<bool> taken(*allocator_);
    if (Status resized = taken.resize(command_buffers_.size()); !resized) {
        return make_unexpected(resized.error());
    }
    for (usize step = 0; step < command_buffers_.size(); ++step) {
        usize best = command_buffers_.size();
        for (usize index = 0; index < command_buffers_.size(); ++index) {
            if (taken[index]) {
                continue;
            }
            if (best == command_buffers_.size()) {
                best = index;
                continue;
            }
            const CommandBuffer& candidate = *command_buffers_[index];
            const CommandBuffer& incumbent = *command_buffers_[best];
            if (candidate.system_order() < incumbent.system_order() ||
                (candidate.system_order() == incumbent.system_order() &&
                 candidate.thread_index() < incumbent.thread_index())) {
                best = index;
            }
        }
        taken[best] = true;
        if (Status pushed = order.push_back(command_buffers_[best]); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    u64 applied = 0;
    for (CommandBuffer* buffer : order) {
        Expected<u64, Error> count = buffer->apply();
        if (!count) {
            return count;
        }
        applied += *count;
    }
    return applied;
}

}  // namespace cy::ecs
