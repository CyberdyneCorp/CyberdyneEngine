// The layered virtual filesystem. Task 3.3.2.

#include <cy/core/assets/vfs.h>

#include <cy/core/assets/diagnostics.h>
#include <cy/core/base/assert.h>
#include <cy/core/memory/scope.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace cy::assets {
namespace {

/// Build the native path for a virtual one under `root`. Shared by DirectoryMount's operations, so
/// that they cannot disagree about what a path means.
Expected<usize, Error> join_native(const char* root, const VirtualPath& path, char* out,
                                   usize capacity) noexcept {
    const usize root_length = std::strlen(root);
    const std::string_view tail = path.view();
    const usize needed = root_length + (tail.empty() ? 0 : 1 + tail.size());
    if (needed + 1 > capacity) {
        return fail(ErrorCode::BufferTooSmall, "the native path does not fit");
    }
    std::memcpy(out, root, root_length);
    usize written = root_length;
    if (!tail.empty()) {
        out[written++] = '/';
        std::memcpy(out + written, tail.data(), tail.size());
        written += tail.size();
    }
    out[written] = '\0';
    return written;
}

}  // namespace

const char* mount_kind_name(MountKind kind) noexcept {
    switch (kind) {
        case MountKind::Package:
            return "package";
        case MountKind::Project:
            return "project";
        case MountKind::User:
            return "user";
        case MountKind::Memory:
            return "memory";
        case MountKind::Remote:
            return "remote";
    }
    return "unknown";
}

// --- Mount ---------------------------------------------------------------------------------------

Mount::~Mount() = default;

bool Mount::is_deleted(const VirtualPath&) const noexcept {
    return false;
}

Expected<MappedFile, Error> Mount::map(const VirtualPath&) const noexcept {
    return fail(ErrorCode::Unsupported, "this mount cannot memory-map a file");
}

bool Mount::writable() const noexcept {
    return false;
}

Status Mount::write(const VirtualPath&, const void*, usize) noexcept {
    return fail(ErrorCode::PermissionDenied, "this mount is read-only");
}

PackageMount* Mount::as_package() noexcept {
    return nullptr;
}

RemoteFileProvider::~RemoteFileProvider() = default;

// --- DirectoryMount ------------------------------------------------------------------------------

Expected<UniquePtr<DirectoryMount>, Error> DirectoryMount::create(const char* root, MountKind kind,
                                                                  bool writable) noexcept {
    CY_ASSERT(root != nullptr);
    if (!fs::is_directory(root)) {
        return fail(ErrorCode::NotFound,
                    "a directory mount was given a path that is not a directory");
    }
    const usize length = std::strlen(root);
    if (length > kMaxPathLength) {
        return fail(ErrorCode::InvalidArgument, "the mount root is longer than a path may be");
    }

    Expected<UniquePtr<DirectoryMount>, Error> mount =
        make_unique<DirectoryMount>(current_allocator());
    if (!mount) {
        return make_unexpected(mount.error());
    }
    DirectoryMount& created = *mount.value();
    std::memcpy(created.root_, root, length);
    // A trailing separator would make every joined path contain a doubled one. Removed here rather
    // than tolerated, so that a diagnostic naming the file names it the way the filesystem does.
    usize trimmed = length;
    while (trimmed > 1 && created.root_[trimmed - 1] == '/') {
        created.root_[--trimmed] = '\0';
    }
    created.kind_ = kind;
    created.writable_ = writable;
    return mount;
}

Expected<usize, Error> DirectoryMount::native_path(const VirtualPath& path, char* out,
                                                   usize capacity) const noexcept {
    return join_native(root_, path, out, capacity);
}

bool DirectoryMount::contains(const VirtualPath& path) const noexcept {
    char native[(kMaxPathLength * 2) + 2] = {};
    if (!native_path(path, native, sizeof(native))) {
        return false;
    }
    return fs::exists(native) && !fs::is_directory(native);
}

