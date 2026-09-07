#include <cy/servers/residency/policy.h>

#include <cmath>

namespace cy::residency {

namespace {

/// How the five eviction terms are weighed. Recency dominates because it is the only one measured
/// rather than declared; the rest shift a page a band up or down within its recency.
constexpr f32 kRecencyHalfLifeSeconds = 2.0F;
constexpr f32 kImportanceWeight = 1.5F;
constexpr f32 kScreenWeight = 1.0F;
constexpr f32 kCostWeight = 0.35F;
/// Cost enters as log2(milliseconds) and stops counting past this. Six is 64 ms — about four
/// frames of production. Beyond that the difference stops meaning anything: a page that expensive
/// is regenerated over several frames whether it cost 64 ms or 1000, and without the cap one
/// pathologically expensive page outranks ten that are actually on screen.
constexpr f32 kCostLog2Cap = 6.0F;
constexpr f32 kActiveBonus = 0.75F;

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    if (!(value > 0.0F)) {
        return 0.0F;
    }
    return (value > 1.0F) ? 1.0F : value;
}

[[nodiscard]] f32 effective_cost_ms(const ResidentPage& page) noexcept {
    return (page.production_cost_ms > 0.0F) ? page.production_cost_ms
                                            : cost_class_weight(page.cost);
}

/// Whether `lhs` reduces before `rhs`: the declared order first, the enumeration's order to break a
/// tie. A named function rather than a lambda, so the ordering rule reads once and is testable.
[[nodiscard]] bool precedes(const SubsystemPolicy* policies, u32 lhs, u32 rhs) noexcept {
    if (policies[lhs].reduction_order != policies[rhs].reduction_order) {
        return policies[lhs].reduction_order < policies[rhs].reduction_order;
    }
    return lhs < rhs;
}

}  // namespace

const char* lever_name(Lever lever) noexcept {
    switch (lever) {
        case Lever::TextureMipBias:
            return "texture-mip-bias";
        case Lever::TexturePrefetchRadius:
            return "texture-prefetch-radius";
        case Lever::GeometryErrorThreshold:
            return "geometry-error-threshold";
        case Lever::ShadowResolutionScale:
            return "shadow-resolution-scale";
        case Lever::ShadowRefreshInterval:
            return "shadow-refresh-interval";
        case Lever::IlluminationCacheDensity:
            return "illumination-cache-density";
        case Lever::Count:
            break;
    }
    return "unknown";
}

f32 eviction_score(const ResidentPage& page, f64 now) noexcept {
    // A HOLD OR A GUARANTEE IS NOT A WEIGHT. `residency` lists "whether a page is pinned or
    // guaranteed resident" beside the four weighted terms, but a mip tail that could be outbid by a
    // sufficiently unimportant frame is not a mip tail. These two return the sentinel instead.
    if (page.holds > 0 || page.guaranteed) {
        return kNeverEvict;
    }

    const f64 idle = (now > page.last_used_at) ? (now - page.last_used_at) : 0.0;
    // Recency as a decay rather than a threshold: a page untouched for one half-life is worth half
    // as much as one touched this frame, and there is no cliff for a page to sit just the wrong
    // side of.
    const f32 recency = 1.0F / (1.0F + (static_cast<f32>(idle) / kRecencyHalfLifeSeconds));

    // Cost enters logarithmically. A shadow page that takes ten times as long to render as a
    // geometry page is worth keeping, and is not worth keeping ten times as hard — that would make
    // one expensive page immovable while ten cheap ones that are actually on screen are dropped.
    const f32 cost = effective_cost_ms(page);
    const f32 cost_log = (cost > 0.0F) ? std::log2(1.0F + cost) : 0.0F;
    const f32 cost_term = kCostWeight * ((cost_log > kCostLog2Cap) ? kCostLog2Cap : cost_log);

    const f32 quality = (kImportanceWeight * clamp01(page.importance)) +
                        (kScreenWeight * clamp01(page.screen_contribution));

    // Active is a preference, not a requirement — an inactive page still scores on its own merits,
    // which is what keeps residency and activation independent. See `server.h`.
    const f32 activity = page.active ? kActiveBonus : 0.0F;

    return recency * (1.0F + quality + cost_term + activity);
}

bool past_minimum_age(const ResidentPage& page, const SubsystemPolicy& policy) noexcept {
    return page.resident_frames >= policy.min_residency_frames;
}

Status ChurnTracker::note_eviction(PageKey key, f64 now) noexcept {
    const auto subsystem = static_cast<u32>(key.subsystem);
    if (subsystem < kSubsystemCount) {
        ++stats_[subsystem].evictions;
    }
    if (auto placed = evicted_.insert(key.packed(), now); !placed) {
        return make_unexpected(placed.error());
    }
    return ok();
}

