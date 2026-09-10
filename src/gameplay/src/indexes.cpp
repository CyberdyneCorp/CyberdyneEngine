// Derived gameplay indexes. M8.b task 3.3.

#include <cy/gameplay/indexes.h>

#include <utility>

namespace cy::gameplay {
namespace {

constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;

[[nodiscard]] u64 mix(u64 hash, u64 value) noexcept {
    for (u32 byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8U)) & 0xFFULL;
        hash *= kFnvPrime;
    }
    return hash;
}

}  // namespace

GameplayIndexes::GameplayIndexes(Allocator& allocator) noexcept
    : allocator_(&allocator),
      buckets_(allocator),
      by_bucket_(allocator),
      rows_(allocator),
      by_entity_(allocator) {}

u64 GameplayIndexes::bucket_key(Axis axis, u64 key, Name kind) noexcept {
    u64 hash = mix(kFnvOffset, static_cast<u64>(axis));
    hash = mix(hash, kind.index());
    return mix(hash, key);
}

GameplayIndexes::Bucket* GameplayIndexes::find_bucket(Axis axis, u64 key, Name kind) noexcept {
    const u32* slot = by_bucket_.find(bucket_key(axis, key, kind));
    return slot != nullptr && *slot < buckets_.size() ? &buckets_[*slot] : nullptr;
}

const GameplayIndexes::Bucket* GameplayIndexes::find_bucket(Axis axis, u64 key,
                                                            Name kind) const noexcept {
    const u32* slot = by_bucket_.find(bucket_key(axis, key, kind));
    return slot != nullptr && *slot < buckets_.size() ? &buckets_[*slot] : nullptr;
}

Expected<GameplayIndexes::Bucket*, Error> GameplayIndexes::ensure_bucket(Axis axis, u64 key,
                                                                         Name kind) noexcept {
    if (Bucket* found = find_bucket(axis, key, kind); found != nullptr) {
        return found;
    }
    Bucket bucket;
    bucket.axis = axis;
    bucket.key = key;
    bucket.kind = kind;
    bucket.members = Array<ecs::Entity>(*allocator_);
    bucket.positions = HashMap<u64, u32>(*allocator_);
    if (Status pushed = buckets_.push_back(std::move(bucket)); !pushed) {
        return make_unexpected(pushed.error());
    }
    auto placed =
        by_bucket_.insert(bucket_key(axis, key, kind), static_cast<u32>(buckets_.size() - 1));
    if (!placed) {
        buckets_.pop_back();
        return make_unexpected(placed.error());
    }
    return &buckets_[buckets_.size() - 1];
}

Status GameplayIndexes::insert(Axis axis, u64 key, Name kind, ecs::Entity entity) noexcept {
    auto bucket = ensure_bucket(axis, key, kind);
    if (!bucket) {
        return make_unexpected(bucket.error());
    }
    Bucket& target = *bucket.value();
    if (target.positions.contains(entity.bits())) {
        return ok();
    }
    if (Status pushed = target.members.push_back(entity); !pushed) {
        return pushed;
    }
    auto placed =
        target.positions.insert(entity.bits(), static_cast<u32>(target.members.size() - 1));
    if (!placed) {
        target.members.pop_back();
        return make_unexpected(placed.error());
    }
    return ok();
}

void GameplayIndexes::drop_from(Bucket& bucket, ecs::Entity entity) noexcept {
    const u32* slot = bucket.positions.find(entity.bits());
    if (slot == nullptr || *slot >= bucket.members.size()) {
        return;
    }
    // Swap-remove and repoint. A bucket is a set, and `digest()` folds it order-independently for
    // exactly this reason.
    const u32 position = *slot;
    (void)bucket.positions.remove(entity.bits());
    const auto last = static_cast<u32>(bucket.members.size() - 1);
    if (position != last) {
        bucket.members[position] = bucket.members[last];
        if (u32* moved = bucket.positions.find(bucket.members[position].bits()); moved != nullptr) {
            *moved = position;
        }
    }
    bucket.members.pop_back();
}

void GameplayIndexes::erase(Axis axis, u64 key, Name kind, ecs::Entity entity) noexcept {
    if (Bucket* bucket = find_bucket(axis, key, kind); bucket != nullptr) {
        drop_from(*bucket, entity);
    }
}

