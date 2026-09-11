// The frame's inference queue: the budget, the deferral, and the staleness. M8.c task 4.3.

#include <cy/ml/schedule.h>

#include <cy/core/jobs/types.h>

#include <algorithm>

namespace cy::ml {
namespace {

/// ONE CLOCK. `cy::jobs::monotonic_now_ns` is the reading the scheduler, the watchdog and the
/// critical-path report already share, and its own header says why: three subsystems that each took
/// their own reading cannot agree about what "now" was.
[[nodiscard]] u64 monotonic_nanoseconds() noexcept {
    return static_cast<u64>(jobs::monotonic_now_ns());
}

constexpr const char* kPriorityNames[static_cast<usize>(InferencePriority::Count)] = {
    "background", "low", "normal", "high", "critical",
};

}  // namespace

const char* inference_priority_name(InferencePriority priority) noexcept {
    const auto index = static_cast<usize>(priority);
    return (index < static_cast<usize>(InferencePriority::Count)) ? kPriorityNames[index]
                                                                  : "unknown";
}

InferenceScheduler::~InferenceScheduler() {
    // TEARDOWN UNDER LOAD. A scheduler destroyed with work in flight must not leave a job holding a
    // pointer into an array that is about to be freed, so every dispatched job is waited for. This
    // is the one place the scheduler blocks, and it blocks at shutdown rather than in a frame.
    if (jobs_ != nullptr && jobs_->is_running()) {
        for (Request& request : requests_.span()) {
            if (request.dispatched && !request.completed) {
                jobs_->wait(request.job);
                request.completed = true;
            }
        }
    }
}

void InferenceScheduler::begin_frame() noexcept {
    // Age every answer a caller is holding. A request that completed last frame is one frame stale
    // this frame, which is the number `AsyncResult::frames_stale` reports.
    for (Held& held : held_.span()) {
        if (held.completed_once && held.frames_stale < 0xFFFF'FFFFu) {
            ++held.frames_stale;
        }
    }
    requests_.clear();
    report_ = FrameReport{};
    next_order_ = 0;
}

Expected<RequestId, Error> InferenceScheduler::submit(InferenceSession& session,
                                                      InferencePriority priority) noexcept {
    if (requests_.size() >= kMaxRequests) {
        return fail(ErrorCode::OutOfRange,
                    "more inference requests in one frame than the scheduler holds",
                    static_cast<i64>(kMaxRequests));
    }
    // CONSTRUCTED IN PLACE, NEVER COPIED IN, and that is a build finding rather than a style
    // preference. `Request::outcome` is a `Status`, whose error half lives in a union that a value
    // state never constructs; copying a default-constructed `Request` into the array instantiates
    // `Expected::construct_from`, and GCC 13 at -O2 with AddressSanitizer's instrumentation cannot
    // see through the `!has_value_` guard around it:
    //
    //   expected.h:206: error: 'request...error_' may be used uninitialized
    //     [-Werror=maybe-uninitialized]
    //
    // The sanitizer build is a permanent gate (`m4:sanitizers`) and it does not run in the ordinary
    // profiles, so M8.c's closing gate is where this surfaced. `emplace_back()` default-constructs
    // the element in the array and the fields are written into it, so no `Request` is ever copied
    // and the union is never read.
    Expected<Request*, Error> slot = requests_.emplace_back();
    if (!slot) {
        return make_unexpected(slot.error());
    }
    Request& request = **slot;
    request.session = &session;
    request.id = RequestId{next_id_++};
    request.priority = priority;
    request.order = next_order_++;
    ++report_.submitted;
    if (held_for(&session) == nullptr) {
        Expected<Held*, Error> entry = held_.emplace_back();
        if (!entry) {
            return make_unexpected(entry.error());
        }
        (*entry)->session = &session;
    }
    return request.id;
}

void InferenceScheduler::run_request(const jobs::TaskContext& context, void* user) noexcept {
    (void)context;
    auto* request = static_cast<Request*>(user);
    request->outcome = request->session->run();
}

Status InferenceScheduler::dispatch() noexcept {
    // HIGHEST PRIORITY FIRST, ties by submission order. Selection sort over at most kMaxRequests
    // entries rather than a heap: sixty-four is small, and the order has to be exactly reproducible
    // — "deferred by priority" is a rule about which request runs, and a sort whose tie-break is
    // unspecified makes that rule depend on the standard library's implementation.
    const usize count = requests_.size();
    for (usize i = 0; i + 1 < count; ++i) {
        usize best = i;
        for (usize j = i + 1; j < count; ++j) {
            const bool higher = requests_[j].priority > requests_[best].priority;
            const bool same_earlier = requests_[j].priority == requests_[best].priority &&
                                      requests_[j].order < requests_[best].order;
            if (higher || same_earlier) {
                best = j;
            }
        }
        if (best != i) {
            Request temporary = requests_[i];
            requests_[i] = requests_[best];
            requests_[best] = temporary;
        }
    }

    // THE BUDGET IS CHECKED BEFORE A REQUEST IS DISPATCHED, not after it has run. The time half is
    // predicted from what the session has cost so far — a model that has never run is charged
    // nothing and is therefore always given its first frame, which is the only honest estimate for
    // a cost nobody has measured yet.
    u64 predicted = 0;
    u32 dispatched = 0;
    for (Request& request : requests_.span()) {
        const u64 estimate = request.session->stats().mean_nanoseconds();
        const bool count_exhausted =
            budget_.max_invocations != 0 && dispatched >= budget_.max_invocations;
        const bool time_exhausted = budget_.max_nanoseconds != 0 && dispatched > 0 &&
                                    predicted + estimate > budget_.max_nanoseconds;
        if (count_exhausted || time_exhausted) {
            ++report_.deferred;
            report_.highest_deferred = std::max(request.priority, report_.highest_deferred);
            continue;
        }
        request.dispatched = true;
        predicted += estimate;
        ++dispatched;
        if (jobs_ != nullptr && jobs_->is_running()) {
            Expected<jobs::JobHandle, Error> handle =
                jobs_->submit(&InferenceScheduler::run_request, &request, "cy.ml.inference");
            if (!handle) {
                // The job system refused the submission. The request stays dispatched and `pump`
                // runs it inline: a frame that cannot enqueue must still answer, and reporting a
                // failure the caller cannot act on would only lose the result.
                request.job = jobs::JobHandle();
            } else {
                request.job = handle.value();
            }
        }
    }
    report_.dispatched = dispatched;
    return ok();
}

u32 InferenceScheduler::pump() noexcept {
    const u64 started = monotonic_nanoseconds();
    u32 completed = 0;
    for (Request& request : requests_.span()) {
        if (!request.dispatched || request.completed) {
            continue;
        }
        if (jobs_ != nullptr && jobs_->is_running() && !request.job.is_null()) {
            jobs_->wait(request.job);
        } else {
            request.outcome = request.session->run();
        }
        request.completed = true;
        ++completed;
        if (Held* held = held_for(request.session); held != nullptr) {
            held->frames_stale = 0;
            held->completed_once = true;
        }
    }
    report_.completed += completed;
    report_.nanoseconds_spent += monotonic_nanoseconds() - started;

    // UTILISATION over whichever limit bound first, and it is allowed to exceed one: a single
    // dispatched request may overrun its whole frame's time, and a report that clamped would be
    // hiding the number a reader came for.
    f32 utilisation = 0.0F;
    if (budget_.max_nanoseconds != 0) {
        utilisation =
            static_cast<f32>(report_.nanoseconds_spent) / static_cast<f32>(budget_.max_nanoseconds);
    }
    if (budget_.max_invocations != 0) {
        const f32 by_count =
            static_cast<f32>(report_.dispatched) / static_cast<f32>(budget_.max_invocations);
        utilisation = (by_count > utilisation) ? by_count : utilisation;
    }
    report_.utilisation = utilisation;
    return completed;
}

AsyncResult InferenceScheduler::result(const InferenceSession& session) const noexcept {
    AsyncResult result;
    for (const Held& held : held_.span()) {
        if (held.session != &session) {
            continue;
        }
        result.never_completed = !held.completed_once;
        result.frames_stale = held.frames_stale;
        result.fresh = held.completed_once && held.frames_stale == 0;
        result.session = held.completed_once ? const_cast<InferenceSession*>(&session) : nullptr;
        return result;
    }
    return result;
}

u32 InferenceScheduler::deferred_count() const noexcept {
    u32 count = 0;
    for (const Request& request : requests_.span()) {
        if (!request.dispatched) {
            ++count;
        }
    }
    return count;
}

InferenceScheduler::Held* InferenceScheduler::held_for(const InferenceSession* session) noexcept {
    for (Held& held : held_.span()) {
        if (held.session == session) {
            return &held;
        }
    }
    return nullptr;
}

}  // namespace cy::ml