Expected<u64, Error> DirectoryMount::size_of(const VirtualPath& path) const noexcept {
    char native[(kMaxPathLength * 2) + 2] = {};
    if (Expected<usize, Error> joined = native_path(path, native, sizeof(native)); !joined) {
        return make_unexpected(joined.error());
    }
    return fs::file_size(native);
}

Status DirectoryMount::read(const VirtualPath& path, u64 offset, void* destination,
                            usize size) const noexcept {
    char native[(kMaxPathLength * 2) + 2] = {};
    if (Expected<usize, Error> joined = native_path(path, native, sizeof(native)); !joined) {
        return make_unexpected(joined.error());
    }
    Expected<File, Error> file = File::open(native, FileMode::Read);
    if (!file) {
        return make_unexpected(file.error());
    }
    Expected<usize, Error> read = file.value().read_at(offset, destination, size);
    if (!read) {
        return make_unexpected(read.error());
    }
    if (read.value() != size) {
        return fail(ErrorCode::OutOfRange, "the read extends past the end of the file");
    }
    return ok();
}

Expected<MappedFile, Error> DirectoryMount::map(const VirtualPath& path) const noexcept {
    char native[(kMaxPathLength * 2) + 2] = {};
    if (Expected<usize, Error> joined = native_path(path, native, sizeof(native)); !joined) {
        return make_unexpected(joined.error());
    }
    return MappedFile::map(native);
}

Status DirectoryMount::enumerate(const VirtualPath& directory, bool recursive,
                                 VirtualVisitor visitor, void* user) const noexcept {
    char native[(kMaxPathLength * 2) + 2] = {};
    if (Expected<usize, Error> joined = native_path(directory, native, sizeof(native)); !joined) {
        return make_unexpected(joined.error());
    }
    if (!fs::is_directory(native)) {
        return fail(ErrorCode::NotFound, "there is no such directory in this mount");
    }

    struct Relay {
        const VirtualPath* base;
        VirtualVisitor visitor;
        void* user;
    } relay{&directory, visitor, user};

    return fs::enumerate(
        native, recursive,
        [](void* state, const DirectoryEntry& entry) noexcept {
            auto* relayed = static_cast<Relay*>(state);
            Expected<VirtualPath, Error> full = relayed->base->join(entry.name);
            if (!full) {
                return true;  // a name this filesystem allows and a virtual path does not
            }
            VirtualEntry reported;
            reported.path = &full.value();
            reported.is_directory = entry.is_directory;
            reported.size = entry.size;
            return relayed->visitor(relayed->user, reported);
        },
        &relay);
}

Status DirectoryMount::write(const VirtualPath& path, const void* data, usize size) noexcept {
    if (!writable_) {
        return fail(ErrorCode::PermissionDenied, "this directory mount is read-only");
    }
    char native[(kMaxPathLength * 2) + 2] = {};
    if (Expected<usize, Error> joined = native_path(path, native, sizeof(native)); !joined) {
        return make_unexpected(joined.error());
    }
    // The parent must exist before the temporary can be created; a save into a directory the game
    // has not made yet is the common case, not an error.
    const std::string_view parent = path.parent();
    if (!parent.empty()) {
        Expected<VirtualPath, Error> parent_path = VirtualPath::normalise(parent);
        if (!parent_path) {
            return make_unexpected(parent_path.error());
        }
        char native_parent[(kMaxPathLength * 2) + 2] = {};
        if (Expected<usize, Error> joined =
                native_path(parent_path.value(), native_parent, sizeof(native_parent));
            !joined) {
            return make_unexpected(joined.error());
        }
        if (Status made = fs::create_directories(native_parent); !made) {
            return made;
        }
    }
    // Atomic, always. `core-assets-and-io` requires it of the user mount, and there is no reason
    // for a project mount's writes to be less safe than a save game's.
    return fs::write_atomic(native, data, size);
}