GameplayIndexes::Row* GameplayIndexes::row_of(ecs::Entity entity) noexcept {
    if (const u32* slot = by_entity_.find(entity.bits()); slot != nullptr && *slot < rows_.size()) {
        return &rows_[*slot];
    }
    if (Status pushed = rows_.push_back(Row{entity, ParticipantId{}, kNoTeam}); !pushed) {
        return nullptr;
    }
    if (auto placed = by_entity_.insert(entity.bits(), static_cast<u32>(rows_.size() - 1));
        !placed) {
        rows_.pop_back();
        return nullptr;
    }
    return &rows_[rows_.size() - 1];
}

Status GameplayIndexes::on_owner_changed(ecs::Entity entity, ParticipantId owner) noexcept {
    Row* row = row_of(entity);
    if (row == nullptr) {
        return make_unexpected(Error{ErrorCode::OutOfMemory, "the index could not grow", 0});
    }
    if (!row->owner.is_null()) {
        erase(Axis::Owner, row->owner.bits(), Name{}, entity);
    }
    row->owner = owner;
    if (owner.is_null()) {
        return ok();
    }
    return insert(Axis::Owner, owner.bits(), Name{}, entity);
}

Status GameplayIndexes::on_team_changed(ecs::Entity entity, TeamId team) noexcept {
    Row* row = row_of(entity);
    if (row == nullptr) {
        return make_unexpected(Error{ErrorCode::OutOfMemory, "the index could not grow", 0});
    }
    if (row->team != kNoTeam) {
        erase(Axis::Team, row->team, Name{}, entity);
    }
    row->team = team;
    if (team == kNoTeam) {
        return ok();
    }
    return insert(Axis::Team, team, Name{}, entity);
}

Status GameplayIndexes::on_affiliation_changed(ecs::Entity entity, Name kind, u32 id) noexcept {
    // An affiliation's previous value is held by the relationship service, so the index drops the
    // entity from every bucket of this kind rather than remembering a second copy of the truth.
    // Each drop is a lookup, not a walk of the members.
    for (Bucket& bucket : buckets_) {
        if (bucket.axis != Axis::Affiliation || bucket.kind != kind) {
            continue;
        }
        drop_from(bucket, entity);
    }
    if (id == 0) {
        return ok();
    }
    return insert(Axis::Affiliation, id, kind, entity);
}

Status GameplayIndexes::on_tag_added(ecs::Entity entity, TagId tag) noexcept {
    return insert(Axis::Tag, tag, Name{}, entity);
}

void GameplayIndexes::on_tag_removed(ecs::Entity entity, TagId tag) noexcept {
    erase(Axis::Tag, tag, Name{}, entity);
}

