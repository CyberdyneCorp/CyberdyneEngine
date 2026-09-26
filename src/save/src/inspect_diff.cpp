// SPDX-License-Identifier: MIT
// The semantic save diff. See inspect.h.
//
// Every walk below is `merge_walk` over two sorted sequences — regions by key, entries by identity,
// components by type, fields by identifier, fragments by (scope, type) — because the overlay keeps
// every one of them sorted (overlay.h, "ORDER IS BY KEY"). So a diff is linear in the size of the
// two saves, and two saves whose bytes differ only in where a chunk was stored produce no
// difference.

#include <cy/save/inspect.h>

#include <cstring>

namespace cy::save {

const char* diff_kind_name(DiffKind kind) noexcept {
    switch (kind) {
        case DiffKind::EntityCreated:
            return "entity-created";
        case DiffKind::EntityDestroyed:
            return "entity-destroyed";
        case DiffKind::EntityReverted:
            return "entity-reverted";
        case DiffKind::FieldAdded:
            return "field-added";
        case DiffKind::FieldRemoved:
            return "field-removed";
        case DiffKind::FieldChanged:
            return "field-changed";
        case DiffKind::FragmentAdded:
            return "fragment-added";
        case DiffKind::FragmentRemoved:
            return "fragment-removed";
        case DiffKind::FragmentChanged:
            return "fragment-changed";
        case DiffKind::SimulationPointChanged:
            return "simulation-point-changed";
        case DiffKind::ContentVersionChanged:
            return "content-version-changed";
        case DiffKind::PluginsChanged:
            return "plugins-changed";
    }
    return "unknown";
}

usize SaveDiff::count_of(DiffKind kind) const noexcept {
    usize count = 0;
    for (const DiffItem& item : items) {
        count += item.kind == kind ? 1U : 0U;
    }
    return count;
}

namespace {

/// Walk two sequences sorted by `key_of` together, calling `visit(before, after, present)` once per
/// key: `before` and `after` are the elements holding it, one of them null when only the other side
/// does, and `present` is whichever is not — so a visitor never reaches through a pointer the walk
/// knows to be set and the compiler does not. Every comparison in this file is this walk.
template <typename T, typename KeyOf, typename Visit>
Status merge_walk(Span<const T> before, Span<const T> after, KeyOf key_of, Visit visit) noexcept {
    usize i = 0;
    usize j = 0;
    while (i < before.size() || j < after.size()) {
        const T* left = nullptr;
        const T* right = nullptr;
        // `present` is taken from the element each branch just addressed, never from a pointer
        // that may be null: GCC's -Wnull-dereference cannot prove `left ? *left : *right` safe.
        const T* present = nullptr;
        if (j == after.size() || (i < before.size() && key_of(before[i]) < key_of(after[j]))) {
            left = &before[i++];
            present = left;
        } else if (i == before.size() || key_of(after[j]) < key_of(before[i])) {
            right = &after[j++];
            present = right;
        } else {
            left = &before[i++];
            right = &after[j++];
            present = left;
        }
        if (Status visited = visit(left, right, *present); !visited) {
            return visited;
        }
    }
    return ok();
}

bool same_field(const serialize::ValueRecord& a, const serialize::FieldValue& left,
                const serialize::ValueRecord& b, const serialize::FieldValue& right) noexcept {
    if (left.wire != right.wire) {
        return false;
    }
    const Span<const u8> x = a.bytes(left);
    const Span<const u8> y = b.bytes(right);
    return x.size() == y.size() && (x.empty() || std::memcmp(x.data(), y.data(), x.size()) == 0);
}

/// Where the fields being compared live, stamped onto every item they produce.
struct Address {
    Scope scope = Scope::World;
    RegionKey region;
    PersistentId entity;
    reflect::TypeId type;
};

Status push(SaveDiff& out, DiffKind kind, const Address& at,
            reflect::FieldId field = reflect::FieldId()) noexcept {
    DiffItem item;
    item.kind = kind;
    item.scope = at.scope;
    item.region = at.region;
    item.entity = at.entity;
    item.type = at.type;
    item.field = field;
    return out.items.push_back(item);
}

/// The kinds a field difference is reported as: entity fields and fragment fields differ in name
/// only, so one walk serves both.
struct FieldKinds {
    DiffKind added;
    DiffKind removed;
    DiffKind changed;
};

constexpr FieldKinds kEntityFields{DiffKind::FieldAdded, DiffKind::FieldRemoved,
                                   DiffKind::FieldChanged};
constexpr FieldKinds kFragmentFields{DiffKind::FragmentChanged, DiffKind::FragmentChanged,
                                     DiffKind::FragmentChanged};

/// Either record may be null: a component present on one side only is every field added/removed.
Status diff_records(const serialize::ValueRecord* before, const serialize::ValueRecord* after,
                    const Address& at, const FieldKinds& kinds, SaveDiff& out) noexcept {
    using serialize::FieldValue;
    // An absent record is an empty one; it allocates nothing until written, and nothing writes it.
    const serialize::ValueRecord none(out.items.allocator());
    const serialize::ValueRecord& a = before == nullptr ? none : *before;
    const serialize::ValueRecord& b = after == nullptr ? none : *after;
    return merge_walk(
        a.fields(), b.fields(), [](const FieldValue& value) noexcept { return value.id; },
        [&](const FieldValue* left, const FieldValue* right,
            const FieldValue& present) noexcept -> Status {
            if (left == nullptr || right == nullptr) {
                return push(out, left == nullptr ? kinds.added : kinds.removed, at, present.id);
            }
            return same_field(a, *left, b, *right) ? ok()
                                                   : push(out, kinds.changed, at, present.id);
        });
}

Status diff_components(const Entry* before, const Entry* after, Address at,
                       SaveDiff& out) noexcept {
    const Span<const ComponentDelta> a =
        before == nullptr ? Span<const ComponentDelta>() : before->components.span();
    const Span<const ComponentDelta> b =
        after == nullptr ? Span<const ComponentDelta>() : after->components.span();
    return merge_walk(
        a, b, [](const ComponentDelta& component) noexcept { return component.type; },
        [&](const ComponentDelta* left, const ComponentDelta* right,
            const ComponentDelta& present) noexcept {
            at.type = present.type;
            if (left == nullptr) {
                return diff_records(nullptr, &present.record, at, kEntityFields, out);
            }
            if (right == nullptr) {
                return diff_records(&present.record, nullptr, at, kEntityFields, out);
            }
            return diff_records(&left->record, &right->record, at, kEntityFields, out);
        });
}

/// What one entity's two entries — either possibly absent — say changed.
Status diff_entry(const Entry* before, const Entry* after, Address at, SaveDiff& out) noexcept {
    const bool destroyed_after = after != nullptr && after->kind == EntryKind::Tombstone;
    const bool destroyed_before = before != nullptr && before->kind == EntryKind::Tombstone;
    const bool created_after = after != nullptr && after->kind == EntryKind::Created;
    const bool created_before = before != nullptr && before->kind == EntryKind::Created;

    if (destroyed_after) {
        return destroyed_before ? ok() : push(out, DiffKind::EntityDestroyed, at);
    }
    if (after == nullptr) {
        // Gone from the second save: a spawned entity was destroyed; anything else is authored
        // content back to what it was authored as.
        return push(out, created_before ? DiffKind::EntityDestroyed : DiffKind::EntityReverted, at);
    }
    if (created_after && !created_before) {
        return push(out, DiffKind::EntityCreated, at);
    }
    if (destroyed_before) {
        if (Status pushed = push(out, DiffKind::EntityReverted, at); !pushed) {
            return pushed;
        }
        return diff_components(nullptr, after, at, out);
    }
    return diff_components(before, after, at, out);
}

Status diff_region(const Region* before, const Region* after, Address at, SaveDiff& out) noexcept {
    const Span<const Entry> a = before == nullptr ? Span<const Entry>() : before->entries.span();
    const Span<const Entry> b = after == nullptr ? Span<const Entry>() : after->entries.span();
    return merge_walk(
        a, b, [](const Entry& entry) noexcept { return entry.id; },
        [&](const Entry* left, const Entry* right, const Entry& present) noexcept {
            at.entity = present.id;
            at.type = reflect::TypeId();
            return diff_entry(left, right, at, out);
        });
}

/// Fragments sort by (scope, type); a pair of them compared as one key.
struct FragmentKey {
    u8 scope = 0;
    reflect::TypeId type;

