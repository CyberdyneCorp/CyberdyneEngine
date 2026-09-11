#pragma once
// The network profiler: causal questions, not only aggregate ones. M9 tasks 4.4 and 4.5.
//
// ================================================================================================
// THE THREE QUESTIONS THAT MAKE IT A PROFILER RATHER THAN A COUNTER
// ================================================================================================
//
// `networking-and-replication` — "Network profiler": it "SHALL answer **causal** questions, not
// only aggregate ones", and names three:
//
//   * **Why was entity N sent to this peer?** — the relevance rule that admitted it, the priority
//     score, and the factors that contributed.
//   * **Why did this peer receive that many bytes this tick?** — attributed down to schema and
//     entity, not a total.
//   * **Why did the player rubber-band?** — which field diverged, by how much, and the tick the
//     divergence originated at.
//
// A profiler that reports bandwidth in and out answers none of them, and every one of them is a
// question a real support ticket asks. So the record below is per (peer, tick, entity, schema),
// with the rule and the score beside the bytes.
//
// ================================================================================================
// IT IS BOUNDED, BECAUSE A PROFILER THAT KEEPS EVERYTHING IS AN OUTAGE
// ================================================================================================
//
// A per-entity, per-schema, per-tick record for sixty-four peers is megabytes a second. The window
// is a ring of `kProfilerTicks` ticks and the eviction is where a bounded structure leaks, which is
// why `tests/test_teardown.cpp` drives it past its bound and asks a tracking allocator what is
// left.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/ids.h>
#include <cy/networking/interest.h>
#include <cy/networking/scheduler.h>

namespace cy::net {

/// How many ticks of attribution the profiler retains. Two seconds at 60 Hz — long enough to cover
/// the moment a player complains about and short enough to be free.
inline constexpr u32 kProfilerTicks = 120;

/// What a byte was spent on. "by category (state, RPC, spawn, acknowledgement, overhead)".
enum class TrafficCategory : u8 {
    State = 0,
    Rpc,
    Spawn,
    Despawn,
    Acknowledgement,
    Overhead,
    Count,
};

const char* traffic_category_name(TrafficCategory category) noexcept;

/// One attributed send.
struct TrafficRecord {
    u64 tick = 0;
    PeerId peer;
    NetworkId entity;
    u32 schema = 0;
    TrafficCategory category = TrafficCategory::State;
    u32 bytes = 0;
    /// Why the entity was a candidate, and what it scored. The causal half.
    RelevanceRule rule = RelevanceRule::None;
    u32 score = 0;
    NetworkLodBand band = NetworkLodBand::Far;
};

/// The answer to "why was entity N sent to this peer?".
struct ReplicationExplanation {
    bool found = false;
    u64 tick = 0;
    RelevanceRule rule = RelevanceRule::None;
    u32 score = 0;
    NetworkLodBand band = NetworkLodBand::Far;
    u32 bytes = 0;
};

/// The answer to "why did this peer receive that many bytes?", for one tick.
struct BandwidthAttribution {
    u64 tick = 0;
    u32 total_bytes = 0;
    u32 by_category[static_cast<u32>(TrafficCategory::Count)] = {};
    u32 entities = 0;
    /// The single largest contributor, so a report leads with the answer rather than with a table.
    NetworkId largest_entity;
    u32 largest_schema = 0;
    u32 largest_bytes = 0;
};

/// The answer to "why did the player rubber-band?".
struct CorrectionExplanation {
    bool found = false;
    /// The schema and field that diverged, as the validator named them.
    u32 schema = 0;
    reflect::FieldId field;
    u64 magnitude = 0;
    /// The tick the divergence originated at, which is not the tick it was noticed at.
    u64 originating_tick = 0;
    u64 noticed_tick = 0;
};

/// Aggregate counters, for the diagnostics overlay. Beside the causal half rather than instead of
/// it.
struct NetworkCounters {
    u64 bytes_in = 0;
    u64 bytes_out = 0;
    u64 datagrams_in = 0;
    u64 datagrams_out = 0;
    u32 round_trip_us = 0;
    u32 jitter_us = 0;
    u32 loss_per_mille = 0;
    u32 interest_set_size = 0;
    u32 candidate_set_size = 0;
    u32 deferred = 0;
    u32 dormant = 0;
    u64 reconciliations = 0;
    u64 largest_prediction_error = 0;
    /// `Lockstep` only: the tick a hash comparison diverged at, or zero.
    u64 lockstep_divergence_tick = 0;
};

class NetworkProfiler {
public:
    explicit NetworkProfiler(Allocator& allocator) noexcept
        : records_(allocator), corrections_(allocator) {}

    NetworkProfiler(const NetworkProfiler&) = delete;
    NetworkProfiler& operator=(const NetworkProfiler&) = delete;

    /// Record one attributed send. Evicts records older than the window.
    [[nodiscard]] Status note_traffic(const TrafficRecord& record) noexcept;

    /// Record a correction the validator localised.
    [[nodiscard]] Status note_correction(const CorrectionExplanation& correction) noexcept;

    [[nodiscard]] ReplicationExplanation why_replicated(PeerId peer,
                                                        NetworkId entity) const noexcept;
    [[nodiscard]] BandwidthAttribution attribute(PeerId peer, u64 tick) const noexcept;
    [[nodiscard]] CorrectionExplanation explain_correction(NetworkId entity) const noexcept;

    [[nodiscard]] NetworkCounters& counters() noexcept { return counters_; }
    [[nodiscard]] const NetworkCounters& counters() const noexcept { return counters_; }

    [[nodiscard]] u32 retained() const noexcept { return static_cast<u32>(records_.size()); }
    [[nodiscard]] u64 evicted() const noexcept { return evicted_; }
    [[nodiscard]] u64 newest_tick() const noexcept { return newest_; }

    void clear() noexcept;

private:
    Array<TrafficRecord> records_;
    Array<CorrectionExplanation> corrections_;
    NetworkCounters counters_{};
    u64 newest_ = 0;
    u64 evicted_ = 0;
};

}  // namespace cy::net
