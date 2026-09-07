#include <cy/servers/residency/request.h>

#include <algorithm>
#include <cmath>

namespace cy::residency {

namespace {

/// How much of the demand term is importance and how much is the coverage the pixels actually
/// asked with. Importance leads because it is the value gameplay can reach; coverage is what makes
/// two equally important instances order by which one fills more of the screen.
constexpr f32 kImportanceWeight = 0.6F;
constexpr f32 kCoverageWeight = 0.4F;

/// A page that is four levels coarse than wanted is worse than one that is a single level coarse,
/// and not four times worse — the deficit is measured in mip levels, and each level is a doubling.
constexpr u32 kDeficitCap = 8;
constexpr f32 kDeficitGain = 0.5F;

/// The multiplier a request with no slack left receives, and how fast it falls back to 1 as slack
/// grows. Eight is chosen so that a page due now outranks the most important page with no deadline
/// at all, which is the behaviour "a camera cut is announced" describes.
constexpr f32 kUrgencyMax = 8.0F;
constexpr f32 kUrgencyFalloff = 2.0F;

/// A prediction nobody believes is still worth a quarter of a measurement. Zero would make an
/// unconfident prefetch indistinguishable from no request at all, and the accuracy reporting in
/// `deadline.h` exists precisely so that this can be tuned against evidence rather than taste.
constexpr f32 kConfidenceFloor = 0.25F;

/// Age is additive, not multiplicative: it is an anti-starvation term, not a claim about need. A
/// request waiting a second gains as much as a moderately important one has to begin with, and
/// stops gaining after four seconds so that an abandoned request cannot climb forever.
constexpr f64 kAgeCap = 4.0;
constexpr f32 kAgeGain = 0.35F;

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    if (!(value > 0.0F)) {
        return 0.0F;
    }
    return (value > 1.0F) ? 1.0F : value;
}

[[nodiscard]] f32 effective_cost_ms(const RequestInputs& inputs) noexcept {
    // A subsystem that has measured passes the measurement; one that has not gets the class
    // weight, which is why an unmeasured shadow page is not treated as free.
    return (inputs.production_cost_ms > 0.0F) ? inputs.production_cost_ms
                                              : cost_class_weight(inputs.cost);
}

}  // namespace

f32 score_request(const RequestInputs& inputs) noexcept {
    const f32 demand = (kImportanceWeight * clamp01(inputs.importance)) +
                       (kCoverageWeight * std::sqrt(clamp01(inputs.screen_coverage)));

    const u32 deficit = (inputs.detail_deficit > kDeficitCap) ? kDeficitCap : inputs.detail_deficit;
    const f32 deficit_gain = 1.0F + (static_cast<f32>(deficit) * kDeficitGain);

    // URGENCY IS SLACK, NOT TIME. This is where "time until needed" and "the cost of producing or
    // fetching the page" meet: a page that takes 40 ms to render and is needed in 30 ms is already
    // late, and a page that streams in 2 ms and is needed in 30 ms is not urgent at all. Scoring on
    // the deadline alone would order those two identically and miss both.
    f32 urgency = 1.0F;
    if (inputs.seconds_until_needed < kNoDeadline) {
        const f64 slack =
            inputs.seconds_until_needed - (static_cast<f64>(effective_cost_ms(inputs)) / 1000.0);
        if (slack <= 0.0) {
            urgency = kUrgencyMax;
        } else {
            urgency = 1.0F +
                      ((kUrgencyMax - 1.0F) / (1.0F + (kUrgencyFalloff * static_cast<f32>(slack))));
        }
    }

    const f32 confidence =
        kConfidenceFloor + ((1.0F - kConfidenceFloor) * clamp01(inputs.prediction_confidence));

    const f64 age = (inputs.age_seconds > kAgeCap) ? kAgeCap : inputs.age_seconds;
    const f32 age_bonus = (age > 0.0) ? (static_cast<f32>(age) * kAgeGain) : 0.0F;

    return (demand * deficit_gain * urgency * confidence) + age_bonus;
}

Status RequestQueue::submit(const Request& request) noexcept {
    ++submissions_;

    const u64 packed = request.key.packed();
    if (usize* slot = slots_.find(packed); slot != nullptr) {
        // THE MERGE TAKES THE STRONGEST CLAIM OF EVERY SUBMISSION, not the first or the last. A
        // page requested weakly by a background view and urgently by the player's is urgent; a
        // merge that kept the first submission would answer with whichever view was extracted
        // first, which is an ordering nobody declared.
        ScoredRequest& existing = entries_[*slot];
        RequestInputs& into = existing.inputs;
        const RequestInputs& from = request.inputs;
        into.importance = std::max(into.importance, from.importance);
        into.screen_coverage = std::max(into.screen_coverage, from.screen_coverage);
        into.detail_deficit = std::max(into.detail_deficit, from.detail_deficit);
        into.prediction_confidence =
            std::max(into.prediction_confidence, from.prediction_confidence);
        into.seconds_until_needed = std::min(into.seconds_until_needed, from.seconds_until_needed);
        into.age_seconds = std::max(into.age_seconds, from.age_seconds);
        into.production_cost_ms = std::max(into.production_cost_ms, from.production_cost_ms);
        if (static_cast<u8>(from.cost) > static_cast<u8>(into.cost)) {
            into.cost = from.cost;
        }
        if (existing.instance == 0) {
            existing.instance = request.instance;
        }
        existing.bytes = std::max(existing.bytes, request.bytes);
        existing.guaranteed = existing.guaranteed || request.guaranteed;
        ++existing.submissions;
        return ok();
    }

    ScoredRequest scored;
    scored.key = request.key;
    scored.inputs = request.inputs;
    scored.instance = request.instance;
    scored.bytes = request.bytes;
    scored.guaranteed = request.guaranteed;
    scored.score = 0.0F;
    scored.submissions = 1;
    if (Status pushed = entries_.push_back(scored); !pushed) {
        --submissions_;
        return pushed;
    }
    if (auto placed = slots_.insert(packed, entries_.size() - 1); !placed) {
        entries_.pop_back();
        --submissions_;
        return make_unexpected(placed.error());
    }
    return ok();
}

void RequestQueue::compact() noexcept {
    for (ScoredRequest& entry : entries_) {
        entry.score = score_request(entry.inputs);
    }

    // Ordered by score, and by the packed page key when two scores are equal. The tiebreak is not
    // tidiness: two pages of the same asset at the same distance score identically to the bit, and
    // without it the admitted set would depend on the order requests happened to arrive in — which
    // is a hash iteration order and a thread interleaving away from being different every run.
    std::ranges::sort(entries_, [](const ScoredRequest& lhs, const ScoredRequest& rhs) noexcept {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        return lhs.key.packed() < rhs.key.packed();
    });

    slots_.clear();
    for (usize index = 0; index < entries_.size(); ++index) {
        // The reinsert cannot grow the table — it was sized by the same keys a moment ago — so the
        // failure branch is unreachable in practice. It is still not ignored: a dropped slot would
        // make the next `submit` for that page append a duplicate instead of merging, and a
        // deduplicating queue that silently stops deduplicating is the defect this file exists to
        // prevent. Clearing is the honest response; the next frame rebuilds from submissions.
        if (auto placed = slots_.insert(entries_[index].key.packed(), index); !placed) {
            slots_.clear();
            return;
        }
    }
}

void RequestQueue::clear() noexcept {
    entries_.clear();
    slots_.clear();
    submissions_ = 0;
}

}  // namespace cy::residency
