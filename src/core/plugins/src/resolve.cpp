// `resolve.h` — resolution into a concrete set, the lockfile, and the conflict report.

#include <cy/core/plugins/resolve.h>
#include <cy/core/serialize/text.h>

#include <algorithm>
#include <cstdio>

namespace cy::plugins {
namespace {

using cy::serialize::TextLine;
using cy::serialize::TextScanner;
using cy::serialize::TextWriter;

[[nodiscard]] const PluginManifest* find_manifest(Span<const PluginManifest* const> available,
                                                  PluginId plugin) noexcept {
    for (const PluginManifest* manifest : available) {
        if (manifest != nullptr && manifest->id == plugin) {
            return manifest;
        }
    }
    return nullptr;
}

/// Who required `subject`, and with what. Filled in as the walk discovers requirements so that a
/// conflict can name two of them rather than the last one.
struct Requirement {
    PluginId requirer;
    PluginId subject;
    VersionConstraint constraint;
};

[[nodiscard]] bool contains_id(Span<const PluginId> set, PluginId plugin) noexcept {
    return std::ranges::any_of(set, [plugin](const PluginId& member) { return member == plugin; });
}

/// A depth-first post-order over the dependency graph, which is a topological order with dependants
/// after their dependencies. Children are visited in the order the manifest declares, which is
/// sorted by the caller before the walk begins — that is the tie-break that makes the order
/// deterministic rather than merely topological.
[[nodiscard]] Status visit(Span<const PluginManifest* const> available, PluginId plugin,
                           Array<PluginId>& order, Array<PluginId>& active,
                           Array<Requirement>& requirements) noexcept {
    if (contains_id(order.span(), plugin)) {
        return ok();
    }
    if (contains_id(active.span(), plugin)) {
        return fail(ErrorCode::InvalidArgument, "a plugin dependency cycle");
    }
    const PluginManifest* manifest = find_manifest(available, plugin);
    if (manifest == nullptr) {
        return fail(ErrorCode::NotFound, "a required plugin is not available");
    }
    if (Status pushed = active.push_back(plugin); !pushed) {
        return pushed;
    }

    // Dependencies in sorted order, so two hosts that declared them differently walk them the same.
    Array<PluginId> children(order.allocator());
    for (const PluginDependency& dependency : manifest->dependencies()) {
        if (Status recorded = requirements.push_back(
                Requirement{plugin, dependency.plugin, dependency.constraint});
            !recorded) {
            return recorded;
        }
        usize at = 0;
        while (at < children.size() && children[at].text() < dependency.plugin.text()) {
            at += 1;
        }
        if (Status grown = children.push_back(dependency.plugin); !grown) {
            return grown;
        }
        for (usize index = children.size() - 1; index > at; --index) {
            const PluginId moved = children[index - 1];
            children[index - 1] = children[index];
            children[index] = moved;
        }
    }
    for (const PluginId& child : children) {
        if (Status walked = visit(available, child, order, active, requirements); !walked) {
            return walked;
        }
    }

    active.pop_back();
    return order.push_back(plugin);
}

}  // namespace

const LockedPlugin* Lockfile::find(PluginId plugin) const noexcept {
    for (const LockedPlugin& entry : entries_) {
        if (entry.plugin == plugin) {
            return &entry;
        }
    }
    return nullptr;
}

Status Lockfile::record(const LockedPlugin& entry) noexcept {
    for (LockedPlugin& existing : entries_) {
        if (existing.plugin == entry.plugin) {
            existing = entry;
            return ok();
        }
    }
    // Sorted by the plugin's TEXT rather than by its `Name` index: an index is interning order,
    // which is the order this process happened to meet the strings in, and a lockfile ordered by
    // that would differ between two runs that read the same manifests in a different order.
    usize at = 0;
    while (at < entries_.size() && entries_[at].plugin.text() < entry.plugin.text()) {
        at += 1;
    }
    if (Status grown = entries_.push_back(entry); !grown) {
        return grown;
    }
    for (usize index = entries_.size() - 1; index > at; --index) {
        const LockedPlugin moved = entries_[index - 1];
        entries_[index - 1] = entries_[index];
        entries_[index] = moved;
    }
    return ok();
}

Status write_lockfile(const Lockfile& lock, Array<char>& out) noexcept {
    TextWriter writer(out);
    if (Status began = writer.begin_line(0); !began) {
        return began;
    }
    if (Status word = writer.word("cyplugin-lock"); !word) {
        return word;
    }
    if (Status word = writer.word_u64(1); !word) {
        return word;
    }
    if (Status ended = writer.end_line(); !ended) {
        return ended;
    }
    for (const LockedPlugin& entry : lock.entries()) {
        if (Status began = writer.begin_line(0); !began) {
            return began;
        }
        if (Status word = writer.word("plugin"); !word) {
            return word;
        }
        if (Status word = writer.word(entry.plugin.text()); !word) {
            return word;
        }
        char version[48] = {};
        const int written = std::snprintf(version, sizeof(version), "%u.%u.%u", entry.version.major,
                                          entry.version.minor, entry.version.patch);
        if (written <= 0) {
            return fail(ErrorCode::Internal, "a version could not be written");
        }
        if (Status word = writer.word(std::string_view(version, static_cast<usize>(written)));
            !word) {
            return word;
        }
        if (Status word = writer.word_u64(entry.content_hash); !word) {
            return word;
        }
        if (Status ended = writer.end_line(); !ended) {
            return ended;
        }
    }
    return ok();
}

Status read_lockfile(std::string_view text, Lockfile& out) noexcept {
    TextScanner scanner(text);
    const Expected<TextLine, Error> head = scanner.next();
    if (!head || head->word(0) != "cyplugin-lock") {
        return fail(ErrorCode::InvalidArgument, "a lockfile does not begin with 'cyplugin-lock'");
    }
    for (;;) {
        const Expected<TextLine, Error> line = scanner.next();
        if (!line) {
            if (line.error().code == ErrorCode::NotFound) {
                break;
            }
            return make_unexpected(line.error());
        }
        if (line->word(0) != "plugin" || line->count() < 4) {
            return fail(ErrorCode::InvalidArgument,
                        "a lockfile entry names a plugin, a version and a content hash");
        }
        const Expected<Version, Error> version = parse_version(line->word(2));
        if (!version) {
            return make_unexpected(version.error());
        }
        const Expected<u64, Error> hash = line->word_u64(3);
        if (!hash) {
            return make_unexpected(hash.error());
        }
        if (Status recorded =
                out.record(LockedPlugin{Name::intern(line->word(1)), *version, *hash});
            !recorded) {
            return recorded;
        }
    }
    return ok();
}

Expected<Resolution, Error> resolve(Span<const PluginManifest* const> available,
                                    Span<const PluginId> requested, Allocator& allocator) noexcept {
    Resolution resolution(allocator);
    Array<PluginId> active(allocator);
    Array<Requirement> requirements(allocator);

    // The roots are visited in sorted order too, for the same reason the children are.
    Array<PluginId> roots(allocator);
    for (const PluginId& plugin : requested) {
        usize at = 0;
        while (at < roots.size() && roots[at].text() < plugin.text()) {
            at += 1;
        }
        if (Status grown = roots.push_back(plugin); !grown) {
            return make_unexpected(grown.error());
        }
        for (usize index = roots.size() - 1; index > at; --index) {
            const PluginId moved = roots[index - 1];
            roots[index - 1] = roots[index];
            roots[index] = moved;
        }
    }
    for (const PluginId& plugin : roots) {
        if (Status walked = visit(available, plugin, resolution.order_, active, requirements);
            !walked) {
            return make_unexpected(walked.error());
        }
    }

    // Every requirement against the one version present. The first failure is reported WITH a
    // second requirement on the same plugin where one exists, because "two plugins require
    // incompatible versions of a third" is the scenario and one requirement explains half of it.
    for (const Requirement& requirement : requirements) {
        const PluginManifest* manifest = find_manifest(available, requirement.subject);
        if (manifest == nullptr) {
            return fail(ErrorCode::NotFound, "a required plugin is not available");
        }
        if (requirement.constraint.satisfied_by(manifest->version)) {
            continue;
        }
        ResolutionConflict conflict;
        conflict.subject = requirement.subject;
        conflict.available = manifest->version;
        conflict.has_available = true;
        conflict.first_requirer = requirement.requirer;
        conflict.first_constraint = requirement.constraint;
        for (const Requirement& other : requirements) {
            if (other.subject == requirement.subject && !(other.requirer == requirement.requirer)) {
                conflict.second_requirer = other.requirer;
                conflict.second_constraint = other.constraint;
                conflict.has_second = true;
                break;
            }
        }
        resolution.conflict_ = conflict;
        resolution.resolved_ = false;
        return resolution;
    }

    resolution.resolved_ = true;
    return resolution;
}

Status resolve_against_lock(Span<const PluginManifest* const> available,
                            const Resolution& resolution, const Lockfile& lock,
                            Array<char>& diagnostic) noexcept {
    if (!resolution.resolved()) {
        return fail(ErrorCode::InvalidArgument,
                    "an unresolved set cannot be checked against a lock");
    }
    for (const PluginId& plugin : resolution.order()) {
        const LockedPlugin* locked = lock.find(plugin);
        if (locked == nullptr) {
            if (Status appended =
                    diagnostic.append(Span<const char>(plugin.text().data(), plugin.text().size()));
                !appended) {
                return appended;
            }
            return fail(ErrorCode::NotFound,
                        "a resolved plugin is not in the lockfile; update it deliberately");
        }
        const PluginManifest* manifest = find_manifest(available, plugin);
        if (manifest != nullptr && !(manifest->version == locked->version)) {
            if (Status appended =
                    diagnostic.append(Span<const char>(plugin.text().data(), plugin.text().size()));
                !appended) {
                return appended;
            }
            return fail(ErrorCode::InvalidArgument,
                        "a plugin's resolved version differs from the lockfile's");
        }
    }
    return ok();
}

}  // namespace cy::plugins
