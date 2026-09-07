#include <cy/servers/residency/server.h>

#include <algorithm>

namespace cy::residency {

namespace {

[[nodiscard]] bool valid(Subsystem subsystem) noexcept {
    return static_cast<u32>(subsystem) < kSubsystemCount;
}

[[nodiscard]] u32 index_of(Subsystem subsystem) noexcept {
    return static_cast<u32>(subsystem);
}

/// One eviction candidate, so that candidates can be ordered before any of them is removed.
/// Gathering keys rather than slots is not a style choice: `remove_page_locked` swaps the last
/// record into the freed slot, so a list of indices would be wrong after the first eviction.
struct Candidate {
    PageKey key;
    f32 score = 0.0F;
};

constexpr u8 kCoarseDependency = 1;
constexpr u8 kHardDependency = 2;

/// Depth-first search over the HARD edges only, marking 0 unvisited, 1 on the stack, 2 done.
///
/// A coarse dependency cannot deadlock by construction — it is satisfied by a guaranteed
/// representation that is already resident — so a cycle of coarse edges is a description of the
/// engine rather than a fault, and only the hard ones are followed. Recursion is bounded by
/// `kSubsystemCount`, which is six.
[[nodiscard]] bool has_hard_cycle(const u8 (&edges)[kSubsystemCount][kSubsystemCount], u32 node,
                                  u8* state) noexcept {
    state[node] = 1;
    for (u32 target = 0; target < kSubsystemCount; ++target) {
        if (edges[node][target] != kHardDependency) {
            continue;
        }
        if (state[target] == 1) {
            return true;
        }
        if (state[target] == 0 && has_hard_cycle(edges, target, state)) {
            return true;
        }
    }
    state[node] = 2;
    return false;
}

}  // namespace

const char* hold_reason_name(HoldReason reason) noexcept {
    switch (reason) {
        case HoldReason::Gameplay:
            return "gameplay";
        case HoldReason::Editor:
            return "editor";
        case HoldReason::Streaming:
            return "streaming";
        case HoldReason::MipTail:
            return "mip-tail";
        case HoldReason::CoarseFallback:
            return "coarse-fallback";
        case HoldReason::Save:
            return "save";
        case HoldReason::Count:
            break;
    }
    return "unknown";
}

ResidencyServer::ResidencyServer(Allocator& allocator) noexcept
    : pages_(allocator),
      page_slots_(allocator),
      hold_records_(allocator),
      hold_counts_(allocator),
      traces_(allocator),
      evicted_this_frame_(allocator),
      queue_(allocator),
      importance_(allocator),
      deadlines_(allocator),
      churn_(allocator),
      last_plan_(allocator) {}

ResidencyServer::~ResidencyServer() {
    // TAKING THE LOCK IN THE DESTRUCTOR IS THE QUIESCE POINT, and it is deliberate. M6 destroys
    // worlds continuously and a completion arriving from an IO worker is inside a public member
    // when the owner decides the world is gone; the lock makes that call finish before any table
    // it touched is destroyed. It does not make it safe for a thread to *enter* afterwards — no
    // lock can — which is why `tests/test_teardown.cpp` joins its workers through a harness
    // declared after the server, so the harness unwinds first.
    const std::lock_guard<std::mutex> guard(mutex_);
}

// --- Configuration -------------------------------------------------------------------------------

Status ResidencyServer::register_subsystem(Subsystem subsystem,
                                           const SubsystemPolicy& policy) noexcept {
    if (!valid(subsystem)) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "residency: registering an unknown subsystem"});
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    const u32 index = index_of(subsystem);
    policies_[index] = policy;
    registered_[index] = true;
    for (u32 lever = 0; lever < kLeverCount; ++lever) {
        levers_[(index * kLeverCount) + lever] = policy.levers[lever].at(level_);
    }
    stats_[index] = ResidencyStats{};
    return ok();
}

