#pragma once
// Network identity and the authority model. M9 task 4.2.
//
// ================================================================================================
// EXACTLY ONE AUTHORITY AT ANY TIME, AND THE HANDOVER IS WHERE THAT IS EASY TO LOSE
// ================================================================================================
//
// `networking-and-replication` — "Authority model" and "Authority migration seams", which say the
// same thing twice and mean it: "the protocol SHALL ensure exactly one peer considers itself
// authoritative at any time, with the transition acknowledged".
//
// The obvious implementation — set the owner, tell the new owner — has a window in which both peers
// believe they own the entity, or neither does, depending on which message arrives first. So a
// transfer here is **two-phase and pessimistic**: the current owner stays authoritative for the
// whole of the pending phase, and the new owner becomes authoritative only when the acknowledgement
// lands. A transfer that is never acknowledged expires back to the original owner rather than
// leaving the entity ownerless. `tests/test_authority.cpp` walks the window tick by tick and
// asserts the count of self-declared authorities is one at every point — including the point where
// the acknowledgement is lost.
//
// ================================================================================================
// NETWORK IDS ARE SESSION-GLOBAL, WHICH IS A SEAM RATHER THAN A CONVENIENCE
// ================================================================================================
//
// "Network entity ids SHALL be globally unique across a session rather than server-local", so that
// distributed simulation across server processes stays possible without reworking identity. A
// `NetworkId` therefore carries the **minter** — which peer created it — beside a counter, so two
// peers spawning at the same moment cannot collide, and an entity that migrates keeps the id it was
// born with. Distributed simulation is not implemented; the specification asks for it to be
// recorded as deferred rather than assumed impossible, and this is the shape that keeps it open.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/entity.h>
#include <cy/networking/transport.h>

namespace cy::net {

/// A replicated entity's identity, stable across peers and distinct from the local `ecs::Entity`.
///
/// Sixteen bits of minter and forty-eight of counter: 65 536 peers and 281 trillion spawns, which
/// is past any session, and the split is what makes the id unique without a central allocator.
class NetworkId {
public:
    constexpr NetworkId() noexcept = default;

    [[nodiscard]] static constexpr NetworkId make(u16 minter, u64 counter) noexcept {
        NetworkId id;
        id.value_ = (static_cast<u64>(minter) << 48) | (counter & 0x0000'FFFF'FFFF'FFFFULL);
        return id;
    }

    [[nodiscard]] constexpr u64 value() const noexcept { return value_; }
    [[nodiscard]] constexpr u16 minter() const noexcept { return static_cast<u16>(value_ >> 48); }
    [[nodiscard]] constexpr u64 counter() const noexcept {
        return value_ & 0x0000'FFFF'FFFF'FFFFULL;
    }
    /// Zero is the null id and is never minted, so a default-constructed one is invalid rather than
    /// being peer zero's first entity.
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

    friend constexpr bool operator==(NetworkId, NetworkId) noexcept = default;

private:
    u64 value_ = 0;
};

/// Mints ids for one peer. One per peer, never shared.
class NetworkIdMinter {
public:
    constexpr explicit NetworkIdMinter(u16 minter) noexcept : minter_(minter) {}

    [[nodiscard]] constexpr NetworkId mint() noexcept { return NetworkId::make(minter_, ++next_); }
    [[nodiscard]] constexpr u64 minted() const noexcept { return next_; }

private:
    u16 minter_;
    u64 next_ = 0;
};

/// `networking-and-replication`'s three topologies.
enum class Topology : u8 {
    /// The server has authority over all gameplay entities.
    DedicatedServer = 0,
    /// One peer is both host and player.
    ListenServer,
    /// Each peer owns its entities.
    PeerToPeerDistributed,
};

const char* topology_name(Topology topology) noexcept;

/// Where a transfer has got to.
enum class AuthorityState : u8 {
    Stable = 0,
    /// The current owner is still authoritative. See the header comment.
    HandoverPending,
};

/// What one replicated entity's authority is.
struct AuthorityRecord {
    NetworkId id;
    /// The peer that may change authoritative state **now**, pending transfer or not.
    PeerId owner;
    /// The peer a pending transfer is to. Invalid when `state` is `Stable`.
    PeerId incoming;
    /// The local entity this id names in this process, if any.
    ecs::Entity local;
    AuthorityState state = AuthorityState::Stable;
    /// The transfer's own identity, so a stale acknowledgement of a previous transfer does not
    /// complete the current one.
    u32 handover = 0;
    /// The tick a pending handover expires at.
    u64 expires_at_tick = 0;
};

/// What happened when a peer tried to write authoritative state.
enum class WriteVerdict : u8 {
    /// The peer owns the entity; the write is authoritative.
    Authoritative = 0,
    /// The peer does not. `networking-and-replication`: the change "SHALL be local-only and
    /// overwritten by the next authoritative update, and in development builds a diagnostic SHALL
    /// warn". It is not an error, because prediction is exactly this write.
    LocalOnly,
    /// No such entity.
    Unknown,
};

const char* write_verdict_name(WriteVerdict verdict) noexcept;

/// Every replicated entity this process knows about, and who owns each.
class AuthorityRegistry {
public:
    AuthorityRegistry(Allocator& allocator, Topology topology, PeerId self) noexcept
        : records_(allocator), topology_(topology), self_(self) {}

