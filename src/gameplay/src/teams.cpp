// Teams, relationships and affiliations. M8.b task 3.1.

#include <cy/gameplay/teams.h>

namespace cy::gameplay {

const char* relationship_name(Relationship relationship) noexcept {
    switch (relationship) {
        case Relationship::Self:
            return "Self";
        case Relationship::Ally:
            return "Ally";
        case Relationship::Neutral:
            return "Neutral";
        case Relationship::Hostile:
            return "Hostile";
        case Relationship::Count:
            break;
    }
    return "Neutral";
}

RelationshipService::RelationshipService(Allocator& allocator,
                                         Relationship default_relationship) noexcept
    : teams_(allocator),
      members_(allocator),
      by_entity_(allocator),
      affiliations_(allocator),
      by_affiliation_(allocator),
      default_(default_relationship) {
    for (auto& row : matrix_) {
        for (auto& cell : row) {
            cell = default_relationship;
        }
    }
}

u32 RelationshipService::slot_of(TeamId team) const noexcept {
    for (usize index = 0; index < teams_.size(); ++index) {
        if (teams_[index].id == team) {
            return static_cast<u32>(index);
        }
    }
    return kMaxTeams;
}

Expected<TeamId, Error> RelationshipService::add_team(Name name) noexcept {
    if (teams_.size() >= kMaxTeams) {
        return make_unexpected(
            Error{ErrorCode::OutOfRange, "more teams than the relationship matrix holds", 0});
    }
    Team team;
    team.id = next_team_++;
    team.name = name;
    if (Status pushed = teams_.push_back(team); !pushed) {
        return make_unexpected(pushed.error());
    }
    return team.id;
}

Name RelationshipService::team_name(TeamId team) const noexcept {
    const u32 slot = slot_of(team);
    return slot < teams_.size() ? teams_[slot].name : Name{};
}

Status RelationshipService::set_relationship(TeamId a, TeamId b, Relationship relationship,
                                             bool symmetric) noexcept {
    const u32 first = slot_of(a);
    const u32 second = slot_of(b);
    if (first >= kMaxTeams || second >= kMaxTeams) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such team", 0});
    }
    matrix_[first][second] = relationship;
    if (symmetric) {
        matrix_[second][first] = relationship;
    }
    return ok();
}

Relationship RelationshipService::between(TeamId a, TeamId b) const noexcept {
    if (a == b && a != kNoTeam) {
        return Relationship::Self;
    }
    const u32 first = slot_of(a);
    const u32 second = slot_of(b);
    if (first >= kMaxTeams || second >= kMaxTeams) {
        // An unknown team is not an enemy. `a != b` is exactly the inference the requirement
        // forbids, and answering Hostile here would smuggle it back in through the error path.
        return default_;
    }
    return matrix_[first][second];
}

Status RelationshipService::set_team(ecs::Entity entity, TeamId team) noexcept {
    if (const u32* slot = by_entity_.find(entity.bits());
        slot != nullptr && *slot < members_.size()) {
        members_[*slot].team = team;
        return ok();
    }
    if (Status pushed = members_.push_back(Membership{entity, team}); !pushed) {
        return pushed;
    }
    if (auto placed = by_entity_.insert(entity.bits(), static_cast<u32>(members_.size() - 1));
        !placed) {
        members_.pop_back();
        return make_unexpected(placed.error());
    }
    return ok();
}

TeamId RelationshipService::team_of(ecs::Entity entity) const noexcept {
    const u32* slot = by_entity_.find(entity.bits());
    return slot != nullptr && *slot < members_.size() ? members_[*slot].team : kNoTeam;
}

Relationship RelationshipService::between(const Participant& participant,
                                          ecs::Entity entity) const noexcept {
    return between(participant.team, team_of(entity));
}