bool ResidencyServer::unregister_subsystem(Subsystem subsystem) noexcept {
    if (!valid(subsystem)) {
        return false;
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    const u32 index = index_of(subsystem);
    if (!registered_[index]) {
        return false;
    }

    // Walk backwards: `remove_page_locked` swaps the last record into the removed slot, so a
    // backward walk never revisits a record it has already moved past.
    for (usize slot = pages_.size(); slot > 0; --slot) {
        if (pages_[slot - 1].key.subsystem == subsystem) {
            remove_page_locked(slot - 1);
        }
    }

    // Every hold on this subsystem's pages is invalidated rather than left dangling. A hold that
    // outlived its subsystem would be released later against a page nobody is counting, and the
    // count would go negative in a build without assertions.
    Array<u64> orphaned;
    for (const auto& entry : hold_records_) {
        if (entry.value.key.subsystem == subsystem) {
            if (Status pushed = orphaned.push_back(entry.key); !pushed) {
                break;
            }
        }
    }
    for (const u64 id : orphaned) {
        if (const HoldRecord* record = hold_records_.find(id); record != nullptr) {
            hold_counts_.remove(record->key.packed());
        }
        hold_records_.remove(id);
    }

    resident_bytes_[index] = 0;
    stats_[index] = ResidencyStats{};
    registered_[index] = false;
    policies_[index] = SubsystemPolicy{};
    for (u32 lever = 0; lever < kLeverCount; ++lever) {
        levers_[(index * kLeverCount) + lever] = 0.0F;
    }
    return true;
}

bool ResidencyServer::registered(Subsystem subsystem) const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return registered_locked(subsystem);
}

bool ResidencyServer::registered_locked(Subsystem subsystem) const noexcept {
    return valid(subsystem) && registered_[index_of(subsystem)];
}

SubsystemPolicy ResidencyServer::policy(Subsystem subsystem) const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return valid(subsystem) ? policies_[index_of(subsystem)] : SubsystemPolicy{};
}

Status ResidencyServer::declare_dependency(Subsystem from, Subsystem to,
                                           DependencyKind kind) noexcept {
    if (!valid(from) || !valid(to)) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "residency: dependency names an unknown subsystem"});
    }
    if (from == to) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "residency: a subsystem cannot depend on itself"});
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    dependencies_[index_of(from)][index_of(to)] =
        (kind == DependencyKind::Hard) ? kHardDependency : kCoarseDependency;
    return ok();
}

Status ResidencyServer::validate_dependencies() const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    u8 state[kSubsystemCount] = {};
    for (u32 root = 0; root < kSubsystemCount; ++root) {
        if (state[root] != 0 || !has_hard_cycle(dependencies_, root, state)) {
            continue;
        }
        return make_unexpected(
            Error{ErrorCode::InvalidArgument,
                  "residency: a cycle of hard residency dependencies. `residency` requires the "
                  "dependency to be satisfied by a guaranteed coarse representation, so that no "
                  "residency system blocks another within a frame"});
    }
    return ok();
}

void ResidencyServer::set_pinned(bool pinned) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    pinned_ = pinned;
}

bool ResidencyServer::pinned() const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return pinned_;
}

void ResidencyServer::set_relax_dwell(f64 seconds) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    relax_dwell_ = (seconds > 0.0) ? seconds : 0.0;
}

// --- Importance and deadlines
// ---------------------------------------------------------------------

Status ResidencyServer::publish_importance(u64 instance, const ImportanceInputs& inputs) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return importance_.publish(instance, inputs);
}

Status ResidencyServer::declare_importance_transform(
    Subsystem subsystem, const ImportanceTransform& transform) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return importance_.declare_transform(subsystem, transform);
}

f32 ResidencyServer::importance(u64 instance) const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return importance_.shared(instance);
}

f32 ResidencyServer::importance_for(u64 instance, Subsystem subsystem) const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return importance_.for_subsystem(instance, subsystem);
}

Status ResidencyServer::announce(const Prediction& prediction, f64 now) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return deadlines_.announce(prediction, now);
}

bool ResidencyServer::satisfy_deadline(Subsystem subsystem, u64 region, f64 now) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return deadlines_.satisfy(subsystem, region, now);
}

f64 ResidencyServer::seconds_until(Subsystem subsystem, u64 region, f64 now) const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return deadlines_.seconds_until(subsystem, region, now);
}

PredictionAccuracy ResidencyServer::prediction_accuracy() const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return deadlines_.accuracy();
}

