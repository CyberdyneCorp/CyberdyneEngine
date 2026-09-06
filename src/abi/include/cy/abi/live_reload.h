// cy/abi/live_reload.h — reloading a module WHILE THE RUNTIME IS RUNNING. M5 task 1.1.
//
// --- WHAT WAS MISSING, AND WHY IT IS THE FIRST THING M5 NEEDS ------------------------------------
//
// `cy/abi/module.h` implements the reload SEQUENCE and M4 proved it: serialize through the
// generation that created each instance, migrate by name, recreate, and never `dlclose`. What M4
// did not build is anything that performs that sequence against a runtime that is *live*. Every
// caller of `BehaviourRuntime::reload()` in the tree before this file was a test that had stopped
// ticking, chose the next image by hand, and knew when the swap was safe.
//
// A live-editing session has none of those. The build writes a new image while the loop is running,
// nobody tells the loop, and the moment at which a swap is safe is a property of where the frame
// is. So this file is three things and nothing else:
//
//   1. NOTICING. A poll over the staging directory that costs one enumeration and no file reads,
//      and that reports a new generation only once its size has stopped changing.
//   2. DECIDING WHEN. `apply_at_frame_boundary()` — the ONE place a swap happens, and it refuses
//      while the world is iterating rather than corrupting it. `cy/ecs/world.h` is explicit that
//      structural change during iteration is refused; a reload is a great deal more than a
//      structural change.
//   3. REMEMBERING. Counters, the last report, and the generation now live, so that a session that
//      has reloaded forty times can say what happened without a log scrape.
//
// --- WHY A DIRECTORY OF GENERATIONS RATHER THAN ONE FILE WATCHED FOR CHANGE ----------------------
//
// Because of the third finding in cy/abi/module.h: a unique FILENAME is not enough for a Swift
// image — the Swift MODULE NAME must differ too, or name-based type lookup in the second image
// resolves to the first image's metadata, first-registration-wins, forever. That is a property of
// how the image was BUILT, so it cannot be recovered by copying a file to a new path at load time.
//
// `bindings/swift/tools/cy_swift_module.py` already builds each generation as its own file with its
// own `-module-name` — `libCyGame_g0.so`, `libCyGame_g1.so`, ... — which is exactly the shape a
// correct reload needs. So this watcher looks for `<stem>_g<N><extension>` and takes the highest
// `N` above the live generation. Watching one path for content change would work for a C module and
// would be silently wrong for a Swift one, which is the failure mode this whole area is written
// against.
//
// A caller that has its own build pipeline and knows the path can skip all of that and call
// `request(path)`. That is the editor's live bridge: it rebuilds, it knows what it wrote, and it
// asks. The two routes converge on the same pending slot, so exactly one reload happens per frame
// boundary whichever way it was asked for.
//
// --- THE SETTLE RULE, AND WHY IT IS SIZE AND NOT CONTENT -----------------------------------------
//
// A file that is being written is a file that is briefly truncated. `cy/core/assets/watch.h` argues
// the general case and fingerprints content; here the cheaper rule is enough, because the failure
// it guards against is already handled: a half-written image fails `dlopen`, which is
// `ReloadFailure::ImageDidNotOpen`, which keeps the previous generation live and reports. So the
// settle is one poll of size stability — it removes the common case at the cost of one poll of
// latency, and the reload path remains correct without it.
//
// --- THREADING ----------------------------------------------------------------------------------
//
// NOT THREAD-SAFE, and deliberately so: `BehaviourRuntime` is not either, and for the same reason.
// Every entry here is called from the thread that owns the runtime — the one that ticks. An editor
// that discovers a new build on a background thread hands the path across its own queue and calls
// `request()` on the tick thread; a mutex here would be a mutex that is never contended and that
// implied a safety this file cannot provide, because the reload itself calls into module code.

#pragma once

#include <cy/abi/module.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

namespace cy::ecs {
class World;
}  // namespace cy::ecs

