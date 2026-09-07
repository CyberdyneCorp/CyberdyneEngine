#pragma once
// `ResidencyServer` — the shared policy, and the place residency stops being activation.
// Tasks 4.1, 4.2 and 4.3.
//
// --- WHAT THIS SERVER OWNS, AND WHAT IT CANNOT
// ----------------------------------------------------
//
// `residency` — "The residency layer SHALL NOT own page storage, page formats, or page production.
// A change that moves storage into it SHALL be treated as violating this requirement."
//
// So this class holds: a policy per subsystem, a record per resident page (a key, a size and some
// numbers), a request queue, a deadline bus, an importance table and a churn tracker. It holds no
// bytes of any page, and it cannot: a `PageKey` is a subsystem tag and an opaque number, and there
// is no interface here through which a page's contents could arrive. The subsystem fetches,
// decodes, renders or composes its own pages, and *tells* the policy what it did.
//
// The cycle, once per frame:
//
//     request(...)          many times, from every subsystem, deduplicated on the way in
//     schedule(options, s)  the policy decides: these may be brought in, those must go
//     ... the subsystem does the work, in its own storage, on its own threads ...
//     note_resident(...)    it reports what arrived
//     note_released(...)    it reports what it let go
//     end_frame(now)        ages the records, prunes churn, resolves deadlines
//
// --- 4.3: RESIDENCY AND ACTIVATION ARE TWO FACTS, NOT ONE
// -----------------------------------------
//
// The M6 exit criterion is that "a test holds bytes resident with simulation off". design.md §3
// says why it is an exit criterion rather than a detail: "If residency and activation cannot be
// separated, streaming becomes an all-or-nothing operation and the frame budget goes with it."
//
// The separation is structural here, not a convention:
//
//   * `hold()` takes a `PageKey` and a reason and returns a `HoldId`. It does not take, mention or
//     consult an activation state, a world, a tick, or a simulation. A held page cannot be evicted.
//   * `set_active()` records that the owning subsystem has activated whatever the page belongs to.
//     It is an *input to eviction preference* and to diagnostics. Nothing in `schedule()`,
//     `note_resident()` or `hold()` requires it, reads it as a precondition, or changes residency
//     because of it.
//   * There is no simulation clock, no tick counter and no "world running" flag anywhere in this
//     header. `end_frame(now)` takes a wall time; a caller that never simulates anything still
//     calls it, and everything here behaves identically.
//
// A test that holds bytes resident with simulation off is therefore not a special mode: it is the
// ordinary path with `set_active` never called. `test_separation.cpp` is that test, and it asserts
// the converse too — that activating and deactivating changes no byte of residency.
//
// --- THREAD SAFETY, AND WHY THE DESTRUCTOR IS A REQUIREMENT
// ---------------------------------------
//
// Requests arrive from the render thread and completions from IO and production workers, so this
// class is internally synchronised: every public member takes one mutex. That is a policy structure
// updated a few thousand times a frame, not a per-texel path, and a lock-free design here would buy
// nothing and cost the ability to reason about it.
//
// M6 creates and destroys worlds *continuously*, so teardown is not a shutdown path — it is a thing
// that happens while work is outstanding. `unregister_subsystem()` and `reset()` are therefore
// defined mid-flight: they drop the records for outstanding pages, invalidate every hold on them,
// and leave the accounting at zero rather than at whatever was in flight. `tests/test_teardown.cpp`
// tears the server down under concurrent load, repeatedly, because M5.5's gate found the first real
// engine defect in six milestones by doing exactly that to Jolt's job bridge.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/core/memory/pressure.h>
#include <cy/servers/residency/deadline.h>
#include <cy/servers/residency/importance.h>
#include <cy/servers/residency/policy.h>
#include <cy/servers/residency/request.h>
#include <cy/servers/residency/types.h>

#include <mutex>