void ResidencyServer::record_prefetched(PredictionSource source, u64 pages) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    deadlines_.record_prefetched(source, pages);
}

void ResidencyServer::record_sampled(PredictionSource source, u64 pages) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    deadlines_.record_sampled(source, pages);
}

// --- The frame
// ------------------------------------------------------------------------------------

Status ResidencyServer::request(const Request& request) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (!registered_locked(request.key.subsystem)) {
        // Refused rather than queued. A request against a subsystem that has been unregistered
        // mid-flight has nowhere to be scheduled, and queueing it would make the next registration
        // inherit work nobody asked for.
        return make_unexpected(
            Error{ErrorCode::Unavailable, "residency: a request for an unregistered subsystem"});
    }
    return queue_.submit(request);
}

u64 ResidencyServer::request_submissions() const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return queue_.submissions();
}

usize ResidencyServer::pending_requests() const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return queue_.size();
}

Status ResidencyServer::schedule(const ScheduleOptions& options, Schedule& out) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    out.clear();
    queue_.compact();

    const f64 now = options.now;
    u32 admitted = 0;
    for (usize entry_index = 0; entry_index < queue_.size(); ++entry_index) {
        const ScoredRequest& entry = queue_.entries()[entry_index];
        if (!registered_locked(entry.key.subsystem)) {
            continue;
        }
        const u32 index = index_of(entry.key.subsystem);
        ResidencyStats& stats = stats_[index];
        ++stats.requests;

        if (ResidentPage* page = find_page_locked(entry.key); page != nullptr) {
            page->last_used_at = now;
            page->importance = std::max(page->importance, entry.inputs.importance);
            page->screen_contribution =
                std::max(page->screen_contribution, entry.inputs.screen_coverage);
            if (page->pending) {
                // Admitted on an earlier frame and still on its way. Not a hit — nothing can
                // sample it yet — and not a second admission either.
                note_trace_locked(entry.key, QualityReason::AwaitingProduction,
                                  entry.inputs.detail_deficit, entry.score);
                continue;
            }
            ++stats.hits;
            note_trace_locked(entry.key, QualityReason::Resident, entry.inputs.detail_deficit,
                              entry.score);
            continue;
        }

        // Churn is measured at the request, not at the eviction: an eviction only becomes churn
        // when somebody asks for the page again inside the window.
        churn_.note_request(entry.key, now, policies_[index].churn_window_seconds);

        if (evicted_this_frame_.contains(entry.key.packed())) {
            // HYSTERESIS, THE OTHER HALF. `residency`: "a page evicted this frame is not requested
            // again the next". The minimum residency age stops a page leaving too soon; this stops
            // it coming straight back, which is the other direction of the same oscillation.
            note_trace_locked(entry.key, QualityReason::Evicted, entry.inputs.detail_deficit,
                              entry.score);
            continue;
        }

        if (options.max_admissions != 0 && admitted >= options.max_admissions) {
            ++stats.outscored;
            note_trace_locked(entry.key, QualityReason::Outscored, entry.inputs.detail_deficit,
                              entry.score);
            continue;
        }

        if (!make_room_locked(entry.key.subsystem, entry.bytes, now, out)) {
            ++stats.budget_blocked;
            note_trace_locked(entry.key, QualityReason::BudgetBlocked, entry.inputs.detail_deficit,
                              entry.score);
            continue;
        }

        if (Status committed = commit_locked(entry, now); !committed) {
            return committed;
        }

        Admission admission;
        admission.key = entry.key;
        admission.bytes = entry.bytes;
        admission.score = entry.score;
        admission.seconds_until_needed = entry.inputs.seconds_until_needed;
        if (Status pushed = out.admissions.push_back(admission); !pushed) {
            return pushed;
        }
        ++stats.admissions;
        ++admitted;
        note_trace_locked(entry.key, QualityReason::AwaitingProduction, entry.inputs.detail_deficit,
                          entry.score);
    }

    queue_.clear();
    return ok();
}

