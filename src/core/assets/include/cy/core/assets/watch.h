#ifndef CY_CORE_ASSETS_WATCH_H
#define CY_CORE_ASSETS_WATCH_H
// Watching the virtual filesystem for change. Task 1.4, carried forward from M2.
//
// `core-assets-and-io` — "Hot reload": "In development builds, the asset system SHALL watch source
// files and cooked outputs and reload changed assets in place, preserving existing `Ref`s", and
// "Reload SHALL notify dependents so derived state (GPU uploads, material instances, shader
// pipelines) is rebuilt."
//
// M2 closed with that at zero: no watcher, no reload entry point. M3's shader iteration is the
// first thing that actually wants one — an edited .slang recompiling without a restart is the
// difference between a shader change costing seconds and costing a minute — so the watcher is built
// here, and `AssetSystem::reload()` is the half that acts on what it reports.
//
// --- WHY POLLING, AND WHY A FINGERPRINT RATHER THAN A TIMESTAMP ----------------------------------
//
// This module is layer 0. inotify, ReadDirectoryChangesW and FSEvents are the platform's, and
// platform code lives in platform/ at layer 3, which layer 0 may not reach — the same rule that
// keeps SDL out of core. A native notification backend therefore belongs behind the display and
// platform seam when someone needs it, and this class is shaped so that adding one changes an
// implementation rather than an interface: the caller's contract is "call poll(); you are told what
// changed", which a native backend satisfies without moving.
//
// Change is detected as (size, content hash) rather than as a modification time. Three reasons, and
// the first is decisive:
//
//   * `Mount` has no modification time. It has `size_of`, `read` and `enumerate`, and it is
//     implemented over directories, memory, packages and a remote host — a timestamp is meaningful
//     for one of those four. A fingerprint is meaningful for all of them, so the watcher works over
//     the namespace rather than over one kind of mount.
//   * Size alone misses the edit that matters most here. Changing a constant in a shader, a
//     parameter in a material, a number in a config: same length, different content. A watcher that
//     misses those is a watcher whose users learn not to trust it.
//   * A timestamp is what a build system uses because it cannot afford to read the file. A
//     development watcher over the set of files somebody is *editing* can afford it.
//
// The cost is honest and stated: one full read per watched file per poll, so watch what is being
// iterated on, not the whole content tree, and poll at a human rate (a few times a second) rather
// than every frame. `max_fingerprint_bytes` is the ceiling past which a file is fingerprinted by
// its size alone, so that a watch which accidentally names a two-gigabyte package does not read it.
//
// --- THE SETTLE PERIOD ---------------------------------------------------------------------------
//
// `core-assets-and-io`'s second hot-reload scenario is "re-import fails (malformed file
// mid-write)". An editor that writes a file in place is briefly holding a file that is truncated,
// and a watcher that reports the instant it changes reports that truncation as content. So a change
// is reported only once the file has held the *same* new fingerprint across two polls at least
// `settle_ns` apart. It costs one poll interval of latency and removes a whole class of failed
// reloads; the reload path still keeps the old asset when the new bytes are bad, because a settle
// period is a mitigation and not a guarantee.

#include <cy/core/assets/hash.h>
#include <cy/core/assets/path.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::assets {

/// What happened to a watched path.
enum class FileChange : u8 {
    /// A path that was not there at the last poll is there now.
    Added = 0,
    /// The same path, different content.
    Modified = 1,
    /// A path that was there is gone.
    Removed = 2,
};

const char* file_change_name(FileChange change) noexcept;

/// One reported change. Handed to the observer during `poll`, and valid only for that call.
struct WatchEvent {
    const VirtualPath* path = nullptr;
    FileChange change = FileChange::Modified;
    /// The size after the change. Zero for `Removed`.
    u64 size = 0;
    /// The content hash after the change, or zero for `Removed` and for a file past
    /// `max_fingerprint_bytes`.
    ContentHash hash;
    /// Which watch reported it — the path passed to `watch()`. A caller that watches several trees
    /// uses this to route the event without re-deriving which tree the file is under.
    const VirtualPath* root = nullptr;
};

/// Called once per change, from inside `poll` on the polling thread.
using WatchObserver = void (*)(void* user, const WatchEvent& event) noexcept;

struct FileWatcherConfig {
    /// How long a new fingerprint must hold before the change is reported. See the header.
    i64 settle_ns = 100'000'000;  // 100 ms
    /// Files larger than this are fingerprinted by size alone, and a same-size edit to one is not
    /// detected. The default is generous for source files and small for content.
    usize max_fingerprint_bytes = usize{16} * 1024 * 1024;
};