    AuthorityRegistry(const AuthorityRegistry&) = delete;
    AuthorityRegistry& operator=(const AuthorityRegistry&) = delete;

    /// Record a replicated entity and its owner. Refuses a duplicate id, because two records for
    /// one id is exactly the "two peers consider themselves authoritative" failure in local form.
    [[nodiscard]] Status track(NetworkId id, PeerId owner, ecs::Entity local) noexcept;
    [[nodiscard]] Status forget(NetworkId id) noexcept;

    [[nodiscard]] const AuthorityRecord* find(NetworkId id) const noexcept;
    [[nodiscard]] PeerId authority_of(NetworkId id) const noexcept;
    [[nodiscard]] bool is_authority(NetworkId id) const noexcept;

    /// The verdict on a write by `peer`. Counted, so a development build can report how often a
    /// client wrote state it does not own without printing a line per write.
    [[nodiscard]] WriteVerdict classify_write(NetworkId id, PeerId peer) noexcept;
    [[nodiscard]] u64 local_only_writes() const noexcept { return local_only_writes_; }

    // --- The handover protocol ---------------------------------------------------------------

    /// Begin transferring `id` to `to`, expiring at `expires_at_tick`. Returns the handover token
    /// the acknowledgement must carry. Refuses a transfer from a peer that is not the owner, and a
    /// second transfer while one is pending.
    [[nodiscard]] Expected<u32, Error> begin_handover(NetworkId id, PeerId to,
                                                      u64 expires_at_tick) noexcept;

    /// Complete a transfer. Refuses a token that is not the pending one — a stale acknowledgement
    /// of a transfer that already expired would otherwise hand the entity to the wrong peer.
    [[nodiscard]] Status acknowledge_handover(NetworkId id, u32 handover) noexcept;

    /// Expire every pending handover whose deadline has passed, back to its original owner.
    /// Returns how many. An entity with no owner is not a state this registry can reach.
    [[nodiscard]] u32 expire_handovers(u64 tick) noexcept;

    /// **The invariant, as a number.** How many peers consider themselves authoritative over `id`.
    /// Always exactly one for a tracked entity, pending transfer or not, and the test asserts it
    /// rather than trusting the paragraph above.
    [[nodiscard]] u32 self_declared_authorities(NetworkId id) const noexcept;

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(records_.size()); }
    [[nodiscard]] const AuthorityRecord& at(u32 index) const noexcept { return records_[index]; }
    [[nodiscard]] Topology topology() const noexcept { return topology_; }
    [[nodiscard]] PeerId self() const noexcept { return self_; }
    [[nodiscard]] u64 handovers_completed() const noexcept { return completed_; }
    [[nodiscard]] u64 handovers_expired() const noexcept { return expired_; }

private:
    [[nodiscard]] AuthorityRecord* locate(NetworkId id) noexcept;

    Array<AuthorityRecord> records_;
    Topology topology_;
    PeerId self_;
    u32 next_handover_ = 1;
    u64 local_only_writes_ = 0;
    u64 completed_ = 0;
    u64 expired_ = 0;
};

/// Who owns an entity spawned by `spawner` under `topology`. A function rather than a rule in a
/// comment, because "the server owns gameplay entities" is a different sentence in each topology.
[[nodiscard]] PeerId authority_for_spawn(Topology topology, PeerId server, PeerId spawner) noexcept;

}  // namespace cy::net
