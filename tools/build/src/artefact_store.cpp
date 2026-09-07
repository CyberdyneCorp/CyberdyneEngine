#include <cy/build/artefact_store.h>
#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>

#include <sys/stat.h>

namespace cy::build {
namespace {

[[nodiscard]] Error io(const char* message) noexcept {
    return Error{ErrorCode::Io, message, 0};
}

/// `<root>/ab/abcdef…` — a two-character fan-out, so a store with a million artefacts does not put
/// a million entries in one directory. The same shape `DerivedCache` uses, for the same reason.
[[nodiscard]] std::string entry_path(const std::string& root, const assets::ContentHash& digest) {
    char text[assets::ContentHash::kTextLength + 1] = {};
    digest.format(text);
    std::string path = root;
    path += '/';
    path.append(text, 2);
    path += '/';
    path += text;
    path += ".cyart";
    return path;
}

}  // namespace

Status ArtefactStore::configure(std::string_view root) {
    if (root.empty()) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "empty artefact store root", 0});
    }
    std::string path(root);
    if (Status made = assets::fs::create_directories(path.c_str()); !made) {
        return made;
    }
    root_ = std::move(path);
    return ok();
}

std::string ArtefactStore::path_of(const assets::ContentHash& digest) const {
    return entry_path(root_, digest);
}

Expected<assets::ContentHash, Error> ArtefactStore::put(const void* data, usize size,
                                                        StoreOutcome* outcome) {
    if (root_.empty()) {
        return make_unexpected(Error{ErrorCode::Unavailable, "artefact store not configured", 0});
    }
    const assets::ContentHash digest = assets::content_hash(data, size);
    const std::string path = entry_path(root_, digest);

    // Present already means present with THIS content: the name is the digest, so there is no
    // version of this check that could be fooled by different bytes.
    if (assets::fs::exists(path.c_str())) {
        already_present_.fetch_add(1, std::memory_order_relaxed);
        if (outcome != nullptr) {
            *outcome = StoreOutcome::AlreadyPresent;
        }
        return digest;
    }

    const std::string directory = path.substr(0, path.rfind('/'));
    if (Status made = assets::fs::create_directories(directory.c_str()); !made) {
        return make_unexpected(made.error());
    }
    // Atomic: a temporary in the same directory, then a rename. A reader never sees a partial
    // artefact, and two writers of the same content write the same bytes to the same name.
    if (Status written = assets::fs::write_atomic(path.c_str(), data, size); !written) {
        return make_unexpected(written.error());
    }
    // Read-only, so an accidental in-place write is refused by the operating system. A failure here
    // is not fatal — some filesystems refuse the mode change — and the digest check in `verify` is
    // the mechanism that does not depend on the filesystem cooperating.
    ::chmod(path.c_str(), S_IRUSR | S_IRGRP | S_IROTH);

    writes_.fetch_add(1, std::memory_order_relaxed);
    bytes_written_.fetch_add(size, std::memory_order_relaxed);
    if (outcome != nullptr) {
        *outcome = StoreOutcome::Written;
    }
    return digest;
}

void ArtefactStore::count_read(u64 bytes) const noexcept {
    reads_.fetch_add(1, std::memory_order_relaxed);
    bytes_read_.fetch_add(bytes, std::memory_order_relaxed);
}

Status ArtefactStore::get(const assets::ContentHash& digest, Array<u8>& out) const {
    const std::string path = entry_path(root_, digest);
    if (!assets::fs::exists(path.c_str())) {
        return make_unexpected(Error{ErrorCode::NotFound, "artefact not in the store", 0});
    }
    if (Status read = assets::fs::read_whole(path.c_str(), out); !read) {
        return read;
    }
    // Verified on every read. `build-and-packaging`: "WHEN two artefacts have the same identity
    // THEN their content SHALL be identical" — serving bytes that do not digest to their name would
    // make that sentence false, so this path refuses rather than serving.
    if (assets::content_hash(out.data(), out.size()) != digest) {
        return make_unexpected(
            Error{ErrorCode::Internal, "artefact content does not match its identity", 0});
    }
    count_read(out.size());
    return ok();
}

bool ArtefactStore::contains(const assets::ContentHash& digest) const noexcept {
    return assets::fs::exists(entry_path(root_, digest).c_str());
}

Expected<u64, Error> ArtefactStore::size_of(const assets::ContentHash& digest) const {
    return assets::fs::file_size(entry_path(root_, digest).c_str());
}

Status ArtefactStore::verify(const assets::ContentHash& digest) const {
    Array<u8> bytes(system_allocator(MemoryDomain::Assets));
    return get(digest, bytes);
}

Expected<u64, Error> ArtefactStore::verify_all(u64* corrupt) const {
    struct Walk {
        const ArtefactStore* store;
        u64 checked;
        u64 corrupt;
    } walk{this, 0, 0};

    const auto visit = [](void* user, const assets::DirectoryEntry& entry) noexcept -> bool {
        Walk& state = *static_cast<Walk*>(user);
        if (entry.is_directory) {
            return true;
        }
        // `enumerate` reports a RELATIVE PATH for a recursive walk — "ab/abcdef….cyart" — so the
        // fan-out directory has to come off before the name is read as a digest. Getting this
        // wrong reports every artefact in the store as corrupt, which is a check that fires
        // always and therefore says nothing.
        std::string_view name(entry.name);
        const usize slash = name.rfind('/');
        if (slash != std::string_view::npos) {
            name.remove_prefix(slash + 1);
        }
        const usize dot = name.rfind('.');
        if (dot == std::string_view::npos || name.substr(dot) != ".cyart") {
            return true;
        }
        const Expected<assets::ContentHash, Error> digest =
            assets::ContentHash::parse(name.substr(0, dot));
        if (!digest) {
            ++state.corrupt;
            return true;
        }
        ++state.checked;
        if (!state.store->verify(*digest)) {
            ++state.corrupt;
        }
        return true;
    };

    if (Status walked = assets::fs::enumerate(root_.c_str(), true, visit, &walk); !walked) {
        return make_unexpected(walked.error());
    }
    if (corrupt != nullptr) {
        *corrupt = walk.corrupt;
    }
    return walk.checked;
}

Status ArtefactStore::clear() {
    if (root_.empty()) {
        return ok();
    }
    if (Status removed = assets::fs::remove_directory_recursive(root_.c_str()); !removed) {
        return removed;
    }
    if (Status made = assets::fs::create_directories(root_.c_str()); !made) {
        return make_unexpected(io("could not recreate the artefact store"));
    }
    return ok();
}

StoreStatistics ArtefactStore::statistics() const noexcept {
    StoreStatistics snapshot;
    snapshot.writes = writes_.load(std::memory_order_relaxed);
    snapshot.already_present = already_present_.load(std::memory_order_relaxed);
    snapshot.reads = reads_.load(std::memory_order_relaxed);
    snapshot.bytes_written = bytes_written_.load(std::memory_order_relaxed);
    snapshot.bytes_read = bytes_read_.load(std::memory_order_relaxed);
    return snapshot;
}

}  // namespace cy::build