    friend bool operator<(const FragmentKey& a, const FragmentKey& b) noexcept {
        return a.scope != b.scope ? a.scope < b.scope : a.type < b.type;
    }
};

FragmentKey key_of(const Fragment& fragment) noexcept {
    return FragmentKey{static_cast<u8>(fragment.scope), fragment.type};
}

Status diff_fragment(const Fragment* before, const Fragment* after, const Fragment& present,
                     SaveDiff& out) noexcept {
    Address at;
    at.scope = present.scope;
    at.region = kGlobalRegion;
    at.type = present.type;
    if (before == nullptr) {
        return push(out, DiffKind::FragmentAdded, at);
    }
    if (after == nullptr) {
        return push(out, DiffKind::FragmentRemoved, at);
    }
    return diff_records(&before->record, &after->record, at, kFragmentFields, out);
}

Status diff_fragments(const Overlay& before, const Overlay& after, SaveDiff& out) noexcept {
    return merge_walk(
        before.fragments(), after.fragments(), key_of,
        [&](const Fragment* left, const Fragment* right, const Fragment& present) noexcept {
            return diff_fragment(left, right, present, out);
        });
}

bool same_plugins(const Manifest& a, const Manifest& b) noexcept {
    if (a.plugins.size() != b.plugins.size()) {
        return false;
    }
    for (usize index = 0; index < a.plugins.size(); ++index) {
        if (std::strcmp(a.plugins[index].name, b.plugins[index].name) != 0 ||
            a.plugins[index].version != b.plugins[index].version) {
            return false;
        }
    }
    return true;
}

Status diff_manifests(const Manifest& before, const Manifest& after, SaveDiff& out) noexcept {
    const Address at;
    if (before.simulation_point != after.simulation_point) {
        if (Status pushed = push(out, DiffKind::SimulationPointChanged, at); !pushed) {
            return pushed;
        }
    }
    if (before.content_version != after.content_version) {
        if (Status pushed = push(out, DiffKind::ContentVersionChanged, at); !pushed) {
            return pushed;
        }
    }
    if (!same_plugins(before, after)) {
        return push(out, DiffKind::PluginsChanged, at);
    }
    return ok();
}

}  // namespace

Status diff_overlays(const Overlay& before, const Overlay& after, SaveDiff& out) noexcept {
    out.items.clear();
    Status regions = merge_walk(
        before.regions(), after.regions(), [](const Region& region) noexcept { return region.key; },
        [&](const Region* left, const Region* right, const Region& present) noexcept {
            Address at;
            at.region = present.key;
            return diff_region(left, right, at, out);
        });
    if (!regions) {
        return regions;
    }
    return diff_fragments(before, after, out);
}

Status diff_saves(const SaveInspection& before, const SaveInspection& after,
                  SaveDiff& out) noexcept {
    if (Status diffed = diff_overlays(before.overlay, after.overlay, out); !diffed) {
        return diffed;
    }
    return diff_manifests(before.manifest, after.manifest, out);
}

}  // namespace cy::save
