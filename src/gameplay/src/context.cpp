// Gameplay lifetimes, scoped services and participants. Task 4.4.1.

#include <cy/gameplay/context.h>

namespace cy::gameplay {

const char* scope_name(Scope scope) noexcept {
    switch (scope) {
        case Scope::Application:
            return "Application";
        case Scope::GameInstance:
            return "GameInstance";
        case Scope::Session:
            return "Session";
        case Scope::World:
            return "World";
        case Scope::Player:
            return "Player";
        case Scope::Count:
            break;
    }
    return "Application";
}

const char* participant_kind_name(ParticipantKind kind) noexcept {
    switch (kind) {
        case ParticipantKind::LocalHuman:
            return "LocalHuman";
        case ParticipantKind::RemoteHuman:
            return "RemoteHuman";
        case ParticipantKind::Bot:
            return "Bot";
        case ParticipantKind::RemoteBot:
            return "RemoteBot";
        case ParticipantKind::Spectator:
            return "Spectator";
        case ParticipantKind::ServerAgent:
            return "ServerAgent";
        case ParticipantKind::Count:
            break;
    }
    return "Bot";
}

Status ServiceRegistry::add_erased(Scope scope, Name name, void* service, const void* tag,
                                   ServiceAccess access) noexcept {
    if (service == nullptr) {
        return fail(ErrorCode::InvalidArgument, "gameplay: a service registration needs a service");
    }
    for (const auto& record : services_) {
        if (record.scope == scope && record.name == name) {
            // A duplicate is refused rather than replaced: two services under one name at one scope
            // makes which one a system gets a function of registration order, and the symptom shows
            // up in the system that registered second.
            return fail(ErrorCode::AlreadyExists,
                        "gameplay: a service is already registered under that name at that scope");
        }
    }
    return services_.push_back(Record{scope, name, service, tag, access});
}

void* ServiceRegistry::find_erased(Scope scope, Name name, const void* tag) noexcept {
    for (auto& record : services_) {
        if (record.scope == scope && record.name == name && record.type_tag == tag) {
            return record.service;
        }
    }
    return nullptr;
}

void ServiceRegistry::clear_scope(Scope scope) noexcept {
    usize index = services_.size();
    while (index-- > 0) {
        if (services_[index].scope == scope) {
            services_.erase(index);
        }
    }
}

u32 ServiceRegistry::count_at(Scope scope) const noexcept {
    u32 found = 0;
    for (const auto& service : services_) {
        found += service.scope == scope ? 1U : 0U;
    }
    return found;
}

GameSession::GameSession(Allocator& allocator, u64 seed) noexcept
    : seed_(seed), participants_(allocator), services_(allocator) {}

Expected<ParticipantId, Error> GameSession::add_participant(ParticipantKind kind, Name name,
                                                            u32 team, u32 local_player) noexcept {
    Participant participant;
    participant.id =
        ParticipantId::from_slot(static_cast<u32>(participants_.size()), next_participant_++);
    participant.kind = kind;
    participant.name = name;
    participant.team = team;
    // A remote participant has no local player, and that is structural rather than a convention:
    // `gameplay-framework` — "Remote participants SHALL NOT require any local resource".
    participant.local_player = kind == ParticipantKind::LocalHuman ? local_player : kNoLocalPlayer;
    if (Status pushed = participants_.push_back(participant); !pushed) {
        return make_unexpected(pushed.error());
    }
    return participant.id;
}

void GameSession::remove_participant(ParticipantId participant) noexcept {
    for (usize index = 0; index < participants_.size(); ++index) {
        if (participants_[index].id == participant) {
            participants_.erase(index);
            return;
        }
    }
}

const Participant* GameSession::participant(ParticipantId id) const noexcept {
    if (id.is_null()) {
        return nullptr;
    }
    for (const auto& participant : participants_) {
        if (participant.id == id) {
            return &participant;
        }
    }
    return nullptr;
}

Expected<WorldSessionId, Error> GameSession::add_world(ecs::World* world, Name role) noexcept {
    if (world_count_ == kMaxWorlds) {
        return fail(ErrorCode::OutOfRange, "gameplay: the session already holds kMaxWorlds worlds");
    }
    WorldSession& session = worlds_[world_count_];
    session.id = WorldSessionId::from_slot(world_count_, next_world_++);
    session.world = world;
    session.role = role;
    ++world_count_;
    return session.id;
}

void GameSession::remove_world(WorldSessionId id) noexcept {
    for (u32 index = 0; index < world_count_; ++index) {
        if (worlds_[index].id != id) {
            continue;
        }
        for (u32 shift = index + 1; shift < world_count_; ++shift) {
            worlds_[shift - 1] = worlds_[shift];
        }
        --world_count_;
        // NOTHING ELSE HAPPENS. Participants are untouched, the seed is untouched, session-scoped
        // services are untouched. "Changing world SHALL NOT end the session", and the way to make
        // that true is for this function to have nothing else to do.
        return;
    }
}

const WorldSession* GameSession::world_session(WorldSessionId id) const noexcept {
    for (u32 index = 0; index < world_count_; ++index) {
        if (worlds_[index].id == id) {
            return &worlds_[index];
        }
    }
    return nullptr;
}

}  // namespace cy::gameplay
