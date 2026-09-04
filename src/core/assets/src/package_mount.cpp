// A package inside the virtual filesystem. Tasks 3.3.2 and 3.3.3.
//
// See the note above `kPackageMountRoot` in package.h for why a package appears in the path
// namespace at all: it puts patch masking and mount priority on the same machinery as every other
// mount, rather than giving packages a second resolution order that could disagree with it.

#include <cy/core/assets/package.h>

#include <cstdio>
#include <cstring>

namespace cy::assets {
namespace {

/// Parse `packaged/<32 hex>[.<variant>]` back into an id and a variant. The inverse of
/// `package_entry_path`, and the only place the spelling is decoded.
struct ParsedEntryPath {
    cy::AssetId id;
    VariantKey variant;
};

Expected<ParsedEntryPath, Error> parse_entry_path(const VirtualPath& path) noexcept {
    const std::string_view whole = path.view();
    const std::string_view root(kPackageMountRoot);
    if (whole.size() <= root.size() + 1 || !whole.starts_with(root) || whole[root.size()] != '/') {
        return fail(ErrorCode::NotFound, "that path is not under a package mount's root");
    }
    std::string_view tail = whole.substr(root.size() + 1);
    if (tail.find('/') != std::string_view::npos) {
        return fail(ErrorCode::NotFound, "a package mount has no subdirectories");
    }

    std::string_view variant_text;
    const usize dot = tail.find('.');
    if (dot != std::string_view::npos) {
        variant_text = tail.substr(dot + 1);
        tail = tail.substr(0, dot);
    }

    ParsedEntryPath parsed;
    Expected<cy::AssetId, Error> id = cy::AssetId::parse(tail);
    if (!id) {
        return make_unexpected(id.error());
    }
    parsed.id = id.value();
    Expected<VariantKey, Error> variant = VariantKey::parse(variant_text);
    if (!variant) {
        return make_unexpected(variant.error());
    }
    parsed.variant = variant.value();
    return parsed;
}

}  // namespace

Expected<VirtualPath, Error> package_entry_path(cy::AssetId id, VariantKey variant) noexcept {
    char id_text[cy::AssetId::kTextLength + 1] = {};
    (void)id.format(id_text);

    char buffer[kMaxPathLength + 1] = {};
    const int written =
        variant.is_any()
            ? std::snprintf(buffer, sizeof(buffer), "%s/%s", kPackageMountRoot, id_text)
            : std::snprintf(buffer, sizeof(buffer), "%s/%s.%s", kPackageMountRoot, id_text,
                            variant.c_str());
    if (written < 0 || static_cast<usize>(written) >= sizeof(buffer)) {
        return fail(ErrorCode::InvalidArgument, "the package entry path does not fit");
    }
    return VirtualPath::normalise(std::string_view(buffer, static_cast<usize>(written)));
}

PackageMount::PackageMount(UniquePtr<PackageReader> reader) noexcept : reader_(std::move(reader)) {}

const char* PackageMount::name() const noexcept {
    return reader_ ? reader_->path() : "package";
}

const PackageEntry* PackageMount::entry_for(const VirtualPath& path) const noexcept {
    if (!reader_) {
        return nullptr;
    }
    Expected<ParsedEntryPath, Error> parsed = parse_entry_path(path);
    if (!parsed) {
        return nullptr;
    }
    return reader_->find(parsed.value().id, parsed.value().variant);
}

bool PackageMount::contains(const VirtualPath& path) const noexcept {
    const PackageEntry* entry = entry_for(path);
    return entry != nullptr && !entry->is_deleted();
}

bool PackageMount::is_deleted(const VirtualPath& path) const noexcept {
    const PackageEntry* entry = entry_for(path);
    return entry != nullptr && entry->is_deleted();
}

Expected<u64, Error> PackageMount::size_of(const VirtualPath& path) const noexcept {
    const PackageEntry* entry = entry_for(path);
    if (entry == nullptr || entry->is_deleted()) {
        return fail(ErrorCode::NotFound, "no such entry in this package");
    }
    return entry->uncompressed_size;
}

Status PackageMount::read(const VirtualPath& path, u64 offset, void* destination,
                          usize size) const noexcept {
    const PackageEntry* entry = entry_for(path);
    if (entry == nullptr || entry->is_deleted()) {
        return fail(ErrorCode::NotFound, "no such entry in this package");
    }
    // Straight through to the framed range read: a read of one mip level through the virtual
    // filesystem touches only the frames that cover it, exactly as a direct package read does.
    return reader_->read_entry_range(*entry, offset, destination, size, nullptr, nullptr);
}

Status PackageMount::enumerate(const VirtualPath& directory, bool recursive, VirtualVisitor visitor,
                               void* user) const noexcept {
    (void)recursive;  // a package mount is one flat directory
    const std::string_view root(kPackageMountRoot);
    if (!directory.empty() && directory.view() != root) {
        return fail(ErrorCode::NotFound, "a package mount holds only its own root");
    }
    if (!reader_) {
        return ok();
    }

    // The directory is already sorted by (id, variant), and the path form sorts the same way, so
    // the listing is reproducible without a second sort.
    for (usize index = 0; index < reader_->entry_count(); ++index) {
        const PackageEntry& entry = reader_->entry_at(index);
        if (entry.is_deleted()) {
            continue;
        }
        Expected<VirtualPath, Error> path = package_entry_path(entry.id, entry.variant);
        if (!path) {
            continue;
        }
        VirtualEntry reported;
        reported.path = &path.value();
        reported.is_directory = false;
        reported.size = entry.uncompressed_size;
        if (!visitor(user, reported)) {
            break;
        }
    }
    return ok();
}

}  // namespace cy::assets