namespace cy::residency {

/// Why something is being held resident. Recorded so that "why is this still in memory" has an
/// answer that is not "somebody called hold".
enum class HoldReason : u8 {
    Gameplay = 0,    // a gameplay system needs the bytes regardless of what is on screen
    Editor,          // the editor is inspecting or authoring it
    Streaming,       // an in-flight streaming operation depends on it
    MipTail,         // part of a guaranteed-resident tail or root
    CoarseFallback,  // the coarse representation another subsystem is allowed to depend on
    Save,            // the persistence overlay is writing it
    Count,
};

[[nodiscard]] const char* hold_reason_name(HoldReason reason) noexcept;

/// A hold. Opaque and non-zero when valid; releasing an id twice is refused rather than double-
/// counted, which is what makes a teardown that invalidates holds safe rather than merely tidy.
struct HoldId {
    u64 value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    [[nodiscard]] constexpr bool operator==(const HoldId& other) const noexcept {
        return value == other.value;
    }
};

/// How one subsystem depends on another's data.
enum class DependencyKind : u8 {
    /// Satisfied by a guaranteed coarse representation, never by waiting. The only kind a shipping
    /// configuration should contain.
    Coarse = 0,
    /// A declared hard dependency: the work cannot proceed without the other's fine data. Legal to
    /// declare, and `validate_dependencies()` reports a *cycle* of them as a configuration error —
    /// `residency`: "Circular residency dependencies SHALL be detected and reported at
    /// configuration
    /// time", rather than discovered as a stall.
    Hard,
};

/// What the policy decided a subsystem may bring in.
struct Admission {
    PageKey key;
    u64 bytes = 0;
    f32 score = 0.0F;
    /// Seconds until it is needed, or `kNoDeadline`. Passed through so the subsystem can order its
    /// own fetches without asking the deadline bus a second question.
    f64 seconds_until_needed = kNoDeadline;
};

/// What the policy decided must go, and why.
struct EvictionOrder {
    PageKey key;
    u64 bytes = 0;
    f32 score = 0.0F;
    QualityReason cause = QualityReason::Evicted;
};

/// The frame's decisions. Reused across frames by the caller, so scheduling allocates nothing in
/// the steady state.
struct Schedule {
    Array<Admission> admissions;
    Array<EvictionOrder> evictions;

    void clear() noexcept {
        admissions.clear();
        evictions.clear();
    }
};

struct ScheduleOptions {
    f64 now = 0.0;
    /// Most admissions to issue this frame, over all subsystems. The frame's own budget for
    /// starting work; zero means unlimited, which is what an editor or a loading screen wants.
    u32 max_admissions = 0;
};

/// Everything `residency`'s diagnostics requirement asks for, per subsystem.
struct ResidencyStats {
    u64 resident_pages = 0;
    /// Admitted and not yet reported resident. These bytes are spent against the budget already;
    /// see `ResidentPage::pending`.
    u64 pending_pages = 0;
    u64 resident_bytes = 0;
    u64 budget_bytes = 0;
    u64 held_pages = 0;
    u64 active_pages = 0;
    u64 guaranteed_pages = 0;

    u64 requests = 0;    // deduplicated requests seen
    u64 hits = 0;        // requests for a page already resident
    u64 admissions = 0;  // requests the policy started work for
    u64 outscored = 0;   // requests deferred because the frame ran out of admissions
    u64 budget_blocked = 0;
    u64 fetches = 0;  // pages the subsystem reported resident
    u64 evictions = 0;
    u64 evicted_bytes = 0;
    u64 deadline_misses = 0;
    /// Admissions the subsystem never reported resident, reclaimed by the pending expiry. A number
    /// that grows is a subsystem dropping work the policy paid for, which is invisible otherwise.
    u64 abandoned_admissions = 0;

    ChurnStats churn;

