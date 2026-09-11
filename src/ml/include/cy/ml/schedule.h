#pragma once
// Scheduling and the per-frame budget. M8.c task 4.3.
//
// `ml-inference`: "Inference SHALL be schedulable on the job system or on a device queue, with a
// declared per-frame budget covering invocation count and time. Asynchronous inference SHALL never
// stall the frame; a result not yet available SHALL yield the previous result or a declared
// default, with the staleness visible to the caller. Inference exceeding its budget SHALL be
// deferred by priority and reported."
//
// Three separate obligations, and the type below keeps them separate:
//
//   THE BUDGET      is a declaration — a count and a time — checked BEFORE a request is dispatched,
//                   not after it has run. A budget enforced afterwards is a report.
//   THE DEFERRAL    is by priority and is REPORTED. `InferenceScheduler::report()` says how many
//                   were dispatched, how many were deferred, and what the frame actually spent.
//   THE STALENESS   is visible on the result. `AsyncResult::frames_stale` is the number the caller
//                   reads to decide whether the previous answer is still worth using; a caller that
//                   never looks still gets an answer, which is the "SHALL never stall" half.
//
// WHERE THE WORK RUNS. On the engine's `JobSystem` when the scheduler is given one, and inline on
// the caller's thread at `pump()` when it is not. The second is not a fallback that pretends: a
// scheduler with no job system runs the request during `pump`, and `pump` is called at the point in
// the frame the caller chose. What never happens either way is a `run()` blocking inside
// `dispatch`.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/memory/array.h>
#include <cy/ml/session.h>

namespace cy::ml {

/// The declared per-frame budget: "invocation count and time".
struct InferenceBudget {
    u32 max_invocations = 8;
    u64 max_nanoseconds = 2'000'000;  // 2 ms of a 16.6 ms frame

    [[nodiscard]] bool unlimited() const noexcept {
        return max_invocations == 0 && max_nanoseconds == 0;
    }
};

/// Priority decides what is dispatched when the budget cannot take everything. Highest first, and
/// ties are broken by submission order so that a frame's dispatch is a function of what was asked
/// rather than of which worker got there first.
enum class InferencePriority : u8 {
    Background = 0,
    Low = 1,
    Normal = 2,
    High = 3,
    Critical = 4,
    Count = 5,
};

[[nodiscard]] const char* inference_priority_name(InferencePriority priority) noexcept;

/// A handle to a submitted request.
struct RequestId {
    u32 value = 0;
    [[nodiscard]] bool is_valid() const noexcept { return value != 0; }
    friend bool operator==(RequestId a, RequestId b) noexcept { return a.value == b.value; }
};

/// What a caller reads back. `ml-inference`: "WHEN an asynchronous inference has not completed
/// THEN the caller SHALL receive the previous result or its declared default, flagged as stale,
/// rather than the frame blocking."
struct AsyncResult {
    /// True when this is the answer to the request the caller submitted most recently.
    bool fresh = false;
    /// Frames since the answer the caller is holding was produced. Zero when `fresh`. A caller that
    /// has never had one reads `never_completed`.
    u32 frames_stale = 0;
    bool never_completed = true;
    /// The session whose outputs hold the answer, or null when there has never been one.
    InferenceSession* session = nullptr;
};

/// What one frame did. Reported whether or not anything was deferred, because a scheduler that
/// dispatched nothing and one that had nothing to dispatch read identically otherwise.
struct FrameReport {
    u32 submitted = 0;
    u32 dispatched = 0;
    u32 deferred = 0;
    u32 completed = 0;
    u64 nanoseconds_spent = 0;
    /// Budget utilisation as a fraction, over whichever of the two limits bound first. Above one is
    /// possible: a single dispatched request may overrun, and hiding that would be the report
    /// lying about the frame it is describing.
    f32 utilisation = 0.0F;
    /// The highest priority that was deferred this frame, for a diagnostic that has to say what was
    /// starved rather than how much.
    InferencePriority highest_deferred = InferencePriority::Background;
};

/// The frame's inference queue.
///
/// Not an owner of sessions: a caller registers the sessions it has and submits requests against
/// them. That keeps the model's lifetime where the asset is and lets one session be submitted from
/// several call sites in a frame, which is what batching across agents needs.
class InferenceScheduler {
public:
    /// How many requests may be in flight in one frame. Fixed: a frame that wants more than this
    /// has a budget problem the scheduler cannot fix by allocating.
    static constexpr u32 kMaxRequests = 64;

    /// No allocator, and that is deliberate: a per-frame queue with a fixed ceiling allocates
    /// nothing, so a frame under budget pressure cannot also be a frame that fails to allocate.
    InferenceScheduler() noexcept = default;
    ~InferenceScheduler();

    InferenceScheduler(const InferenceScheduler&) = delete;
    InferenceScheduler& operator=(const InferenceScheduler&) = delete;

    void set_budget(const InferenceBudget& budget) noexcept { budget_ = budget; }
    [[nodiscard]] const InferenceBudget& budget() const noexcept { return budget_; }

    /// Run dispatched work on `jobs`. Null — the default — runs it inline during `pump()`.
    void set_job_system(jobs::JobSystem* jobs) noexcept { jobs_ = jobs; }

    /// Begin a frame: clears the previous frame's report and ages every held result by one.
    void begin_frame() noexcept;

    /// Ask for `session` to be run this frame. Returns the request's id; the caller reads the
    /// answer with `result()`.
    [[nodiscard]] Expected<RequestId, Error> submit(InferenceSession& session,
                                                    InferencePriority priority) noexcept;

    /// Dispatch what the budget allows, highest priority first, and defer the rest. Never runs a
    /// model itself: with a job system it submits, without one it queues for `pump`.
    [[nodiscard]] Status dispatch() noexcept;

    /// Complete what has finished, and run what was dispatched inline. Returns the number
    /// completed.
    ///
    /// This is the only call that can spend time on a model, and it is the caller's choice where in
    /// the frame it happens.
    [[nodiscard]] u32 pump() noexcept;

    /// The answer a caller holds for `session`, fresh or stale.
    [[nodiscard]] AsyncResult result(const InferenceSession& session) const noexcept;

    [[nodiscard]] const FrameReport& report() const noexcept { return report_; }

    /// Requests that were deferred and are still waiting. They do not survive `begin_frame`: a
    /// deferred request is re-submitted by whoever wanted it, because a request nobody still wants
    /// is the one a queue silently accumulates.
    [[nodiscard]] u32 deferred_count() const noexcept;

private:
    struct Request {
        InferenceSession* session = nullptr;
        RequestId id;
        InferencePriority priority = InferencePriority::Normal;
        u32 order = 0;
        bool dispatched = false;
        bool completed = false;
        jobs::JobHandle job;
        Status outcome = ok();
    };

    struct Held {
        const InferenceSession* session = nullptr;
        u32 frames_stale = 0;
        bool completed_once = false;
    };

    static void run_request(const jobs::TaskContext& context, void* user) noexcept;

    [[nodiscard]] Held* held_for(const InferenceSession* session) noexcept;

    jobs::JobSystem* jobs_ = nullptr;
    InferenceBudget budget_;
    /// Inline, never reallocated. A job holds a pointer to its `Request` for as long as it runs,
    /// and an array that grew underneath one would hand the worker a moved-from object.
    FixedArray<Request, kMaxRequests> requests_;
    FixedArray<Held, kMaxRequests> held_;
    FrameReport report_;
    u32 next_id_ = 1;
    u32 next_order_ = 0;
};

}  // namespace cy::ml