Status RelationshipService::add_affiliation(ecs::Entity entity, Name kind, u32 id) noexcept {
    u32 position = 0;
    if (const u32* slot = by_affiliation_.find(entity.bits());
        slot != nullptr && *slot < affiliations_.size()) {
        position = *slot;
    } else {
        AffiliationRow created;
        created.entity = entity;
        if (Status pushed = affiliations_.push_back(created); !pushed) {
            return pushed;
        }
        position = static_cast<u32>(affiliations_.size() - 1);
        if (auto placed = by_affiliation_.insert(entity.bits(), position); !placed) {
            affiliations_.pop_back();
            return make_unexpected(placed.error());
        }
    }
    AffiliationRow& row = affiliations_[position];
    for (u8 index = 0; index < row.count; ++index) {
        if (row.kinds[index] == kind) {
            row.ids[index] = id;
            return ok();
        }
    }
    if (row.count >= kMaxAffiliationKinds) {
        return make_unexpected(Error{ErrorCode::OutOfRange,
                                     "more affiliation kinds on one entity than a unit holds", 0});
    }
    row.kinds[row.count] = kind;
    row.ids[row.count] = id;
    ++row.count;
    return ok();
}

bool RelationshipService::remove_affiliation(ecs::Entity entity, Name kind) noexcept {
    const u32* slot = by_affiliation_.find(entity.bits());
    if (slot == nullptr || *slot >= affiliations_.size()) {
        return false;
    }
    AffiliationRow& row = affiliations_[*slot];
    for (u8 index = 0; index < row.count; ++index) {
        if (row.kinds[index] != kind) {
            continue;
        }
        row.kinds[index] = row.kinds[row.count - 1];
        row.ids[index] = row.ids[row.count - 1];
        --row.count;
        return true;
    }
    return false;
}

Affiliation RelationshipService::affiliation_of(ecs::Entity entity, Name kind) const noexcept {
    const u32* slot = by_affiliation_.find(entity.bits());
    if (slot == nullptr || *slot >= affiliations_.size()) {
        return Affiliation{kind, 0};
    }
    const AffiliationRow& row = affiliations_[*slot];
    for (u8 index = 0; index < row.count; ++index) {
        if (row.kinds[index] == kind) {
            return Affiliation{kind, row.ids[index]};
        }
    }
    return Affiliation{kind, 0};
}

u32 RelationshipService::affiliations_of(ecs::Entity entity, Affiliation* out,
                                         u32 capacity) const noexcept {
    const u32* slot = by_affiliation_.find(entity.bits());
    if (slot == nullptr || *slot >= affiliations_.size()) {
        return 0;
    }
    const AffiliationRow& row = affiliations_[*slot];
    u32 found = 0;
    for (u8 index = 0; index < row.count; ++index) {
        if (out != nullptr && found < capacity) {
            out[found] = Affiliation{row.kinds[index], row.ids[index]};
        }
        ++found;
    }
    return found;
}

bool RelationshipService::share_affiliation(ecs::Entity a, ecs::Entity b,
                                            Name kind) const noexcept {
    const Affiliation first = affiliation_of(a, kind);
    const Affiliation second = affiliation_of(b, kind);
    return first.id != 0 && first.id == second.id;
}

void RelationshipService::forget(ecs::Entity entity) noexcept {
    if (const u32* slot = by_entity_.find(entity.bits());
        slot != nullptr && *slot < members_.size()) {
        const u32 position = *slot;
        (void)by_entity_.remove(entity.bits());
        const auto last = static_cast<u32>(members_.size() - 1);
        if (position != last) {
            members_[position] = members_[last];
            if (u32* moved = by_entity_.find(members_[position].entity.bits()); moved != nullptr) {
                *moved = position;
            }
        }
        members_.pop_back();
    }
    if (const u32* slot = by_affiliation_.find(entity.bits());
        slot != nullptr && *slot < affiliations_.size()) {
        const u32 position = *slot;
        (void)by_affiliation_.remove(entity.bits());
        const auto last = static_cast<u32>(affiliations_.size() - 1);
        if (position != last) {
            affiliations_[position] = affiliations_[last];
            if (u32* moved = by_affiliation_.find(affiliations_[position].entity.bits());
                moved != nullptr) {
                *moved = position;
            }
        }
        affiliations_.pop_back();
    }
}

}  // namespace cy::gameplay
