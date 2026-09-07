#pragma once
// Request priority: the one scoring function, and the queue that deduplicates before it runs.
// Task 4.1.
//
// `residency` — "Request priority": every request is scored from *importance, screen coverage, the
// detail deficit between what is wanted and what is resident, prediction confidence, time until
// needed, age, and the cost of producing or fetching the page*. All seven appear in `RequestInputs`
// and all seven are read by `score_request`, because a term that is in the specification and not in
// the function is a term nobody will notice is missing.
//
// "Scoring SHALL be defined once and applied by every subsystem, so that a texture page and a
// shadow page competing for the same budget are comparable." One function, in one translation unit,
// taking no subsystem argument — a subsystem cannot reach a different answer without changing the
// number it passes in, which is exactly the declared-transform discipline `importance.h` describes.
//
// --- COMPACTION HAPPENS BEFORE SCHEDULING, NOT AFTER
// -----------------------------------------------
//
// "Requests SHALL be **deduplicated and compacted before scheduling**, and where a subsystem
// generates requests on the GPU that compaction SHALL happen there." A million pixels sampling one
// texture page must become one request; if the deduplication ran after scoring, the scheduler would
// have scored a million identical entries to discard 999,999 of them.
//
// `RequestQueue::submit` therefore merges into an existing entry keyed on the page rather than
// appending. Merging is not "keep the first": the surviving entry takes the *strongest* claim of
// every merged one — the highest importance, the largest deficit, the nearest deadline, the highest
// confidence — because a page requested weakly by one view and urgently by another is urgent.
// `submissions()` and `size()` are both reported so a test can state the ratio.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/servers/residency/types.h>

namespace cy::residency {

/// The seven terms, and nothing else.
struct RequestInputs {
    /// The shared render importance, already through the requesting subsystem's declared transform.
    f32 importance = 0.0F;
    /// Fraction of the view the requesting surface covers, in [0, 1].
    f32 screen_coverage = 0.0F;
    /// Levels between what was wanted and what is resident. Zero means the page would be an
    /// improvement over nothing at all but nothing is currently missing.
    u32 detail_deficit = 0;
    /// How much the predictor believes itself, in [0, 1]. A feedback-driven request is 1: it is not
    /// a prediction, it is a measurement.
    f32 prediction_confidence = 1.0F;
    /// Seconds until the page is needed; `kNoDeadline` when nothing has predicted a time.
    f64 seconds_until_needed = kNoDeadline;
    /// How long this request has been waiting. Present so that a request that is never the most
    /// urgent still eventually runs, rather than starving behind a stream of fresher ones.
    f64 age_seconds = 0.0;
    /// Measured or declared cost of producing or fetching, in milliseconds. See `CostClass`.
    f32 production_cost_ms = 0.0F;
    /// What class of work producing it is, for the pages nobody has measured.
    CostClass cost = CostClass::Streamed;
};

/// The score. Higher is more urgent. Deterministic, and a pure function of its argument — the two
/// properties that make "the decision SHALL be explicable" a thing a test can hold to.
[[nodiscard]] f32 score_request(const RequestInputs& inputs) noexcept;

/// A request as it is submitted: which page, and the seven terms.
struct Request {
    PageKey key;
    RequestInputs inputs;
    /// The instance that wanted it, for diagnostics. Zero when the requester is not an instance —
    /// a prefetch from a world cell prediction, for example.
    u64 instance = 0;
    /// How much memory the page will occupy once resident. A *description* of the subsystem's
    /// storage, not the storage: the policy cannot budget what it cannot size, and asking for the
    /// number is the smallest thing it can ask for that is not the page itself.
    u64 bytes = 0;
    /// Whether the page belongs to a guaranteed-resident set — a mip tail, a virtual geometry root,
    /// a coarse fallback. `residency`: "No residency system blocks another" holds because these
    /// exist, so the flag travels with the request rather than being configured elsewhere.
    bool guaranteed = false;
};

/// A request after deduplication: the merged claim, its score, and how many submissions it
/// absorbed.
struct ScoredRequest {
    PageKey key;
    RequestInputs inputs;
    u64 instance = 0;
    u64 bytes = 0;
    bool guaranteed = false;
    f32 score = 0.0F;
    u32 submissions = 1;
};

/// The per-frame request set. Deduplicating on the way in, ordered on the way out.
class RequestQueue {
public:
    explicit RequestQueue(Allocator& allocator = current_allocator()) noexcept
        : entries_(allocator), slots_(allocator) {}

    RequestQueue(const RequestQueue&) = delete;
    RequestQueue& operator=(const RequestQueue&) = delete;
    RequestQueue(RequestQueue&&) noexcept = default;
    RequestQueue& operator=(RequestQueue&&) noexcept = default;
    ~RequestQueue() = default;

    /// Submit one request, merging it into any existing entry for the same page.
    Status submit(const Request& request) noexcept;

    /// Score every entry and order it, most urgent first. Idempotent; call it once a frame.
    void compact() noexcept;

    /// Entries, valid after `compact()` and in its order.
    [[nodiscard]] const ScoredRequest* entries() const noexcept { return entries_.data(); }
    [[nodiscard]] usize size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    /// How many `submit` calls produced the current entries. The numerator of the compaction ratio.
    [[nodiscard]] u64 submissions() const noexcept { return submissions_; }

    /// Drop everything. The frame's requests do not outlive the frame; a request that still matters
    /// is re-submitted, which is also how `age_seconds` stays honest.
    void clear() noexcept;

private:
    Array<ScoredRequest> entries_;
    HashMap<u64, usize> slots_;  // packed page key -> index into entries_
    u64 submissions_ = 0;
};

}  // namespace cy::residency
