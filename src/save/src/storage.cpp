// The storage backends. Task 6.2.

#include <cy/save/storage.h>

#include <cy/core/assets/file.h>

#include <cstring>
#include <utility>

namespace cy::save {
namespace {

/// Join a root and a key into a native path. Fails rather than truncating: a truncated path names
/// a different file, and this is the layer where that would be a save written somewhere else.
Status join(const char* root, std::string_view key, char* out, usize capacity) noexcept {
    const usize root_length = std::strlen(root);
    if (key.empty() || key.size() > kMaxSaveKeyLength) {
        return fail(ErrorCode::InvalidArgument, "a save key is between one and 255 characters");
    }
    if (key.front() == '/' || key.find("..") != std::string_view::npos) {
        return fail(ErrorCode::InvalidArgument, "a save key is relative and contains no '..'");
    }
    if (root_length + 1 + key.size() + 1 > capacity) {
        return fail(ErrorCode::BufferTooSmall, "the save path does not fit");
    }
    std::memcpy(out, root, root_length);
    out[root_length] = '/';
    std::memcpy(out + root_length + 1, key.data(), key.size());
    out[root_length + 1 + key.size()] = '\0';
    return ok();
}

/// Create the directories a key's own path needs, so a caller writes "chunks/<hash>" without
/// having to know that chunks/ is a directory.
Status create_parent(const char* path) noexcept {
    const auto* last = std::strrchr(path, '/');
    if (last == nullptr) {
        return ok();
    }
    char directory[4096] = {};
    const auto length = static_cast<usize>(last - path);
    if (length + 1 > sizeof(directory)) {
        return fail(ErrorCode::BufferTooSmall, "the save directory path does not fit");
    }
    std::memcpy(directory, path, length);
    return assets::fs::create_directories(directory);
}

}  // namespace

SaveBackend::~SaveBackend() = default;

// --- The filesystem ------------------------------------------------------------------------------

FilesystemBackend::~FilesystemBackend() = default;

Expected<usize, Error> FilesystemBackend::open(const char* root) noexcept {
    if (root == nullptr || root[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "a filesystem save store needs a root directory");
    }
    const usize length = std::strlen(root);
    if (length > kMaxRootLength) {
        return fail(ErrorCode::BufferTooSmall, "the save root path is too long");
    }
    std::memcpy(root_, root, length);
    root_[length] = '\0';
    if (Status created = assets::fs::create_directories(root_); !created) {
        return make_unexpected(created.error());
    }
    return assets::fs::discard_temporaries(root_);
}

Status FilesystemBackend::write(std::string_view key, Span<const u8> bytes) noexcept {
    char path[4096] = {};
    if (Status joined = join(root_, key, path, sizeof(path)); !joined) {
        return joined;
    }
    if (Status created = create_parent(path); !created) {
        return created;
    }
    return assets::fs::write_atomic(path, bytes.data(), bytes.size());
}

Status FilesystemBackend::read(std::string_view key, Array<u8>& out) const noexcept {
    char path[4096] = {};
    if (Status joined = join(root_, key, path, sizeof(path)); !joined) {
        return joined;
    }
    if (!assets::fs::exists(path)) {
        return fail(ErrorCode::NotFound, "there is no such object in the save store");
    }
    return assets::fs::read_whole(path, out);
}

bool FilesystemBackend::exists(std::string_view key) const noexcept {
    char path[4096] = {};
    if (Status joined = join(root_, key, path, sizeof(path)); !joined) {
        return false;
    }
    return assets::fs::exists(path);
}

Status FilesystemBackend::remove(std::string_view key) noexcept {
    char path[4096] = {};
    if (Status joined = join(root_, key, path, sizeof(path)); !joined) {
        return joined;
    }
    if (!assets::fs::exists(path)) {
        return ok();
    }
    return assets::fs::remove_file(path);
}

namespace {

/// The state `list` carries through the filesystem walk. `enumerate` reports relative names, which
/// are exactly the keys, so the prefix test is a string comparison and nothing is rebuilt.
struct ListState {
    std::string_view prefix;
    SaveBackend::KeyVisitor visitor;
    void* user;
};

bool visit_entry(void* user, const assets::DirectoryEntry& entry) noexcept {
    ListState& state = *static_cast<ListState*>(user);
    if (entry.is_directory) {
        return true;
    }
    const std::string_view key(entry.name);
    if (!state.prefix.empty() && !key.starts_with(state.prefix)) {
        return true;
    }
    // A temporary an interrupted write left behind is not an object: reporting it would make a
    // collector delete it as if it were, and a loader read it as if it were a chunk.
    if (key.ends_with(assets::fs::kTemporarySuffix)) {
        return true;
    }
    return state.visitor(state.user, key);
}

}  // namespace

Status FilesystemBackend::list(std::string_view prefix, KeyVisitor visitor,
                               void* user) const noexcept {
    if (visitor == nullptr) {
        return fail(ErrorCode::InvalidArgument, "list needs a visitor");
    }
    if (!assets::fs::is_directory(root_)) {
        return ok();
    }
    ListState state{prefix, visitor, user};
    return assets::fs::enumerate(root_, true, visit_entry, &state);
}

// --- Memory --------------------------------------------------------------------------------------

MemoryBackend::~MemoryBackend() = default;

usize MemoryBackend::index_of(std::string_view key) const noexcept {
    usize low = 0;
    usize high = objects_.size();
    while (low < high) {
        const usize middle = low + ((high - low) / 2);
        if (objects_[middle].name() < key) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

Status MemoryBackend::write(std::string_view key, Span<const u8> bytes) noexcept {
    if (key.empty() || key.size() > kMaxSaveKeyLength) {
        return fail(ErrorCode::InvalidArgument, "a save key is between one and 255 characters");
    }
    if (write_budget_ == 0) {
        return fail(ErrorCode::Io, "the save store was told to fail this write");
    }
    if (write_budget_ != kNoLimit) {
        --write_budget_;
    }

    // Assembled whole before anything is replaced, so a failure to allocate leaves the previous
    // object intact — the same promise the filesystem backend gets from write-then-rename.
    Object fresh(*allocator_);
    if (Status grown = fresh.key.append(Span<const char>(key.data(), key.size())); !grown) {
        return grown;
    }
    if (Status grown = fresh.bytes.append(bytes); !grown) {
        return grown;
    }

    ++writes_;
    const usize index = index_of(key);
    if (index < objects_.size() && objects_[index].name() == key) {
        objects_[index] = std::move(fresh);
        return ok();
    }
    if (Status pushed = objects_.push_back(std::move(fresh)); !pushed) {
        return pushed;
    }
    for (usize position = objects_.size() - 1; position > index; --position) {
        Object moved = std::move(objects_[position - 1]);
        objects_[position - 1] = std::move(objects_[position]);
        objects_[position] = std::move(moved);
    }
    return ok();
}

Status MemoryBackend::read(std::string_view key, Array<u8>& out) const noexcept {
    const usize index = index_of(key);
    if (index >= objects_.size() || objects_[index].name() != key) {
        return fail(ErrorCode::NotFound, "there is no such object in the save store");
    }
    out.clear();
    return out.append(objects_[index].bytes.span());
}

bool MemoryBackend::exists(std::string_view key) const noexcept {
    const usize index = index_of(key);
    return index < objects_.size() && objects_[index].name() == key;
}

Status MemoryBackend::remove(std::string_view key) noexcept {
    const usize index = index_of(key);
    if (index >= objects_.size() || objects_[index].name() != key) {
        return ok();
    }
    objects_.erase(index);
    return ok();
}

Status MemoryBackend::list(std::string_view prefix, KeyVisitor visitor, void* user) const noexcept {
    if (visitor == nullptr) {
        return fail(ErrorCode::InvalidArgument, "list needs a visitor");
    }
    for (const Object& object : objects_) {
        const std::string_view key = object.name();
        if (!prefix.empty() && !key.starts_with(prefix)) {
            continue;
        }
        if (!visitor(user, key)) {
            break;
        }
    }
    return ok();
}

Status MemoryBackend::corrupt(std::string_view key) noexcept {
    const usize index = index_of(key);
    if (index >= objects_.size() || objects_[index].name() != key) {
        return fail(ErrorCode::NotFound, "there is no such object in the save store");
    }
    Array<u8>& bytes = objects_[index].bytes;
    if (bytes.empty()) {
        return fail(ErrorCode::InvalidArgument, "an empty object cannot be corrupted");
    }
    bytes[bytes.size() / 2] ^= 0xFFU;
    return ok();
}

u64 MemoryBackend::total_bytes() const noexcept {
    u64 total = 0;
    for (const Object& object : objects_) {
        total += object.bytes.size();
    }
    return total;
}

}  // namespace cy::save