Status ResidencyServer::note_resident(const ResidentReport& report, f64 now) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (!registered_locked(report.key.subsystem)) {
        return make_unexpected(
            Error{ErrorCode::Unavailable,
                  "residency: a page reported resident for an unregistered subsystem"});
    }
    const u32 index = index_of(report.key.subsystem);

    if (ResidentPage* existing = find_page_locked(report.key); existing != nullptr) {
        resident_bytes_[index] -= std::min(existing->bytes, resident_bytes_[index]);
        const bool arriving = existing->pending;
        existing->pending = false;
        existing->bytes = report.bytes;
        existing->level = report.level;
        existing->guaranteed = existing->guaranteed || report.guaranteed;
        existing->last_used_at = now;
        if (arriving) {
            existing->became_resident_at = now;
            existing->resident_frames = 0;
            if (report.production_cost_ms > 0.0F) {
                existing->production_cost_ms = report.production_cost_ms;
                existing->cost = report.cost;
            }
            ++stats_[index].fetches;
            note_trace_locked(report.key, QualityReason::Resident, report.level, 0.0F);
        }
        resident_bytes_[index] += report.bytes;
        return ok();
    }

    ResidentPage page;
    page.key = report.key;
    page.bytes = report.bytes;
    page.level = report.level;
    page.became_resident_at = now;
    page.resident_frames = 0;
    page.last_used_at = now;
    page.production_cost_ms = report.production_cost_ms;
    page.cost = (report.production_cost_ms > 0.0F) ? report.cost : policies_[index].default_cost;
    page.guaranteed = report.guaranteed;
    if (const u32* held = hold_counts_.find(report.key.packed()); held != nullptr) {
        // A hold taken BEFORE the page arrived. Pinning something that is on its way is the normal
        // shape of a streaming pin, and losing the hold at the moment it starts to matter would be
        // the worst possible time to lose it.
        page.holds = *held;
    }

    if (Status pushed = pages_.push_back(page); !pushed) {
        return pushed;
    }
    if (auto placed = page_slots_.insert(report.key.packed(), pages_.size() - 1); !placed) {
        pages_.pop_back();
        return make_unexpected(placed.error());
    }
    resident_bytes_[index] += report.bytes;
    ++stats_[index].fetches;
    note_trace_locked(report.key, QualityReason::Resident, report.level, 0.0F);
    return ok();
}

bool ResidencyServer::cancel_admission(PageKey key) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    const usize* slot = page_slots_.find(key.packed());
    if (slot == nullptr || !pages_[*slot].pending) {
        return false;
    }
    const u32 index = index_of(key.subsystem);
    resident_bytes_[index] -= std::min(pages_[*slot].bytes, resident_bytes_[index]);
    ++stats_[index].abandoned_admissions;
    // Deliberately NOT counted as an eviction and NOT recorded in the churn tracker: nothing was
    // ever resident, so counting it would make a cache too small look like a policy that thrashes.
    note_trace_locked(key, QualityReason::BudgetBlocked, 0, 0.0F);
    remove_page_locked(*slot);
    return true;
}

bool ResidencyServer::note_released(PageKey key, f64 now) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    const usize* slot = page_slots_.find(key.packed());
    if (slot == nullptr) {
        return false;
    }
    const u32 index = index_of(key.subsystem);
    const u64 bytes = pages_[*slot].bytes;
    resident_bytes_[index] -= std::min(bytes, resident_bytes_[index]);
    churn_.note_eviction(key, now);
    remove_page_locked(*slot);
    note_trace_locked(key, QualityReason::Evicted, 0, 0.0F);
    return true;
}

bool ResidencyServer::touch(PageKey key, f64 now, f32 screen_contribution,
                            f32 page_importance) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    ResidentPage* page = find_page_locked(key);
    if (page == nullptr) {
        return false;
    }
    page->last_used_at = now;
    page->screen_contribution = screen_contribution;
    page->importance = page_importance;
    return true;
}

void ResidencyServer::end_frame(f64 now) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    for (ResidentPage& page : pages_) {
        ++page.resident_frames;
    }

    f64 widest_window = 0.0;
    for (u32 index = 0; index < kSubsystemCount; ++index) {
        if (registered_[index]) {
            widest_window = std::max(widest_window, policies_[index].churn_window_seconds);
        }
    }
    churn_.prune(now, widest_window);
    expire_pending_locked();

    deadlines_.advance(now);
    deadlines_.retire_resolved();
    evicted_this_frame_.clear();

    // The deferred half of the asymmetric hysteresis: pressure fell, the levers stayed where they
    // were, and the dwell has now elapsed. Applied here rather than in `on_pressure`, because the
    // monitor only calls that on a transition and a relaxation that waits is not a transition.
    if (pending_level_ != level_) {
        apply_pressure_locked(pending_level_, now);
    }

    last_now_ = now;
    ++frame_;
}