// --- MemoryMount ---------------------------------------------------------------------------------

MemoryMount::MemoryMount(const char* label) noexcept {
    const usize length = std::strlen(label);
    const usize copied = length < sizeof(label_) - 1 ? length : sizeof(label_) - 1;
    std::memcpy(label_, label, copied);
}

const MemoryMount::Entry* MemoryMount::find(const VirtualPath& path) const noexcept {
    for (const Entry& entry : files_) {
        if (entry.path == path) {
            return &entry;
        }
    }
    return nullptr;
}

bool MemoryMount::contains(const VirtualPath& path) const noexcept {
    const Entry* entry = find(path);
    return entry != nullptr && !entry->deleted;
}

bool MemoryMount::is_deleted(const VirtualPath& path) const noexcept {
    const Entry* entry = find(path);
    return entry != nullptr && entry->deleted;
}

Expected<u64, Error> MemoryMount::size_of(const VirtualPath& path) const noexcept {
    const Entry* entry = find(path);
    if (entry == nullptr || entry->deleted) {
        return fail(ErrorCode::NotFound, "no such file in the memory mount");
    }
    return static_cast<u64>(entry->bytes.size());
}

Status MemoryMount::read(const VirtualPath& path, u64 offset, void* destination,
                         usize size) const noexcept {
    const Entry* entry = find(path);
    if (entry == nullptr || entry->deleted) {
        return fail(ErrorCode::NotFound, "no such file in the memory mount");
    }
    if (offset > entry->bytes.size() || size > entry->bytes.size() - offset) {
        return fail(ErrorCode::OutOfRange, "the read extends past the end of the file");
    }
    if (size != 0) {
        std::memcpy(destination, entry->bytes.data() + offset, size);
    }
    return ok();
}

Status MemoryMount::enumerate(const VirtualPath& directory, bool recursive, VirtualVisitor visitor,
                              void* user) const noexcept {
    CY_ASSERT(visitor != nullptr);
    // Sorted, so a listing is reproducible; the storage is insertion-ordered.
    std::vector<const Entry*> ordered;
    ordered.reserve(files_.size());
    for (const Entry& entry : files_) {
        if (entry.deleted || !entry.path.is_within(directory) || entry.path == directory) {
            continue;
        }
        if (!recursive) {
            // Direct children only: the remainder after the directory prefix holds no separator.
            const usize prefix = directory.empty() ? 0 : directory.size() + 1;
            if (entry.path.view().substr(prefix).find('/') != std::string_view::npos) {
                continue;
            }
        }
        ordered.push_back(&entry);
    }
    std::ranges::sort(ordered, [](const Entry* a, const Entry* b) { return a->path < b->path; });

    for (const Entry* entry : ordered) {
        VirtualEntry reported;
        reported.path = &entry->path;
        reported.is_directory = false;
        reported.size = entry->bytes.size();
        if (!visitor(user, reported)) {
            break;
        }
    }
    return ok();
}

Status MemoryMount::write(const VirtualPath& path, const void* data, usize size) noexcept {
    return add(path, data, size);
}

Status MemoryMount::add(const VirtualPath& path, const void* data, usize size) noexcept {
    Array<u8> bytes;
    if (Status grown = bytes.resize(size); !grown) {
        return grown;
    }
    if (size != 0) {
        std::memcpy(bytes.data(), data, size);
    }

    for (Entry& entry : files_) {
        if (entry.path == path) {
            entry.bytes = std::move(bytes);
            entry.deleted = false;
            return ok();
        }
    }
    Expected<Entry*, Error> added = files_.emplace_back();
    if (!added) {
        return make_unexpected(added.error());
    }
    added.value()->path = path;
    added.value()->bytes = std::move(bytes);
    added.value()->deleted = false;
    return ok();
}

