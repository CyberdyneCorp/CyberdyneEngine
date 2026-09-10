#pragma once
// The knowledge store: what an agent believes about the world, with confidence that decays.
// M8.b task 6.4.
//
// ================================================================================================
// WHY THIS EXISTS RATHER THAN A `canSeeEnemy` FLAG
// ================================================================================================
//
// `ai-system` states the reason in its own purpose: agents hold "knowledge with decaying confidence
// rather than answering `canSeeEnemy`, which is what produces SEARCH BEHAVIOUR instead of instant
// forgetting". A boolean has one bit of history; an entry with a last-known position, a time and a
// confidence has enough for a graph to branch from pursuing to searching, and that branch is the
// behaviour a player recognises as an agent that noticed them.
//
// So the store is the ONLY thing a behaviour graph reads. `ai-system`: "Behaviour graphs SHALL read
// knowledge, not sensors directly." Perception writes here and nowhere else.
//
// ================================================================================================
// DECAY IS A FUNCTION OF TICKS, NOT OF SECONDS
// ================================================================================================
//
// Every time in this file is a TICK NUMBER. `ai-system` requires AI to be deterministic "so that
// network reconciliation, replay, and automated testing are valid", and a confidence that fell by
// a measured elapsed time would make a re-simulated tick disagree with the original one by however
// much the machine was busy. `decay_per_tick` is a rate per tick and `KnowledgeStore::age()` takes
// the tick — there is no clock in this file and there must not be one.
//
// ================================================================================================
// SHARING IS A CHANNEL WITH A DELAY, NOT A BROADCAST
// ================================================================================================
//
// `ai-system`: "Knowledge SHALL be shareable between agents through a declared channel (a squad, a
// faction), so one agent's perception can inform others, WITH CONFIGURABLE DELAY AND FIDELITY
// LOSS." A squad that learnt everything instantly and perfectly is a squad that behaves like one
// creature, so `KnowledgeChannel` holds posts for `delay_ticks` and applies `fidelity` to the
// confidence and a spread to the position before delivering them.

#include <cy/core/base/expected.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/entity.h>

namespace cy::ai {

using ecs::Entity;

/// Which sense supplied an entry. `ai-system` requires the store to record "the sensor that
/// supplied it", because "I heard something" and "I can see it" support different behaviour.
enum class SenseKind : u8 { Vision = 0, Hearing, Damage, Touch, Proximity, Shared, Count };

[[nodiscard]] const char* sense_kind_name(SenseKind kind) noexcept;

/// One thing an agent believes. `ai-system`'s list, in its order: "entity reference, last known
/// position and velocity, first and last perceived time, a confidence value, a threat or relevance
/// assessment, and the sensor that supplied it".
struct KnowledgeEntry {
    Entity subject;
    Vec3 last_position;
    Vec3 last_velocity;
    u32 first_tick = 0;
    u32 last_tick = 0;
    /// One when just perceived, falling towards zero while it is not.
    f32 confidence = 0.0F;
    /// What this is worth thinking about. The eviction order when the store is full.
    f32 relevance = 0.0F;
    SenseKind sense = SenseKind::Vision;
};

struct KnowledgeParams {
    /// How much confidence is lost per tick an entry is not re-perceived.
    f32 decay_per_tick = 0.01F;
    /// Below this, the entry is forgotten. `ai-system`: "entries SHALL be forgotten below a
    /// threshold".
    f32 forget_below = 0.05F;
    /// The most entries one agent holds. "evicted when the store is full, by lowest relevance".
    u32 capacity = 16;
};

/// What one `age()` did, so forgetting is observable rather than inferred from a missing entry.
struct KnowledgeUpdate {
    u32 entries = 0;
    u32 forgotten = 0;
    u32 evicted = 0;
};

/// One agent's beliefs.
class KnowledgeStore {
public:
    KnowledgeStore(Allocator& allocator, const KnowledgeParams& params) noexcept;

