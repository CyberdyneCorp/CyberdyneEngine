// The polling file watcher. See cy/core/assets/hot_reload.h for why it polls and why a change has
// to settle before it is reported.

#include <cy/core/assets/hot_reload.h>

#include <cstring>

namespace cy::assets {
namespace {

/// The longest path the watcher will hold. The same limit `VirtualPath` uses, for the same reason:
/// a path longer than this is a defect somewhere else, and accepting it would mean the watcher's
/// storage is bounded by whatever produced it.
constexpr usize kMaxWatchedPath = 1024;

/// Join a directory and an entry name into `out`. Returns false when the result would not fit.
bool join_path(const char* directory, const char* name, char* out, usize capacity) noexcept {
    const usize directory_length = std::strlen(directory);
    const usize name_length = std::strlen(name);
    if (directory_length + 1 + name_length + 1 > capacity) {
        return false;
    }
    std::memcpy(out, directory, directory_length);
    usize written = directory_length;
    if (written > 0 && out[written - 1] != '/') {
        out[written++] = '/';
    }
    std::memcpy(out + written, name, name_length);
    out[written + name_length] = '\0';
    return true;
}

/// What one poll needs to know about a file that may or may not be there.
struct Observation {
    u64 size = 0;
    i64 modified_ns = 0;
    bool present = false;
};

Observation observe(const char* path) noexcept {
    Observation seen;
    const Expected<u64, Error> size = fs::file_size(path);
    if (!size) {
        return seen;
    }
    const Expected<i64, Error> modified = fs::file_modified_ns(path);
    if (!modified) {
        // The size read but the time did not: the file is being replaced under us. Reported as
        // absent, which the settling rule turns into "not yet" rather than into an event.
        return seen;
    }
    seen.size = *size;
    seen.modified_ns = *modified;
    seen.present = true;
    return seen;
}

/// The state a directory walk collects. A visitor is a plain function pointer, so what it needs
/// travels in `user`.
struct WalkState {
    FileWatcher* watcher = nullptr;
    const char* root = nullptr;
    Status outcome = ok();
};

}  // namespace

const char* file_change_name(FileChange change) noexcept {
    switch (change) {
        case FileChange::Created:
            return "created";
        case FileChange::Modified:
            return "modified";
        case FileChange::Removed:
            return "removed";
    }
    return "unknown";
}

FileWatcher::FileWatcher(Allocator& allocator) noexcept
    : allocator_(&allocator), text_(allocator), entries_(allocator), roots_(allocator) {}

FileWatcher::Entry* FileWatcher::find(const char* path) noexcept {
    for (Entry& entry : entries_) {
        if (std::strcmp(text_at(entry.offset), path) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

Expected<u32, Error> FileWatcher::intern(const char* path) noexcept {
    const usize length = std::strlen(path);
    if (length == 0 || length >= kMaxWatchedPath) {
        return fail(ErrorCode::InvalidArgument,
                    "a watched path is empty or longer than the watcher's limit");
    }
    const auto offset = static_cast<u32>(text_.size());
    for (usize index = 0; index <= length; ++index) {
        if (Status pushed = text_.push_back(path[index]); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    return offset;
}

Status FileWatcher::add_entry(const char* path, bool from_directory) noexcept {
    if (find(path) != nullptr) {
        return ok();
    }
    const Expected<u32, Error> offset = intern(path);
    if (!offset) {
        return make_unexpected(offset.error());
    }
    Entry entry;
    entry.offset = *offset;
    entry.length = static_cast<u32>(std::strlen(path));
    entry.from_directory = from_directory;
    // Deliberately NOT observed here. An entry starts as "never seen", so a file that already
    // exists is reported as Created on the poll after it settles — which is what a consumer that
    // watches a directory and then asks what is in it actually wants.
    if (Status pushed = entries_.push_back(entry); !pushed) {
        return pushed;
    }
    stats_.watched = static_cast<u32>(entries_.size());
    return ok();
}

Status FileWatcher::watch_file(const char* path) noexcept {
    if (path == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a watched path may not be null");
    }
    return add_entry(path, false);
}

Status FileWatcher::watch_directory(const char* path, bool recursive) noexcept {
    if (path == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a watched directory may not be null");
    }
    if (!fs::is_directory(path)) {
        return fail(ErrorCode::NotFound, "there is no such directory to watch");
    }
    for (const Root& root : roots_) {
        if (std::strcmp(text_at(root.offset), path) == 0) {
            return ok();
        }
    }
    const Expected<u32, Error> offset = intern(path);
    if (!offset) {
        return make_unexpected(offset.error());
    }
    Root root;
    root.offset = *offset;
    root.length = static_cast<u32>(std::strlen(path));
    root.recursive = recursive;
    if (Status pushed = roots_.push_back(root); !pushed) {
        return pushed;
    }
    stats_.roots = static_cast<u32>(roots_.size());
    return scan_roots();
}

Status FileWatcher::unwatch(const char* path) noexcept {
    if (path == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a watched path may not be null");
    }
    bool removed = false;
    for (usize index = entries_.size(); index > 0; --index) {
        if (std::strcmp(text_at(entries_[index - 1].offset), path) == 0) {
            entries_.erase(index - 1);
            removed = true;
        }
    }
    for (usize index = roots_.size(); index > 0; --index) {
        if (std::strcmp(text_at(roots_[index - 1].offset), path) == 0) {
            roots_.erase(index - 1);
            removed = true;
        }
    }
    if (!removed) {
        return fail(ErrorCode::NotFound, "that path is not being watched");
    }
    stats_.watched = static_cast<u32>(entries_.size());
    stats_.roots = static_cast<u32>(roots_.size());
    return compact();
}

void FileWatcher::clear() noexcept {
    entries_.clear();
    roots_.clear();
    text_.clear();
    stats_.watched = 0;
    stats_.roots = 0;
    stats_.changes_pending = 0;
}

Status FileWatcher::compact() noexcept {
    // The paths live in one blob, so removing an entry leaves a hole. Rebuilding is O(total path
    // bytes) and happens only on `unwatch`, which is rare; the alternative — a free list over the
    // blob — would be a second allocator for no measurable gain.
    Array<char> rebuilt(*allocator_);
    if (Status reserved = rebuilt.reserve(text_.size()); !reserved) {
        return reserved;
    }
    const auto move_text = [&](u32& offset, u32 length) noexcept -> Status {
        const char* source = text_at(offset);
        const auto moved = static_cast<u32>(rebuilt.size());
        for (u32 index = 0; index <= length; ++index) {
            if (Status pushed = rebuilt.push_back(source[index]); !pushed) {
                return pushed;
            }
        }
        offset = moved;
        return ok();
    };
    for (Entry& entry : entries_) {
        if (Status moved = move_text(entry.offset, entry.length); !moved) {
            return moved;
        }
    }
    for (Root& root : roots_) {
        if (Status moved = move_text(root.offset, root.length); !moved) {
            return moved;
        }
    }
    text_ = std::move(rebuilt);
    return ok();
}

Status FileWatcher::scan_roots() noexcept {
    // `roots_` is not touched by the walk below — `add_entry` appends to `entries_` and to `text_`
    // — so iterating it directly is safe. The root's PATH is a different matter: it lives in
    // `text_`, which `add_entry` may reallocate, so it is copied out before the walk starts.
    for (const Root& entry : roots_) {
        char root[kMaxWatchedPath] = {};
        if (entry.length + 1 > sizeof(root)) {
            return fail(ErrorCode::OutOfRange, "a watched root is longer than the watcher's limit");
        }
        std::memcpy(root, text_at(entry.offset), entry.length + 1);
        const bool recursive = entry.recursive;

        WalkState state;
        state.watcher = this;
        state.root = root;
        // `fs::enumerate` visits in a sorted order, so a directory's files are added — and later
        // reported — in the same sequence on every platform.
        Status walked = fs::enumerate(
            root, recursive,
            [](void* user, const DirectoryEntry& found) noexcept -> bool {
                auto& walk = *static_cast<WalkState*>(user);
                if (found.is_directory) {
                    return true;
                }
                char path[kMaxWatchedPath] = {};
                if (!join_path(walk.root, found.name, path, sizeof(path))) {
                    walk.outcome =
                        fail(ErrorCode::OutOfRange, "a path under a watched root is too long");
                    return false;
                }
                walk.outcome = walk.watcher->add_entry(path, true);
                return walk.outcome.has_value();
            },
            &state);
        if (!walked) {
            return walked;
        }
        if (!state.outcome) {
            return state.outcome;
        }
    }
    return ok();
}

Status FileWatcher::examine(Entry& entry, Array<FileEvent>& out) noexcept {
    ++stats_.checks;
    const char* path = text_at(entry.offset);
    const Observation seen = observe(path);

    if (!seen.present) {
        entry.has_pending = false;
        if (!entry.present) {
            return ok();
        }
        entry.present = false;
        entry.size = 0;
        entry.modified_ns = 0;
        FileEvent event;
        event.path = path;
        event.change = FileChange::Removed;
        return out.push_back(event);
    }

    if (entry.present && seen.size == entry.size && seen.modified_ns == entry.modified_ns) {
        entry.has_pending = false;
        return ok();
    }

    // Changed. It is reported only once a second poll agrees with the first — the settling rule in
    // the header, and the reason a file being written is not handed to a loader half-finished.
    if (!entry.has_pending || entry.pending_size != seen.size ||
        entry.pending_modified_ns != seen.modified_ns) {
        entry.has_pending = true;
        entry.pending_size = seen.size;
        entry.pending_modified_ns = seen.modified_ns;
        ++stats_.changes_pending;
        return ok();
    }

    FileEvent event;
    event.path = path;
    event.change = entry.present ? FileChange::Modified : FileChange::Created;
    event.size = seen.size;
    event.modified_ns = seen.modified_ns;
    entry.present = true;
    entry.size = seen.size;
    entry.modified_ns = seen.modified_ns;
    entry.has_pending = false;
    return out.push_back(event);
}

Expected<u32, Error> FileWatcher::poll(Array<FileEvent>& out) noexcept {
    out.clear();
    ++stats_.polls;

    // Roots first, so a file created since the last poll is examined in the same poll that finds
    // it. It still has to settle before it is reported, exactly like a file that was already there.
    if (Status scanned = scan_roots(); !scanned) {
        return make_unexpected(scanned.error());
    }

    for (Entry& entry : entries_) {
        if (Status examined = examine(entry, out); !examined) {
            return make_unexpected(examined.error());
        }
    }

    // A file that came from a directory walk and has gone is dropped, after its Removed event has
    // been reported. An explicitly watched path is kept: the caller asked about the path, and the
    // file arriving again is a Created it is entitled to hear about.
    for (usize index = entries_.size(); index > 0; --index) {
        const Entry& entry = entries_[index - 1];
        if (entry.from_directory && !entry.present && !entry.has_pending) {
            entries_.erase(index - 1);
        }
    }
    stats_.watched = static_cast<u32>(entries_.size());
    stats_.events_reported += out.size();
    return static_cast<u32>(out.size());
}

}  // namespace cy::assets
