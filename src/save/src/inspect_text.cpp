// SPDX-License-Identifier: MIT
// The inspector's text: what `cy_save_inspect` prints. See inspect.h.
//
// Line-oriented and stable, so a person can read it and a test or a script can grep it: every line
// begins with a word naming what it is, and identifiers are printed in their canonical text forms
// — 32 hex digits for an entity, 64 for a content hash — so a line can be searched for by the
// identifier a log or a crash report printed.

#include <cy/save/inspect.h>

#include <cstdarg>
#include <cstdio>

namespace cy::save {
namespace {

/// Append formatted text. Truncation is refused rather than written: a report missing the end of a
/// line would read as a report saying something else.
[[gnu::format(printf, 2, 3)]] Status appendf(Array<char>& out, const char* format, ...) noexcept {
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    if (written < 0 || static_cast<usize>(written) >= sizeof(line)) {
        return fail(ErrorCode::BufferTooSmall, "an inspector line does not fit");
    }
    return out.append(Span<const char>(line, static_cast<usize>(written)));
}

struct IdText {
    char text[PersistentId::kTextLength + 1] = {};
};

IdText id_text(PersistentId id) noexcept {
    IdText out;
    id.format(out.text);
    return out;
}

struct HashText {
    char text[assets::ContentHash::kTextLength + 1] = {};
};

HashText hash_text(const assets::ContentHash& hash) noexcept {
    HashText out;
    hash.format(out.text);
    return out;
}

using u64_print = unsigned long long;

Status render_manifest(const SaveInspection& inspection, Array<char>& out) noexcept {
    const Manifest& manifest = inspection.manifest;
    char save_id[AssetId::kTextLength + 1] = {};
    char campaign[AssetId::kTextLength + 1] = {};
    char project[AssetId::kTextLength + 1] = {};
    (void)manifest.save.format(save_id);
    (void)manifest.campaign.format(campaign);
    (void)manifest.project.format(project);
    const HashText content = hash_text(manifest.content_version);
    const HashText plugins = hash_text(manifest.plugin_version);
    Status status = appendf(out,
                            "manifest generation=%u format=%u build=%s\n"
                            "manifest project=%s save=%s campaign=%s\n"
                            "manifest simulation-point=%llu session-seed=%llu progress=%llu\n"
                            "manifest content=%s plugins=%s\n",
                            manifest.generation, static_cast<unsigned>(manifest.format_version),
                            manifest.build_id, project, save_id, campaign,
                            static_cast<u64_print>(manifest.simulation_point),
                            static_cast<u64_print>(manifest.session_seed),
                            static_cast<u64_print>(manifest.progress), content.text, plugins.text);
    for (const PluginRequirement& plugin : manifest.plugins) {
        if (!status) {
            return status;
        }
        status =
            appendf(out, "manifest requires-plugin=%s version=%u\n", plugin.name, plugin.version);
    }
    for (const u32 generation : inspection.generations) {
        if (!status) {
            return status;
        }
        status = appendf(out, "retained generation=%u%s\n", generation,
                         generation == inspection.generation ? " (inspected)" : "");
    }
    return status;
}

Status render_restore(const SaveInspection& inspection, Array<char>& out) noexcept {
    const LoadReport& report = inspection.restore;
    if (!report.failed()) {
        return appendf(out, "restore ok records=%u migrated=%u preserved-unknown=%u\n",
                       report.records_read, report.records_migrated, report.records_unknown);
    }
    const HashText chunk = hash_text(report.chunk);
    Status status = appendf(out, "restore refused reason=%s detail=\"%s\"",
                            load_failure_name(report.failure), report.detail);
    if (status && report.subject[0] != '\0') {
        status = appendf(out, " names=\"%s\" version=%u", report.subject, report.subject_version);
    }
    if (status && !report.chunk.is_zero()) {
        status = appendf(out, " chunk=%s", chunk.text);
    }
    if (status && inspection.contents.failed()) {
        status =
            appendf(out, "\ncontents unreadable reason=%s detail=\"%s\"",
                    load_failure_name(inspection.contents.failure), inspection.contents.detail);
    }
    return status ? appendf(out, "\n") : status;
}

Status render_usage(const SaveInspection& inspection, Array<char>& out) noexcept {
    Status status = appendf(out, "entities modified=%u created=%u tombstoned=%u fragments=%u\n",
                            inspection.modified, inspection.created, inspection.tombstoned,
                            inspection.fragments);
    for (const ScopeUsage& scope : inspection.scopes) {
        if (!status) {
            return status;
        }
        status =
            appendf(out, "size scope=%s chunks=%u records=%u bytes=%llu\n", scope_name(scope.scope),
                    scope.chunks, scope.records, static_cast<u64_print>(scope.bytes));
    }
    for (const RegionUsage& region : inspection.regions) {
        if (!status) {
            return status;
        }
        status =
            appendf(out, "size region=%016llx modified=%u created=%u tombstoned=%u bytes=%llu\n",
                    static_cast<u64_print>(region.region.value()), region.modified, region.created,
                    region.tombstoned, static_cast<u64_print>(region.bytes));
    }
    for (const ComponentUsage& component : inspection.components) {
        if (!status) {
            return status;
        }
        status =
            appendf(out, "size component=%u name=%s module=%s records=%u fields=%u bytes=%llu\n",
                    component.type.value(), component.name[0] != '\0' ? component.name : "-",
                    component.module, component.records, component.fields,
                    static_cast<u64_print>(component.bytes));
    }
    for (const PluginUsage& plugin : inspection.plugins) {
        if (!status) {
            return status;
        }
        status = appendf(out, "size plugin=%s records=%u bytes=%llu\n", plugin.module,
                         plugin.records, static_cast<u64_print>(plugin.bytes));
    }
    return status ? appendf(out, "size total bytes=%llu\n",
                            static_cast<u64_print>(inspection.total_bytes()))
                  : status;
}

/// The traits a field carries, in the specification's vocabulary, joined with '+'.
Status append_traits(Array<char>& out, Trait traits) noexcept {
    struct Named {
        Trait trait;
        const char* name;
    };
    static constexpr Named kNames[] = {
        {Trait::Authoring, "authoring"},   {Trait::SaveGame, "save-game"},
        {Trait::Profile, "profile"},       {Trait::Replicated, "replicated"},
        {Trait::ReplayRelevant, "replay"}, {Trait::RuntimeOnly, "runtime-only"},
        {Trait::Derived, "derived"},
    };
    bool first = true;
    Status status = ok();
    for (const Named& named : kNames) {
        if (status && has_trait(traits, named.trait)) {
            status = appendf(out, "%s%s", first ? "" : "+", named.name);
            first = false;
        }
    }
    return status && first ? appendf(out, "none") : status;
}

Status render_origin(const FieldOrigin& origin, Array<char>& out) noexcept {
    const IdText entity = id_text(origin.entity);
    Status status =
        origin.fragment
            ? appendf(out, "why scope=%s fragment", scope_name(origin.scope))
            : appendf(out, "why scope=%s region=%016llx entity=%s kind=%s",
                      scope_name(origin.scope), static_cast<u64_print>(origin.region.value()),
                      entity.text, entry_kind_name(origin.kind));
    if (status && origin.reason != FieldReason::EntryRecord) {
        status = appendf(
            out, " component=%u:%s field=%u:%s wire=%s module=%s trait=", origin.type.value(),
            origin.component[0] != '\0' ? origin.component : "-", origin.field.value(),
            origin.field_name[0] != '\0' ? origin.field_name : "-",
            serialize::wire_type_name(origin.wire), origin.module);
        status = status ? append_traits(out, origin.traits) : status;
    }
    if (status) {
        status =
            appendf(out, " reason=%s dirty-since=%s%u tick=%llu\n",
                    field_reason_name(origin.reason), origin.since_oldest_retained ? "<=" : "",
                    origin.dirty_since_generation, static_cast<u64_print>(origin.dirty_since_tick));
    }
    return status;
}

const char* type_name(const reflect::TypeRegistry* types, reflect::TypeId type) noexcept {
    const reflect::TypeInfo* info = types == nullptr ? nullptr : types->find(type);
    return info == nullptr ? "-" : info->name;
}

const char* field_name(const reflect::TypeRegistry* types, reflect::TypeId type,
                       reflect::FieldId field) noexcept {
    const reflect::TypeInfo* info = types == nullptr ? nullptr : types->find(type);
    const reflect::FieldInfo* found = info == nullptr ? nullptr : info->find_field(field);
    return found == nullptr ? "-" : found->name;
}

}  // namespace

Status render_inspection(const SaveInspection& inspection, const InspectOptions& options,
                         Array<char>& out) noexcept {
    (void)options;
    if (Status rendered = render_manifest(inspection, out); !rendered) {
        return rendered;
    }
    if (Status rendered = render_restore(inspection, out); !rendered) {
        return rendered;
    }
    return render_usage(inspection, out);
}

Status render_origins(Span<const FieldOrigin> origins, Array<char>& out) noexcept {
    for (const FieldOrigin& origin : origins) {
        if (Status rendered = render_origin(origin, out); !rendered) {
            return rendered;
        }
    }
    return ok();
}

Status render_diff(const SaveDiff& diff, const reflect::TypeRegistry* types,
                   Array<char>& out) noexcept {
    for (const DiffItem& item : diff.items) {
        const IdText entity = id_text(item.entity);
        Status status = appendf(out, "diff %s scope=%s region=%016llx entity=%s",
                                diff_kind_name(item.kind), scope_name(item.scope),
                                static_cast<u64_print>(item.region.value()), entity.text);
        if (status && item.type.valid()) {
            status =
                appendf(out, " component=%u:%s", item.type.value(), type_name(types, item.type));
        }
        if (status && item.field.valid()) {
            status = appendf(out, " field=%u:%s", item.field.value(),
                             field_name(types, item.type, item.field));
        }
        status = status ? appendf(out, "\n") : status;
        if (!status) {
            return status;
        }
    }
    return appendf(out, "diff total=%llu\n", static_cast<u64_print>(diff.items.size()));
}

}  // namespace cy::save
