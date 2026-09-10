#pragma once
// Animation level of detail, and the pose cache a reduced tier shares poses through. M8.b tasks 5.2
// and 5.3.
//
// ================================================================================================
// A TIER IS A FUNCTION OF SIMULATION STATE, NOT OF MEASURED FRAME TIME
// ================================================================================================
//
// `animation-and-skinning` — "Animation determinism": "Animation LOD tier SHALL be a function of
// simulation state, not of measured frame time, for instances whose root motion is authoritative —
// mirroring the AI system's rule."
//
// `select_tier()` therefore takes distance, screen coverage, visibility and importance, and nothing
// else. There is no clock in this header and no frame-time argument in that signature, so a caller
// cannot pass one without changing this file. `LodPolicy::authoritative` is the declaration an
// instance makes about itself, and an authoritative instance is refused a tier below `Simplified`
// on the same reasoning: its pose feeds nothing authoritative, but its EVENTS and its cost model
// do, and a cached pose is chosen by a bucket a non-deterministic input could otherwise reach.
//
// HYSTERESIS IS IN THE POLICY, NOT IN THE CALLER. "Tier transitions SHALL be hysteretic and blended
// so a promoted instance does not visibly snap." The thresholds are one-way with a margin, and
// `select_tier()` is handed the current tier so the margin has something to hold against.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>

#include <cy/animation/skeleton.h>

namespace cy::animation {

/// `animation-and-skinning`'s own table, in its own order.
enum class LodTier : u8 {
    /// Full graph, all layers, IK and control rig; the full bone set.
    Full = 0,
    /// Reduced graph and a reduced bone set. No IK, no control rig.
    Simplified,
    /// Sampled from the shared pose cache; no per-instance graph evaluation.
    Cached,
    /// A pose texture or vertex animation. No skeleton evaluation at all.
    Baked,
};

[[nodiscard]] const char* lod_tier_name(LodTier tier) noexcept;

/// The bone level of detail a tier evaluates at. `Baked` has none and answers the coarsest.
[[nodiscard]] u8 bone_lod_for(LodTier tier) noexcept;

/// Whether a tier evaluates its own pose at all.
[[nodiscard]] bool evaluates_pose(LodTier tier) noexcept;

/// Whether a tier may take a pose from the shared cache. "Pose sharing SHALL be tier-gated:
/// disabled at `Full`, available at reduced tiers."
[[nodiscard]] bool may_share_pose(LodTier tier) noexcept;

/// Whether IK, constraints and the control rig run. "WHEN an instance is at `Simplified` tier or
/// lower THEN IK and secondary motion SHALL be skipped."
[[nodiscard]] bool evaluates_modifiers(LodTier tier) noexcept;

/// The evaluation rate a tier targets, in hertz. Zero means every frame.
[[nodiscard]] f32 evaluation_hertz(LodTier tier) noexcept;

/// What decides a tier. Distances are in world units; coverage is the fraction of the screen's
/// height the instance's bounds occupy.
struct LodPolicy {
    f32 full_distance = 15.0F;
    f32 simplified_distance = 40.0F;
    f32 cached_distance = 120.0F;
    /// The margin a promotion must beat, as a fraction of the threshold. "so a promoted instance
    /// does not visibly snap" — the demotion boundary sits further out than the promotion one, so
    /// an instance hovering at a threshold does not oscillate.
    f32 hysteresis = 0.1F;
    /// Coverage at or above which an instance is `Full` whatever its distance.
    f32 full_coverage = 0.25F;
    /// An instance whose root motion is authoritative is never taken below `Simplified`.
    bool authoritative = false;
};

/// What one instance's tier is decided from. Simulation state only: no frame time, no measured
/// cost.
struct LodInputs {
    f32 distance = 0.0F;
    f32 coverage = 0.0F;
    bool visible = true;
    bool important = false;
    /// A tier the author pinned. `LodTier::Full` with `pinned` false is "not pinned".
    bool pinned = false;
    LodTier pinned_tier = LodTier::Full;
};

/// The tier for these inputs, given the tier the instance is at now.
[[nodiscard]] LodTier select_tier(const LodPolicy& policy, const LodInputs& inputs,
                                  LodTier current) noexcept;

/// How the tier distribution looks across a population. "tier distribution and per-batch cost SHALL
/// identify which instances and which programs are responsible."
struct LodDistribution {
    u32 counts[4] = {0, 0, 0, 0};