u64 ResidencyServer::frame() const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return frame_;
}

// --- Residency, held independently of activation
// ---------------------------------------------------

Expected<HoldId, Error> ResidencyServer::hold(PageKey key, HoldReason reason) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);

    // NOTHING ABOUT ACTIVATION IS CONSULTED HERE, and nothing about a simulation could be: neither
    // is a parameter, a member, or reachable from one. That is the M6 exit criterion expressed as a
    // signature rather than as a promise.
    const u64 id = next_hold_;
    HoldRecord record;
    record.key = key;
    record.reason = reason;
    if (auto placed = hold_records_.insert(id, record); !placed) {
        return make_unexpected(placed.error());
    }
    if (u32* count = hold_counts_.find(key.packed()); count != nullptr) {
        ++*count;
    } else if (auto placed = hold_counts_.insert(key.packed(), 1U); !placed) {
        hold_records_.remove(id);
        return make_unexpected(placed.error());
    }
    ++next_hold_;
    sync_holds_locked(key);
    return HoldId{id};
}

bool ResidencyServer::release(HoldId id) noexcept {
    if (!id.valid()) {
        return false;
    }
    const std::lock_guard<std::mutex> guard(mutex_);
    const HoldRecord* record = hold_records_.find(id.value);
    if (record == nullptr) {
        return false;  // released twice, or invalidated by a teardown. Refused, not double-counted.
    }
    const PageKey key = record->key;
    hold_records_.remove(id.value);
    if (u32* count = hold_counts_.find(key.packed()); count != nullptr) {
        if (*count <= 1) {
            hold_counts_.remove(key.packed());
        } else {
            --*count;
        }
    }
    sync_holds_locked(key);
    return true;
}

u32 ResidencyServer::holds(PageKey key) const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    const u32* count = hold_counts_.find(key.packed());
    return (count == nullptr) ? 0U : *count;
}

usize ResidencyServer::outstanding_holds() const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return hold_records_.size();
}

Status ResidencyServer::set_active(PageKey key, bool active) noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    ResidentPage* page = find_page_locked(key);
    if (page == nullptr || page->pending) {
        // Activation is reported ABOUT a resident page. It is not an error for a subsystem to
        // activate something whose pages are not resident — that is the whole point of the
        // separation — so this is `NotFound` rather than a failure, and the caller may ignore it.
        return make_unexpected(Error{ErrorCode::NotFound,
                                     "residency: activation reported for a page that is not "
                                     "resident; residency and activation are independent"});
    }
    page->active = active;
    return ok();
}

bool ResidencyServer::is_active(PageKey key) const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    const ResidentPage* page = find_page_locked(key);
    return (page != nullptr) && page->active && !page->pending;
}

bool ResidencyServer::is_resident(PageKey key) const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    const ResidentPage* page = find_page_locked(key);
    return (page != nullptr) && !page->pending;
}

bool ResidencyServer::is_pending(PageKey key) const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    const ResidentPage* page = find_page_locked(key);
    return (page != nullptr) && page->pending;
}

u64 ResidencyServer::resident_bytes(Subsystem subsystem) const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return valid(subsystem) ? resident_bytes_[index_of(subsystem)] : 0;
}

u64 ResidencyServer::active_pages(Subsystem subsystem) const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    u64 count = 0;
    for (const ResidentPage& page : pages_) {
        if (page.key.subsystem == subsystem && page.active) {
            ++count;
        }
    }
    return count;
}

// --- Pressure
// ---------------------------------------------------------------------------------------

void ResidencyServer::on_pressure(PressureLevel level, PressureLevel previous) noexcept {
    (void)previous;
    const std::lock_guard<std::mutex> guard(mutex_);
    pending_level_ = level;
    apply_pressure_locked(level, last_now_);
}