void GameplayIndexes::on_entity_removed(ecs::Entity entity) noexcept {
    for (Bucket& bucket : buckets_) {
        drop_from(bucket, entity);
    }
    const u32* slot = by_entity_.find(entity.bits());
    if (slot == nullptr || *slot >= rows_.size()) {
        return;
    }
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

u32 GameplayIndexes::read(Axis axis, u64 key, Name kind, ecs::Entity* out,
                          u32 capacity) const noexcept {
    const Bucket* bucket = find_bucket(axis, key, kind);
    if (bucket == nullptr) {
        return 0;
    }
    u32 found = 0;
    for (const ecs::Entity member : bucket->members) {
        ++scanned_;
        if (out != nullptr && found < capacity) {
            out[found] = member;
        }
        ++found;
    }
    return found;
}

u32 GameplayIndexes::owned_by(ParticipantId owner, ecs::Entity* out, u32 capacity) const noexcept {
    return read(Axis::Owner, owner.bits(), Name{}, out, capacity);
}

u32 GameplayIndexes::on_team(TeamId team, ecs::Entity* out, u32 capacity) const noexcept {
    return read(Axis::Team, team, Name{}, out, capacity);
}

u32 GameplayIndexes::affiliated(Name kind, u32 id, ecs::Entity* out, u32 capacity) const noexcept {
    return read(Axis::Affiliation, id, kind, out, capacity);
}

u32 GameplayIndexes::tagged(TagId tag, ecs::Entity* out, u32 capacity) const noexcept {
    return read(Axis::Tag, tag, Name{}, out, capacity);
}

u32 GameplayIndexes::tagged_matching(const TagRegistry& registry, TagId query, ecs::Entity* out,
                                     u32 capacity) const noexcept {
    u32 found = 0;
    for (const Bucket& bucket : buckets_) {
        if (bucket.axis != Axis::Tag || !registry.matches(query, static_cast<TagId>(bucket.key))) {
            continue;
        }
        for (const ecs::Entity member : bucket.members) {
            ++scanned_;
            if (out != nullptr && found < capacity) {
                out[found] = member;
            }
            ++found;
        }
    }
    return found;
}

Status GameplayIndexes::rebuild(const OwnershipRegistry& ownership,
                                const RelationshipService& relationships,
                                const EntityTagStore& tags) noexcept {
    buckets_.clear();
    by_bucket_.clear();
    rows_.clear();
    by_entity_.clear();
    for (u32 index = 0; index < ownership.count(); ++index) {
        const ecs::Entity entity = ownership.entity_at(index);
        const Owner owner = ownership.resolve_owner(entity);
        if (owner.kind == OwnerKind::Participant) {
            if (Status added = on_owner_changed(entity, owner.participant); !added) {
                return added;
            }
        }
        const TeamId team = relationships.team_of(entity);
        if (team != kNoTeam) {
            if (Status added = on_team_changed(entity, team); !added) {
                return added;
            }
        }
        Affiliation affiliations[8] = {};
        const u32 count = relationships.affiliations_of(entity, affiliations, 8);
        for (u32 slot = 0; slot < count && slot < 8; ++slot) {
            if (Status added =
                    on_affiliation_changed(entity, affiliations[slot].kind, affiliations[slot].id);
                !added) {
                return added;
            }
        }
    }
    for (u32 index = 0; index < tags.entity_count(); ++index) {
        const ecs::Entity entity = tags.entity_at(index);
        const TagSet* set = tags.tags_of(entity);
        if (set == nullptr) {
            continue;
        }
        for (const TagId tag : set->span()) {
            if (Status added = on_tag_added(entity, tag); !added) {
                return added;
            }
        }
    }
    return ok();
}

u64 GameplayIndexes::digest() const noexcept {
    // CANONICAL ORDER, NOT INSERTION ORDER. An incremental index and a rebuilt one hold the same
    // buckets in different positions, so the digest sorts by (axis, kind, key) and each bucket's
    // members by their bits before folding. Without that, the requirement's own scenario would fail
    // on a structure that is in fact identical.
    Array<u32> order(*allocator_);
    for (u32 index = 0; index < buckets_.size(); ++index) {
        if (!order.push_back(index).has_value()) {
            return 0;
        }
    }
    for (usize outer = 1; outer < order.size(); ++outer) {
        const u32 value = order[outer];
        usize slot = outer;
        while (slot > 0) {
            const Bucket& a = buckets_[order[slot - 1]];
            const Bucket& b = buckets_[value];
            const bool greater = (a.axis > b.axis) ||
                                 (a.axis == b.axis && a.kind.index() > b.kind.index()) ||
                                 (a.axis == b.axis && a.kind == b.kind && a.key > b.key);
            if (!greater) {
                break;
            }
            order[slot] = order[slot - 1];
            --slot;
        }
        order[slot] = value;
    }

    u64 hash = kFnvOffset;
    for (const u32 index : order) {
        const Bucket& bucket = buckets_[index];
        if (bucket.members.empty()) {
            continue;
        }
        hash = mix(hash, static_cast<u64>(bucket.axis));
        hash = mix(hash, bucket.kind.index());
        hash = mix(hash, bucket.key);
        u64 members = kFnvOffset;
        // Order-independent over the members: a sum of per-member hashes. A bucket is a set, and
        // an incremental index may have added its members in any order.
        for (const ecs::Entity member : bucket.members) {
            members += mix(kFnvOffset, member.bits());
        }
        hash = mix(hash, members);
        hash = mix(hash, bucket.members.size());
    }
    return hash;
}

}  // namespace cy::gameplay
