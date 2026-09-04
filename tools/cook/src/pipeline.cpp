#include <cy/cook/pipeline.h>

#include <cy/core/assets/cooked.h>
#include <cy/scene/serialization/format.h>

#include <cstring>
#include <utility>

namespace cy::cook {
namespace {

using scene::serialization::AssetKind;
using scene::serialization::ComponentLayoutTable;
using scene::serialization::CookedAsset;
using scene::serialization::CookOptions;
using scene::serialization::CookReport;
using scene::serialization::Document;
using scene::serialization::Library;

/// True when `path` ends with `suffix`.
[[nodiscard]] bool ends_with(std::string_view path, std::string_view suffix) noexcept {
    return path.size() >= suffix.size() && path.substr(path.size() - suffix.size()) == suffix;
}

/// What the enumeration collects: the paths to read, in the order the walk reported them.
struct Collector {
    Array<assets::VirtualPath>* paths = nullptr;
    bool failed = false;
};

bool collect(void* user, const assets::VirtualEntry& entry) noexcept {
    Collector& collector = *static_cast<Collector*>(user);
    if (entry.is_directory || entry.path == nullptr) {
        return true;
    }
    const std::string_view path = entry.path->view();
    if (!ends_with(path, kSceneExtension) && !ends_with(path, kPrefabExtension)) {
        return true;
    }
    if (Status added = collector.paths->push_back(*entry.path); !added) {
        collector.failed = true;
        return false;
    }
    return true;
}

/// The assets cooking one document depends on: every instance's source, and a variant's base.
[[nodiscard]] Status dependencies_of(const Document& document, Array<AssetId>& out) noexcept {
    return scene::serialization::direct_dependencies(document, out);
}

/// The engine's own asset kind for a document kind. A cooked scene and a cooked prefab are
/// different things to the loader, which is the distinction `serialization-and-prefabs` refuses to
/// collapse; a world cooks to a scene entry until the world cooker lands at M6.
[[nodiscard]] assets::AssetKind package_kind_of(AssetKind kind) noexcept {
    return (kind == AssetKind::Prefab) ? assets::AssetKind::Prefab : assets::AssetKind::Scene;
}

}  // namespace

Status read_documents(assets::VirtualFileSystem& vfs, const assets::VirtualPath& source,
                      Array<Document>& out) noexcept {
    Array<assets::VirtualPath> paths(out.allocator());
    Collector collector{&paths};
    if (Status walked = vfs.enumerate(source, /*recursive=*/true, &collect, &collector); !walked) {
        return walked;
    }
    if (collector.failed) {
        return fail(ErrorCode::OutOfMemory, "could not record the documents to read");
    }
    // The walk is already sorted (`VirtualFileSystem::enumerate` says so), which is what makes two
    // runs over one tree produce the same package byte for byte.

    Array<u8> bytes(out.allocator());
    for (const assets::VirtualPath& path : paths) {
        if (Status read = vfs.read(path, bytes); !read) {
            return read;
        }
        Document document(out.allocator());
        const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (Status parsed = scene::serialization::read_text(text, document); !parsed) {
            return parsed;
        }
        if (document.id.is_nil()) {
            return fail(ErrorCode::InvalidArgument,
                        "an authoring document carries no asset id; it cannot be cooked");
        }
        if (Status added = out.push_back(std::move(document)); !added) {
            return added;
        }
    }
    return ok();
}

Status cook_documents(const CookRequest& request, Array<Document>& documents,
                      assets::PackageWriter& package, CookRunReport& report) noexcept {
    if (request.world == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "a cook needs the world whose components define the layouts it emits");
    }

    Allocator& allocator = documents.allocator();
    ComponentLayoutTable layouts(allocator);
    if (Status described = scene::serialization::describe_from_world(*request.world, layouts);
        !described) {
        return described;
    }

    Library library(allocator);
    for (Document& document : documents) {
        if (Status added = library.add(document); !added) {
            return added;
        }
    }
    report.documents_read = static_cast<u32>(documents.size());

    // Cycles are rejected before a single asset is cooked, and reported as a chain. A package half
    // of whose assets were cooked from a graph the other half could not be is worse than none.
    if (Status validated = library.validate(report.cycle); !validated) {
        return validated;
    }

    CookOptions options;
    options.layouts = &layouts;
    options.resolve.transform = request.transform;
    options.retain_provenance = !request.shipping;
    options.fail_on_conflicts = request.fail_on_conflicts;

    Array<AssetId> dependencies(allocator);
    Array<u8> wrapped(allocator);
    for (const Document& document : documents) {
        CookedAsset cooked(allocator);
        CookReport cook_report(allocator);
        if (Status baked =
                scene::serialization::cook(library, document.id, options, cooked, cook_report);
            !baked) {
            return baked;
        }

        wrapped.clear();
        if (Status written = assets::write_cooked_asset(
                package_kind_of(document.kind), request.variant, cooked.stream().span(), wrapped);
            !written) {
            return written;
        }

        if (Status listed = dependencies_of(document, dependencies); !listed) {
            return listed;
        }

        assets::PackageWriter::EntryOptions entry;
        entry.kind = package_kind_of(document.kind);
        // A cooked block is packed component data: compressible, and worth compressing, because a
        // cell of ten thousand transforms is mostly small floats.
        entry.form = assets::PayloadForm::Compressible;
        if (Status added = package.add(document.id, request.variant, wrapped.span(), entry,
                                       dependencies.span());
            !added) {
            return added;
        }

        CookedDocumentReport entry_report;
        entry_report.id = document.id;
        entry_report.kind = document.kind;
        entry_report.entities = cook_report.entities;
        entry_report.blocks = cook_report.blocks;
        entry_report.relationships_flattened = cook_report.relationships_flattened;
        entry_report.relationships_retained = cook_report.relationships_retained;
        entry_report.reference_sites = cook_report.reference_sites;
        entry_report.conflicts = static_cast<u32>(cook_report.resolve.conflicts.size());
        entry_report.payload_bytes = cook_report.payload_bytes;
        entry_report.cooked_bytes = static_cast<u32>(wrapped.size());
        if (Status recorded = report.documents.push_back(entry_report); !recorded) {
            return recorded;
        }

        ++report.documents_cooked;
        report.total_entities += entry_report.entities;
        report.total_relationships_flattened += entry_report.relationships_flattened;
        report.total_conflicts += entry_report.conflicts;
    }
    return ok();
}

Status run(assets::VirtualFileSystem& vfs, const CookRequest& request, const char* destination,
           CookRunReport& report) noexcept {
    Array<Document> documents(report.documents.allocator());
    if (Status read = read_documents(vfs, request.source, documents); !read) {
        return read;
    }

    assets::PackageWriter package;
    assets::PackageManifest manifest;
    if (Status named = manifest.set_bundle("cooked"); !named) {
        return named;
    }
    if (Status set = package.set_manifest(manifest); !set) {
        return set;
    }
    if (Status cooked = cook_documents(request, documents, package, report); !cooked) {
        return cooked;
    }
    return package.write(destination);
}

}  // namespace cy::cook