bool ChurnTracker::note_request(PageKey key, f64 now, f64 window_seconds) noexcept {
    const f64* when = evicted_.find(key.packed());
    if (when == nullptr) {
        return false;
    }
    const bool inside = (now - *when) <= window_seconds;
    // Consumed either way: a page evicted long ago and asked for again is an ordinary miss, and
    // leaving the record behind would make the *next* eviction of the same page look like churn
    // that had already been counted.
    evicted_.remove(key.packed());
    if (!inside) {
        return false;
    }
    const auto subsystem = static_cast<u32>(key.subsystem);
    if (subsystem < kSubsystemCount) {
        ++stats_[subsystem].refetches;
    }
    return true;
}

void ChurnTracker::prune(f64 now, f64 window_seconds) noexcept {
    // The map has no erase-while-iterating, so the expired keys are gathered first. Bounded by the
    // number of evictions inside a window, which is exactly the number the budget allows to leave.
    Array<u64> expired;
    for (const auto& entry : evicted_) {
        if ((now - entry.value) > window_seconds) {
            if (Status pushed = expired.push_back(entry.key); !pushed) {
                break;  // out of memory while pruning is not a reason to fail a frame
            }
        }
    }
    for (const u64 key : expired) {
        evicted_.remove(key);
    }
}

ChurnStats ChurnTracker::stats(Subsystem subsystem) const noexcept {
    const auto index = static_cast<u32>(subsystem);
    return (index < kSubsystemCount) ? stats_[index] : ChurnStats{};
}

void ChurnTracker::clear() noexcept {
    evicted_.clear();
    for (ChurnStats& entry : stats_) {
        entry = ChurnStats{};
    }
}

namespace {

/// The registered subsystems, in declared reduction order, into `order`. Returns how many.
///
/// An insertion sort over at most six entries, rather than `std::sort`. Two reasons: six items is
/// where insertion sort is simply faster, and introsort's generic body over a fixed-size stack
/// array is what makes GCC's -Warray-bounds report a subscript it cannot prove is in range.
[[nodiscard]] u32 reduction_order(const SubsystemPolicy* policies, const bool* registered,
                                  u32* order) noexcept {
    u32 count = 0;
    for (u32 index = 0; index < kSubsystemCount; ++index) {
        if (registered[index]) {
            order[count] = index;
            ++count;
        }
    }
    for (u32 slot = 1; slot < count; ++slot) {
        const u32 candidate = order[slot];
        u32 place = slot;
        while (place > 0 && !precedes(policies, order[place - 1], candidate)) {
            order[place] = order[place - 1];
            --place;
        }
        order[place] = candidate;
    }
    return count;
}

/// Append the steps one subsystem's declared levers take to reach `level`.
[[nodiscard]] Status plan_subsystem(const SubsystemPolicy& policy, u32 index,
                                    const f32* current_levers, PressureLevel level,
                                    Array<ReductionStep>& out) noexcept {
    for (u32 lever = 0; lever < kLeverCount; ++lever) {
        const LeverSchedule& schedule = policy.levers[lever];
        if (!schedule.declared) {
            continue;  // a lever nobody declared is not adjusted
        }
        const f32 target = schedule.at(level);
        const f32 current = current_levers[(index * kLeverCount) + lever];
        if (target == current) {
            continue;
        }
        ReductionStep step;
        step.subsystem = static_cast<Subsystem>(index);
        step.lever = static_cast<Lever>(lever);
        step.from = current;
        step.to = target;
        step.order = policy.reduction_order;
        if (Status pushed = out.push_back(step); !pushed) {
            return pushed;
        }
    }
    return ok();
}

}  // namespace

Status plan_reduction(const SubsystemPolicy* policies, const bool* registered,
                      const f32* current_levers, PressureLevel level,
                      Array<ReductionStep>& out) noexcept {
    if (policies == nullptr || registered == nullptr || current_levers == nullptr) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "residency: plan_reduction needs all three tables"});
    }
    out.clear();

    // Visit in declared order. Sorting indices rather than the policies keeps the caller's table
    // stable — the server addresses its subsystems by enumerator, and a plan that reordered them
    // would invalidate every index it holds.
    u32 order[kSubsystemCount];
    const u32 count = reduction_order(policies, registered, order);
    for (u32 slot = 0; slot < count; ++slot) {
        const u32 index = order[slot];
        if (Status planned = plan_subsystem(policies[index], index, current_levers, level, out);
            !planned) {
            return planned;
        }
    }
    return ok();
}

}  // namespace cy::residency