void ResidencyServer::apply_pressure_locked(PressureLevel level, f64 now) noexcept {
    if (pinned_) {
        // `residency`: "pinned mode SHALL disable coordinated adjustment together with the
        // renderer's budget arbiter". The level is still recorded so a report tells the truth about
        // what the engine was under; the levers do not move.
        level_ = level;
        pending_level_ = level;
        level_since_ = now;
        return;
    }

    const bool relaxing = static_cast<u8>(level) < static_cast<u8>(level_);
    if (relaxing && (now - level_since_) < relax_dwell_) {
        return;  // the dwell has not elapsed; `end_frame` will retry
    }

    if (Status planned = plan_reduction(policies_, registered_, levers_, level, last_plan_);
        !planned) {
        return;  // out of memory building a plan is not a reason to leave the levers half applied
    }
    for (const ReductionStep& step : last_plan_) {
        levers_[(index_of(step.subsystem) * kLeverCount) + static_cast<u32>(step.lever)] = step.to;
    }
    level_ = level;
    pending_level_ = level;
    level_since_ = now;
}

PressureLevel ResidencyServer::level() const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    return level_;
}

f32 ResidencyServer::lever(Subsystem subsystem, Lever lever) const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (!valid(subsystem) || static_cast<u32>(lever) >= kLeverCount) {
        return 0.0F;
    }
    return levers_[(index_of(subsystem) * kLeverCount) + static_cast<u32>(lever)];
}

Status ResidencyServer::last_reduction(Array<ReductionStep>& out) const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    out.clear();
    for (const ReductionStep& step : last_plan_) {
        if (Status pushed = out.push_back(step); !pushed) {
            return pushed;
        }
    }
    return ok();
}

// --- Diagnostics
// --------------------------------------------------------------------------------------

ResidencyStats ResidencyServer::stats(Subsystem subsystem) const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (!valid(subsystem)) {
        return ResidencyStats{};
    }
    const u32 index = index_of(subsystem);
    ResidencyStats result = stats_[index];
    result.budget_bytes = policies_[index].budget_bytes;
    result.resident_bytes = resident_bytes_[index];
    result.churn = churn_.stats(subsystem);
    result.deadline_misses = deadlines_.misses(subsystem);
    for (const ResidentPage& page : pages_) {
        if (page.key.subsystem != subsystem) {
            continue;
        }
        if (page.pending) {
            ++result.pending_pages;
        } else {
            ++result.resident_pages;
        }
        result.held_pages += (page.holds > 0) ? 1 : 0;
        result.active_pages += page.active ? 1 : 0;
        result.guaranteed_pages += page.guaranteed ? 1 : 0;
    }
    return result;
}

ResidencyStats ResidencyServer::stats() const noexcept {
    ResidencyStats total;
    for (u32 index = 0; index < kSubsystemCount; ++index) {
        const ResidencyStats one = stats(static_cast<Subsystem>(index));
        total.resident_pages += one.resident_pages;
        total.pending_pages += one.pending_pages;
        total.resident_bytes += one.resident_bytes;
        total.budget_bytes += one.budget_bytes;
        total.held_pages += one.held_pages;
        total.active_pages += one.active_pages;
        total.guaranteed_pages += one.guaranteed_pages;
        total.requests += one.requests;
        total.hits += one.hits;
        total.admissions += one.admissions;
        total.outscored += one.outscored;
        total.budget_blocked += one.budget_blocked;
        total.fetches += one.fetches;
        total.evictions += one.evictions;
        total.evicted_bytes += one.evicted_bytes;
        total.deadline_misses += one.deadline_misses;
        total.abandoned_admissions += one.abandoned_admissions;
        total.churn.evictions += one.churn.evictions;
        total.churn.refetches += one.churn.refetches;
    }
    return total;
}

Explanation ResidencyServer::explain(PageKey key) const noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    Explanation answer;
    answer.key = key;
    if (const RequestTrace* trace = traces_.find(key.packed()); trace != nullptr) {
        answer.reason = trace->reason;
        answer.desired_level = trace->desired_level;
        answer.last_score = trace->score;
    }
    if (const u32* held = hold_counts_.find(key.packed()); held != nullptr) {
        answer.holds = *held;
    }
    if (const ResidentPage* page = find_page_locked(key); page != nullptr) {
        answer.reason = page->pending ? QualityReason::AwaitingProduction : QualityReason::Resident;
        answer.resident_level = page->pending ? 0U : page->level;
        answer.active = page->active && !page->pending;
    }
    return answer;
}

