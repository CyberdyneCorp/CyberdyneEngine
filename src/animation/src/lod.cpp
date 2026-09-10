#include <cy/animation/lod.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::animation {
namespace {

/// A distance threshold widened for an instance that is ALREADY AT OR INSIDE the tier it names, so
/// the boundary an instance leaves a tier by is further out than the one it entered by. An instance
/// sitting on a threshold therefore stays where it is instead of oscillating across it.
[[nodiscard]] f32 threshold(f32 base, f32 hysteresis, bool currently_inside) noexcept {
    return currently_inside ? base * (1.0F + hysteresis) : base;
}

}  // namespace

const char* lod_tier_name(LodTier tier) noexcept {
    switch (tier) {
        case LodTier::Full:
            return "Full";
        case LodTier::Simplified:
            return "Simplified";
        case LodTier::Cached:
            return "Cached";
        case LodTier::Baked:
            return "Baked";
    }
    return "?";
}

u8 bone_lod_for(LodTier tier) noexcept {
    switch (tier) {
        case LodTier::Full:
            return 0;
        case LodTier::Simplified:
            return 1;
        case LodTier::Cached:
            return 2;
        case LodTier::Baked:
            return kBoneLodLevels - 1;
    }
    return 0;
}

bool evaluates_pose(LodTier tier) noexcept {
    return tier != LodTier::Baked;
}

bool may_share_pose(LodTier tier) noexcept {
    return tier != LodTier::Full;
}

bool evaluates_modifiers(LodTier tier) noexcept {
    return tier == LodTier::Full;
}

f32 evaluation_hertz(LodTier tier) noexcept {
    switch (tier) {
        case LodTier::Full:
            return 0.0F;
        case LodTier::Simplified:
            return 30.0F;
        case LodTier::Cached:
            return 12.0F;
        case LodTier::Baked:
            return 0.0F;
    }
    return 0.0F;
}

LodTier select_tier(const LodPolicy& policy, const LodInputs& inputs, LodTier current) noexcept {
    if (inputs.pinned) {
        return inputs.pinned_tier;
    }
    if (!inputs.visible) {
        // Invisible is not free: root motion still runs, which is why this is `Baked` — no pose —
        // rather than "skip the instance".
        return policy.authoritative ? LodTier::Simplified : LodTier::Baked;
    }
    if (inputs.important || inputs.coverage >= policy.full_coverage) {
        return LodTier::Full;
    }

    const auto inside = [current](LodTier tier) noexcept {
        return static_cast<u8>(current) <= static_cast<u8>(tier);
    };

    LodTier chosen = LodTier::Baked;
    if (inputs.distance <=
        threshold(policy.full_distance, policy.hysteresis, inside(LodTier::Full))) {
        chosen = LodTier::Full;
    } else if (inputs.distance <= threshold(policy.simplified_distance, policy.hysteresis,
                                            inside(LodTier::Simplified))) {
        chosen = LodTier::Simplified;
    } else if (inputs.distance <=
               threshold(policy.cached_distance, policy.hysteresis, inside(LodTier::Cached))) {
        chosen = LodTier::Cached;
    }

    if (policy.authoritative && static_cast<u8>(chosen) > static_cast<u8>(LodTier::Simplified)) {
        // "Animation LOD tier SHALL be a function of simulation state ... for instances whose root
        // motion is authoritative." Their root motion is integrated whatever the tier, but the
        // cached and baked tiers pick a pose from a shared bucket, and an authoritative instance
        // must not have its visible pose decided by a neighbour's phase.
        return LodTier::Simplified;
    }
    return chosen;
}

void LodDistribution::note(LodTier tier) noexcept {
    const auto index = static_cast<usize>(tier);
    if (index < 4) {
        ++counts[index];
    }
}

u32 LodDistribution::total() const noexcept {
    return counts[0] + counts[1] + counts[2] + counts[3];
}

// --- PoseCache ----------------------------------------------------------------------------------

PoseCache::PoseCache(Allocator& allocator, u32 joints, u32 capacity) noexcept
    : entries_(allocator), poses_(allocator), joints_(joints), capacity_(capacity) {
    if (Status sized = entries_.resize(capacity); !sized) {
        capacity_ = 0;
        return;
    }
    if (Status sized = poses_.resize(static_cast<usize>(capacity) * joints); !sized) {
        capacity_ = 0;
        entries_.clear();
    }
}

u16 PoseCache::bucket_of(f32 phase, u16 buckets) noexcept {
    if (buckets == 0) {
        return 0;
    }
    const f32 wrapped = phase - std::floor(phase);
    const auto bucket = static_cast<u32>(wrapped * static_cast<f32>(buckets));
    return static_cast<u16>(bucket < buckets ? bucket : buckets - 1);
}

Expected<u32, Error> PoseCache::acquire(const PoseCacheKey& key, bool& must_fill) noexcept {
    ++stats_.lookups;
    for (usize index = 0; index < used_; ++index) {
        if (entries_[index].live && entries_[index].key == key) {
            ++stats_.hits;
            must_fill = false;
            return static_cast<u32>(index);
        }
    }
    if (used_ >= capacity_) {
        ++stats_.evictions;
        return make_unexpected(Error{ErrorCode::Unavailable,
                                     "the pose cache is full for this frame; the caller evaluates "
                                     "its own pose rather than sharing one",
                                     0});
    }
    const u32 slot = used_++;
    entries_[slot].key = key;
    entries_[slot].live = true;
    stats_.occupancy = used_;
    ++stats_.evaluations;
    must_fill = true;
    return slot;
}

Span<Transform> PoseCache::pose(u32 slot) noexcept {
    if (slot >= capacity_) {
        return {};
    }
    return {poses_.data() + (static_cast<usize>(slot) * joints_), joints_};
}

Span<const Transform> PoseCache::pose(u32 slot) const noexcept {
    if (slot >= capacity_) {
        return {};
    }
    return {poses_.data() + (static_cast<usize>(slot) * joints_), joints_};
}

void PoseCache::begin_frame() noexcept {
    for (usize index = 0; index < used_; ++index) {
        entries_[index].live = false;
    }
    used_ = 0;
    stats_.occupancy = 0;
}

void apply_variation(Span<const Transform> shared, const PoseVariation& variation,
                     Span<Transform> out) noexcept {
    const usize count = shared.size() < out.size() ? shared.size() : out.size();
    for (usize index = 0; index < count; ++index) {
        out[index] = shared[index];
        if (variation.mask.test(static_cast<u32>(index))) {
            out[index].rotation = normalize(variation.aim * out[index].rotation);
        }
        if (variation.uniform_scale != 1.0F) {
            out[index].scale = out[index].scale * variation.uniform_scale;
        }
    }
}

}  // namespace cy::animation
