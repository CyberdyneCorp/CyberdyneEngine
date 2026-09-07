#pragma once
// Decals: projected oriented boxes, resident in the GPU scene, budgeted by the arbiter. Task 10.3.
//
// `rendering-lighting-and-shadows` — "Decals".
//
// ================================================================================================
// A DECAL IS AN INSTANCE, NOT A DRAW CALL, AND THAT IS THE REQUIREMENT
// ================================================================================================
//
// "Decals SHALL be **GPU scene residents**, not per-decal draw calls: a decal is an instance with a
// transform, bounds, a material reference, and sort order, culled and gathered on the GPU like any
// other instance, and applied through tile or cluster assignment."
//
// So `DecalInstance` carries exactly those four things and no command buffer, and everything in
// this file operates on ARRAYS of them. There is no `draw(decal)` and there is nowhere to put one.
// The scenario — "tens of thousands of impact decals... without a CPU draw call each" — is a
// property of that shape rather than of a number.
//
// ================================================================================================
// THE BUDGET IS THE ARBITER'S, AND THE EVICTION IS DETERMINISTIC
// ================================================================================================
//
// "with a **decal budget** governed by the renderer budget arbiter and eviction of the least
// important decals by age, screen coverage, and importance when the budget is reached", and
// "WHEN the decal budget is reached and a new decal is spawned THEN the least important existing
// decal SHALL be evicted deterministically, and the eviction SHALL be reportable".
//
// `DecalBudget` holds a capacity and evicts by a score over those three quantities. It does NOT
// measure frame time and there is no setter through which one could arrive: the capacity comes
// from the arbiter, which is the only thing that may look at a frame. That is the same division
// `src/rendering/arbiter/subsystem.h` draws and for the same reason.
//
// "Deterministically" is a property of the SCORE, not of the container: every term is a number the
// decal carries, ties break on the decal's own identifier, and the identifier is assigned in spawn
// order. Two runs of one session evict the same decal.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/lighting/channels.h>

namespace cy::rendering {

/// What a decal writes into. Per-channel weights, so a scorch mark can darken albedo and roughen
/// the surface without claiming to know its metallic value.
struct DecalBlendWeights {
    f32 albedo = 1.0F;
    f32 normal = 1.0F;
    f32 roughness = 1.0F;
    f32 metallic = 0.0F;
    f32 emission = 0.0F;
};

/// One decal in the GPU scene.
struct DecalInstance {
    /// Assigned by `DecalBudget::spawn`, monotonic within a session. The eviction tie-break, and
    /// the reason eviction is reproducible.
    u64 id = 0;

    /// The projector's oriented box: a centre, a half-extent, and the frame it projects along.
    /// `axis_z` is the projection direction — a decal projects along -Z of its own frame onto
    /// whatever the box contains.
    Vec3 center{0.0F, 0.0F, 0.0F};
    Vec3 half_extent{0.5F, 0.5F, 0.5F};
    Vec3 axis_x{1.0F, 0.0F, 0.0F};
    Vec3 axis_y{0.0F, 1.0F, 0.0F};
    Vec3 axis_z{0.0F, 0.0F, 1.0F};

    /// The material this decal applies. An index into the GPU material table — the same handle an
    /// ordinary instance carries, which is what "like any other instance" means concretely.
    u32 material_index = 0;

    /// Application order. A later decal covers an earlier one; equal orders break on `id`, so
    /// "WHEN decals overlap THEN they SHALL be applied in sort order" is total rather than partial.
    i32 sort_order = 0;

    /// Which receivers this decal may be applied to. The same mechanism lights use, so a
    /// character-only decal costs a bitfield test at assignment.
    ChannelMask channels = kAllChannels;

    /// Beyond this angle between the receiver's normal and the decal's projection axis, the decal
    /// fades out rather than stretching. Radians; pi/2 would be no fade at all.
    f32 fade_angle_radians = 1.0472F;  // 60 degrees
    /// Distance fade, in metres: full strength within `fade_start`, gone by `fade_end`.
    f32 fade_start = 20.0F;
    f32 fade_end = 40.0F;

    /// How much of the receiver's own normal detail survives. 1 replaces it, 0 keeps it entirely;
    /// the blend in between is the "normal blending that preserves the receiver's detail" the
    /// requirement names, and `blend_decal_normal` is where it happens.
    f32 normal_strength = 1.0F;

    DecalBlendWeights weights;

