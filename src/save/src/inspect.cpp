// SPDX-License-Identifier: MIT
// The save inspector: what is in a save, and why each field is there. See inspect.h.

#include <cy/save/inspect.h>

#include <cy/core/serialize/tagged.h>

#include <cstring>
#include <utility>

namespace cy::save {
namespace {

/// The policy the contents are read under: every check a game makes switched off, so that a save
/// this build would refuse can still be looked at. Migration chains are kept, so the fields read
/// back are the ones a successful load would have produced.
LoadPolicy permissive(const LoadPolicy& policy) noexcept {
    LoadPolicy relaxed;
    relaxed.compatibility = Compatibility::BestEffort;
    relaxed.schemas = policy.schemas;
    return relaxed;
}

/// The bytes one value record occupies in a chunk, measured by writing it the way the container
/// does. Exact, because a size attributed to a component is a claim about the file.
Expected<u64, Error> encoded_record_bytes(const serialize::ValueRecord& record,
                                          Array<u8>& scratch) noexcept {
    scratch.clear();
    serialize::TaggedWriter writer(scratch);
    if (Status began = writer.begin_stream(); !began) {
        return make_unexpected(began.error());
    }
    if (Status began = writer.begin_chunk(kRegionChunkTag); !began) {
        return make_unexpected(began.error());
    }
    const usize before = scratch.size();
    if (Status written = writer.write_record(record); !written) {
        return make_unexpected(written.error());
    }
    return static_cast<u64>(scratch.size() - before);
}

const reflect::TypeInfo* find_type(const InspectOptions& options, reflect::TypeId type) noexcept {
    return options.types == nullptr ? nullptr : options.types->find(type);
}

const char* module_of(const reflect::TypeInfo* info) noexcept {
    if (info == nullptr || info->module == nullptr || info->module[0] == '\0') {
        return kUndeclaredModule;
    }
    return info->module;
}

ComponentUsage* component_usage(SaveInspection& out, reflect::TypeId type,
                                const InspectOptions& options) noexcept {
    usize index = 0;
    while (index < out.components.size() && out.components[index].type < type) {
        ++index;
    }
    if (index < out.components.size() && out.components[index].type == type) {
        return &out.components[index];
    }
    ComponentUsage usage;
    usage.type = type;
    const reflect::TypeInfo* info = find_type(options, type);
    usage.name = info == nullptr ? "" : info->name;
    usage.module = module_of(info);
    if (!out.components.push_back(usage)) {
        return nullptr;
    }
    // Keep ascending order: rotate the appended element into place.
    for (usize slot = out.components.size() - 1; slot > index; --slot) {
        std::swap(out.components[slot], out.components[slot - 1]);
    }
    return &out.components[index];
}

Status tally_record(SaveInspection& out, const InspectOptions& options, reflect::TypeId type,
                    const serialize::ValueRecord& record, Array<u8>& scratch) noexcept {
    const Expected<u64, Error> bytes = encoded_record_bytes(record, scratch);
    if (!bytes) {
        return make_unexpected(bytes.error());
    }
    ComponentUsage* usage = component_usage(out, type, options);
    if (usage == nullptr) {
        return fail(ErrorCode::OutOfMemory, "the inspector could not grow its component table");
    }
    ++usage->records;
    usage->fields += static_cast<u32>(record.size());
    usage->bytes += *bytes;
    return ok();
}

void count_kind(RegionUsage& usage, EntryKind kind) noexcept {
    switch (kind) {
        case EntryKind::Modified:
            ++usage.modified;
            break;
        case EntryKind::Created:
            ++usage.created;
            break;
        case EntryKind::Tombstone:
            ++usage.tombstoned;
            break;
    }
}

Status tally_region(SaveInspection& out, const InspectOptions& options, const Region& region,
                    Array<u8>& scratch) noexcept {
    RegionUsage usage;
    usage.region = region.key;
    u32 records = 0;
    for (const Entry& entry : region.entries) {
        count_kind(usage, entry.kind);
        for (const ComponentDelta& component : entry.components) {
            if (Status tallied =
                    tally_record(out, options, component.type, component.record, scratch);
                !tallied) {
                return tallied;
            }
            ++records;
        }
    }
    if (Status encoded = encode_region(out.overlay, region.key, scratch); !encoded) {
        return encoded;
    }
    usage.bytes = scratch.size();
    out.modified += usage.modified;
    out.created += usage.created;
    out.tombstoned += usage.tombstoned;

    if (out.scopes.empty() || out.scopes.back().scope != Scope::World) {
        ScopeUsage world;
        world.scope = Scope::World;
        if (Status pushed = out.scopes.push_back(world); !pushed) {
            return pushed;
        }
    }
    ScopeUsage& world = out.scopes.back();
    ++world.chunks;
    world.records += records;
    world.bytes += usage.bytes;
    return out.regions.push_back(usage);
}

Status tally_fragment_scope(SaveInspection& out, const InspectOptions& options, Scope scope,
                            Array<u8>& scratch) noexcept {
    ScopeUsage usage;
    usage.scope = scope;
    for (const Fragment& fragment : out.overlay.fragments()) {
        if (fragment.scope != scope) {
            continue;
        }
        if (Status tallied = tally_record(out, options, fragment.type, fragment.record, scratch);
            !tallied) {
            return tallied;
        }
        ++usage.records;
    }
    if (usage.records == 0) {
        return ok();
    }
    if (Status encoded = encode_fragments(out.overlay, scope, scratch); !encoded) {
        return encoded;
    }
    usage.chunks = 1;
    usage.bytes = scratch.size();
    out.fragments += usage.records;
    // The world scope's fragments share its row with its regions: one scope, one line of the
    // report.
    if (!out.scopes.empty() && out.scopes.back().scope == scope) {
        ScopeUsage& shared = out.scopes.back();
        shared.chunks += usage.chunks;
        shared.records += usage.records;
        shared.bytes += usage.bytes;
        return ok();
    }
    return out.scopes.push_back(usage);
}

Status tally_plugins(SaveInspection& out) noexcept {
    for (const ComponentUsage& component : out.components) {
        usize index = 0;
        while (index < out.plugins.size() &&
               std::strcmp(out.plugins[index].module, component.module) < 0) {
            ++index;
        }
        if (index == out.plugins.size() ||
            std::strcmp(out.plugins[index].module, component.module) != 0) {
            PluginUsage usage;
            usage.module = component.module;
            if (Status pushed = out.plugins.push_back(usage); !pushed) {
                return pushed;
            }
            for (usize slot = out.plugins.size() - 1; slot > index; --slot) {
                std::swap(out.plugins[slot], out.plugins[slot - 1]);
            }
        }
        out.plugins[index].records += component.records;
        out.plugins[index].bytes += component.bytes;
    }
    return ok();
}

/// Scopes in enumerator order, the world's regions where its number puts it.
Status tally(SaveInspection& out, const InspectOptions& options) noexcept {
    Array<u8> scratch(out.overlay.allocator());
    for (u8 value = 0; value < static_cast<u8>(Scope::Count); ++value) {
        const auto scope = static_cast<Scope>(value);
        if (scope != Scope::World) {
            if (Status tallied = tally_fragment_scope(out, options, scope, scratch); !tallied) {
                return tallied;
            }
            continue;
        }
        for (const Region& region : out.overlay.regions()) {
            if (Status tallied = tally_region(out, options, region, scratch); !tallied) {
                return tallied;
            }
        }
        if (Status tallied = tally_fragment_scope(out, options, scope, scratch); !tallied) {
            return tallied;
        }
    }
    return tally_plugins(out);
}

Expected<u32, Error> resolve_generation(SaveArchive& archive, u32 generation) noexcept {
    if (generation != 0) {
        return generation;
    }
    Expected<u32, Error> active = archive.active_generation();
    if (active && *active == 0) {
        return fail(ErrorCode::NotFound, "this save store holds no committed generation");
    }
    return active;
}

void reset(SaveInspection& out) noexcept {
    out.overlay.clear();
    out.scopes.clear();
    out.regions.clear();
    out.components.clear();
    out.plugins.clear();
    out.generations.clear();
    out.restore = LoadReport();
    out.contents = LoadReport();
    out.modified = 0;
    out.created = 0;
    out.tombstoned = 0;
    out.fragments = 0;
}

}  // namespace

u64 SaveInspection::total_bytes() const noexcept {
    u64 total = 0;
    for (const ScopeUsage& scope : scopes) {
        total += scope.bytes;
    }
    return total;
}

Status inspect_save(SaveArchive& archive, u32 generation, const InspectOptions& options,
                    SaveInspection& out) noexcept {
    reset(out);
    const Expected<u32, Error> resolved = resolve_generation(archive, generation);
    if (!resolved) {
        return make_unexpected(resolved.error());
    }
    out.generation = *resolved;
    if (Status listed = archive.generations(out.generations); !listed) {
        return listed;
    }
    if (Status read = archive.read_manifest(out.generation, out.manifest, out.restore); !read) {
        return read;
    }

    // Why state would not be restored: the answer a load under the caller's policy gives. When it
    // restores, what it read IS the contents; only a refused save is read a second time, under a
    // policy that refuses nothing, so the inspector still has something to show.
    const Status restored =
        archive.load_generation(out.generation, options.policy, out.overlay, out.restore);
    if (restored) {
        out.contents = out.restore;
        return tally(out, options);
    }
    if (!out.restore.failed()) {
        out.restore.failure = LoadFailure::CorruptChunk;
        out.restore.detail = restored.error().message;
    }
    out.overlay.clear();
    if (Status read = archive.load_generation(out.generation, permissive(options.policy),
                                              out.overlay, out.contents);
        !read) {
        out.overlay.clear();
        return ok();
    }
    return tally(out, options);
}

// --- Why is this field here ---------------------------------------------------------------------

const char* field_reason_name(FieldReason reason) noexcept {
    switch (reason) {
        case FieldReason::SaveGameTrait:
            return "save-game-trait";
        case FieldReason::PreservedUnknownField:
            return "preserved-unknown-field";
        case FieldReason::PreservedUnknownType:
            return "preserved-unknown-type";
        case FieldReason::NotASavedField:
            return "not-a-saved-field";
        case FieldReason::EntryRecord:
            return "entry-record";
    }
    return "unknown";
}

namespace {

/// The generations older than the inspected one, newest first, each loaded once.
struct History {
    explicit History(Allocator& allocator) noexcept
        : overlays(allocator), numbers(allocator), ticks(allocator) {}

