#pragma once
// Budgets, reduction levers, eviction and churn: the policy every paged subsystem shares.
// Tasks 4.1 and 4.2.
//
// `residency` — "Budgets and pressure response" and "Eviction and churn control". Two requirements,
// one file, because they are the same argument seen from either end: what to stop bringing in, and
// what to let go of.
//
// --- COORDINATED REDUCTION IS AN ORDER, NOT A BROADCAST
// -------------------------------------------
//
// "On rising memory pressure the layer SHALL apply a **coordinated reduction** across subsystems
// weighted by importance and by visible impact, rather than each subsystem independently evicting —
// which produces one subsystem freeing memory another immediately consumes."
//
// The failure named in that sentence is the reason `SubsystemPolicy::reduction_order` exists and
// the reason reduction is applied by walking the subsystems in it. A pressure signal delivered to
// six subsystems at once is six independent evictions with extra steps; a declared order is a plan,
// and `ReductionStep` is that plan written down so a diagnostic can print it.
//
// --- THE LEVERS ARE DECLARED PER SUBSYSTEM
// ---------------------------------------------------------
//
// "Reduction levers SHALL be declared per subsystem: texture mip bias and prefetch radius, geometry
// error threshold, shadow page resolution and refresh rate, illumination cache density." Those six
// are `Lever`, and a subsystem declares a value for each pressure level in its `LeverSchedule`. A
// lever nobody declared is not adjusted — which is what lets the audio and world-cell consumers sit
// in the same enumeration as the four caches without pretending to have a mip bias.
//
// --- HYSTERESIS IS ASYMMETRIC ON PURPOSE
// ------------------------------------------------------------
//
// Tightening is immediate; relaxing waits out a dwell. `PressureMonitor` already applies hysteresis
// to the *level* (see `cy/core/memory/pressure.h`), and this is a second, coarser one on the
// *response*: pressure that oscillates between Normal and Elevated inside a second would otherwise
// re-fill the caches it just trimmed, which is the churn the next requirement measures.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/budget.h>
#include <cy/core/memory/domain.h>
#include <cy/core/memory/hash_map.h>
#include <cy/core/memory/pressure.h>
#include <cy/servers/residency/types.h>

namespace cy::residency {

/// `residency`'s six levers, in the order it lists them.
enum class Lever : u8 {
    TextureMipBias = 0,
    TexturePrefetchRadius,
    GeometryErrorThreshold,
    ShadowResolutionScale,
    ShadowRefreshInterval,
    IlluminationCacheDensity,
    Count,
};

inline constexpr u32 kLeverCount = static_cast<u32>(Lever::Count);

[[nodiscard]] const char* lever_name(Lever lever) noexcept;

/// What one lever reads at each pressure level. `declared` is not redundant with the three numbers:
/// a subsystem that leaves a lever alone and one that declares the same value at every level are
/// different statements, and only the second should appear in a reduction plan.
struct LeverSchedule {
    bool declared = false;
    f32 normal = 0.0F;
    f32 elevated = 0.0F;
    f32 critical = 0.0F;

    [[nodiscard]] f32 at(PressureLevel level) const noexcept {
        switch (level) {
            case PressureLevel::Normal:
                return normal;
            case PressureLevel::Elevated:
                return elevated;
            case PressureLevel::Critical:
                return critical;
        }
        return normal;
    }
};

/// Everything the policy knows about one subsystem. It knows no storage — see the note at the top
/// of `types.h`.
struct SubsystemPolicy {
    /// Which budget in the memory budget tree this subsystem draws from.
    MemoryDomain domain = MemoryDomain::Gpu;
    /// Bytes. Zero means unbudgeted, which is `BudgetTree`'s meaning of zero and is not the same as
    /// a budget of zero.
    u64 budget_bytes = 0;
    BudgetKind budget_kind = BudgetKind::Hard;

    /// Lower reduces first. The declared order in "reduce across subsystems in a declared order".
    u32 reduction_order = 0;

    /// Instances at or above this shared importance keep their quality while everything below it
    /// degrades. `residency`: "high-importance instances SHALL retain quality while background
    /// content degrades first".
    f32 protected_importance = 0.8F;

    /// `residency`: "A page SHALL have a **minimum residency age** before becoming evictable".
    /// Counted in frames rather than seconds because the oscillation it prevents is per frame.
    u32 min_residency_frames = 2;

    /// How recently a page must have been evicted for its re-request to count as churn.
    f64 churn_window_seconds = 1.0;

    /// What producing a page of this subsystem costs when nobody has measured one.
    CostClass default_cost = CostClass::Streamed;

