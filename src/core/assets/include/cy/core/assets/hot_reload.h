#ifndef CY_CORE_ASSETS_HOT_RELOAD_H
#define CY_CORE_ASSETS_HOT_RELOAD_H
// Watching files for change, so that an edit becomes a reload. Task 1.4.
//
// `core-assets-and-io` — "Hot reload": in development builds the asset system watches source files
// and cooked outputs and reloads changed assets **in place, preserving existing `Ref`s**, and
// notifies dependents so that derived state — GPU uploads, material instances, shader pipelines —
// is rebuilt. M2 shipped none of it: no watcher, no reload entry point. This header is the first
// half, and `AssetSystem::reload` is the second.
//
// --- WHY IT POLLS -------------------------------------------------------------------------------
//
// Every operating system has a better answer than polling — inotify, FSEvents,
// ReadDirectoryChangesW — and every one of them is a platform API. This module is layer 0, and the
// platform seam is layer 3: `core-assets-and-io` sits below the thing that would own a native
// watcher, so reaching for one from here is exactly the upward dependency the layer check exists to
// refuse. Polling over the standard filesystem interface needs nothing new, behaves identically on
// the three target platforms, and is bounded by the number of files being watched rather than by
// the size of the tree.
//
// The cost is stated rather than hidden: one `stat` per watched file per poll, plus one directory
// enumeration per watched root per poll. A shader directory of a few hundred files is nothing at a
// poll or two a second; a whole project tree is not what this is for. A native backend belongs
// behind the platform seam when a project's content directory is large enough to want one, and
// nothing in this interface would change if it arrived — which is the point of the interface.
//
// --- WHY A CHANGE HAS TO SETTLE BEFORE IT IS REPORTED --------------------------------------------
//
// `core-assets-and-io`'s second hot-reload scenario is "reload failure keeps the old asset": a
// re-import that meets a file half written — a texture an art tool is still saving — must not
// replace the asset that works. There are two defences and this is the first: a file is reported
// only once two consecutive polls have seen the SAME size and modification time. A file still being
// written changes between polls and stays unreported until it stops. The second defence is in the
// reload itself, which builds the new payload before it touches the old one.
//
// That does mean a change is reported one poll later than it happened. That is the price of not
// handing a loader a half-written file, and it is why `poll()` is cheap enough to call often.

#include <cy/core/assets/file.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::assets {

/// What happened to a watched file.
enum class FileChange : u8 {
    /// A file appeared under a watched directory, or a watched path that did not exist now does.
    Created = 0,
    /// The contents changed: a different size, a different modification time, or both.
    Modified = 1,
    /// The file is gone. A watched path stays watched — a file deleted and rewritten by an editor
    /// that saves by rename is a Removed followed by a Created, and dropping the watch on the first
    /// would miss the second.
    Removed = 2,
};

const char* file_change_name(FileChange change) noexcept;

/// One settled change.
///
/// `path` points into the watcher's own storage and is valid until the next call that changes what
/// is watched — another `poll()`, a `watch_*` or an `unwatch`. A consumer that keeps the path keeps
/// a copy of it.
struct FileEvent {
    const char* path = "";
    FileChange change = FileChange::Modified;
    u64 size = 0;
    i64 modified_ns = 0;
};

struct FileWatcherStats {
    /// Files under watch, whether or not they currently exist.
    u32 watched = 0;
    /// Directories being re-enumerated on every poll.
    u32 roots = 0;
    u64 polls = 0;
    /// Files examined across every poll. The watcher's cost, in the unit it is paid in.
    u64 checks = 0;
    u64 events_reported = 0;
    /// Changes seen but not yet reported, because the file had not settled. A number that stays
    /// high means something is writing continuously, which is worth knowing before blaming the
    /// watcher for latency.
    u64 changes_pending = 0;
};

/// Files and directories watched for change, by polling.
///
/// Not thread-safe, on purpose: it is polled from one place — the frame's asset update — and a
/// watcher with a lock invites being polled from several, which is how a reload lands in the middle
/// of a frame that is reading the asset.
class FileWatcher {
public:
    explicit FileWatcher(Allocator& allocator) noexcept;

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    /// Watch one file, whether or not it exists yet. Watching a path that does not exist is
    /// legitimate and useful: the file appearing is a `Created` event.
    ///
    /// Idempotent: watching a path already watched succeeds and adds nothing.
    [[nodiscard]] Status watch_file(const char* path) noexcept;

    /// Watch every file in a directory, and keep watching for files that appear in it.
    ///
    /// The directory is re-enumerated on every poll, which is what makes a new file a `Created`
    /// event rather than something nobody notices. `recursive` descends into subdirectories, at the
    /// cost of enumerating them too.
    [[nodiscard]] Status watch_directory(const char* path, bool recursive) noexcept;

    /// Stop watching a path, and forget what was known about it. A path that came from a watched
    /// directory returns on the next poll; unwatching a directory root is what stops that.
    [[nodiscard]] Status unwatch(const char* path) noexcept;

    /// Forget everything.
    void clear() noexcept;

    /// Report every change that has settled since the last poll, replacing `out`.
    ///
    /// Returns the number of events. Events are in the order the watcher holds its entries, which
    /// is the order they were added — deterministic across runs, which matters because a reload
    /// order that depends on directory enumeration order is a reload order that differs between
    /// machines.
    [[nodiscard]] Expected<u32, Error> poll(Array<FileEvent>& out) noexcept;

    [[nodiscard]] FileWatcherStats stats() const noexcept { return stats_; }

private:
    /// One watched file. The path is a range in `text_` rather than a string of its own: a watcher
    /// over a shader directory holds hundreds of these, and a fixed buffer per entry would be a
    /// kilobyte each for paths that average a fraction of that.
    struct Entry {
        u32 offset = 0;
        u32 length = 0;
        /// The state last REPORTED. What a change is measured against.
        u64 size = 0;
        i64 modified_ns = 0;
        /// The state seen on the previous poll but not yet reported. See the settling rule.
        u64 pending_size = 0;
        i64 pending_modified_ns = 0;
        bool has_pending = false;
        /// Whether the file existed at the last report. A watched path that has never existed
        /// starts false, so its arrival is a Created.
        bool present = false;
        /// Discovered by a directory walk rather than named by a caller. Such an entry is dropped
        /// when its file disappears; an explicitly watched one is kept, because the caller asked
        /// about that path and not about that file.
        bool from_directory = false;
    };

    struct Root {
        u32 offset = 0;
        u32 length = 0;
        bool recursive = false;
    };

    [[nodiscard]] const char* text_at(u32 offset) const noexcept { return text_.data() + offset; }
    [[nodiscard]] Entry* find(const char* path) noexcept;
    [[nodiscard]] Expected<u32, Error> intern(const char* path) noexcept;
    [[nodiscard]] Status add_entry(const char* path, bool from_directory) noexcept;
    /// Walk every root, adding files that are not tracked yet.
    [[nodiscard]] Status scan_roots() noexcept;
    /// Compare one entry against the filesystem, and report it when it has settled.
    [[nodiscard]] Status examine(Entry& entry, Array<FileEvent>& out) noexcept;
    /// Rebuild the path storage, dropping what nothing points at.
    [[nodiscard]] Status compact() noexcept;

    Allocator* allocator_;
    Array<char> text_;
    Array<Entry> entries_;
    Array<Root> roots_;
    FileWatcherStats stats_;
};

}  // namespace cy::assets

#endif  // CY_CORE_ASSETS_HOT_RELOAD_H
