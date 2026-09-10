// Ownership, control and network authority are three things. M8.b task 3.2.

#include <cy/gameplay/ownership.h>

namespace cy::gameplay {

const char* owner_kind_name(OwnerKind kind) noexcept {
    switch (kind) {
        case OwnerKind::None:
            return "None";
        case OwnerKind::Participant:
            return "Participant";
        case OwnerKind::Team:
            return "Team";
        case OwnerKind::Inherited:
            return "Inherited";
        case OwnerKind::Count:
            break;
    }
    return "None";
}

const char* network_authority_name(NetworkAuthority authority) noexcept {
    switch (authority) {
        case NetworkAuthority::Server:
            return "Server";
        case NetworkAuthority::Client:
            return "Client";
        case NetworkAuthority::Local:
            return "Local";
        case NetworkAuthority::Deterministic:
            return "Deterministic";
        case NetworkAuthority::Count:
            break;
    }
    return "Server";
}

OwnershipRegistry::OwnershipRegistry(Allocator& allocator) noexcept
    : rows_(allocator), by_entity_(allocator) {}

OwnershipRegistry::Row* OwnershipRegistry::find(ecs::Entity entity) noexcept {
    const u32* slot = by_entity_.find(entity.bits());
    return slot != nullptr && *slot < rows_.size() ? &rows_[*slot] : nullptr;
}

const OwnershipRegistry::Row* OwnershipRegistry::find(ecs::Entity entity) const noexcept {
    const u32* slot = by_entity_.find(entity.bits());
    return slot != nullptr && *slot < rows_.size() ? &rows_[*slot] : nullptr;
}

Expected<OwnershipRegistry::Row*, Error> OwnershipRegistry::ensure(ecs::Entity entity) noexcept {
    if (Row* found = find(entity); found != nullptr) {
        return found;
    }
    Row added;
    added.entity = entity;
    if (Status pushed = rows_.push_back(added); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (auto placed = by_entity_.insert(entity.bits(), static_cast<u32>(rows_.size() - 1));
        !placed) {
        rows_.pop_back();
        return make_unexpected(placed.error());
    }
    return &rows_[rows_.size() - 1];
}

Status OwnershipRegistry::set_owner(ecs::Entity entity, const Owner& owner) noexcept {
    auto row = ensure(entity);
    if (!row) {
        return make_unexpected(row.error());
    }
    row.value()->owner = owner;
    return ok();
}

Owner OwnershipRegistry::owner(ecs::Entity entity) const noexcept {
    const Row* row = find(entity);
    return row != nullptr ? row->owner : Owner{};
}

Owner OwnershipRegistry::resolve_owner(ecs::Entity entity) const noexcept {
    const Row* row = find(entity);
    u32 steps = 0;
    while (row != nullptr && row->owner.kind == OwnerKind::Inherited && steps < kMaxDepth) {
        if (!row->parent.valid()) {
            return Owner{};
        }
        row = find(row->parent);
        ++steps;
    }
    if (row == nullptr || row->owner.kind == OwnerKind::Inherited) {
        // A cycle, or a hierarchy deeper than the bound. Unowned is the honest answer; inventing
        // one would make a mis-authored hierarchy look like a correctly owned one.
        return Owner{};
    }
    return row->owner;
}

Status OwnershipRegistry::set_parent(ecs::Entity child, ecs::Entity parent) noexcept {
    auto row = ensure(child);
    if (!row) {
        return make_unexpected(row.error());
    }
    row.value()->parent = parent;
    return ok();
}

ecs::Entity OwnershipRegistry::parent(ecs::Entity entity) const noexcept {
    const Row* row = find(entity);
    return row != nullptr ? row->parent : ecs::Entity{};
}

Status OwnershipRegistry::set_authority(ecs::Entity entity, NetworkAuthority authority) noexcept {
    auto row = ensure(entity);
    if (!row) {
        return make_unexpected(row.error());
    }
    row.value()->authority = authority;
    return ok();
}

NetworkAuthority OwnershipRegistry::authority(ecs::Entity entity) const noexcept {
    const Row* row = find(entity);
    return row != nullptr ? row->authority : NetworkAuthority::Server;
}

u32 OwnershipRegistry::owned_by(ParticipantId participant, ecs::Entity* out,
                                u32 capacity) const noexcept {
    u32 found = 0;
    for (const Row& row : rows_) {
        const Owner resolved = resolve_owner(row.entity);
        if (resolved.kind != OwnerKind::Participant || resolved.participant != participant) {
            continue;
        }
        if (out != nullptr && found < capacity) {
            out[found] = row.entity;
        }
        ++found;
    }
    return found;
}

void OwnershipRegistry::forget(ecs::Entity entity) noexcept {
    const u32* slot = by_entity_.find(entity.bits());
    if (slot == nullptr || *slot >= rows_.size()) {
        return;
    }
    // Swap-remove and repoint. Row order is not meaningful — `owned_by` reports a set — and an
    // ordered erase would move every index the table holds.
    const u32 position = *slot;
    (void)by_entity_.remove(entity.bits());
    const auto last = static_cast<u32>(rows_.size() - 1);
    if (position != last) {
        rows_[position] = rows_[last];
        if (u32* moved = by_entity_.find(rows_[position].entity.bits()); moved != nullptr) {
            *moved = position;
        }
    }
    rows_.pop_back();
}

}  // namespace cy::gameplay
