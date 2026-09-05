// Control sources, channels, bindings and groups. Task 4.4.2.

#include <cy/gameplay/control.h>

#include <utility>

namespace cy::gameplay {

const char* control_source_kind_name(ControlSourceKind kind) noexcept {
    switch (kind) {
        case ControlSourceKind::Human:
            return "Human";
        case ControlSourceKind::ArtificialIntelligence:
            return "ArtificialIntelligence";
        case ControlSourceKind::RemotePeer:
            return "RemotePeer";
        case ControlSourceKind::Replay:
            return "Replay";
        case ControlSourceKind::Script:
            return "Script";
        case ControlSourceKind::Automation:
            return "Automation";
        case ControlSourceKind::Count:
            break;
    }
    return "Human";
}

namespace channels {

Name primary() noexcept {
    return CY_NAME("primary");
}
Name movement() noexcept {
    return CY_NAME("movement");
}
Name weapons() noexcept {
    return CY_NAME("weapons");
}
Name camera() noexcept {
    return CY_NAME("camera");
}
Name turret() noexcept {
    return CY_NAME("turret");
}
Name command() noexcept {
    return CY_NAME("command");
}

}  // namespace channels

ControlRegistry::ControlRegistry(Allocator& allocator) noexcept
    : allocator_(&allocator), sources_(allocator), bindings_(allocator), groups_(allocator) {}

Expected<ControlSourceId, Error> ControlRegistry::create_source(ControlSourceKind kind,
                                                                ParticipantId participant,
                                                                Name debug_name) noexcept {
    ControlSourceRecord record;
    record.id = ControlSourceId::from_slot(static_cast<u32>(sources_.size()), next_source_++);
    record.kind = kind;
    record.participant = participant;
    record.debug_name = debug_name;
    if (Status pushed = sources_.push_back(record); !pushed) {
        return make_unexpected(pushed.error());
    }
    return record.id;
}

void ControlRegistry::destroy_source(ControlSourceId source_id) noexcept {
    usize index = bindings_.size();
    while (index-- > 0) {
        if (bindings_[index].source == source_id) {
            bindings_.erase(index);
        }
    }
    for (usize slot = 0; slot < sources_.size(); ++slot) {
        if (sources_[slot].id == source_id) {
            sources_.erase(slot);
            return;
        }
    }
}

const ControlSourceRecord* ControlRegistry::source(ControlSourceId id) const noexcept {
    if (id.is_null()) {
        return nullptr;
    }
    for (const auto& source : sources_) {
        if (source.id == id) {
            return &source;
        }
    }
    return nullptr;
}

Expected<GroupId, Error> ControlRegistry::create_group(Name debug_name) noexcept {
    if (groups_.size() == kMaxGroups) {
        return fail(ErrorCode::OutOfRange, "gameplay: no room for another entity group");
    }
    Group group{GroupId::from_slot(static_cast<u32>(groups_.size()), next_group_++), debug_name,
                Array<ecs::Entity>(*allocator_)};
    const GroupId id = group.id;
    if (Status pushed = groups_.push_back(std::move(group)); !pushed) {
        return make_unexpected(pushed.error());
    }
    return id;
}

ControlRegistry::Group* ControlRegistry::find_group(GroupId group) noexcept {
    if (group.is_null()) {
        return nullptr;
    }
    for (auto& index : groups_) {
        if (index.id == group) {
            return &index;
        }
    }
    return nullptr;
}

const ControlRegistry::Group* ControlRegistry::find_group(GroupId group) const noexcept {
    if (group.is_null()) {
        return nullptr;
    }
    for (const auto& index : groups_) {
        if (index.id == group) {
            return &index;
        }
    }
    return nullptr;
}

Status ControlRegistry::add_to_group(GroupId group, ecs::Entity entity) noexcept {
    Group* record = find_group(group);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "gameplay: no such entity group");
    }
    for (auto member : record->members) {
        if (member == entity) {
            return ok();
        }
    }
    return record->members.push_back(entity);
}

