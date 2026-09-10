// Hierarchical gameplay tags. M8.b task 3.2.

#include <cy/gameplay/tags.h>

#include <algorithm>

#include <utility>

namespace cy::gameplay {
namespace {

/// Where the next dot is, or the end. A tag's text is split at cook time and never at runtime.
[[nodiscard]] usize next_separator(std::string_view text, usize from) noexcept {
    for (usize index = from; index < text.size(); ++index) {
        if (text[index] == '.') {
            return index;
        }
    }
    return text.size();
}

}  // namespace

TagRegistry::TagRegistry(Allocator& allocator) noexcept : entries_(allocator) {}

const TagRegistry::Entry* TagRegistry::entry(TagId tag) const noexcept {
    if (tag == kInvalidTag || tag > entries_.size()) {
        return nullptr;
    }
    return &entries_[tag - 1];
}

Expected<TagId, Error> TagRegistry::declare_segment(Name full, Name leaf, TagId parent,
                                                    u16 depth) noexcept {
    for (usize index = 0; index < entries_.size(); ++index) {
        if (entries_[index].full == full) {
            return static_cast<TagId>(index + 1);
        }
    }
    Entry added;
    added.full = full;
    added.leaf = leaf;
    added.parent = parent;
    added.depth = depth;
    if (Status pushed = entries_.push_back(added); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<TagId>(entries_.size());
}

Expected<TagId, Error> TagRegistry::declare(std::string_view dotted) noexcept {
    if (dotted.empty()) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "a tag has no empty spelling", 0});
    }
    TagId parent = kInvalidTag;
    u16 depth = 0;
    usize from = 0;
    while (from <= dotted.size()) {
        const usize dot = next_separator(dotted, from);
        if (dot == from) {
            return make_unexpected(
                Error{ErrorCode::InvalidArgument, "a tag segment has no empty spelling", 0});
        }
        if (depth >= kMaxDepth) {
            return make_unexpected(
                Error{ErrorCode::OutOfRange, "a tag deeper than kMaxDepth is a data structure", 0});
        }
        const Name full = Name::intern(dotted.substr(0, dot));
        const Name leaf = Name::intern(dotted.substr(from, dot - from));
        auto declared = declare_segment(full, leaf, parent, depth);
        if (!declared) {
            return declared;
        }
        parent = declared.value();
        ++depth;
        if (dot == dotted.size()) {
            break;
        }
        from = dot + 1;
    }
    return parent;
}

TagId TagRegistry::find(std::string_view dotted) const noexcept {
    const Name full = Name::find(dotted);
    if (full.is_empty()) {
        return kInvalidTag;
    }
    for (usize index = 0; index < entries_.size(); ++index) {
        if (entries_[index].full == full) {
            return static_cast<TagId>(index + 1);
        }
    }
    return kInvalidTag;
}

Name TagRegistry::name(TagId tag) const noexcept {
    const Entry* found = entry(tag);
    return found != nullptr ? found->full : Name{};
}

Name TagRegistry::leaf(TagId tag) const noexcept {
    const Entry* found = entry(tag);
    return found != nullptr ? found->leaf : Name{};
}

TagId TagRegistry::parent(TagId tag) const noexcept {
    const Entry* found = entry(tag);
    return found != nullptr ? found->parent : kInvalidTag;
}

u16 TagRegistry::depth(TagId tag) const noexcept {
    const Entry* found = entry(tag);
    return found != nullptr ? found->depth : 0;
}

bool TagRegistry::matches(TagId query, TagId candidate) const noexcept {
    // INTEGER OPERATIONS ONLY. `Unit.RobotFactory` does not match `Unit.Robot` here, and it would
    // under a text prefix comparison — which is the whole reason the requirement names the
    // mechanism rather than the outcome.
    const Entry* wanted = entry(query);
    const Entry* found = entry(candidate);
    if (wanted == nullptr || found == nullptr) {
        return false;
    }
    TagId walk = candidate;
    u16 depth = found->depth;
    while (depth > wanted->depth) {
        const Entry* step = entry(walk);
        if (step == nullptr) {
            return false;
        }
        walk = step->parent;
        --depth;
    }
    return walk == query;
}

Status TagSet::add(TagId tag) noexcept {
    if (tag == kInvalidTag) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "the null tag is not a member", 0});
    }
    usize low = 0;
    usize high = tags_.size();
    while (low < high) {
        const usize middle = low + ((high - low) / 2);
        if (tags_[middle] < tag) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low < tags_.size() && tags_[low] == tag) {
        return ok();
    }
    if (Status pushed = tags_.push_back(tag); !pushed) {
        return pushed;
    }
    for (usize index = tags_.size() - 1; index > low; --index) {
        std::swap(tags_[index], tags_[index - 1]);
    }
    return ok();
}

bool TagSet::remove(TagId tag) noexcept {
    for (usize index = 0; index < tags_.size(); ++index) {
        if (tags_[index] == tag) {
            tags_.erase(index);
            return true;
        }
    }
    return false;
}

