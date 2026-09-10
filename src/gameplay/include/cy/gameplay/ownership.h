#pragma once
// Ownership, control and network authority are three things. M8.b task 3.2.
//
// `gameplay-framework` — "Ownership, control, and authority are three things":
//
//   | Ownership          | Whose it is — a participant or a team                       |
//   | Control            | Who is driving it right now                                 |
//   | Network authority  | Which peer may change its authoritative state               |
//
// "These SHALL be independently assignable, and **no one of them SHALL be inferred from another**."
//
// ================================================================================================
// WHY THIS FILE HOLDS TWO OF THE THREE AND NOT THE THIRD
// ================================================================================================
//
// Control is `control.h`'s `ControlRegistry`, and it is a different shape from the other two: it is
// many-to-many and channelled, while ownership and authority are one value per entity. Keeping the
// three in one table would have made a captured turret — owned by the enemy faction, driven by a
// player, authoritative on the server — expressible only as a special case, and the requirement's
// own scenario is exactly that turret.
//
// So: three storages, three setters, no derivation between them, and `tests/test_ownership.cpp`
// asserts the turret. There is deliberately no `owner_of_controller()` and no
// `authority_follows_owner()`; the day one of those exists, the three have become one again.
//
// ================================================================================================
// INHERITANCE IS A DECLARATION, NOT A COPY
// ================================================================================================
//
// "Ownership SHALL be inheritable through entity hierarchies by declaration, so a robot's parts
// resolve their owner from the root rather than duplicating it."
//
// A part therefore stores `OwnerKind::Inherited` and a parent link; `resolve_owner()` walks to the
// first ancestor that declares one. Changing the root's owner changes every part's answer and
// writes nothing to any part — which is what makes "its parts SHALL resolve the new owner without
// each storing a copy" checkable: the test reads a part's STORED owner after the change and finds
// it still `Inherited`.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/ecs/entity.h>
#include <cy/gameplay/context.h>
#include <cy/gameplay/teams.h>

namespace cy::gameplay {

/// Whose an entity is.
enum class OwnerKind : u8 {
    /// Nobody's — scenery, a projectile with no attribution.
    None = 0,
    Participant,
    Team,
    /// The parent's answer. The declaration, not a copy — see the header comment.
    Inherited,
    Count,
};

const char* owner_kind_name(OwnerKind kind) noexcept;

struct Owner {
    OwnerKind kind = OwnerKind::None;
    ParticipantId participant;
    TeamId team = kNoTeam;

    [[nodiscard]] static Owner of_participant(ParticipantId id) noexcept {
        return Owner{OwnerKind::Participant, id, kNoTeam};
    }
    [[nodiscard]] static Owner of_team(TeamId team) noexcept {
        return Owner{OwnerKind::Team, ParticipantId{}, team};
    }
    [[nodiscard]] static Owner inherited() noexcept {
        return Owner{OwnerKind::Inherited, ParticipantId{}, kNoTeam};
    }

    friend bool operator==(const Owner& a, const Owner& b) noexcept {
        return a.kind == b.kind && a.participant == b.participant && a.team == b.team;
    }
};

/// Which peer may change an entity's authoritative state. **Not derived from ownership**: a client
/// owns its character and the server is authoritative over it, which is the ordinary case rather
/// than the exception.
enum class NetworkAuthority : u8 {
    /// The server decides. The default, because a default of "whoever asks" is a defect.
    Server = 0,
    /// This client decides — a cosmetic entity, a local-only prop.
    Client,
    /// Local to this process; never transmitted.
    Local,
    /// Every peer computes it identically from the command stream. Lockstep.
    Deterministic,
    Count,
};

const char* network_authority_name(NetworkAuthority authority) noexcept;

/// Ownership and authority, per entity, independently assignable.
class OwnershipRegistry {
public:
    /// A hierarchy deeper than this is not a robot's parts. The bound makes `resolve_owner()`'s
    /// walk terminate on a cycle rather than hang, which matters because the parent links are
    /// authored data.
    static constexpr u32 kMaxDepth = 32;

    explicit OwnershipRegistry(Allocator& allocator) noexcept;

    OwnershipRegistry(const OwnershipRegistry&) = delete;
    OwnershipRegistry& operator=(const OwnershipRegistry&) = delete;

    [[nodiscard]] Status set_owner(ecs::Entity entity, const Owner& owner) noexcept;
    /// What the entity itself stores — `Inherited` for a part. The question a save asks.
    [[nodiscard]] Owner owner(ecs::Entity entity) const noexcept;
    /// What the entity's owner actually is, following `Inherited` up the hierarchy. The question
    /// targeting, the interface and validation ask.
    [[nodiscard]] Owner resolve_owner(ecs::Entity entity) const noexcept;

    /// Declare the hierarchy ownership is inherited through. A null parent detaches.
    [[nodiscard]] Status set_parent(ecs::Entity child, ecs::Entity parent) noexcept;
    [[nodiscard]] ecs::Entity parent(ecs::Entity entity) const noexcept;

    [[nodiscard]] Status set_authority(ecs::Entity entity, NetworkAuthority authority) noexcept;
    [[nodiscard]] NetworkAuthority authority(ecs::Entity entity) const noexcept;

    /// Every entity a participant owns, resolving inheritance. Linear here on purpose: the indexed
    /// answer is `GameplayIndexes`, and an index that shares its storage with the authority would
    /// not be a cache. See indexes.h.
    [[nodiscard]] u32 owned_by(ParticipantId participant, ecs::Entity* out,
                               u32 capacity) const noexcept;

    void forget(ecs::Entity entity) noexcept;

    [[nodiscard]] u32 count() const noexcept { return static_cast<u32>(rows_.size()); }
    [[nodiscard]] ecs::Entity entity_at(u32 index) const noexcept { return rows_[index].entity; }

private:
    struct Row {
        ecs::Entity entity;
        Owner owner;
        ecs::Entity parent;
        NetworkAuthority authority = NetworkAuthority::Server;
    };

    [[nodiscard]] Row* find(ecs::Entity entity) noexcept;
    [[nodiscard]] const Row* find(ecs::Entity entity) const noexcept;
    [[nodiscard]] Expected<Row*, Error> ensure(ecs::Entity entity) noexcept;

    Array<Row> rows_;
    /// Entity -> its row. Hashed for the reason the other framework tables are: a hundred thousand
    /// active entities is an architectural target, and a scan per assignment is quadratic in it.
    HashMap<u64, u32> by_entity_;
};

}  // namespace cy::gameplay
