#pragma once
// Provenance and the generation profiler. M10 task 4.3.
//
// `procedural-content-generation` — "Provenance": "Development builds SHALL retain provenance for
// generated results: the generator, its version, the region, the seed, the graph node that produced
// the output, and the attribute values that determined it. The editor SHALL answer WHY IS THIS HERE
// for any generated instance ... SHALL ALSO ANSWER WHY IS NOTHING HERE for a location: which filter
// rejected it, with the value tested and the threshold applied. Provenance SHALL be strippable from
// shipping builds."
//
// ================================================================================================
// WHY IS NOTHING HERE IS THE HARDER HALF, AND IT IS WHY REJECTIONS ARE RECORDED
// ================================================================================================
//
// "Why is this here" can be reconstructed from the output: the instance exists, so its slot, region
// and node are all derivable from its identity. "Why is nothing here" cannot — a rejected candidate
// leaves no trace in the output at all, so the only way to answer it is to have KEPT the rejection,
// with the value that was tested and the threshold it failed. `RejectionRecord` is that, and the
// requirement's "and by how much" is `margin`.
//
// STRIPPABLE MEANS HOLDS NOTHING, NOT "IS EMPTY". `ProvenanceMode::Off` is checked before the
// record is built rather than after, so a shipping build pays neither the memory nor the store —
// and `RegionProvenance::bytes()` reports the arrays' CAPACITY rather than their size, so
// `tests/test_scale.cpp` measures what is held rather than what is stored. An empty array that was
// still reserved is exactly the shape that reads like a pass and ships the cost.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/pcg/dataset.h>
#include <cy/pcg/identity.h>
#include <cy/pcg/invalidation.h>

namespace cy::pcg {

/// Whether provenance is retained. Checked before a record is built.
enum class ProvenanceMode : u8 {
    /// Shipping. Nothing is recorded and nothing is reserved.
    Off = 0,
    /// Development and editor. Every accepted instance and every rejected candidate is recorded.
    On,
};

/// Why one generated instance exists.
struct ProvenanceRecord {
    GeneratedId identity;
    RegionCoord region;
    u64 seed = 0;
    u32 generator_version = 0;
    /// The stage that produced it, by index and by authored name.
    u8 stage = 0;
    const char* stage_name = "";
    const char* generator_name = "";
    /// The candidate's own slot, which is what identity derives from.
    u32 slot = 0;
    /// The attribute that selected it and the value it carried: "the attribute values that
    /// determined it". One value rather than the whole row, because the whole row is the point set
    /// and the editor can read it — what provenance adds is WHICH value the rule tested.
    AttributeId deciding_attribute;
    f32 deciding_value = 0.0F;
    f32 position_x = 0.0F;
    f32 position_z = 0.0F;
};

/// Why one candidate is NOT there.
struct RejectionRecord {
    RegionCoord region;
    u32 slot = 0;
    u8 stage = 0;
    const char* stage_name = "";
    /// The attribute the filter tested. Zero for a spacing rejection, which tests a distance rather
    /// than an attribute.
    AttributeId tested_attribute;
    f32 tested_value = 0.0F;
    f32 threshold = 0.0F;
    /// How far short it fell. "THEN the editor SHALL report which filter rejected it AND BY HOW
    /// MUCH" — this is the "how much", signed so a reader can tell over from under.
    f32 margin = 0.0F;
    f32 position_x = 0.0F;
    f32 position_z = 0.0F;
    /// The identity of the candidate that beat it, for a spacing rejection. Invalid otherwise, and
    /// it is a CANDIDATE's identity rather than an accepted instance's — a spacing resolution that
    /// named an accepted point would be the order-dependent one the compiler refuses.
    GeneratedId beaten_by;
    /// The region the winning candidate came from.
    ///
    /// Recorded so that CROSS-REGION contention can be COUNTED rather than assumed. This project
    /// has shipped a determinism test that passed on the very defect it was written for, its scene
    /// having never contended; the spike's own first contention meter had the same bug and exits 3
    /// with "this report is void" when the count is zero. The suite here reads this field for the
    /// same reason: a conflict-resolution test whose regions never fight proves nothing.
    RegionCoord beaten_in_region;
};

/// One region's provenance. Held beside the region's output, so evicting a region evicts its
/// explanation with it rather than growing a ledger nothing prunes.
class RegionProvenance {
public:
    explicit RegionProvenance(Allocator& allocator) noexcept
        : accepted_(allocator), rejected_(allocator) {}