    LeverSchedule levers[kLeverCount] = {};
};

/// One line of a reduction plan: which lever of which subsystem moved, from what to what.
struct ReductionStep {
    Subsystem subsystem = Subsystem::Geometry;
    Lever lever = Lever::TextureMipBias;
    f32 from = 0.0F;
    f32 to = 0.0F;
    u32 order = 0;
};

/// A page the owning subsystem has told the policy it is holding. The policy stores this record and
/// *not the page* — the distinction the whole capability rests on.
struct ResidentPage {
    PageKey key;
    u64 bytes = 0;
    /// The detail level actually resident — a mip, a geometry LOD, a shadow resolution step. The
    /// "resident level" half of `residency`'s "the desired and resident levels"; the desired half
    /// lives on the request, because it is a property of the asking and not of the page.
    u32 level = 0;
    f64 became_resident_at = 0.0;
    u64 resident_frames = 0;
    f64 last_used_at = 0.0;
    f32 importance = 0.0F;
    f32 screen_contribution = 0.0F;
    f32 production_cost_ms = 0.0F;
    CostClass cost = CostClass::Streamed;

    /// Admitted and not yet reported resident. A pending record holds its bytes against the budget
    /// but is NOT resident: nothing can sample it, activate it or evict it.
    ///
    /// WHY THE POLICY COUNTS BYTES THAT HAVE NOT ARRIVED. Without this, a frame that admits five
    /// pages into a three-page budget sees an empty cache at every decision and admits all five —
    /// the arrivals only push the accounting over afterwards, when it is too late to refuse. The
    /// budget has to be spent at the moment it is committed, not at the moment it is occupied.
    bool pending = false;
    /// Outstanding holds. A page with a hold is not evictable, and a hold says nothing whatever
    /// about whether the page is active. See `server.h`.
    u32 holds = 0;
    /// Part of a guaranteed-resident set: a mip tail, a virtual geometry root, a coarse fallback.
    /// `residency`: "No residency system blocks another" is satisfied by these existing.
    bool guaranteed = false;
    /// Whether the owning subsystem has *activated* what this page belongs to. Reported to the
    /// policy so eviction can prefer inactive pages — and never required for residency.
    bool active = false;
};

/// Eviction preference. LOWER evicts first.
///
/// `residency`: eviction "SHALL consider recency, importance, screen contribution, regeneration
/// cost, and whether a page is pinned or guaranteed resident". All five are here. A held or
/// guaranteed page returns `kNeverEvict` rather than a large number, so that "pinned" is a fact
/// rather than a weight that a sufficiently desperate frame could outbid.
[[nodiscard]] f32 eviction_score(const ResidentPage& page, f64 now) noexcept;

inline constexpr f32 kNeverEvict = 3.4e38F;

/// Whether the minimum residency age has elapsed. The other half of "no oscillation": a page that
/// arrived this frame cannot leave this frame, however badly it scores.
[[nodiscard]] bool past_minimum_age(const ResidentPage& page,
                                    const SubsystemPolicy& policy) noexcept;

/// Churn: pages evicted and re-requested inside the window. Measured per subsystem, because
/// "sustained churn indicates a budget too small or a policy misconfigured, and is invisible in
/// hit-rate statistics alone" — a cache can hold a 95% hit rate while thrashing the other 5%.
struct ChurnStats {
    u64 evictions = 0;
    u64 refetches = 0;  // re-requested within the window after eviction

    /// Refetches over evictions, in [0, 1]. Zero when nothing was evicted.
    [[nodiscard]] f64 rate() const noexcept {
        return (evictions == 0) ? 0.0 : static_cast<f64>(refetches) / static_cast<f64>(evictions);
    }
};

class ChurnTracker {
public:
    explicit ChurnTracker(Allocator& allocator = current_allocator()) noexcept
        : evicted_(allocator) {}

    ChurnTracker(const ChurnTracker&) = delete;
    ChurnTracker& operator=(const ChurnTracker&) = delete;
    ChurnTracker(ChurnTracker&&) noexcept = default;
    ChurnTracker& operator=(ChurnTracker&&) noexcept = default;
    ~ChurnTracker() = default;

    Status note_eviction(PageKey key, f64 now) noexcept;
    /// Returns true when this request re-asks for a page evicted inside the window — a churn event.
    bool note_request(PageKey key, f64 now, f64 window_seconds) noexcept;
    /// Forget evictions older than the window, so the table does not grow with the session.
    void prune(f64 now, f64 window_seconds) noexcept;

    [[nodiscard]] ChurnStats stats(Subsystem subsystem) const noexcept;
    [[nodiscard]] usize tracked() const noexcept { return evicted_.size(); }
    void clear() noexcept;

private:
    HashMap<u64, f64> evicted_;  // packed page key -> when
    ChurnStats stats_[kSubsystemCount] = {};
};

/// Compute the reduction plan for a pressure level: the levers that move, in declared order.
///
/// A free function taking the policies and their current lever values, so that the plan can be
/// computed and inspected without a server — which is what lets a test assert the ORDER rather than
/// only the outcome.
Status plan_reduction(const SubsystemPolicy* policies, const bool* registered,
                      const f32* current_levers, PressureLevel level,
                      Array<ReductionStep>& out) noexcept;

}  // namespace cy::residency