struct FileWatcherStats {
    u64 polls = 0;
    /// Paths examined, summed over every poll. The cost, in one number.
    u64 paths_examined = 0;
    u64 bytes_read = 0;
    u64 added = 0;
    u64 modified = 0;
    u64 removed = 0;
    /// Settle periods started — a new fingerprint sighted. It exceeds `added + modified` by the
    /// number of changes that were superseded or reverted before they settled, so a number climbing
    /// far past them means something is rewriting a file continuously.
    u64 settling = 0;
    /// Files whose size exceeded `max_fingerprint_bytes`, so a same-size edit would be missed.
    u64 size_only = 0;
};

/// Watches paths in a `VirtualFileSystem` and reports what changed.
///
/// Not thread-safe and deliberately not internally threaded: `poll` reads files, and a class that
/// started a thread to do that would be the second place in the engine deciding when I/O happens.
/// The caller owns the schedule — an editor ticks it, a tool calls it in a loop — and the caller is
/// responsible for calling it from a thread where blocking is legal, which is never a job worker.
class FileWatcher {
public:
    explicit FileWatcher(Allocator& allocator) noexcept;

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    /// Attach to a namespace. The watcher holds no ownership of it and must not outlive it.
    [[nodiscard]] Status start(VirtualFileSystem& files, const FileWatcherConfig& config) noexcept;
    void stop() noexcept;
    [[nodiscard]] bool is_running() const noexcept { return files_ != nullptr; }

    /// Watch a file, or a directory.
    ///
    /// A directory watch is recursive and reports files rather than directories: a directory has no
    /// content of its own to fingerprint, and "a file appeared three levels down" is the event a
    /// caller wants rather than "something under here changed".
    ///
    /// The first poll after a watch is added reports nothing for it: the paths already present are
    /// the baseline, not a hundred `Added` events for files nobody touched. `prime()` states that
    /// explicitly for a caller that wants the baseline taken now.
    [[nodiscard]] Status watch(const VirtualPath& path) noexcept;

    /// Stop watching, and forget what was known about everything beneath it.
    [[nodiscard]] Status unwatch(const VirtualPath& path) noexcept;

    [[nodiscard]] usize watch_count() const noexcept { return roots_.size(); }
    /// How many paths the watcher currently knows the state of.
    [[nodiscard]] usize tracked_count() const noexcept { return tracked_.size(); }

    /// Take the baseline without reporting anything. Equivalent to a poll whose events are dropped.
    [[nodiscard]] Status prime(i64 now_ns) noexcept;

    /// Examine every watched path and report what changed. Returns how many events were reported.
    ///
    /// `now_ns` is the caller's monotonic clock, passed in rather than read here: a test drives it
    /// with `cy::test::DeterministicClock` and gets a settle period it can step across, which is
    /// the difference between testing the debounce and sleeping in a test.
    [[nodiscard]] Expected<u32, Error> poll(i64 now_ns, WatchObserver observer,
                                            void* user) noexcept;

    [[nodiscard]] const FileWatcherStats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = FileWatcherStats{}; }

private:
    /// What is known about one path, and what is proposed for it.
    struct Tracked {
        VirtualPath path;
        u64 size = 0;
        ContentHash hash;
        /// Which watch root brought it in, as an index into `roots_`.
        u32 root = 0;
        /// Cleared before each sweep and set by it; what is still clear afterwards is gone.
        bool present = false;
        /// Tracked for the first time: the fingerprint above is empty and the event this entry
        /// eventually reports is `Added` rather than `Modified`.
        bool is_new = false;
        /// A different fingerprint has been seen but has not settled yet.
        bool settling = false;
        i64 settling_since_ns = 0;
        u64 settling_size = 0;
        ContentHash settling_hash;
    };

    /// The state one sweep of one path produced.
    struct Fingerprint {
        u64 size = 0;
        ContentHash hash;
    };

    [[nodiscard]] Expected<Fingerprint, Error> fingerprint_of(const VirtualPath& path,
                                                              u64 size) noexcept;
    /// Find a tracked path by binary search; `tracked_` is kept sorted so this is not a scan.
    [[nodiscard]] usize find_tracked(const VirtualPath& path) const noexcept;
    [[nodiscard]] Status observe(const VirtualPath& path, u32 root, u64 size, i64 now_ns) noexcept;

    /// The sweep's shared state, so the enumeration visitor — a plain function pointer — can reach
    /// it without the watcher having to be a global.
    struct Sweep {
        FileWatcher* watcher = nullptr;
        u32 root = 0;
        i64 now_ns = 0;
        Status status = ok();
    };
    static bool visit_entry(void* user, const VirtualEntry& entry) noexcept;

    VirtualFileSystem* files_ = nullptr;
    FileWatcherConfig config_{};
    Allocator* allocator_;
    Array<VirtualPath> roots_;
    /// Sorted by path. A sorted array rather than a hash map because the sweep walks it in order
    /// and because a watcher over an editing session holds hundreds of entries, not millions.
    Array<Tracked> tracked_;
    Array<u8> scratch_;
    FileWatcherStats stats_{};
    bool primed_ = false;
};

}  // namespace cy::assets

#endif  // CY_CORE_ASSETS_WATCH_H