    KnowledgeStore(const KnowledgeStore&) = delete;
    KnowledgeStore& operator=(const KnowledgeStore&) = delete;
    KnowledgeStore(KnowledgeStore&&) noexcept = default;
    KnowledgeStore& operator=(KnowledgeStore&&) noexcept = default;

    [[nodiscard]] const KnowledgeParams& params() const noexcept { return params_; }
    [[nodiscard]] Span<const KnowledgeEntry> entries() const noexcept { return entries_.span(); }
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(entries_.size()); }
    [[nodiscard]] const KnowledgeEntry* find(Entity subject) const noexcept;

    /// Record a perception. An existing entry is refreshed; a new one is added, evicting the least
    /// relevant when the store is full.
    [[nodiscard]] Status perceive(Entity subject, Vec3 position, Vec3 velocity, SenseKind sense,
                                  f32 confidence, f32 relevance, u32 tick) noexcept;

    /// Advance to `tick`: decay every entry that was not perceived on it, and forget what falls
    /// below the threshold. Deterministic — the decay is (ticks elapsed) times the rate.
    [[nodiscard]] KnowledgeUpdate age(u32 tick) noexcept;

    /// The entry with the highest `confidence * relevance`, or null. What a graph's "is there a
    /// target" condition asks, and the reason it is a function here rather than a search in every
    /// behaviour: two graphs asking it differently would rank targets differently.
    [[nodiscard]] const KnowledgeEntry* best_target() const noexcept;

    void clear() noexcept { entries_.clear(); }

private:
    KnowledgeParams params_;
    Array<KnowledgeEntry> entries_;
    u32 last_tick_ = 0;
};

/// One post on a channel, waiting out its delay.
struct KnowledgePost {
    Entity subject;
    Vec3 position;
    Vec3 velocity;
    /// The agent that saw it, so a channel does not deliver a post back to its author.
    Entity author;
    u32 deliver_tick = 0;
    f32 confidence = 0.0F;
    f32 relevance = 0.0F;
};

struct ChannelParams {
    /// How long a post waits. `ai-system`: "with the configured delay, rather than instantly and
    /// perfectly".
    u32 delay_ticks = 30;
    /// What fraction of the author's confidence a receiver gets. The fidelity loss.
    f32 fidelity = 0.6F;
    /// How far the delivered position may differ from the true one, in metres, applied as a
    /// deterministic function of the post rather than as noise.
    f32 position_spread = 2.0F;
};

/// A squad, a faction, a radio net: a declared channel one agent posts to and others read.
///
/// Delivery is on `post_tick + delay_ticks` and in post order, for the same reason
/// `cy::navigation::PathQueue` delivers on a derived tick: `ai-system` requires asynchronous work
/// to "complete at a deterministic tick and be applied in a deterministic order, not when it
/// happens to finish".
class KnowledgeChannel {
public:
    KnowledgeChannel(Allocator& allocator, const ChannelParams& params) noexcept;

    KnowledgeChannel(const KnowledgeChannel&) = delete;
    KnowledgeChannel& operator=(const KnowledgeChannel&) = delete;

    [[nodiscard]] const ChannelParams& params() const noexcept { return params_; }
    [[nodiscard]] u32 pending() const noexcept;

    [[nodiscard]] Status post(Entity author, const KnowledgeEntry& entry, u32 tick) noexcept;

    /// Deliver everything due on `tick` into `receiver`, skipping posts it wrote itself. Returns
    /// how many were written.
    [[nodiscard]] u32 deliver(Entity subject, KnowledgeStore& receiver, u32 tick) noexcept;

    /// Drop posts already delivered to everyone. Called once per tick by the runtime.
    [[nodiscard]] u32 collect(u32 tick) noexcept;

private:
    ChannelParams params_;
    Array<KnowledgePost> posts_;
};

}  // namespace cy::ai