    Array<Overlay> overlays;
    Array<u32> numbers;
    Array<u64> ticks;
    /// True when every older generation the store retains loaded, so running out of history means
    /// "at or before the oldest retained" rather than "at or before the last one that loaded".
    bool complete = true;
};

Status load_history(SaveArchive& archive, const SaveInspection& inspection,
                    const InspectOptions& options, History& history) noexcept {
    Allocator& allocator = inspection.overlay.allocator();
    for (usize index = inspection.generations.size(); index > 0; --index) {
        const u32 generation = inspection.generations[index - 1];
        if (generation >= inspection.generation) {
            continue;
        }
        Overlay older(allocator);
        LoadReport report;
        if (!archive.load_generation(generation, permissive(options.policy), older, report)) {
            history.complete = false;
            return ok();
        }
        const u64 tick = older.simulation_point();
        if (Status pushed = history.overlays.push_back(std::move(older)); !pushed) {
            return pushed;
        }
        if (Status pushed = history.numbers.push_back(generation); !pushed) {
            return pushed;
        }
        if (Status pushed = history.ticks.push_back(tick); !pushed) {
            return pushed;
        }
    }
    return ok();
}

bool same_value(const serialize::ValueRecord& a, const serialize::ValueRecord& b,
                reflect::FieldId field) noexcept {
    const serialize::FieldValue* left = a.find(field);
    const serialize::FieldValue* right = b.find(field);
    if (left == nullptr || right == nullptr || left->wire != right->wire) {
        return false;
    }
    const Span<const u8> x = a.bytes(*left);
    const Span<const u8> y = b.bytes(*right);
    return x.size() == y.size() && (x.empty() || std::memcmp(x.data(), y.data(), x.size()) == 0);
}

/// Whether an older overlay holds what the origin names, with the value `current` holds.
bool holds_same(const Overlay& older, const FieldOrigin& origin,
                const serialize::ValueRecord* current) noexcept {
    if (origin.fragment) {
        const Fragment* fragment = older.find_fragment(origin.scope, origin.type);
        return fragment != nullptr && current != nullptr &&
               same_value(fragment->record, *current, origin.field);
    }
    const Entry* entry = older.find_entry(origin.region, origin.entity);
    if (entry == nullptr || entry->kind != origin.kind) {
        return false;
    }
    if (current == nullptr) {
        return true;  // the entry record itself: same kind is the same statement
    }
    const ComponentDelta* component =
        older.find_component(origin.region, origin.entity, origin.type);
    return component != nullptr && same_value(component->record, *current, origin.field);
}

void date_origin(FieldOrigin& origin, const serialize::ValueRecord* current, u32 generation,
                 u64 tick, const History& history) noexcept {
    origin.dirty_since_generation = generation;
    origin.dirty_since_tick = tick;
    for (usize index = 0; index < history.overlays.size(); ++index) {
        if (!holds_same(history.overlays[index], origin, current)) {
            return;
        }
        origin.dirty_since_generation = history.numbers[index];
        origin.dirty_since_tick = history.ticks[index];
    }
    origin.since_oldest_retained = history.complete;
}

void describe_field(FieldOrigin& origin, const reflect::TypeInfo* info) noexcept {
    origin.module = module_of(info);
    if (info == nullptr) {
        origin.reason = FieldReason::PreservedUnknownType;
        return;
    }
    origin.component = info->name;
    const reflect::FieldInfo* field = info->find_field(origin.field);
    if (field == nullptr) {
        origin.reason = FieldReason::PreservedUnknownField;
        return;
    }
    origin.field_name = field->name;
    origin.traits = traits_of(*field);
    origin.reason =
        field_is_saved(*field) ? FieldReason::SaveGameTrait : FieldReason::NotASavedField;
}

struct ExplainContext {
    const SaveInspection& inspection;
    const InspectOptions& options;
    const History& history;
    Array<FieldOrigin>& out;
};

Status explain_record(ExplainContext& context, FieldOrigin base, reflect::TypeId type,
                      const serialize::ValueRecord& record) noexcept {
    base.type = type;
    const reflect::TypeInfo* info = find_type(context.options, type);
    for (const serialize::FieldValue& value : record.fields()) {
        FieldOrigin origin = base;
        origin.field = value.id;
        origin.wire = value.wire;
        describe_field(origin, info);
        date_origin(origin, &record, context.inspection.generation,
                    context.inspection.overlay.simulation_point(), context.history);
        if (Status pushed = context.out.push_back(origin); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status explain_entry(ExplainContext& context, const Region& region, const Entry& entry) noexcept {
    FieldOrigin base;
    base.scope = Scope::World;
    base.region = region.key;
    base.entity = entry.id;
    base.kind = entry.kind;
    if (entry.kind != EntryKind::Modified) {
        FieldOrigin record = base;
        record.reason = FieldReason::EntryRecord;
        date_origin(record, nullptr, context.inspection.generation,
                    context.inspection.overlay.simulation_point(), context.history);
        if (Status pushed = context.out.push_back(record); !pushed) {
            return pushed;
        }
    }
    for (const ComponentDelta& component : entry.components) {
        if (Status explained = explain_record(context, base, component.type, component.record);
            !explained) {
            return explained;
        }
    }
    return ok();
}

}  // namespace

Status explain_fields(SaveArchive& archive, const SaveInspection& inspection,
                      const InspectOptions& options, Array<FieldOrigin>& out) noexcept {
    out.clear();
    History history(inspection.overlay.allocator());
    if (Status loaded = load_history(archive, inspection, options, history); !loaded) {
        return loaded;
    }
    ExplainContext context{inspection, options, history, out};
    for (const Region& region : inspection.overlay.regions()) {
        for (const Entry& entry : region.entries) {
            if (Status explained = explain_entry(context, region, entry); !explained) {
                return explained;
            }
        }
    }
    for (const Fragment& fragment : inspection.overlay.fragments()) {
        FieldOrigin base;
        base.scope = fragment.scope;
        base.region = kGlobalRegion;
        base.fragment = true;
        if (Status explained = explain_record(context, base, fragment.type, fragment.record);
            !explained) {
            return explained;
        }
    }
    return ok();
}

}  // namespace cy::save