    [[nodiscard]] f64 hit_rate() const noexcept {
        return (requests == 0) ? 1.0 : static_cast<f64>(hits) / static_cast<f64>(requests);
    }
    [[nodiscard]] f64 utilisation() const noexcept {
        return (budget_bytes == 0)
                   ? 0.0
                   : static_cast<f64>(resident_bytes) / static_cast<f64>(budget_bytes);
    }
};

/// What a subsystem reports when a page has arrived in its own storage.
///
/// A struct rather than five parameters, because every field is a *description* of storage the
/// policy will never see, and a positional call would make it easy to hand the policy the wrong
/// number for the one thing it does budget on.
struct ResidentReport {
    PageKey key;
    /// Bytes the page occupies in the subsystem's storage.
    u64 bytes = 0;
    /// The detail level actually resident.
    u32 level = 0;
    /// Part of a guaranteed-resident set — a mip tail, a geometry root, a coarse fallback.
    bool guaranteed = false;
    /// What producing it actually cost. Zero leaves the policy's class weight in force.
    f32 production_cost_ms = 0.0F;
    CostClass cost = CostClass::Streamed;
};

/// What a page is at its current quality, and what it would take to change that.
struct Explanation {
    PageKey key;
    QualityReason reason = QualityReason::NeverRequested;
    /// The level that was last asked for, and the level that is resident. Both zero when nothing
    /// asked. `residency`: "the diagnostics SHALL name the cause and the desired and resident
    /// levels".
    u32 desired_level = 0;
    u32 resident_level = 0;
    f32 last_score = 0.0F;
    u32 holds = 0;
    bool active = false;
};

class ResidencyServer final : public PressureResponder {
public:
    /// How many frames an admission may go unreported before the policy takes its bytes back.
    /// Two seconds at 60 Hz: long enough for a slow disk and a cold decompressor, short enough that
    /// a subsystem which dropped the work does not hold a budget for the rest of the session.
    static constexpr u64 kPendingExpiryFrames = 120;

    explicit ResidencyServer(Allocator& allocator = current_allocator()) noexcept;
    ~ResidencyServer() override;

    ResidencyServer(const ResidencyServer&) = delete;
    ResidencyServer& operator=(const ResidencyServer&) = delete;
    ResidencyServer(ResidencyServer&&) = delete;
    ResidencyServer& operator=(ResidencyServer&&) = delete;

    // --- Configuration ---------------------------------------------------------------------------

    Status register_subsystem(Subsystem subsystem, const SubsystemPolicy& policy) noexcept;
    /// Tear one subsystem's policy and page records down. Legal mid-flight: every hold on its pages
    /// is invalidated and its accounting returns to zero. Requests for it submitted concurrently
    /// are refused rather than queued against a policy that no longer exists.
    bool unregister_subsystem(Subsystem subsystem) noexcept;
    [[nodiscard]] bool registered(Subsystem subsystem) const noexcept;
    [[nodiscard]] SubsystemPolicy policy(Subsystem subsystem) const noexcept;

    Status declare_dependency(Subsystem from, Subsystem to, DependencyKind kind) noexcept;
    /// Reports the first cycle of `Hard` dependencies, naming the subsystems in it. Called at
    /// configuration time, which is the requirement.
    [[nodiscard]] Status validate_dependencies() const noexcept;

    /// `residency`: "pinned mode SHALL disable coordinated adjustment together with the renderer's
    /// budget arbiter". Pressure is still recorded and reported; the levers stop moving.
    void set_pinned(bool pinned) noexcept;
    [[nodiscard]] bool pinned() const noexcept;

    /// How long the layer stays at a reduced setting after pressure falls, in seconds. The response
    /// half of the hysteresis; see the note in `policy.h`.
    void set_relax_dwell(f64 seconds) noexcept;

    // --- Importance and deadlines
    // ------------------------------------------------------------------

    Status publish_importance(u64 instance, const ImportanceInputs& inputs) noexcept;
    Status declare_importance_transform(Subsystem subsystem,
                                        const ImportanceTransform& transform) noexcept;
    [[nodiscard]] f32 importance(u64 instance) const noexcept;
    [[nodiscard]] f32 importance_for(u64 instance, Subsystem subsystem) const noexcept;

