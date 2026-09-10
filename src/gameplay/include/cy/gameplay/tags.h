#pragma once
// Hierarchical gameplay tags. M8.b task 3.2.
//
// `gameplay-framework` — "Gameplay tags": tags are "authored as dotted text, declared in a
// registry, cooked to identifiers, and compared as integers. Strings SHALL NOT be the runtime
// identity of a tag. Hierarchical queries — matching `Unit.Robot` against `Unit.Robot.Harvester` —
// SHALL resolve through compact metadata, not string prefix comparison."
//
// ================================================================================================
// WHY THE HIERARCHY IS A PARENT INDEX AND NOT A STRING
// ================================================================================================
//
// `Unit.Robot` matches `Unit.Robot.Harvester` because the second's parent chain contains the first.
// Every entry stores its parent's identifier and its depth, and `matches()` walks the candidate up
// until the depths agree and then compares two integers. Nothing in that path reads a character,
// which is what the requirement asks for and what a `starts_with` implementation would quietly
// break the day somebody declared `Unit.RobotFactory`: the text of `Unit.Robot` is a prefix of it
// and the tag is not an ancestor of it. That bug is the reason the requirement exists, and
// `tests/test_tags.cpp` holds it as a case.
//
// Declaring `Unit.Robot.Harvester` declares `Unit` and `Unit.Robot` as a side effect, because a
// hierarchy with a hole in it cannot answer a query about the missing level. Declaration is a cook-
// time operation; the runtime identity handed around afterwards is a `TagId`, which is a `u32`.
//
// ================================================================================================
// GAMEPLAY TAGS ARE NOT ECS TAG COMPONENTS, AND THE DISTINCTION IS ENFORCED
// ================================================================================================
//
// "ECS tags are structural and make queries cheap by changing an archetype; gameplay tags are
// dynamic state carried in a set. Using a gameplay tag where an archetype query belongs turns a
// query into a scan, and **tooling SHALL be able to report where this occurs**."
//
// `EntityTagStore` counts the hierarchical queries made for each tag and how many entities carry
// it, and `scan_risk()` names the tags where the two together mean a query is a scan. That is the
// tooling the requirement asks for; it reports rather than refuses, because a classification that
// is dynamic today may be structural tomorrow and only the project knows which.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/core/values/name.h>
#include <cy/ecs/entity.h>

#include <string_view>

namespace cy::gameplay {

/// A tag's runtime identity. **A `u32`, never a string** — see the header comment.
using TagId = u32;

/// The null tag. Zero so that a zeroed record carries "no tag" rather than an arbitrary one.
inline constexpr TagId kInvalidTag = 0;

/// The dotted tag names, cooked to identifiers, with the metadata a hierarchical query needs.
class TagRegistry {
public:
    /// A tag deeper than this is a tag being used as a data structure. The limit exists so that
    /// `matches()`'s walk is bounded by a constant rather than by authored data.
    static constexpr u16 kMaxDepth = 16;

    explicit TagRegistry(Allocator& allocator) noexcept;

    TagRegistry(const TagRegistry&) = delete;
    TagRegistry& operator=(const TagRegistry&) = delete;

    /// Declare `dotted`, and every ancestor of it that is not declared yet. Idempotent: declaring
    /// the same text twice returns the same identifier and adds no entry.
    [[nodiscard]] Expected<TagId, Error> declare(std::string_view dotted) noexcept;

    /// The identifier for `dotted`, or `kInvalidTag`. Does **not** declare — a lookup that declared
    /// would make a typo in an interface string a permanent registry entry.
    [[nodiscard]] TagId find(std::string_view dotted) const noexcept;

    /// The full dotted text. Presentation and diagnostics only; nothing on a hot path calls it.
    [[nodiscard]] Name name(TagId tag) const noexcept;
    /// The last segment: `Harvester` of `Unit.Robot.Harvester`.
    [[nodiscard]] Name leaf(TagId tag) const noexcept;
    [[nodiscard]] TagId parent(TagId tag) const noexcept;
    [[nodiscard]] u16 depth(TagId tag) const noexcept;

    /// Does `candidate` match `query` — is it that tag or a descendant of it?
    ///
    /// INTEGER OPERATIONS ONLY. The candidate is walked up its parent chain until the depths agree
    /// and the two identifiers are compared. No character is read.
    [[nodiscard]] bool matches(TagId query, TagId candidate) const noexcept;

    [[nodiscard]] u32 count() const noexcept { return static_cast<u32>(entries_.size()); }

private:
    struct Entry {
        Name full;
        Name leaf;
        TagId parent = kInvalidTag;
        u16 depth = 0;
    };

    [[nodiscard]] const Entry* entry(TagId tag) const noexcept;
    [[nodiscard]] Expected<TagId, Error> declare_segment(Name full, Name leaf, TagId parent,
                                                         u16 depth) noexcept;

