#include <cy/networking/authority.h>

namespace cy::net {

const char* topology_name(Topology topology) noexcept {
    switch (topology) {
        case Topology::DedicatedServer:
            return "DedicatedServer";
        case Topology::ListenServer:
            return "ListenServer";
        case Topology::PeerToPeerDistributed:
            return "PeerToPeerDistributed";
    }
    return "unknown";
}

const char* write_verdict_name(WriteVerdict verdict) noexcept {
    switch (verdict) {
        case WriteVerdict::Authoritative:
            return "Authoritative";
        case WriteVerdict::LocalOnly:
            return "LocalOnly";
        case WriteVerdict::Unknown:
            return "Unknown";
    }
    return "unknown";
}

PeerId authority_for_spawn(Topology topology, PeerId server, PeerId spawner) noexcept {
    switch (topology) {
        case Topology::DedicatedServer:
        case Topology::ListenServer:
            return server;
        case Topology::PeerToPeerDistributed:
            return spawner;
    }
    return server;
}

AuthorityRecord* AuthorityRegistry::locate(NetworkId id) noexcept {
    for (auto& record : records_) {
        if (record.id == id) {
            return &record;
        }
    }
    return nullptr;
}

const AuthorityRecord* AuthorityRegistry::find(NetworkId id) const noexcept {
    for (const auto& record : records_) {
        if (record.id == id) {
            return &record;
        }
    }
    return nullptr;
}

Status AuthorityRegistry::track(NetworkId id, PeerId owner, ecs::Entity local) noexcept {
    if (!id.valid()) {
        return fail(ErrorCode::InvalidArgument, "networking: the null network id is never minted");
    }
    if (!owner.valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "networking: a replicated entity has an owner from the moment it exists");
    }
    if (locate(id) != nullptr) {
        return fail(ErrorCode::AlreadyExists,
                    "networking: that network id is already tracked; two records for one id is the "
                    "two-authorities failure in local form");
    }
    AuthorityRecord record;
    record.id = id;
    record.owner = owner;
    record.local = local;
    return records_.push_back(record);
}

Status AuthorityRegistry::forget(NetworkId id) noexcept {
    for (usize index = 0; index < records_.size(); ++index) {
        if (records_[index].id == id) {
            records_.remove_unordered(index);
            return ok();
        }
    }
    return fail(ErrorCode::NotFound, "networking: no such network id");
}

PeerId AuthorityRegistry::authority_of(NetworkId id) const noexcept {
    const AuthorityRecord* record = find(id);
    return record == nullptr ? PeerId{} : record->owner;
}

bool AuthorityRegistry::is_authority(NetworkId id) const noexcept {
    return authority_of(id) == self_ && authority_of(id).valid();
}

WriteVerdict AuthorityRegistry::classify_write(NetworkId id, PeerId peer) noexcept {
    const AuthorityRecord* record = find(id);
    if (record == nullptr) {
        return WriteVerdict::Unknown;
    }
    if (record->owner == peer) {
        return WriteVerdict::Authoritative;
    }
    ++local_only_writes_;
    return WriteVerdict::LocalOnly;
}

Expected<u32, Error> AuthorityRegistry::begin_handover(NetworkId id, PeerId to,
                                                       u64 expires_at_tick) noexcept {
    AuthorityRecord* record = locate(id);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "networking: no such network id");
    }
    if (record->state == AuthorityState::HandoverPending) {
        return fail(ErrorCode::Unavailable,
                    "networking: a handover is already pending for that entity; a second one would "
                    "put two peers in the incoming position");
    }
    if (!to.valid() || to == record->owner) {
        return fail(ErrorCode::InvalidArgument,
                    "networking: a handover goes to a different, valid peer");
    }
    record->state = AuthorityState::HandoverPending;
    record->incoming = to;
    record->handover = next_handover_++;
    record->expires_at_tick = expires_at_tick;
    return record->handover;
}

Status AuthorityRegistry::acknowledge_handover(NetworkId id, u32 handover) noexcept {
    AuthorityRecord* record = locate(id);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "networking: no such network id");
    }
    if (record->state != AuthorityState::HandoverPending) {
        return fail(ErrorCode::Unavailable, "networking: no handover is pending for that entity");
    }
    if (record->handover != handover) {
        return fail(ErrorCode::InvalidArgument,
                    "networking: that acknowledgement is for a different handover; a stale one "
                    "would hand the entity to a peer that has stopped expecting it");
    }
    // The one moment authority moves. Before it the old owner is authoritative; after it the new
    // one is; there is no instant in which both or neither are.
    record->owner = record->incoming;
    record->incoming = PeerId{};
    record->state = AuthorityState::Stable;
    record->handover = 0;
    record->expires_at_tick = 0;
    ++completed_;
    return ok();
}

u32 AuthorityRegistry::expire_handovers(u64 tick) noexcept {
    u32 expired = 0;
    for (auto& record : records_) {
        if (record.state != AuthorityState::HandoverPending || tick < record.expires_at_tick) {
            continue;
        }
        record.state = AuthorityState::Stable;
        record.incoming = PeerId{};
        record.handover = 0;
        record.expires_at_tick = 0;
        ++expired;
        ++expired_;
    }
    return expired;
}

u32 AuthorityRegistry::self_declared_authorities(NetworkId id) const noexcept {
    const AuthorityRecord* record = find(id);
    if (record == nullptr) {
        return 0;
    }
    // One, by construction: `owner` is the only field any peer consults, and a pending handover
    // does not change it. The function exists so that the claim is measured rather than reasoned
    // about, and so that a future change which made `incoming` authoritative early fails a test.
    u32 count = record->owner.valid() ? 1U : 0U;
    if (record->state == AuthorityState::HandoverPending && record->incoming == record->owner) {
        ++count;
    }
    return count;
}

}  // namespace cy::net