namespace cy::abi {

/// The longest module path this watcher carries. A pending path is held in the object rather than
/// borrowed, because the caller that asked for the reload is usually gone by the frame boundary
/// that performs it.
inline constexpr usize kMaxLiveReloadPath = 512;

/// Where the next generation comes from.
///
/// `directory` is scanned for `<stem>_g<N><extension>`. Nothing else in it is looked at, so a
/// staging directory that also holds object files, a `.swiftmodule` or a previous generation's
/// debug information costs one string comparison each.
struct LiveReloadSource {
    const char* directory = "";
    const char* stem = "";
    const char* extension = "";
};

/// What a poll or an apply did.
enum class LiveReloadOutcome : u8 {
    /// Nothing new. The overwhelmingly common answer, and the one that must cost nothing.
    Idle = 0,
    /// A newer generation exists and is not yet stable, or is stable and waiting for the boundary.
    Waiting = 1,
    /// The swap happened. `report.generation` is now live and every instance was carried across.
    Applied = 2,
    /// The swap was attempted and refused. The previous generation is still live and every instance
    /// is still valid — `native-abi`'s "Incompatible reload". `report.failure` says which of the
    /// five it was.
    Refused = 3,
    /// The boundary was not a boundary: the world was iterating. Nothing was attempted, and the
    /// request is still pending for the next call.
    Deferred = 4,
};

[[nodiscard]] const char* live_reload_outcome_name(LiveReloadOutcome outcome) noexcept;

/// The state a live session reads. Every field is a number or a report so that an editor panel and
/// a test assert on the same thing.
struct LiveReloadStatus {
    LiveReloadOutcome outcome = LiveReloadOutcome::Idle;
    /// The generation live right now.
    u32 generation = 0;
    /// The generation a pending reload would move to, or `generation` when nothing is pending.
    u32 candidate = 0;
    /// The last completed attempt, successful or not. Zeroed until the first one.
    ReloadReport report{};
    u64 polls = 0;
    u32 applied = 0;
    u32 refused = 0;
    /// Boundaries at which a pending reload could not be applied because the world was iterating.
    /// A number that is never zero in a healthy session means the caller is polling from inside a
    /// system rather than between frames.
    u32 deferred = 0;
};

/// The thing a running loop calls once per frame.
///
/// IT MUST NOT OUTLIVE ITS RUNTIME, for the reason `BehaviourRuntime` must not outlive its host:
/// a pending reload holds nothing of the runtime, but `apply_at_frame_boundary()` calls into it.
/// Declare the host, then the runtime, then this.
class LiveReload {
public:
    explicit LiveReload(BehaviourRuntime& runtime) noexcept;

    LiveReload(const LiveReload&) = delete;
    LiveReload& operator=(const LiveReload&) = delete;
    LiveReload(LiveReload&&) = delete;
    LiveReload& operator=(LiveReload&&) = delete;

    /// Start watching. Records the generation that is live now, so the image already loaded does
    /// not read as a change on the first poll.
    ///
    /// Fails with `InvalidArgument` when any of the three parts is empty, and with `NotFound` when
    /// the directory does not exist — a watcher pointed at a directory that is never created is a
    /// watcher that reports "nothing changed" for the whole session, which is the failure that
    /// looks exactly like a working one.
    [[nodiscard]] Status watch(const LiveReloadSource& source) noexcept;

    /// Ask for a reload from an explicit path at the next boundary. THE EDITOR'S ROUTE: it built
    /// the image, it knows where it is, and it does not want a directory scan to rediscover it.
    ///
    /// The path is copied. A second request before the boundary replaces the first rather than
    /// queueing behind it: two pending reloads are two swaps of which only the last matters, and
    /// performing the first would destroy and recreate every instance for nothing.
    [[nodiscard]] Status request(const char* library_path) noexcept;

    /// One cheap look at the staging directory. Safe to call every frame; safe to call never, if
    /// the caller only ever uses `request()`.
    ///
    /// Returns the outcome it reached: `Idle`, or `Waiting` once a newer generation has been seen
    /// (whether or not it has settled yet).
    LiveReloadOutcome poll() noexcept;

    /// THE ONE PLACE A SWAP HAPPENS.
    ///
    /// Does nothing and returns `Idle` when nothing is pending — which is every frame but a handful
    /// in a long session, so that path allocates nothing, opens nothing and stats nothing.
    ///
    /// `world` is inspected and not modified: a reload while a query is iterating would move
    /// storage under it, so this returns `Deferred` and leaves the request pending. The caller's
    /// fix is to call this between stages rather than inside one.
    [[nodiscard]] Expected<LiveReloadStatus, Error> apply_at_frame_boundary(
        const ecs::World& world) noexcept;

    [[nodiscard]] bool pending() const noexcept { return pending_; }
    [[nodiscard]] const char* pending_path() const noexcept {
        return pending_ ? pending_path_ : "";
    }
    [[nodiscard]] const LiveReloadStatus& status() const noexcept { return status_; }

private:
    /// The highest `<stem>_g<N><extension>` in the staging directory, and its size. `N` is zero and
    /// the size is zero when there is none.
    struct Candidate {
        u32 generation = 0;
        u64 size = 0;
        bool found = false;
    };

    [[nodiscard]] Candidate newest_generation() const noexcept;
    [[nodiscard]] Status set_pending(const char* path, u32 generation) noexcept;

    BehaviourRuntime& runtime_;

    LiveReloadSource source_{};
    bool watching_ = false;

    /// The candidate seen at the previous poll, and its size. A candidate is promoted to pending
    /// only when the same generation shows the same size twice — the settle rule in the header.
    u32 seen_generation_ = 0;
    u64 seen_size_ = 0;

    /// Every generation this watcher is finished with: loaded, refused, or unnameable. A scan never
    /// looks below it, which is what stops one bad build from being retried at every poll.
    u32 judged_generation_ = 0;

    bool pending_ = false;
    char pending_path_[kMaxLiveReloadPath] = {};
    u32 pending_generation_ = 0;

    LiveReloadStatus status_{};
};

}  // namespace cy::abi