    /// One prediction becomes one deadline per consumer. See `deadline.h`.
    Status announce(const Prediction& prediction, f64 now) noexcept;
    bool satisfy_deadline(Subsystem subsystem, u64 region, f64 now) noexcept;
    [[nodiscard]] f64 seconds_until(Subsystem subsystem, u64 region, f64 now) const noexcept;
    [[nodiscard]] PredictionAccuracy prediction_accuracy() const noexcept;
    void record_prefetched(PredictionSource source, u64 pages) noexcept;
    void record_sampled(PredictionSource source, u64 pages) noexcept;

    // --- The frame
    // ----------------------------------------------------------------------------------

    /// Submit one request. Deduplicated against the frame's other requests for the same page.
    Status request(const Request& request) noexcept;
    [[nodiscard]] u64 request_submissions() const noexcept;
    [[nodiscard]] usize pending_requests() const noexcept;

    /// Decide. Fills `out` with what may be brought in and what must go, and clears the queue.
    Status schedule(const ScheduleOptions& options, Schedule& out) noexcept;

    /// The subsystem reports a page it now holds. `bytes` is its size in the subsystem's own
    /// storage; the policy stores the number and nothing else.
    Status note_resident(const ResidentReport& report, f64 now) noexcept;
    /// The subsystem could not act on an admission — its own cache was full, its producer was
    /// missing, its queue refused the job. Refunds the committed bytes and counts the admission as
    /// abandoned. Only a PENDING record is cancellable; a page that has already arrived is released
    /// with `note_released`.
    bool cancel_admission(PageKey key) noexcept;
    /// The subsystem reports it has let a page go — because the policy told it to, or because it
    /// invalidated it, or because whatever owned it was destroyed.
    bool note_released(PageKey key, f64 now) noexcept;
    /// The subsystem reports a page was sampled or drawn from. Recency, for eviction.
    bool touch(PageKey key, f64 now, f32 screen_contribution, f32 page_importance) noexcept;

    /// Age the records, prune churn, resolve deadlines. Once per frame, after the subsystems have
    /// reported. Takes a wall clock; there is deliberately no simulation time here.
    void end_frame(f64 now) noexcept;
    [[nodiscard]] u64 frame() const noexcept;

    // --- Residency, held independently of activation
    // ------------------------------------------------

    /// Hold a page resident. See the header note: this consults nothing about activation.
    [[nodiscard]] Expected<HoldId, Error> hold(PageKey key, HoldReason reason) noexcept;
    bool release(HoldId id) noexcept;
    [[nodiscard]] u32 holds(PageKey key) const noexcept;
    [[nodiscard]] usize outstanding_holds() const noexcept;

    /// Record that the owning subsystem has activated (or deactivated) what this page belongs to.
    /// An input to eviction preference and to diagnostics. Never a precondition for residency.
    Status set_active(PageKey key, bool active) noexcept;
    [[nodiscard]] bool is_active(PageKey key) const noexcept;
    /// Whether the bytes are actually there. A page that has been admitted and not yet reported is
    /// NOT resident: it holds budget, and nothing else.
    [[nodiscard]] bool is_resident(PageKey key) const noexcept;
    [[nodiscard]] bool is_pending(PageKey key) const noexcept;
    [[nodiscard]] u64 resident_bytes(Subsystem subsystem) const noexcept;
    [[nodiscard]] u64 active_pages(Subsystem subsystem) const noexcept;

    // --- Pressure
    // ------------------------------------------------------------------------------------

    [[nodiscard]] const char* responder_name() const noexcept override { return "residency"; }
    void on_pressure(PressureLevel level, PressureLevel previous) noexcept override;

    [[nodiscard]] PressureLevel level() const noexcept;
    [[nodiscard]] f32 lever(Subsystem subsystem, Lever lever) const noexcept;
    /// The last reduction plan applied, in the order it was applied. Copies out under the lock, so
    /// a diagnostic can print it without racing the next pressure change.
    Status last_reduction(Array<ReductionStep>& out) const noexcept;

    // --- Diagnostics
    // ---------------------------------------------------------------------------------

    [[nodiscard]] ResidencyStats stats(Subsystem subsystem) const noexcept;
    [[nodiscard]] ResidencyStats stats() const noexcept;  // summed
    [[nodiscard]] Explanation explain(PageKey key) const noexcept;

