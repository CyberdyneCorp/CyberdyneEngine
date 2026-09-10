#pragma once
// Teams, relationships and affiliations. M8.b task 3.1.
//
// `gameplay-framework` — "Teams and affiliations": teams are first-class with membership as data;
// relationships are **explicit** — self, ally, neutral, hostile — and "SHALL NOT be derived from
// identifier inequality"; relationships are changeable at runtime; affiliations beyond teams
// (faction, squad, party, project-defined) let a unit belong to several groupings at once; and
// "Targeting, artificial intelligence, interface, and interest management SHALL consult **one**
// relationship service rather than each implementing its own rule."
//
// ================================================================================================
// THE DEFAULT IS NEUTRAL, AND THAT IS THE REQUIREMENT
// ================================================================================================
//
// `a != b` is the implementation the requirement forbids by name, and it is forbidden because it is
// *nearly* right: it produces the correct answer for a two-team deathmatch and the wrong one for
// every alliance, truce and betrayal after that. So an undeclared pair here is **Neutral**, not
// Hostile: a project that wants free-for-all says so once by setting the default, and a project
// that forgets gets units that ignore each other rather than units that shoot their allies.
//
// `set_relationship` is symmetric by default and asymmetric on request. Asymmetry is not exotic —
// a wildlife faction hostile to everything that is neutral towards it is the ordinary shape — and a
// service that could only express symmetry would push the exception back into the four consumers
// the requirement exists to keep in agreement.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/core/values/name.h>
#include <cy/ecs/entity.h>
#include <cy/gameplay/context.h>

namespace cy::gameplay {

/// A team's stable identifier. Zero is "no team", so a zeroed record is teamless rather than a
/// member of team zero.
using TeamId = u32;
inline constexpr TeamId kNoTeam = 0;

/// How two teams stand to one another. **Read, never inferred.**
enum class Relationship : u8 {
    Self = 0,
    Ally,
    Neutral,
    Hostile,
    Count,
};

const char* relationship_name(Relationship relationship) noexcept;

/// A grouping that is not a team: faction, squad, party, or a project's own kind.
struct Affiliation {
    /// `faction`, `squad`, `party`, or whatever the project interns. A `Name` so that adding a kind
    /// changes no engine enumeration.
    Name kind;
    u32 id = 0;
};

/// The one relationship service. Targeting, AI, the interface and interest management all consult
/// this; none of them holds a rule of its own.
class RelationshipService {
public:
    static constexpr u32 kMaxTeams = 64;
    /// A unit holds a team, a faction, a squad and a few project kinds — not a list. The bound is
    /// what lets an entity's affiliations be one row and its removal one lookup.
    static constexpr u32 kMaxAffiliationKinds = 8;

    /// `default_relationship` is what an undeclared pair of distinct teams is. Neutral unless the
    /// project says otherwise — see the header comment.
    explicit RelationshipService(
        Allocator& allocator, Relationship default_relationship = Relationship::Neutral) noexcept;

    RelationshipService(const RelationshipService&) = delete;
    RelationshipService& operator=(const RelationshipService&) = delete;

    [[nodiscard]] Expected<TeamId, Error> add_team(Name name) noexcept;
    [[nodiscard]] Name team_name(TeamId team) const noexcept;
    [[nodiscard]] u32 team_count() const noexcept { return static_cast<u32>(teams_.size()); }
    [[nodiscard]] TeamId team_at(u32 index) const noexcept { return teams_[index].id; }

    /// Declare how `a` stands to `b`. Symmetric unless `symmetric` is false.
    [[nodiscard]] Status set_relationship(TeamId a, TeamId b, Relationship relationship,
                                          bool symmetric = true) noexcept;

    /// How `a` stands to `b`. `Self` when they are the same team; otherwise the declared value, or
    /// the default. **Never `a != b`.**
    [[nodiscard]] Relationship between(TeamId a, TeamId b) const noexcept;

    /// Membership as data: an entity's team, and the reverse question.
    [[nodiscard]] Status set_team(ecs::Entity entity, TeamId team) noexcept;
    [[nodiscard]] TeamId team_of(ecs::Entity entity) const noexcept;

    /// A participant's team. Participants carry their own field on `Participant`; this is the
    /// bridge, so that "how does this participant stand to that entity" is one call.
    [[nodiscard]] Relationship between(const Participant& participant,
                                       ecs::Entity entity) const noexcept;

    [[nodiscard]] Status add_affiliation(ecs::Entity entity, Name kind, u32 id) noexcept;
    bool remove_affiliation(ecs::Entity entity, Name kind) noexcept;
    /// The entity's affiliation of that kind, or `id == 0`. A unit may hold several kinds at once,
    /// which is what "a team, a faction, and a squad simultaneously" means.
    [[nodiscard]] Affiliation affiliation_of(ecs::Entity entity, Name kind) const noexcept;
    [[nodiscard]] u32 affiliations_of(ecs::Entity entity, Affiliation* out,
                                      u32 capacity) const noexcept;
    /// Do two entities share an affiliation of this kind? The squad-mate question.
    [[nodiscard]] bool share_affiliation(ecs::Entity a, ecs::Entity b, Name kind) const noexcept;

    void forget(ecs::Entity entity) noexcept;

private:
    struct Team {
        TeamId id = kNoTeam;
        Name name;
    };
    struct Membership {
        ecs::Entity entity;
        TeamId team = kNoTeam;
    };
    /// One entity's affiliations, all of them, in one row. A row rather than a row per (entity,
    /// kind) so that forgetting an entity is one lookup: a hundred thousand entities forgotten one
    /// at a time over a table keyed by pairs is quadratic, and streaming does exactly that.
    struct AffiliationRow {
        ecs::Entity entity;
        Name kinds[kMaxAffiliationKinds];
        u32 ids[kMaxAffiliationKinds] = {};
        u8 count = 0;
    };

    [[nodiscard]] u32 slot_of(TeamId team) const noexcept;

    Array<Team> teams_;
    Array<Membership> members_;
    /// Entity -> its membership row, and entity -> its affiliation row. Both hashed, for the
    /// reason `gameplay-framework`'s performance contract gives: a hundred thousand active
    /// entities, and a scan per assignment would make populating a world quadratic.
    HashMap<u64, u32> by_entity_;
    Array<AffiliationRow> affiliations_;
    HashMap<u64, u32> by_affiliation_;
    /// A dense matrix over team slots, so `between()` is one indexed read. `kMaxTeams` is a
    /// declared bound rather than an assumption: a project with more teams than this is a project
    /// whose relationships are not a matrix, and it should say so rather than pay N squared.
    Relationship matrix_[kMaxTeams][kMaxTeams] = {};
    Relationship default_ = Relationship::Neutral;
    TeamId next_team_ = 1;
};

}  // namespace cy::gameplay