void ResidencyServer::reset() noexcept {
    const std::lock_guard<std::mutex> guard(mutex_);
    pages_.clear();
    page_slots_.clear();
    hold_records_.clear();
    hold_counts_.clear();
    traces_.clear();
    evicted_this_frame_.clear();
    queue_.clear();
    deadlines_.clear();
    churn_.clear();
    importance_.clear();
    for (u32 index = 0; index < kSubsystemCount; ++index) {
        resident_bytes_[index] = 0;
        stats_[index] = ResidencyStats{};
    }
    frame_ = 0;
}

// --- Internals
// ------------------------------------------------------------------------------------------

ResidentPage* ResidencyServer::find_page_locked(PageKey key) noexcept {
    const usize* slot = page_slots_.find(key.packed());
    return (slot == nullptr) ? nullptr : &pages_[*slot];
}

const ResidentPage* ResidencyServer::find_page_locked(PageKey key) const noexcept {
    const usize* slot = page_slots_.find(key.packed());
    return (slot == nullptr) ? nullptr : &pages_[*slot];
}

void ResidencyServer::remove_page_locked(usize slot) noexcept {
    const u64 packed = pages_[slot].key.packed();
    page_slots_.remove(packed);
    const usize last = pages_.size() - 1;
    if (slot != last) {
        pages_[slot] = pages_[last];
        // The moved record's key is already in the table, so this is an assignment rather than an
        // insertion and cannot fail or allocate.
        if (usize* moved = page_slots_.find(pages_[slot].key.packed()); moved != nullptr) {
            *moved = slot;
        }
    }
    pages_.pop_back();
}

void ResidencyServer::evict_locked(usize slot, QualityReason cause, f64 now,
                                   Schedule& out) noexcept {
    const ResidentPage& page = pages_[slot];
    const u32 index = index_of(page.key.subsystem);
    const PageKey key = page.key;
    const u64 bytes = page.bytes;

    EvictionOrder order;
    order.key = key;
    order.bytes = bytes;
    order.score = eviction_score(page, now);
    order.cause = cause;
    if (Status pushed = out.evictions.push_back(order); !pushed) {
        return;  // no room to tell the subsystem is no reason to stop counting the page
    }

    resident_bytes_[index] -= std::min(bytes, resident_bytes_[index]);
    ++stats_[index].evictions;
    stats_[index].evicted_bytes += bytes;
    churn_.note_eviction(key, now);
    if (auto placed = evicted_this_frame_.insert(key.packed(), frame_); !placed) {
        // Losing the hysteresis record for one page is a missed anti-oscillation guard, not a
        // correctness failure; it is not worth failing the frame over and it is worth not hiding.
    }
    note_trace_locked(key, cause, 0, order.score);
    remove_page_locked(slot);
}

bool ResidencyServer::has_room_locked(Subsystem subsystem, u64 bytes) const noexcept {
    const SubsystemPolicy& policy = policies_[index_of(subsystem)];
    if (policy.budget_bytes == 0) {
        return true;  // unbudgeted: `BudgetTree`'s meaning of zero
    }
    if (policy.budget_kind == BudgetKind::Soft) {
        // A soft budget always admits — crossing it is what raises pressure, not what stops the
        // growth. `cy/core/memory/budget.h` says this in as many words, and disagreeing with it
        // here would give the engine two meanings of "soft".
        return true;
    }
    return (resident_bytes_[index_of(subsystem)] + bytes) <= policy.budget_bytes;
}