    /// Drop every page record, hold, request and deadline; keep the registrations, the policies and
    /// the lever positions. What a world teardown calls.
    void reset() noexcept;

private:
    struct HoldRecord {
        PageKey key;
        HoldReason reason = HoldReason::Gameplay;
    };

    struct RequestTrace {
        QualityReason reason = QualityReason::NeverRequested;
        u32 desired_level = 0;
        f32 score = 0.0F;
    };

    [[nodiscard]] bool registered_locked(Subsystem subsystem) const noexcept;
    [[nodiscard]] ResidentPage* find_page_locked(PageKey key) noexcept;
    [[nodiscard]] const ResidentPage* find_page_locked(PageKey key) const noexcept;
    /// Remove a page from the books and record the order. The eviction *is* the removal here: the
    /// policy stops counting the bytes at the moment it tells the subsystem to let them go, and a
    /// subsystem that fails to comply reports the page resident again on its next frame.
    void evict_locked(usize slot, QualityReason cause, f64 now, Schedule& out) noexcept;
    void remove_page_locked(usize slot) noexcept;
    /// Try to make `bytes` of room in `subsystem` by ordering evictions. Returns whether it could.
    [[nodiscard]] bool make_room_locked(Subsystem subsystem, u64 bytes, f64 now,
                                        Schedule& out) noexcept;
    [[nodiscard]] bool has_room_locked(Subsystem subsystem, u64 bytes) const noexcept;
    void apply_pressure_locked(PressureLevel level, f64 now) noexcept;
    void note_trace_locked(PageKey key, QualityReason reason, u32 desired_level,
                           f32 score) noexcept;
    /// Spend the budget at the moment an admission is issued. See `ResidentPage::pending`.
    [[nodiscard]] Status commit_locked(const ScoredRequest& entry, f64 now) noexcept;
    void expire_pending_locked() noexcept;
    void sync_holds_locked(PageKey key) noexcept;

    mutable std::mutex mutex_;

    SubsystemPolicy policies_[kSubsystemCount] = {};
    bool registered_[kSubsystemCount] = {};
    f32 levers_[kSubsystemCount * kLeverCount] = {};
    u8 dependencies_[kSubsystemCount][kSubsystemCount] = {};  // 0 none, 1 coarse, 2 hard
    ResidencyStats stats_[kSubsystemCount] = {};
    u64 resident_bytes_[kSubsystemCount] = {};

    /// An array plus an index rather than a map of records. Two reasons, both load-bearing: the
    /// records are *mutated* every frame (ages, recency, activation) and `HashMap`'s iterator is
    /// const; and eviction has to consider every candidate in a deterministic order, which a hash
    /// table's iteration order is not.
    Array<ResidentPage> pages_;
    HashMap<u64, usize> page_slots_;

    HashMap<u64, HoldRecord> hold_records_;  // hold id -> what it holds
    HashMap<u64, u32> hold_counts_;          // packed page key -> outstanding holds
    HashMap<u64, RequestTrace> traces_;
    /// Pages evicted in the current frame. The other half of "a page evicted this frame is not
    /// requested again the next": nothing evicted this frame is re-admitted this frame, whatever it
    /// scores.
    HashMap<u64, u64> evicted_this_frame_;

    RequestQueue queue_;
    ImportanceTable importance_;
    DeadlineBus deadlines_;
    ChurnTracker churn_;
    Array<ReductionStep> last_plan_;

    u64 next_hold_ = 1;
    u64 frame_ = 0;
    f64 level_since_ = 0.0;
    f64 last_now_ = 0.0;
    f64 relax_dwell_ = 0.5;
    PressureLevel level_ = PressureLevel::Normal;
    /// The level the monitor last announced. It differs from `level_` only while a relaxation is
    /// waiting out the dwell, which is the one state the asymmetric hysteresis needs a name for.
    PressureLevel pending_level_ = PressureLevel::Normal;
    bool pinned_ = false;
};

}  // namespace cy::residency
