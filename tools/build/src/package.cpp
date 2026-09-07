#include <cy/build/package.h>

#include <algorithm>
#include <ranges>

#include "text.h"

namespace cy::build {
namespace {

[[nodiscard]] Error invalid(const char* message) noexcept {
    return Error{ErrorCode::InvalidArgument, message, 0};
}

[[nodiscard]] std::string hash_text(const assets::ContentHash& hash) {
    char text[assets::ContentHash::kTextLength + 1] = {};
    hash.format(text);
    return {static_cast<const char*>(text)};
}

void append_number(std::string& out, u64 value) {
    char buffer[24] = {};
    usize length = 0;
    if (value == 0) {
        buffer[length++] = '0';
    }
    while (value != 0) {
        buffer[length++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    for (usize index = length; index > 0; --index) {
        out.push_back(buffer[index - 1]);
    }
}

[[nodiscard]] Expected<u64, Error> parse_number(std::string_view word) {
    if (word.empty()) {
        return make_unexpected(invalid("expected a number"));
    }
    u64 value = 0;
    for (const char character : word) {
        if (character < '0' || character > '9') {
            return make_unexpected(invalid("expected a number"));
        }
        value = (value * 10) + static_cast<u64>(character - '0');
    }
    return value;
}

}  // namespace

u64 Bundle::size() const noexcept {
    u64 total = 0;
    for (const PackageEntry& entry : entries) {
        total += entry.size;
    }
    return total;
}

const Bundle* PackageSet::bundle(std::string_view name) const noexcept {
    for (const Bundle& candidate : bundles) {
        if (candidate.name == name) {
            return &candidate;
        }
    }
    return nullptr;
}

std::vector<PackageEntry> PackageSet::entries() const {
    std::vector<PackageEntry> all;
    for (const Bundle& bundle : bundles) {
        all.insert(all.end(), bundle.entries.begin(), bundle.entries.end());
    }
    std::ranges::sort(all,
                      [](const PackageEntry& a, const PackageEntry& b) { return a.name < b.name; });
    return all;
}

u64 PackageSet::size() const noexcept {
    u64 total = 0;
    for (const Bundle& bundle : bundles) {
        total += bundle.size();
    }
    return total;
}

namespace {

/// The build's identity: a digest over the manifest's own sorted content. Content-derived, so two
/// identical builds have one identity and the patch between them is empty — which is the property
/// that makes `applies_to` meaningful.
[[nodiscard]] std::string compute_build_id(const PackageSet& packages) {
    assets::ContentHasher hasher;
    for (const Bundle& bundle : packages.bundles) {
        const u64 name_length = bundle.name.size();
        hasher.update(&name_length, sizeof(name_length));
        hasher.update(bundle.name.data(), bundle.name.size());
        for (const PackageEntry& entry : bundle.entries) {
            const u64 entry_length = entry.name.size();
            hasher.update(&entry_length, sizeof(entry_length));
            hasher.update(entry.name.data(), entry.name.size());
            hasher.update(entry.digest.bytes, assets::ContentHash::kByteLength);
        }
    }
    return hash_text(hasher.finish());
}

void sort_packages(PackageSet& packages) {
    for (Bundle& bundle : packages.bundles) {
        std::ranges::sort(bundle.entries, [](const PackageEntry& a, const PackageEntry& b) {
            return a.name < b.name;
        });
    }
    std::ranges::sort(packages.bundles,
                      [](const Bundle& a, const Bundle& b) { return a.name < b.name; });
}

[[nodiscard]] Bundle& bundle_for(PackageSet& packages, const std::string& name) {
    for (Bundle& candidate : packages.bundles) {
        if (candidate.name == name) {
            return candidate;
        }
    }
    packages.bundles.push_back(Bundle{name, {}});
    return packages.bundles.back();
}

}  // namespace

Expected<PackageSet, Error> assemble(const BuildGraph& graph, const BuildReport& report,
                                     Provenance provenance) {
    // Validation before packaging, not after. A failed node means an output nothing produced, and
    // discovering that after writing a package is the failure the specification names.
    if (report.failed != 0) {
        return make_unexpected(invalid("the build failed; nothing is packaged"));
    }
    if (report.cancelled) {
        return make_unexpected(invalid("the build was cancelled; nothing is packaged"));
    }

    PackageSet packages;
    packages.provenance = std::move(provenance);
    for (const NodeResult& result : report.nodes) {
        const NodeId id = graph.find(result.name);
        if (id == NodeId::Invalid) {
            return make_unexpected(
                Error{ErrorCode::NotFound, "the report names an absent node", 0});
        }
        const NodeDesc& node = graph.node(id);
        const std::string name = node.bundle.empty() ? std::string(kBaseBundle) : node.bundle;
        Bundle& bundle = bundle_for(packages, name);
        for (const NodeOutput& output : result.outputs) {
            bundle.entries.push_back(
                PackageEntry{output.name, output.digest, output.size, node.name});
        }
    }

    sort_packages(packages);
    packages.build_id = compute_build_id(packages);
    return packages;
}

std::string write_package(const PackageSet& packages) {
    std::string out = "cypackage 1\n";
    out += "build ";
    out += packages.build_id;
    out += "\nprovenance\n";
    const std::pair<const char*, const std::string*> fields[] = {
        {"project", &packages.provenance.project},     {"revision", &packages.provenance.revision},
        {"platform", &packages.provenance.platform},   {"profile", &packages.provenance.profile},
        {"toolchain", &packages.provenance.toolchain},
    };
    for (const auto& field : fields) {
        out += "  ";
        out += field.first;
        out += ' ';
        out += text::quote(*field.second);
        out += '\n';
    }
    out += "  content-version ";
    append_number(out, packages.provenance.content_version);
    out += '\n';

    for (const Bundle& bundle : packages.bundles) {
        out += "bundle ";
        out += text::quote(bundle.name);
        out += '\n';
        for (const PackageEntry& entry : bundle.entries) {
            out += "  chunk ";
            out += text::quote(entry.name);
            out += ' ';
            out += hash_text(entry.digest);
            out += ' ';
            append_number(out, entry.size);
            out += ' ';
            out += text::quote(entry.node);
            out += '\n';
        }
    }
    return out;
}

namespace {

[[nodiscard]] Status read_provenance_field(const text::Line& line, Provenance& provenance) {
    const std::string_view keyword = line.word(0);
    const std::string value(line.word(1));
    if (keyword == "project") {
        provenance.project = value;
    } else if (keyword == "revision") {
        provenance.revision = value;
    } else if (keyword == "platform") {
        provenance.platform = value;
    } else if (keyword == "profile") {
        provenance.profile = value;
    } else if (keyword == "toolchain") {
        provenance.toolchain = value;
    } else if (keyword == "content-version") {
        const Expected<u64, Error> parsed = parse_number(line.word(1));
        if (!parsed) {
            return make_unexpected(parsed.error());
        }
        provenance.content_version = static_cast<u32>(*parsed);
    }
    return ok();
}

[[nodiscard]] Status read_chunk(const text::Line& line, Bundle& bundle) {
    if (line.words.size() < 5) {
        return make_unexpected(invalid("a chunk needs a name, a digest, a size and a node"));
    }
    const Expected<assets::ContentHash, Error> digest = assets::ContentHash::parse(line.word(2));
    if (!digest) {
        return make_unexpected(digest.error());
    }
    const Expected<u64, Error> size = parse_number(line.word(3));
    if (!size) {
        return make_unexpected(size.error());
    }
    bundle.entries.push_back(
        PackageEntry{std::string(line.word(1)), *digest, *size, std::string(line.word(4))});
    return ok();
}

}  // namespace

Expected<PackageSet, Error> read_package(std::string_view document) {
    const Expected<std::vector<text::Line>, Error> lines = text::read(document);
    if (!lines) {
        return make_unexpected(lines.error());
    }
    if (lines->empty() || (*lines)[0].word(0) != "cypackage" || (*lines)[0].word(1) != "1") {
        return make_unexpected(invalid("not a cypackage 1 document"));
    }

    PackageSet packages;
    bool in_provenance = false;
    for (usize index = 1; index < lines->size(); ++index) {
        const text::Line& line = (*lines)[index];
        const std::string_view keyword = line.word(0);
        if (line.depth == 0) {
            in_provenance = keyword == "provenance";
            if (keyword == "build") {
                packages.build_id = std::string(line.word(1));
            } else if (keyword == "bundle") {
                packages.bundles.push_back(Bundle{std::string(line.word(1)), {}});
            }
            continue;
        }
        if (in_provenance) {
            if (Status read = read_provenance_field(line, packages.provenance); !read) {
                return make_unexpected(read.error());
            }
            continue;
        }
        if (packages.bundles.empty() || keyword != "chunk") {
            return make_unexpected(invalid("a chunk outside a bundle"));
        }
        if (Status read = read_chunk(line, packages.bundles.back()); !read) {
            return make_unexpected(read.error());
        }
    }
    return packages;
}

std::string bundle_report(const PackageSet& packages, u32 top) {
    std::string out;
    for (const Bundle& bundle : packages.bundles) {
        out += bundle.name;
        out += "  ";
        append_number(out, bundle.size());
        out += " bytes in ";
        append_number(out, bundle.entries.size());
        out += " chunks\n";

        std::vector<const PackageEntry*> largest;
        largest.reserve(bundle.entries.size());
        for (const PackageEntry& entry : bundle.entries) {
            largest.push_back(&entry);
        }
        // By size, then by name: two entries of one size must not swap places between two runs, or
        // a report cannot be diffed between builds.
        std::ranges::sort(largest, [](const PackageEntry* a, const PackageEntry* b) {
            return a->size != b->size ? a->size > b->size : a->name < b->name;
        });
        for (usize index = 0; index < largest.size() && index < top; ++index) {
            out += "    ";
            append_number(out, largest[index]->size);
            out += "  ";
            out += largest[index]->name;
            out += '\n';
        }
    }
    return out;
}

Expected<AuditAnswer, Error> audit(const BuildGraph& graph, std::string_view node) {
    const NodeId id = graph.find(node);
    if (id == NodeId::Invalid) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such node", 0});
    }
    AuditAnswer answer;
    for (const NodeId step : graph.reference_chain(id)) {
        answer.chain.push_back(graph.node(step).name);
    }
    for (const NodeId dependent : graph.dependents(id)) {
        answer.dependents.push_back(graph.node(dependent).name);
    }
    return answer;
}

}  // namespace cy::build