    void note(LodTier tier) noexcept;
    [[nodiscard]] u32 total() const noexcept;
};

// --- Pose sharing --------------------------------------------------------------------------------

/// What a shared pose is keyed on. "keyed on clip, quantised phase, and skeleton LOD."
struct PoseCacheKey {
    u16 clip = 0;
    u16 phase_bucket = 0;
    u8 bone_lod = 0;

    friend bool operator==(const PoseCacheKey& a, const PoseCacheKey& b) noexcept {
        return a.clip == b.clip && a.phase_bucket == b.phase_bucket && a.bone_lod == b.bone_lod;
    }
};

struct PoseCacheStats {
    u32 lookups = 0;
    u32 hits = 0;
    u32 evaluations = 0;
    u32 evictions = 0;
    /// How many of the cache's slots are in use.
    u32 occupancy = 0;
};

/// The shared pose cache.
///
/// "Instances evaluating the same clip at a similar phase SHALL be able to share an evaluated pose
/// ... A shared pose SHALL be evaluated once per frame and sampled by many instances."
///
/// `acquire()` answers with a slot and whether it must be filled. A caller that is told to fill it
/// evaluates once; every other caller with the same key that frame is told it is already filled.
/// The cache holds poses, not instances: it never knows how many characters read one.
class PoseCache {
public:
    PoseCache(Allocator& allocator, u32 joints, u32 capacity) noexcept;

    /// The phase bucket a normalised phase falls in. "Phase quantisation SHALL be a configurable
    /// quality setting" — `buckets` is that setting.
    [[nodiscard]] static u16 bucket_of(f32 phase, u16 buckets) noexcept;

    /// Find or reserve the slot for `key`. `must_fill` is true when the caller is the one that has
    /// to evaluate the pose into `pose(slot)`.
    [[nodiscard]] Expected<u32, Error> acquire(const PoseCacheKey& key, bool& must_fill) noexcept;

    [[nodiscard]] Span<Transform> pose(u32 slot) noexcept;
    [[nodiscard]] Span<const Transform> pose(u32 slot) const noexcept;

    /// Drop every entry. Called once a frame: a shared pose is evaluated once PER FRAME, and an
    /// entry that outlived the frame would hold a phase that has moved on.
    void begin_frame() noexcept;

    [[nodiscard]] const PoseCacheStats& stats() const noexcept { return stats_; }
    [[nodiscard]] u32 capacity() const noexcept { return capacity_; }
    [[nodiscard]] u32 joints() const noexcept { return joints_; }

private:
    struct Entry {
        PoseCacheKey key;
        bool live = false;
    };

    Array<Entry> entries_;
    Array<Transform> poses_;
    PoseCacheStats stats_;
    u32 joints_ = 0;
    u32 capacity_ = 0;
    u32 used_ = 0;
};

/// The cheap per-instance variation a shared pose is allowed. "MAY then apply cheap per-instance
/// variation: phase offset, additive upper-body override, aim offset, and per-instance scaling."
struct PoseVariation {
    /// Applied to the joints in `mask` as a post-rotation. An aim offset is one of these.
    Quat aim = Quat::identity();
    JointMask mask;
    f32 uniform_scale = 1.0F;
};

/// Copy a shared pose into `out`, applying the variation. The shared pose is never modified.
void apply_variation(Span<const Transform> shared, const PoseVariation& variation,
                     Span<Transform> out) noexcept;

}  // namespace cy::animation
