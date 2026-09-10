// Session state fragments. M8.b task 3.1.

#include <cy/gameplay/fragments.h>

#include <utility>

namespace cy::gameplay {

const char* fragment_authority_name(FragmentAuthority authority) noexcept {
    switch (authority) {
        case FragmentAuthority::Server:
            return "Server";
        case FragmentAuthority::Client:
            return "Client";
        case FragmentAuthority::Local:
            return "Local";
        case FragmentAuthority::Deterministic:
            return "Deterministic";
        case FragmentAuthority::Count:
            break;
    }
    return "Server";
}

const char* fragment_visibility_name(FragmentVisibility visibility) noexcept {
    switch (visibility) {
        case FragmentVisibility::Everyone:
            return "Everyone";
        case FragmentVisibility::Owner:
            return "Owner";
        case FragmentVisibility::Team:
            return "Team";
        case FragmentVisibility::AuthorityOnly:
            return "AuthorityOnly";
        case FragmentVisibility::LocalOnly:
            return "LocalOnly";
        case FragmentVisibility::Count:
            break;
    }
    return "Everyone";
}

const char* persistence_class_name(PersistenceClass persistence) noexcept {
    switch (persistence) {
        case PersistenceClass::SessionTransient:
            return "SessionTransient";
        case PersistenceClass::WorldPersistent:
            return "WorldPersistent";
        case PersistenceClass::ProfilePersistent:
            return "ProfilePersistent";
        case PersistenceClass::SaveGame:
            return "SaveGame";
        case PersistenceClass::Derived:
            return "Derived";
        case PersistenceClass::Count:
            break;
    }
    return "SessionTransient";
}

FragmentStore::FragmentStore(Allocator& allocator) noexcept
    : allocator_(&allocator),
      declarations_(allocator),
      offsets_(allocator),
      session_bytes_(allocator),
      players_(allocator) {}

Expected<FragmentId, Error> FragmentStore::register_fragment(
    const FragmentDeclaration& declaration) noexcept {
    if (declaration.size == 0) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a fragment declares its size in bytes", 0});
    }
    if (declaration.stable_id == 0) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a fragment's stable identity is never zero", 0});
    }
    if (find(declaration.stable_id) != kInvalidFragment) {
        return make_unexpected(
            Error{ErrorCode::AlreadyExists, "that fragment identity is registered", 0});
    }
    const bool player = declaration.scope == FragmentScope::Player;
    const u32 offset = player ? player_stride_ : static_cast<u32>(session_bytes_.size());
    if (Status pushed = declarations_.push_back(declaration); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (Status pushed = offsets_.push_back(offset); !pushed) {
        declarations_.pop_back();
        return make_unexpected(pushed.error());
    }
    if (player) {
        player_stride_ = offset + declaration.size;
        for (PlayerRow& row : players_) {
            if (Status grown = row.bytes.resize(player_stride_); !grown) {
                return make_unexpected(grown.error());
            }
        }
    } else if (Status grown = session_bytes_.resize(offset + declaration.size); !grown) {
        return make_unexpected(grown.error());
    }
    return static_cast<FragmentId>(declarations_.size() - 1);
}

Status FragmentStore::add_player(ParticipantId participant) noexcept {
    if (player_row(participant) != nullptr) {
        return ok();
    }
    PlayerRow row{participant, Array<u8>(*allocator_)};
    if (Status grown = row.bytes.resize(player_stride_); !grown) {
        return grown;
    }
    return players_.push_back(std::move(row));
}

void FragmentStore::remove_player(ParticipantId participant) noexcept {
    for (usize index = 0; index < players_.size(); ++index) {
        if (players_[index].participant == participant) {
            players_.erase(index);
            return;
        }
    }
}

FragmentId FragmentStore::find(u32 stable_id) const noexcept {
    for (usize index = 0; index < declarations_.size(); ++index) {
        if (declarations_[index].stable_id == stable_id) {
            return static_cast<FragmentId>(index);
        }
    }
    return kInvalidFragment;
}

const FragmentStore::PlayerRow* FragmentStore::player_row(
    ParticipantId participant) const noexcept {
    for (const PlayerRow& row : players_) {
        if (row.participant == participant) {
            return &row;
        }
    }
    return nullptr;
}

FragmentStore::PlayerRow* FragmentStore::player_row(ParticipantId participant) noexcept {
    for (PlayerRow& row : players_) {
        if (row.participant == participant) {
            return &row;
        }
    }
    return nullptr;
}