void ControlRegistry::remove_from_group(GroupId group, ecs::Entity entity) noexcept {
    Group* record = find_group(group);
    if (record == nullptr) {
        return;
    }
    for (usize index = 0; index < record->members.size(); ++index) {
        if (record->members[index] == entity) {
            record->members.erase(index);
            return;
        }
    }
}

u32 ControlRegistry::group_size(GroupId group) const noexcept {
    const Group* record = find_group(group);
    return record == nullptr ? 0U : static_cast<u32>(record->members.size());
}

ecs::Entity ControlRegistry::group_member(GroupId group, u32 index) const noexcept {
    const Group* record = find_group(group);
    if (record == nullptr || index >= record->members.size()) {
        return ecs::Entity{};
    }
    return record->members[index];
}

Status ControlRegistry::bind_entity(ControlSourceId source_id, Name channel,
                                    ecs::Entity entity) noexcept {
    if (source(source_id) == nullptr) {
        return fail(ErrorCode::NotFound, "gameplay: no such control source");
    }
    ControlBinding binding;
    binding.source = source_id;
    binding.channel = channel;
    binding.entity = entity;
    return bindings_.push_back(binding);
}

Status ControlRegistry::bind_group(ControlSourceId source_id, Name channel,
                                   GroupId group) noexcept {
    if (find_group(group) == nullptr) {
        return fail(ErrorCode::NotFound, "gameplay: no such entity group");
    }
    ControlBinding binding;
    binding.source = source_id;
    binding.channel = channel;
    binding.group = group;
    // ONE binding, however many members. `gameplay-framework`'s forbidden-patterns list names
    // "Representing a large controlled group as one control relationship per entity", and the
    // reason is not tidiness: two hundred relationships is two hundred rows to keep consistent
    // every time the selection changes.
    return bindings_.push_back(binding);
}

void ControlRegistry::unbind(ControlSourceId source_id, Name channel) noexcept {
    usize index = bindings_.size();
    while (index-- > 0) {
        if (bindings_[index].source == source_id && bindings_[index].channel == channel) {
            bindings_.erase(index);
        }
    }
}

bool ControlRegistry::controls(ControlSourceId source_id, ecs::Entity entity,
                               Name channel) const noexcept {
    for (const auto& binding : bindings_) {
        if (binding.source != source_id || binding.channel != channel) {
            continue;
        }
        if (!binding.is_group()) {
            if (binding.entity == entity) {
                return true;
            }
            continue;
        }
        const Group* group = find_group(binding.group);
        if (group == nullptr) {
            continue;
        }
        for (auto member : group->members) {
            if (member == entity) {
                return true;
            }
        }
    }
    return false;
}

u32 ControlRegistry::sources_controlling(ecs::Entity entity, ControlSourceId* out,
                                         u32 capacity) const noexcept {
    u32 found = 0;
    for (const auto& binding : bindings_) {
        bool matches = false;
        if (!binding.is_group()) {
            matches = binding.entity == entity;
        } else if (const Group* group = find_group(binding.group); group != nullptr) {
            for (auto member : group->members) {
                matches = matches || member == entity;
            }
        }
        if (!matches) {
            continue;
        }
        if (found < capacity && out != nullptr) {
            out[found] = binding.source;
        }
        ++found;
    }
    return found;
}

u32 ControlRegistry::controlled_entities(ControlSourceId source_id, Name channel, ecs::Entity* out,
                                         u32 capacity) const noexcept {
    u32 found = 0;
    for (const auto& binding : bindings_) {
        if (binding.source != source_id || binding.channel != channel) {
            continue;
        }
        if (!binding.is_group()) {
            if (found < capacity && out != nullptr) {
                out[found] = binding.entity;
            }
            ++found;
            continue;
        }
        const Group* group = find_group(binding.group);
        if (group == nullptr) {
            continue;
        }
        for (auto member : group->members) {
            if (found < capacity && out != nullptr) {
                out[found] = member;
            }
            ++found;
        }
    }
    return found;
}

}  // namespace cy::gameplay