    RegionProvenance(const RegionProvenance&) = delete;
    RegionProvenance& operator=(const RegionProvenance&) = delete;
    RegionProvenance(RegionProvenance&&) noexcept = default;
    RegionProvenance& operator=(RegionProvenance&&) noexcept = default;

    [[nodiscard]] Status record_accepted(const ProvenanceRecord& record) noexcept;
    [[nodiscard]] Status record_rejected(const RejectionRecord& record) noexcept;
    void clear() noexcept;

    [[nodiscard]] Span<const ProvenanceRecord> accepted() const noexcept {
        return accepted_.span();
    }
    [[nodiscard]] Span<const RejectionRecord> rejected() const noexcept { return rejected_.span(); }

    /// "Why is this here", for one instance.
    [[nodiscard]] const ProvenanceRecord* why_here(GeneratedId identity) const noexcept;

    /// "Why is nothing here", for a position in region-local metres: the nearest rejected candidate
    /// within `radius`, which is the filter that rejected it and by how much. Null when nothing was
    /// ever a candidate there, which is itself the answer and a different one.
    [[nodiscard]] const RejectionRecord* why_nothing_here(f32 x, f32 z, f32 radius) const noexcept;

    [[nodiscard]] u64 bytes() const noexcept;

private:
    Array<ProvenanceRecord> accepted_;
    Array<RejectionRecord> rejected_;
};

// --- The profiler -----------------------------------------------------------------------------

/// Per node, so that a slow generator is attributable to a node rather than to the graph.
///
/// `procedural-content-generation` — "PCG diagnostics": "The profiler SHALL report per node:
/// processor and GPU time, candidates in and out, memory, and cache hit rate."
struct StageProfile {
    const char* name = "";
    u8 stage = 0;
    /// Microseconds of processor time across every region this stage evaluated.
    u64 cpu_micros = 0;
    /// Microseconds of GPU time. Always zero on this host: nothing in this module dispatches to a
    /// device, and a number invented for a path that does not exist would be the false green the
    /// milestone's brief forbids. See the module README's gaps.
    u64 gpu_micros = 0;
    u64 regions_evaluated = 0;
    u64 candidates_in = 0;
    u64 candidates_out = 0;
    u64 bytes = 0;
    /// Sweeps, for an iterative stage. One for everything else.
    u32 sweeps = 0;
    /// Regions the fixed point added AFTER the declared dilation — the edge a statically-closed
    /// invalidation would have missed. A non-zero here on a partial regeneration is the 7-of-12
    /// cell being avoided, counted.
    u32 fixed_point_additions = 0;
};

/// One run's profile, plus the cache's own counters.
struct GenerationProfile {
    explicit GenerationProfile(Allocator& allocator) noexcept : stages(allocator) {}

    GenerationProfile(const GenerationProfile&) = delete;
    GenerationProfile& operator=(const GenerationProfile&) = delete;
    GenerationProfile(GenerationProfile&&) noexcept = default;
    GenerationProfile& operator=(GenerationProfile&&) noexcept = default;

    Array<StageProfile> stages;
    u64 cache_hits = 0;
    u64 cache_misses = 0;
    u64 cache_refusals = 0;
    u64 regions_in_dirty_set = 0;
    u64 regions_actually_changed = 0;

    void reset() noexcept;

    /// Hits over hits plus misses, in [0, 1]. Zero when nothing was looked up, which is a different
    /// thing from a zero hit rate and is why `cache_misses` is reported beside it.
    [[nodiscard]] f32 cache_hit_rate() const noexcept;

    /// The stage that dominated, or null when nothing ran. "WHEN a generator is slow THEN the
    /// profiler SHALL show which nodes dominate its time."
    [[nodiscard]] const StageProfile* dominant_stage() const noexcept;

    [[nodiscard]] StageProfile* stage_at(u8 index) noexcept;
};

}  // namespace cy::pcg