bool ResidencyServer::make_room_locked(Subsystem subsystem, u64 bytes, f64 now,
                                       Schedule& out) noexcept {
    if (has_room_locked(subsystem, bytes)) {
        return true;
    }
    const SubsystemPolicy& policy = policies_[index_of(subsystem)];

    Array<Candidate> candidates;
    for (const ResidentPage& page : pages_) {
        if (page.key.subsystem != subsystem || page.holds > 0 || page.guaranteed || page.pending) {
            continue;  // a page that has not arrived cannot be given back
        }
        if (!past_minimum_age(page, policy)) {
            continue;  // arrived too recently to leave: the anti-oscillation guard
        }
        Candidate candidate;
        candidate.key = page.key;
        candidate.score = eviction_score(page, now);
        if (Status pushed = candidates.push_back(candidate); !pushed) {
            break;
        }
    }

    std::ranges::sort(candidates, [](const Candidate& lhs, const Candidate& rhs) noexcept {
        if (lhs.score != rhs.score) {
            return lhs.score < rhs.score;  // cheapest to lose, first
        }
        return lhs.key.packed() < rhs.key.packed();
    });

    for (const Candidate& candidate : candidates) {
        if (has_room_locked(subsystem, bytes)) {
            break;
        }
        if (const usize* slot = page_slots_.find(candidate.key.packed()); slot != nullptr) {
            evict_locked(*slot, QualityReason::Evicted, now, out);
        }
    }
    return has_room_locked(subsystem, bytes);
}

Status ResidencyServer::commit_locked(const ScoredRequest& entry, f64 now) noexcept {
    // THE BUDGET IS SPENT WHEN IT IS COMMITTED, NOT WHEN IT IS OCCUPIED. Without this record the
    // next request in the same frame would look at an empty cache and be admitted too, and a
    // three-page budget would admit five pages every frame — which is exactly what
    // `test_server.cpp`'s "the frame admits what fits and blocks what does not" found.
    ResidentPage page;
    page.key = entry.key;
    page.bytes = entry.bytes;
    page.pending = true;
    page.became_resident_at = now;
    page.last_used_at = now;
    page.importance = entry.inputs.importance;
    page.screen_contribution = entry.inputs.screen_coverage;
    page.guaranteed = entry.guaranteed;
    page.cost = policies_[index_of(entry.key.subsystem)].default_cost;
    page.production_cost_ms = entry.inputs.production_cost_ms;
    if (const u32* held = hold_counts_.find(entry.key.packed()); held != nullptr) {
        page.holds = *held;
    }

    if (Status pushed = pages_.push_back(page); !pushed) {
        return pushed;
    }
    if (auto placed = page_slots_.insert(entry.key.packed(), pages_.size() - 1); !placed) {
        pages_.pop_back();
        return make_unexpected(placed.error());
    }
    resident_bytes_[index_of(entry.key.subsystem)] += entry.bytes;
    return ok();
}

void ResidencyServer::expire_pending_locked() noexcept {
    // An admission the subsystem never reported. Its bytes are taken back rather than held for the
    // rest of the session, and the reclamation is counted so that a subsystem quietly dropping work
    // shows up as a number instead of as a budget that mysteriously shrinks.
    for (usize slot = pages_.size(); slot > 0; --slot) {
        ResidentPage& page = pages_[slot - 1];
        if (!page.pending || page.resident_frames < kPendingExpiryFrames) {
            continue;
        }
        const u32 index = index_of(page.key.subsystem);
        resident_bytes_[index] -= std::min(page.bytes, resident_bytes_[index]);
        ++stats_[index].abandoned_admissions;
        note_trace_locked(page.key, QualityReason::BudgetBlocked, 0, 0.0F);
        remove_page_locked(slot - 1);
    }
}

void ResidencyServer::note_trace_locked(PageKey key, QualityReason reason, u32 desired_level,
                                        f32 score) noexcept {
    RequestTrace trace;
    trace.reason = reason;
    trace.desired_level = desired_level;
    trace.score = score;
    if (auto placed = traces_.insert(key.packed(), trace); !placed) {
        // A diagnostic that cannot record is a diagnostic that answers "never requested" later.
        // That is a worse answer than the truth and a better one than a failed frame.
    }
}

void ResidencyServer::sync_holds_locked(PageKey key) noexcept {
    ResidentPage* page = find_page_locked(key);
    if (page == nullptr) {
        return;
    }
    const u32* count = hold_counts_.find(key.packed());
    page->holds = (count == nullptr) ? 0U : *count;
}

}  // namespace cy::residency