u8* FragmentStore::slice(FragmentId id, ParticipantId participant, u16 size) noexcept {
    if (id >= declarations_.size() || declarations_[id].size != size) {
        return nullptr;
    }
    const u32 offset = offsets_[id];
    if (declarations_[id].scope == FragmentScope::Player) {
        PlayerRow* row = player_row(participant);
        if (row == nullptr || offset + size > row->bytes.size()) {
            return nullptr;
        }
        return row->bytes.data() + offset;
    }
    if (offset + size > session_bytes_.size()) {
        return nullptr;
    }
    return session_bytes_.data() + offset;
}

const u8* FragmentStore::slice(FragmentId id, ParticipantId participant, u16 size) const noexcept {
    if (id >= declarations_.size() || declarations_[id].size != size) {
        return nullptr;
    }
    const u32 offset = offsets_[id];
    if (declarations_[id].scope == FragmentScope::Player) {
        const PlayerRow* row = player_row(participant);
        if (row == nullptr || offset + size > row->bytes.size()) {
            return nullptr;
        }
        return row->bytes.data() + offset;
    }
    if (offset + size > session_bytes_.size()) {
        return nullptr;
    }
    return session_bytes_.data() + offset;
}

Status FragmentStore::write(FragmentId id, const void* bytes, u16 size,
                            ParticipantId participant) noexcept {
    u8* target = slice(id, participant, size);
    if (target == nullptr || bytes == nullptr) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "no fragment of that identity and size", 0});
    }
    std::memcpy(static_cast<void*>(target), bytes, size);
    return ok();
}

Status FragmentStore::read(FragmentId id, void* bytes, u16 size,
                           ParticipantId participant) const noexcept {
    const u8* source = slice(id, participant, size);
    if (source == nullptr || bytes == nullptr) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "no fragment of that identity and size", 0});
    }
    std::memcpy(bytes, static_cast<const void*>(source), size);
    return ok();
}

bool FragmentStore::replicated(FragmentId id) const noexcept {
    if (id >= declarations_.size()) {
        return false;
    }
    const FragmentDeclaration& declaration = declarations_[id];
    if (declaration.visibility == FragmentVisibility::LocalOnly ||
        declaration.authority == FragmentAuthority::Local) {
        return false;
    }
    // Deterministic state is computed identically on every peer from the command stream. Sending
    // it would be sending what the receiver already has, and disagreeing with it would be a desync
    // the wire quietly papered over.
    return declaration.authority != FragmentAuthority::Deterministic &&
           declaration.persistence != PersistenceClass::Derived;
}

bool FragmentStore::saved(FragmentId id) const noexcept {
    if (id >= declarations_.size()) {
        return false;
    }
    switch (declarations_[id].persistence) {
        case PersistenceClass::WorldPersistent:
        case PersistenceClass::ProfilePersistent:
        case PersistenceClass::SaveGame:
            return true;
        case PersistenceClass::SessionTransient:
        case PersistenceClass::Derived:
        case PersistenceClass::Count:
            break;
    }
    return false;
}

bool FragmentStore::replay_relevant(FragmentId id) const noexcept {
    if (id >= declarations_.size()) {
        return false;
    }
    const FragmentDeclaration& declaration = declarations_[id];
    // Derived state is recomputed and deterministic state is reproduced from the commands; neither
    // belongs in a recording. Everything else has to be carried, because a replay that could not
    // restore it would start from a state the original run was not in.
    return declaration.persistence != PersistenceClass::Derived &&
           declaration.authority != FragmentAuthority::Deterministic;
}

bool FragmentStore::observable_by(FragmentId id, ObserverRelation relation) const noexcept {
    if (id >= declarations_.size()) {
        return false;
    }
    switch (declarations_[id].visibility) {
        case FragmentVisibility::Everyone:
            return true;
        case FragmentVisibility::Owner:
            return relation == ObserverRelation::Owner || relation == ObserverRelation::Authority;
        case FragmentVisibility::Team:
            return relation == ObserverRelation::Owner || relation == ObserverRelation::Teammate ||
                   relation == ObserverRelation::Authority;
        case FragmentVisibility::AuthorityOnly:
            return relation == ObserverRelation::Authority;
        case FragmentVisibility::LocalOnly:
            return relation == ObserverRelation::Local;
        case FragmentVisibility::Count:
            break;
    }
    return false;
}

}  // namespace cy::gameplay
