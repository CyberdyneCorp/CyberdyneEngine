// Provenance and the generation profiler. See include/cy/pcg/diagnostics.h.

#include <cy/pcg/diagnostics.h>

namespace cy::pcg {

Status RegionProvenance::record_accepted(const ProvenanceRecord& record) noexcept {
    return accepted_.push_back(record);
}

Status RegionProvenance::record_rejected(const RejectionRecord& record) noexcept {
    return rejected_.push_back(record);
}

void RegionProvenance::clear() noexcept {
    accepted_.clear();
    rejected_.clear();
}

const ProvenanceRecord* RegionProvenance::why_here(GeneratedId identity) const noexcept {
    for (const ProvenanceRecord& record : accepted_) {
        if (record.identity == identity) {
            return &record;
        }
    }
    return nullptr;
}

const RejectionRecord* RegionProvenance::why_nothing_here(f32 x, f32 z, f32 radius) const noexcept {
    const RejectionRecord* best = nullptr;
    f32 best_distance = radius * radius;
    for (const RejectionRecord& record : rejected_) {
        const f32 dx = record.position_x - x;
        const f32 dz = record.position_z - z;
        const f32 distance = dx * dx + dz * dz;
        // Strictly nearer, and ties broken by the SLOT rather than by array order — two rejected
        // candidates at the same distance must give one answer whatever order they were recorded
        // in, or "why is nothing here" would answer differently after a regeneration that changed
        // nothing.
        const bool nearer =
            distance < best_distance ||
            (best != nullptr && distance == best_distance && record.slot < best->slot);
        if (nearer) {
            best_distance = distance;
            best = &record;
        }
    }
    return best;
}

u64 RegionProvenance::bytes() const noexcept {
    return accepted_.capacity() * sizeof(ProvenanceRecord) +
           rejected_.capacity() * sizeof(RejectionRecord);
}

// --- The profiler -----------------------------------------------------------------------------

void GenerationProfile::reset() noexcept {
    for (StageProfile& stage : stages) {
        const char* name = stage.name;
        const u8 index = stage.stage;
        stage = StageProfile{};
        stage.name = name;
        stage.stage = index;
    }
    cache_hits = 0;
    cache_misses = 0;
    cache_refusals = 0;
    regions_in_dirty_set = 0;
    regions_actually_changed = 0;
}

f32 GenerationProfile::cache_hit_rate() const noexcept {
    const u64 looked_up = cache_hits + cache_misses;
    if (looked_up == 0) {
        // Not a zero hit rate: nothing was looked up. The two read identically in a chart and mean
        // opposite things, which is why `cache_misses` is reported beside this.
        return 0.0F;
    }
    return static_cast<f32>(cache_hits) / static_cast<f32>(looked_up);
}

const StageProfile* GenerationProfile::dominant_stage() const noexcept {
    const StageProfile* best = nullptr;
    for (const StageProfile& stage : stages) {
        if (best == nullptr || stage.cpu_micros > best->cpu_micros) {
            best = &stage;
        }
    }
    return best;
}

StageProfile* GenerationProfile::stage_at(u8 index) noexcept {
    return index < stages.size() ? &stages[index] : nullptr;
}

}  // namespace cy::pcg