Status MemoryMount::mark_deleted(const VirtualPath& path) noexcept {
    for (Entry& entry : files_) {
        if (entry.path == path) {
            entry.bytes.clear();
            entry.deleted = true;
            return ok();
        }
    }
    Expected<Entry*, Error> added = files_.emplace_back();
    if (!added) {
        return make_unexpected(added.error());
    }
    added.value()->path = path;
    added.value()->deleted = true;
    return ok();
}

// --- RemoteMount ---------------------------------------------------------------------------------

bool RemoteMount::contains(const VirtualPath& path) const noexcept {
    return provider_->stat(path).has_value();
}

Expected<u64, Error> RemoteMount::size_of(const VirtualPath& path) const noexcept {
    return provider_->stat(path);
}

Status RemoteMount::read(const VirtualPath& path, u64 offset, void* destination,
                         usize size) const noexcept {
    return provider_->fetch(path, offset, destination, size);
}

Status RemoteMount::enumerate(const VirtualPath& directory, bool recursive, VirtualVisitor visitor,
                              void* user) const noexcept {
    return provider_->list(directory, recursive, visitor, user);
}

// --- VirtualFileSystem ---------------------------------------------------------------------------

Expected<MountId, Error> VirtualFileSystem::adopt(Mount* mount, MountDeleter deleter,
                                                  Allocator& allocator, i32 priority) noexcept {
    if (mount == nullptr || deleter == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a null mount cannot be mounted");
    }
    Expected<Entry*, Error> added = mounts_.emplace_back();
    if (!added) {
        deleter(mount, &allocator);
        return make_unexpected(added.error());
    }
    const MountId id = next_id_++;
    added.value()->id = id;
    added.value()->priority = priority;
    added.value()->mount = mount;
    added.value()->deleter = deleter;
    added.value()->allocator = &allocator;

    // Descending priority, and stable in reverse insertion order among equals: std::stable_sort
    // over a descending comparison keeps the earlier-inserted first, so the list is reversed among
    // equals afterwards to give the most recent mount precedence. Done as one pass instead: sort by
    // (priority, id) descending, which is exactly that rule stated directly.
    std::ranges::sort(mounts_, [](const Entry& a, const Entry& b) noexcept {
        return a.priority != b.priority ? a.priority > b.priority : a.id > b.id;
    });
    return id;
}

Status VirtualFileSystem::unmount(MountId id) noexcept {
    for (usize index = 0; index < mounts_.size(); ++index) {
        if (mounts_[index].id == id) {
            mounts_.erase(index);
            return ok();
        }
    }
    return fail(ErrorCode::NotFound, "no such mount");
}

void VirtualFileSystem::unmount_all() noexcept {
    mounts_.clear();
}

Mount* VirtualFileSystem::find_mount(MountId id) noexcept {
    for (Entry& entry : mounts_) {
        if (entry.id == id) {
            return entry.mount;
        }
    }
    return nullptr;
}

Expected<VirtualFileSystem::Resolution, Error> VirtualFileSystem::resolve(
    const VirtualPath& path) const noexcept {
    for (const Entry& entry : mounts_) {
        // Masking is checked FIRST: a deleted-entry marker in a patch hides the base's copy, and a
        // mount that both masks and serves the path would otherwise serve it.
        if (entry.mount->is_deleted(path)) {
            counters::record_mount_resolution(false);
            return fail(ErrorCode::NotFound,
                        "the path is masked by a higher-priority mount's deleted-entry marker");
        }
        if (!entry.mount->contains(path)) {
            continue;
        }
        Expected<u64, Error> size = entry.mount->size_of(path);
        if (!size) {
            return make_unexpected(size.error());
        }
        Resolution resolution;
        resolution.mount = entry.id;
        resolution.source = entry.mount;
        resolution.size = size.value();
        counters::record_mount_resolution(true);
        return resolution;
    }
    counters::record_mount_resolution(false);
    return fail(ErrorCode::NotFound, "no mount serves that path");
}