    /// What the eviction score is built from. `importance` is authored — persistent damage matters
    /// more than a splatter; `spawn_time_seconds` and `screen_coverage` are measured.
    f32 importance = 0.5F;
    f64 spawn_time_seconds = 0.0;
    f32 screen_coverage = 0.0F;
};

/// The score a decal is kept by. HIGHER survives. Every term is a quantity the decal carries, so
/// two runs of one session score identically.
///
/// `now_seconds` is the frame's time. Age is a decay rather than a subtraction so that a decal
/// spawned an hour ago and one spawned two hours ago are not distinguished by a number that has
/// grown past f32's useful precision.
[[nodiscard]] f32 decal_retention_score(const DecalInstance& decal, f64 now_seconds,
                                        f32 age_half_life_seconds) noexcept;

/// The angle fade: 1 facing the projector, 0 past the fade angle. `receiver_normal` and the
/// decal's `axis_z` are both world space and both unit length.
///
/// "WHEN the receiving normal deviates beyond the decal's fade angle THEN the decal SHALL fade out
/// rather than stretch." The stretching is what happens when a decal is applied to a surface nearly
/// parallel to its projection axis: one texel covers an unbounded length of surface. Fading is the
/// answer because there is no projection that does not stretch there.
[[nodiscard]] f32 decal_angle_fade(const DecalInstance& decal, Vec3 receiver_normal) noexcept;

/// The distance fade: 1 within `fade_start`, 0 beyond `fade_end`.
[[nodiscard]] f32 decal_distance_fade(const DecalInstance& decal, f32 distance_metres) noexcept;

/// The decal's texture coordinate for a world-space point, and whether the point is inside the
/// projector's box. Outside is not an error — it is most of the world.
struct DecalProjection {
    Vec2 uv{0.0F, 0.0F};
    /// Depth through the box, in [0, 1] from the near face. What a soft-edged decal fades on.
    f32 depth = 0.0F;
    bool inside = false;
};

[[nodiscard]] DecalProjection project_decal(const DecalInstance& decal,
                                            Vec3 world_position) noexcept;

/// Blend a decal's normal over the receiver's, preserving the receiver's detail.
///
/// Reoriented normal mapping rather than a lerp: a lerp of two unit normals is not a unit normal
/// and, worse, it flattens the receiver wherever the decal is strong — which is exactly the detail
/// the requirement says to preserve. Both are world space; the result is unit length.
[[nodiscard]] Vec3 blend_decal_normal(Vec3 receiver_normal, Vec3 decal_normal,
                                      f32 strength) noexcept;

/// Why a decal left the set.
enum class DecalEvictionCause : u8 {
    /// The budget was reached and this was the least worth keeping.
    BudgetReached = 0,
    /// Explicitly removed.
    Removed,
    Count,
};

[[nodiscard]] const char* decal_eviction_cause_name(DecalEvictionCause cause) noexcept;

/// One eviction, reportable. "the eviction SHALL be reportable" is a requirement about the
/// DIAGNOSTIC, so the record carries why and what, not just that something happened.
struct DecalEviction {
    u64 id = 0;
    DecalEvictionCause cause = DecalEvictionCause::BudgetReached;
    f32 score = 0.0F;
    f32 age_seconds = 0.0F;
    f32 importance = 0.0F;
    f32 screen_coverage = 0.0F;
};

/// The decal set and its budget.
///
/// NOT thread-safe and not internally threaded: spawning is the game thread's and the reads are the
/// extract stage's, which is the same division the rest of this layer draws.
class DecalBudget {
public:
    explicit DecalBudget(Allocator& allocator = current_allocator()) noexcept
        : decals_(allocator), evictions_(allocator) {}

    /// The capacity, in decals. **Set by the renderer budget arbiter**, which is the only thing
    /// that may measure a frame. There is deliberately no `report_frame_ms` here.
    void set_capacity(u32 capacity) noexcept;
    [[nodiscard]] u32 capacity() const noexcept { return capacity_; }

    /// How quickly a decal's age tells against it. A long half life keeps persistent damage; a
    /// short one lets a splatter go first.
    void set_age_half_life(f32 seconds) noexcept;

    /// Add a decal. When the set is full, the least worth keeping is evicted first — including,
    /// possibly, the decal being spawned, which is the honest answer when a splatter arrives into a
    /// wall of persistent damage. Returns the assigned id, or zero when the new decal was the one
    /// evicted.
    [[nodiscard]] Expected<u64, Error> spawn(const DecalInstance& decal, f64 now_seconds) noexcept;

    /// Remove one by id. Returns whether it was there.
    bool remove(u64 id) noexcept;

    [[nodiscard]] Span<const DecalInstance> decals() const noexcept { return decals_.span(); }
    [[nodiscard]] usize size() const noexcept { return decals_.size(); }

    /// Update a decal's measured screen coverage, which the extract stage has and the spawner does
    /// not. Silently ignores an unknown id: a decal evicted between the measurement and the report
    /// is the ordinary case, not an error.
    void report_screen_coverage(u64 id, f32 coverage) noexcept;

    /// The evictions since `clear_evictions()`. The report the requirement asks for.
    [[nodiscard]] Span<const DecalEviction> evictions() const noexcept { return evictions_.span(); }
    void clear_evictions() noexcept { evictions_.clear(); }

    /// Order the set for application: ascending `sort_order`, ties on `id`. Writes indices into
    /// `out`, which must be at least `size()` long. Returns how many were written.
    [[nodiscard]] u32 application_order(Span<u32> out) const noexcept;

private:
    [[nodiscard]] usize weakest(f64 now_seconds) const noexcept;
    Status record(const DecalInstance& decal, f64 now_seconds, DecalEvictionCause cause) noexcept;

    Array<DecalInstance> decals_;
    Array<DecalEviction> evictions_;
    u32 capacity_ = 4096;
    f32 age_half_life_ = 120.0F;
    u64 next_id_ = 1;
};

}  // namespace cy::rendering