bool TagSet::has_exact(TagId tag) const noexcept {
    usize low = 0;
    usize high = tags_.size();
    while (low < high) {
        const usize middle = low + ((high - low) / 2);
        if (tags_[middle] == tag) {
            return true;
        }
        if (tags_[middle] < tag) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return false;
}

bool TagSet::has(const TagRegistry& registry, TagId query) const noexcept {
    if (has_exact(query)) {
        return true;
    }
    return std::ranges::any_of(tags_, [&](TagId held) { return registry.matches(query, held); });
}

bool TagSet::has_any(const TagRegistry& registry, Span<const TagId> queries) const noexcept {
    return std::ranges::any_of(queries, [&](TagId query) { return has(registry, query); });
}

bool TagSet::has_all(const TagRegistry& registry, Span<const TagId> queries) const noexcept {
    return std::ranges::all_of(queries, [&](TagId query) { return has(registry, query); });
}

EntityTagStore::EntityTagStore(Allocator& allocator) noexcept
    : allocator_(&allocator), rows_(allocator), by_entity_(allocator), usage_(allocator) {}

EntityTagStore::Row* EntityTagStore::find_row(ecs::Entity entity) noexcept {
    const u32* slot = by_entity_.find(entity.bits());
    return slot != nullptr && *slot < rows_.size() ? &rows_[*slot] : nullptr;
}

const EntityTagStore::Row* EntityTagStore::find_row(ecs::Entity entity) const noexcept {
    const u32* slot = by_entity_.find(entity.bits());
    return slot != nullptr && *slot < rows_.size() ? &rows_[*slot] : nullptr;
}

EntityTagStore::Usage* EntityTagStore::usage_of(TagId tag) const noexcept {
    for (Usage& entry : usage_) {
        if (entry.tag == tag) {
            return &entry;
        }
    }
    Usage added;
    added.tag = tag;
    if (Status pushed = usage_.push_back(added); !pushed) {
        return nullptr;
    }
    return &usage_[usage_.size() - 1];
}

Status EntityTagStore::add(ecs::Entity entity, TagId tag) noexcept {
    Row* row = find_row(entity);
    if (row == nullptr) {
        if (Status pushed = rows_.push_back(Row{entity, TagSet(*allocator_)}); !pushed) {
            return pushed;
        }
        if (auto placed = by_entity_.insert(entity.bits(), static_cast<u32>(rows_.size() - 1));
            !placed) {
            rows_.pop_back();
            return make_unexpected(placed.error());
        }
        row = &rows_[rows_.size() - 1];
    }
    const bool held = row->tags.has_exact(tag);
    if (Status added = row->tags.add(tag); !added) {
        return added;
    }
    if (!held) {
        Usage* usage = usage_of(tag);
        if (usage != nullptr) {
            ++usage->carriers;
        }
    }
    return ok();
}

bool EntityTagStore::remove(ecs::Entity entity, TagId tag) noexcept {
    Row* row = find_row(entity);
    if (row == nullptr || !row->tags.remove(tag)) {
        return false;
    }
    Usage* usage = usage_of(tag);
    if (usage != nullptr && usage->carriers > 0) {
        --usage->carriers;
    }
    return true;
}

void EntityTagStore::forget(ecs::Entity entity) noexcept {
    const u32* slot = by_entity_.find(entity.bits());
    if (slot == nullptr || *slot >= rows_.size()) {
        return;
    }
    const u32 position = *slot;
    for (const TagId tag : rows_[position].tags.span()) {
        Usage* usage = usage_of(tag);
        if (usage != nullptr && usage->carriers > 0) {
            --usage->carriers;
        }
    }
    // Swap-remove and repoint: row order is not meaningful and an ordered erase would move every
    // index the table holds.
    (void)by_entity_.remove(entity.bits());
    const auto last = static_cast<u32>(rows_.size() - 1);
    if (position != last) {
        rows_[position] = std::move(rows_[last]);
        if (u32* moved = by_entity_.find(rows_[position].entity.bits()); moved != nullptr) {
            *moved = position;
        }
    }
    rows_.pop_back();
}

bool EntityTagStore::has(const TagRegistry& registry, ecs::Entity entity,
                         TagId query) const noexcept {
    Usage* usage = usage_of(query);
    if (usage != nullptr) {
        ++usage->queries;
    }
    const Row* row = find_row(entity);
    return row != nullptr && row->tags.has(registry, query);
}

const TagSet* EntityTagStore::tags_of(ecs::Entity entity) const noexcept {
    const Row* row = find_row(entity);
    return row != nullptr ? &row->tags : nullptr;
}

u32 EntityTagStore::carriers(TagId tag) const noexcept {
    const Usage* usage = usage_of(tag);
    return usage != nullptr ? usage->carriers : 0;
}

u64 EntityTagStore::queries(TagId tag) const noexcept {
    const Usage* usage = usage_of(tag);
    return usage != nullptr ? usage->queries : 0;
}

u32 EntityTagStore::scan_risk(u32 min_carriers, u64 min_queries, ScanRisk* out,
                              u32 capacity) const noexcept {
    u32 found = 0;
    for (const Usage& entry : usage_) {
        if (entry.carriers < min_carriers || entry.queries < min_queries) {
            continue;
        }
        if (out != nullptr && found < capacity) {
            // Worst first: insertion sort into the caller's buffer, which is at most a handful of
            // entries and never on a frame path.
            u32 slot = found;
            while (slot > 0 && out[slot - 1].queries < entry.queries) {
                out[slot] = out[slot - 1];
                --slot;
            }
            out[slot] = ScanRisk{entry.tag, entry.carriers, entry.queries};
        }
        ++found;
    }
    return found;
}

}  // namespace cy::gameplay