bool VirtualFileSystem::exists(const VirtualPath& path) const noexcept {
    return resolve(path).has_value();
}

Expected<u64, Error> VirtualFileSystem::size_of(const VirtualPath& path) const noexcept {
    Expected<Resolution, Error> resolved = resolve(path);
    if (!resolved) {
        return make_unexpected(resolved.error());
    }
    return resolved.value().size;
}

Status VirtualFileSystem::read(const VirtualPath& path, Array<u8>& out) const noexcept {
    Expected<Resolution, Error> resolved = resolve(path);
    if (!resolved) {
        return make_unexpected(resolved.error());
    }
    out.clear();
    if (Status grown = out.resize(static_cast<usize>(resolved.value().size)); !grown) {
        return grown;
    }
    if (out.empty()) {
        return ok();
    }
    return resolved.value().source->read(path, 0, out.data(), out.size());
}

Status VirtualFileSystem::read_range(const VirtualPath& path, u64 offset, void* destination,
                                     usize size) const noexcept {
    Expected<Resolution, Error> resolved = resolve(path);
    if (!resolved) {
        return make_unexpected(resolved.error());
    }
    return resolved.value().source->read(path, offset, destination, size);
}

Expected<MappedFile, Error> VirtualFileSystem::map(const VirtualPath& path) const noexcept {
    Expected<Resolution, Error> resolved = resolve(path);
    if (!resolved) {
        return make_unexpected(resolved.error());
    }
    return resolved.value().source->map(path);
}

Status VirtualFileSystem::enumerate(const VirtualPath& directory, bool recursive,
                                    VirtualVisitor visitor, void* user) const noexcept {
    CY_ASSERT(visitor != nullptr);

    // Collected across every mount, then de-duplicated and sorted. A path a higher-priority mount
    // has already claimed — or masked — is not reported again by a lower one, which is what makes
    // the union behave like one namespace rather than like a concatenation.
    struct Row {
        VirtualPath path;
        bool is_directory;
        u64 size;
        MountId mount;
    };
    struct Collector {
        std::vector<Row> rows;
        MountId mount = kInvalidMount;
    } collector;

    for (const Entry& entry : mounts_) {
        collector.mount = entry.id;
        Status walked = entry.mount->enumerate(
            directory, recursive,
            [](void* state, const VirtualEntry& found) noexcept {
                auto* into = static_cast<Collector*>(state);
                into->rows.push_back(Row{*found.path, found.is_directory, found.size, into->mount});
                return true;
            },
            &collector);
        // A mount that does not have the directory at all is not an error for the union: another
        // mount may. Any other failure is.
        if (!walked && walked.error().code != ErrorCode::NotFound) {
            return walked;
        }
    }

    std::ranges::sort(collector.rows, [](const Row& a, const Row& b) { return a.path < b.path; });

    const VirtualPath* previous = nullptr;
    for (const Row& row : collector.rows) {
        if (previous != nullptr && *previous == row.path) {
            continue;  // already reported by a higher-priority mount
        }
        // Ask the namespace, not the mount: a lower mount's copy of a path a higher one masks must
        // not be reported, and only resolution knows that.
        if (!row.is_directory && !exists(row.path)) {
            previous = &row.path;
            continue;
        }
        previous = &row.path;

        VirtualEntry reported;
        reported.path = &row.path;
        reported.is_directory = row.is_directory;
        reported.size = row.size;
        reported.mount = row.mount;
        if (!visitor(user, reported)) {
            break;
        }
    }
    return ok();
}

Status VirtualFileSystem::write(const VirtualPath& path, const void* data, usize size) noexcept {
    for (Entry& entry : mounts_) {
        if (entry.mount->writable()) {
            return entry.mount->write(path, data, size);
        }
    }
    return fail(ErrorCode::PermissionDenied, "no writable mount is present");
}

}  // namespace cy::assets
