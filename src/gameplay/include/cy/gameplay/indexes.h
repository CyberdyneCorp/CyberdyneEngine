#pragma once
// Derived gameplay indexes. M8.b task 3.3.
//
// `gameplay-framework` — "Gameplay indexes": the engine maintains derived indexes "for the queries
// gameplay makes constantly: entities by owner, by team, by affiliation, and by gameplay tag.
// These SHALL be maintained incrementally from ECS state and SHALL be **caches, not authoritative
// storage**: rebuilding them from the world SHALL produce the same result. Answering 'what does
// this participant own' SHALL NOT require scanning every entity."
//
// ================================================================================================
// A CACHE IS ONLY A CACHE IF DISCARDING IT CHANGES NOTHING, AND THAT IS MEASURED HERE
// ================================================================================================
//
// `digest()` is a value identity over everything the index holds. `rebuild()` throws the whole
// thing away and derives it again from the registries. The requirement's own scenario — "WHEN an
// index is discarded and rebuilt THEN the result SHALL be identical to the incrementally
// maintained one" — is therefore one assertion: the digest before equals the digest after. Without
// a digest it is an assertion about two structures nobody compares.
//
// ================================================================================================
// AND "NOT A SCAN" IS A NUMBER
// ================================================================================================
//
// `entities_scanned()` counts the entities a lookup touched. An indexed answer touches the members
// of one bucket; a scan touches the world. A test over a hundred thousand entities asserts the
// count, which is the only form in which "SHALL NOT require scanning every entity" is checkable —
// a comment saying it is indexed survives the refactor that stops indexing it.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/ecs/entity.h>
#include <cy/gameplay/ownership.h>
#include <cy/gameplay/tags.h>
#include <cy/gameplay/teams.h>

namespace cy::gameplay {

/// Entities by owner, by team, by affiliation and by tag. **A cache.** The authority is
/// `OwnershipRegistry`, `RelationshipService` and `EntityTagStore`.
class GameplayIndexes {
public:
    explicit GameplayIndexes(Allocator& allocator) noexcept;

    GameplayIndexes(const GameplayIndexes&) = delete;
    GameplayIndexes& operator=(const GameplayIndexes&) = delete;

    // --- Incremental maintenance. A host calls these where it changes the authority. -------------

    [[nodiscard]] Status on_owner_changed(ecs::Entity entity, ParticipantId owner) noexcept;
    [[nodiscard]] Status on_team_changed(ecs::Entity entity, TeamId team) noexcept;
    [[nodiscard]] Status on_affiliation_changed(ecs::Entity entity, Name kind, u32 id) noexcept;
    [[nodiscard]] Status on_tag_added(ecs::Entity entity, TagId tag) noexcept;
    void on_tag_removed(ecs::Entity entity, TagId tag) noexcept;
    void on_entity_removed(ecs::Entity entity) noexcept;

    // --- The queries. Each touches one bucket. ---------------------------------------------------

    [[nodiscard]] u32 owned_by(ParticipantId owner, ecs::Entity* out, u32 capacity) const noexcept;
    [[nodiscard]] u32 on_team(TeamId team, ecs::Entity* out, u32 capacity) const noexcept;
    [[nodiscard]] u32 affiliated(Name kind, u32 id, ecs::Entity* out, u32 capacity) const noexcept;
    /// Entities carrying `tag` **exactly**. The hierarchical answer is `tagged_matching`, which
    /// costs one bucket per matching declared tag rather than one entity per world.
    [[nodiscard]] u32 tagged(TagId tag, ecs::Entity* out, u32 capacity) const noexcept;
    [[nodiscard]] u32 tagged_matching(const TagRegistry& registry, TagId query, ecs::Entity* out,
                                      u32 capacity) const noexcept;

    /// Throw the index away and derive it again from the authorities. The cache claim, executed.
    [[nodiscard]] Status rebuild(const OwnershipRegistry& ownership,
                                 const RelationshipService& relationships,
                                 const EntityTagStore& tags) noexcept;

    /// A value identity over every bucket and its members, in a canonical order. Two indexes that
    /// hold the same thing produce the same number whatever order they were built in.
    [[nodiscard]] u64 digest() const noexcept;

    /// Entities touched by the lookups since `reset_counters()`. The measurement.
    [[nodiscard]] u64 entities_scanned() const noexcept { return scanned_; }
    void reset_counters() noexcept { scanned_ = 0; }

    [[nodiscard]] u32 bucket_count() const noexcept { return static_cast<u32>(buckets_.size()); }
    [[nodiscard]] u32 tracked_entities() const noexcept { return static_cast<u32>(rows_.size()); }

private:
    /// What a bucket is keyed on. One array of buckets rather than four maps: the four questions
    /// have the same shape, and four containers would be four places to forget to update.
    enum class Axis : u8 { Owner = 0, Team, Affiliation, Tag, Count };

    struct Bucket {
        Axis axis = Axis::Owner;
        /// The participant's bits, the team, the affiliation identity, or the tag.
        u64 key = 0;
        /// The affiliation kind; the empty name on every other axis.
        Name kind;
        Array<ecs::Entity> members;
        /// Entity -> its position among the members, so adding and removing are both constant.
        /// Without it a bucket of a hundred thousand costs a hundred thousand comparisons per
        /// change, and maintaining the index becomes quadratic in the world — which is the exact
        /// cost the index exists to remove.
        HashMap<u64, u32> positions;
    };

    /// What the index believes about one entity, so that a change can remove it from the bucket it
    /// was in without the caller having to remember the old value.
    struct Row {
        ecs::Entity entity;
        ParticipantId owner;
        TeamId team = kNoTeam;
    };

    [[nodiscard]] static u64 bucket_key(Axis axis, u64 key, Name kind) noexcept;
    [[nodiscard]] Bucket* find_bucket(Axis axis, u64 key, Name kind) noexcept;
    [[nodiscard]] const Bucket* find_bucket(Axis axis, u64 key, Name kind) const noexcept;
    [[nodiscard]] Expected<Bucket*, Error> ensure_bucket(Axis axis, u64 key, Name kind) noexcept;
    [[nodiscard]] Status insert(Axis axis, u64 key, Name kind, ecs::Entity entity) noexcept;
    void erase(Axis axis, u64 key, Name kind, ecs::Entity entity) noexcept;
    /// Drop `entity` from one bucket. Constant time: a lookup, a swap and a pop.
    static void drop_from(Bucket& bucket, ecs::Entity entity) noexcept;
    [[nodiscard]] Row* row_of(ecs::Entity entity) noexcept;
    [[nodiscard]] u32 read(Axis axis, u64 key, Name kind, ecs::Entity* out,
                           u32 capacity) const noexcept;

    Allocator* allocator_;
    Array<Bucket> buckets_;
    /// (axis, kind, key) -> its bucket.
    HashMap<u64, u32> by_bucket_;
    Array<Row> rows_;
    /// Entity -> its row.
    HashMap<u64, u32> by_entity_;
    mutable u64 scanned_ = 0;
};

}  // namespace cy::gameplay