    /// Index `n` holds tag `n + 1`, so that `kInvalidTag` is not an entry.
    Array<Entry> entries_;
};

/// A compact set of tags, sorted so that membership is a binary search over integers.
///
/// This is the "dynamic state carried in a set" half of the distinction above. It is a value with
/// storage, not a component: an entity's tags live in `EntityTagStore`.
class TagSet {
public:
    explicit TagSet(Allocator& allocator) noexcept : tags_(allocator) {}

    TagSet(const TagSet&) = delete;
    TagSet& operator=(const TagSet&) = delete;
    TagSet(TagSet&&) noexcept = default;
    TagSet& operator=(TagSet&&) noexcept = default;

    [[nodiscard]] Status add(TagId tag) noexcept;
    bool remove(TagId tag) noexcept;
    void clear() noexcept { tags_.clear(); }

    /// Exactly this tag. A binary search: `log2(n)` integer comparisons.
    [[nodiscard]] bool has_exact(TagId tag) const noexcept;
    /// This tag or any descendant of it. The hierarchical query.
    [[nodiscard]] bool has(const TagRegistry& registry, TagId query) const noexcept;
    [[nodiscard]] bool has_any(const TagRegistry& registry,
                               Span<const TagId> queries) const noexcept;
    [[nodiscard]] bool has_all(const TagRegistry& registry,
                               Span<const TagId> queries) const noexcept;

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(tags_.size()); }
    [[nodiscard]] TagId at(u32 index) const noexcept { return tags_[index]; }
    [[nodiscard]] Span<const TagId> span() const noexcept { return tags_.span(); }

private:
    Array<TagId> tags_;
};

/// The tags entities carry, and the tooling that reports when a tag is doing an archetype's job.
class EntityTagStore {
public:
    explicit EntityTagStore(Allocator& allocator) noexcept;

    EntityTagStore(const EntityTagStore&) = delete;
    EntityTagStore& operator=(const EntityTagStore&) = delete;

    [[nodiscard]] Status add(ecs::Entity entity, TagId tag) noexcept;
    bool remove(ecs::Entity entity, TagId tag) noexcept;
    void forget(ecs::Entity entity) noexcept;

    /// Hierarchical, and **counted**: this is the call `scan_risk()` reports on.
    [[nodiscard]] bool has(const TagRegistry& registry, ecs::Entity entity,
                           TagId query) const noexcept;

    [[nodiscard]] const TagSet* tags_of(ecs::Entity entity) const noexcept;
    [[nodiscard]] u32 entity_count() const noexcept { return static_cast<u32>(rows_.size()); }
    [[nodiscard]] ecs::Entity entity_at(u32 index) const noexcept { return rows_[index].entity; }

    /// How many entities carry `tag` exactly. Cheap: maintained on add and remove.
    [[nodiscard]] u32 carriers(TagId tag) const noexcept;
    /// How many hierarchical queries have been made for `tag`.
    [[nodiscard]] u64 queries(TagId tag) const noexcept;

    /// One tag that is behaving like an archetype: queried often, over a large population.
    struct ScanRisk {
        TagId tag = kInvalidTag;
        u32 carriers = 0;
        u64 queries = 0;
    };

    /// The tags where a query is a scan, worst first. `gameplay-framework`'s "The distinction is
    /// enforced" scenario: tooling reports it as a performance issue rather than refusing it.
    [[nodiscard]] u32 scan_risk(u32 min_carriers, u64 min_queries, ScanRisk* out,
                                u32 capacity) const noexcept;

private:
    struct Row {
        ecs::Entity entity;
        TagSet tags;
    };
    struct Usage {
        TagId tag = kInvalidTag;
        u32 carriers = 0;
        u64 queries = 0;
    };

    [[nodiscard]] Row* find_row(ecs::Entity entity) noexcept;
    [[nodiscard]] const Row* find_row(ecs::Entity entity) const noexcept;
    /// Find `tag`'s usage row, appending one if it has none. Null only when the append failed,
    /// in which case the count is lost and the answer is not — a diagnostic counter may not be a
    /// reason for a query to fail.
    [[nodiscard]] Usage* usage_of(TagId tag) const noexcept;

    Allocator* allocator_;
    Array<Row> rows_;
    /// Entity -> its row. `gameplay-framework`'s performance contract is a hundred thousand active
    /// gameplay entities; a scan per tag operation makes maintaining them quadratic in the world.
    HashMap<u64, u32> by_entity_;
    /// Mutable because counting a query is not a change to what the store answers — `has()` is
    /// const to its callers and stays that way.
    mutable Array<Usage> usage_;
};

}  // namespace cy::gameplay
