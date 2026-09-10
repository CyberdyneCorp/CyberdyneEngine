// The knowledge store and the channels agents share it over. See cy/ai/knowledge.h.

#include <cy/ai/knowledge.h>

#include <cmath>

namespace cy::ai {
namespace {

/// A deterministic spread derived from the post itself, so a re-simulated tick delivers the same
/// approximate position. `ai-system` requires shared knowledge to lose fidelity and requires the
/// simulation to be reproducible; a random source would satisfy the first and break the second.
[[nodiscard]] Vec3 spread(Vec3 position, Entity subject, u32 tick, f32 amount) noexcept {
    if (amount <= 0.0F) {
        return position;
    }
    const u64 mixed =
        (subject.bits() * 0x9E3779B97F4A7C15ULL) ^ (u64{tick} * 0xBF58476D1CE4E5B9ULL);
    const auto unit = [mixed](u32 shift) noexcept {
        const u32 bits = static_cast<u32>((mixed >> shift) & 0xFFFFu);
        return (static_cast<f32>(bits) / 32767.5F) - 1.0F;
    };
    return Vec3{position.x + (unit(0) * amount), position.y, position.z + (unit(16) * amount)};
}

}  // namespace

const char* sense_kind_name(SenseKind kind) noexcept {
    switch (kind) {
        case SenseKind::Vision:
            return "Vision";
        case SenseKind::Hearing:
            return "Hearing";
        case SenseKind::Damage:
            return "Damage";
        case SenseKind::Touch:
            return "Touch";
        case SenseKind::Proximity:
            return "Proximity";
        case SenseKind::Shared:
            return "Shared";
        case SenseKind::Count:
            break;
    }
    return "unknown";
}

KnowledgeStore::KnowledgeStore(Allocator& allocator, const KnowledgeParams& params) noexcept
    : params_(params), entries_(allocator) {}

const KnowledgeEntry* KnowledgeStore::find(Entity subject) const noexcept {
    for (const KnowledgeEntry& entry : entries_.span()) {
        if (entry.subject == subject) {
            return &entry;
        }
    }
    return nullptr;
}

Status KnowledgeStore::perceive(Entity subject, Vec3 position, Vec3 velocity, SenseKind sense,
                                f32 confidence, f32 relevance, u32 tick) noexcept {
    for (KnowledgeEntry& entry : entries_.span()) {
        if (entry.subject != subject) {
            continue;
        }
        // A better look replaces a worse one; a worse look does not erase a better one taken this
        // same tick, which is what stops a hearing report from downgrading a sighting.
        if (confidence >= entry.confidence || entry.last_tick != tick) {
            entry.last_position = position;
            entry.last_velocity = velocity;
            entry.sense = sense;
            entry.confidence = confidence;
        }
        entry.relevance = relevance;
        entry.last_tick = tick;
        return ok();
    }

    if (entries_.size() >= params_.capacity) {
        // `ai-system`: "evicted when the store is full, by lowest relevance". The tie-break is the
        // entity's own bits, so a full store evicts the same entry in every run.
        usize worst = 0;
        for (usize index = 1; index < entries_.size(); ++index) {
            const KnowledgeEntry& candidate = entries_[index];
            const KnowledgeEntry& incumbent = entries_[worst];
            const f32 candidate_score = candidate.relevance * candidate.confidence;
            const f32 incumbent_score = incumbent.relevance * incumbent.confidence;
            if (candidate_score < incumbent_score ||
                (candidate_score == incumbent_score &&
                 candidate.subject.bits() < incumbent.subject.bits())) {
                worst = index;
            }
        }
        if ((entries_[worst].relevance * entries_[worst].confidence) >= (relevance * confidence)) {
            return ok();  // nothing here is worth less than what arrived
        }
        entries_.remove_unordered(worst);
    }

    KnowledgeEntry entry;
    entry.subject = subject;
    entry.last_position = position;
    entry.last_velocity = velocity;
    entry.first_tick = tick;
    entry.last_tick = tick;
    entry.confidence = confidence;
    entry.relevance = relevance;
    entry.sense = sense;
    return entries_.push_back(entry);
}

KnowledgeUpdate KnowledgeStore::age(u32 tick) noexcept {
    KnowledgeUpdate update;
    if (tick <= last_tick_) {
        update.entries = static_cast<u32>(entries_.size());
        return update;
    }
    last_tick_ = tick;

    usize index = 0;
    while (index < entries_.size()) {
        KnowledgeEntry& entry = entries_[index];
        if (entry.last_tick < tick) {
            const f32 elapsed = static_cast<f32>(tick - entry.last_tick);
            entry.confidence = 1.0F - (elapsed * params_.decay_per_tick);
            entry.confidence = (entry.confidence < 0.0F) ? 0.0F : entry.confidence;
        }
        if (entry.confidence < params_.forget_below) {
            entries_.remove_unordered(index);
            ++update.forgotten;
            continue;
        }
        ++index;
    }
    update.entries = static_cast<u32>(entries_.size());
    return update;
}

const KnowledgeEntry* KnowledgeStore::best_target() const noexcept {
    const KnowledgeEntry* best = nullptr;
    f32 best_score = 0.0F;
    for (const KnowledgeEntry& entry : entries_.span()) {
        const f32 score = entry.confidence * entry.relevance;
        // A strict comparison plus an entity tie-break: two equally interesting targets must
        // resolve the same way in every run.
        if (best == nullptr || score > best_score ||
            (score == best_score && entry.subject.bits() < best->subject.bits())) {
            best_score = score;
            best = &entry;
        }
    }
    return best;
}

KnowledgeChannel::KnowledgeChannel(Allocator& allocator, const ChannelParams& params) noexcept
    : params_(params), posts_(allocator) {}

u32 KnowledgeChannel::pending() const noexcept {
    return static_cast<u32>(posts_.size());
}

Status KnowledgeChannel::post(Entity author, const KnowledgeEntry& entry, u32 tick) noexcept {
    KnowledgePost message;
    message.subject = entry.subject;
    message.position = entry.last_position;
    message.velocity = entry.last_velocity;
    message.author = author;
    message.deliver_tick = tick + params_.delay_ticks;
    message.confidence = entry.confidence * params_.fidelity;
    message.relevance = entry.relevance;
    return posts_.push_back(message);
}

u32 KnowledgeChannel::deliver(Entity subject, KnowledgeStore& receiver, u32 tick) noexcept {
    u32 delivered = 0;
    // In post order, which is the order they were written: `ai-system` requires asynchronous work
    // to be "applied in a deterministic order".
    for (const KnowledgePost& message : posts_.span()) {
        if (message.deliver_tick != tick || message.author == subject) {
            continue;
        }
        const Vec3 approximate =
            spread(message.position, message.subject, tick, params_.position_spread);
        if (receiver
                .perceive(message.subject, approximate, message.velocity, SenseKind::Shared,
                          message.confidence, message.relevance, tick)
                .has_value()) {
            ++delivered;
        }
    }
    return delivered;
}

u32 KnowledgeChannel::collect(u32 tick) noexcept {
    // A post is dropped once its delivery tick is BEHIND the current one, so every receiver had
    // exactly one tick to take it — which is what makes delivery independent of the order the
    // receivers are visited in.
    //
    // A STABLE compaction rather than `remove_unordered`: `deliver()` walks this array in order and
    // says so, and a swap-with-the-last removal would quietly make "in post order" a different
    // order after the first collection.
    usize keep = 0;
    for (const KnowledgePost& message : posts_.span()) {
        if (message.deliver_tick >= tick) {
            posts_[keep] = message;
            ++keep;
        }
    }
    const u32 dropped = static_cast<u32>(posts_.size() - keep);
    while (posts_.size() > keep) {
        posts_.pop_back();
    }
    return dropped;
}

}  // namespace cy::ai
