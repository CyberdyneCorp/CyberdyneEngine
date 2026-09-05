#pragma once
// The camera stack: priorities, weights, and the blends between contributions. Task 4.3.2.
//
// `camera-system` — "Camera stack and blending": each local player "SHALL own a **camera stack** of
// contributions with priorities and weights: a base gameplay camera, additive modifiers, effects,
// volume influence, cinematic override, and debug override"; transitions "SHALL be **blends** with
// a declared duration, curve, and per-channel policy for position, rotation, and lens"; and "The
// stack SHALL be inspectable: each contribution's weight and its effect SHALL be reportable."
//
// --- WHY THE STACK IS A SEPARATE OBJECT FROM THE RIG --------------------------------------------
//
// Because a cinematic taking over is not a change to the gameplay camera. `camera-system`'s
// scenario is "a cinematic camera takes priority and later releases", and the requirement it lands
// under says the gameplay camera state "SHALL be preserved for the return". If the cinematic wrote
// into the gameplay rig there would be nothing to return to. So a stack holds entries, each naming
// a rig; every rig keeps evaluating; and the stack blends their evaluated results.
//
// --- THE REPORT IS NOT A DEBUGGING EXTRA --------------------------------------------------------
//
// "**WHEN** the camera is not where a developer expects **THEN** the stack SHALL show each
// contribution and its weight." `blend()` fills a `StackContribution` per entry with the weight it
// actually had and the distance and angle it moved the result. That is a requirement, so it is a
// parameter of the blend rather than something a debug build recomputes — a second computation is a
// second thing that can disagree with the first.
//
// --- ORDER IS DETERMINISTIC, AND NOT BY ACCIDENT ------------------------------------------------
//
// Entries are kept sorted by `(priority, sequence)`, where `sequence` is a monotonic counter
// assigned at push. Two entries at the same priority therefore resolve in the order they were
// pushed and never in the order a container happened to store them. `simulation-and-determinism`
// requires that of anything ordered; a camera is not authoritative state, but a camera whose blend
// order changed between two runs would make a replay's *presentation* differ for no visible reason.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/servers/camera/camera.h>
#include <cy/servers/camera/handles.h>

namespace cy::camera {

/// What a contribution is. Not a switch the blend branches on — every kind blends the same way,
/// except `Additive`, which is the one genuine difference — but the attribution the report needs
/// and the vocabulary `camera-system` enumerates.
enum class ContributionKind : u8 {
    Base = 0,
    /// Added over whatever is below rather than blended toward. Recoil and hand-held sway.
    Additive,
    Effect,
    Volume,
    Cinematic,
    Debug,
    Count,
};

[[nodiscard]] const char* contribution_kind_name(ContributionKind kind) noexcept;

enum class BlendCurve : u8 {
    Linear = 0,
    EaseIn,
    EaseOut,
    EaseInOut,
    /// No blend: the contribution is at full weight from the first frame. A cut, spelled as a
    /// curve, so that "this transition is a cut" is authored in the same field as every other
    /// transition rather than as a special case somewhere else.
    Step,
    Count,
};

[[nodiscard]] const char* blend_curve_name(BlendCurve curve) noexcept;

/// Evaluate a curve at `t` in [0, 1].
[[nodiscard]] f32 apply_curve(BlendCurve curve, f32 t) noexcept;

/// A transition's declared shape. Per channel, because a cinematic that should take the lens
/// immediately and the position gradually is ordinary.
struct BlendPolicy {
    f32 duration_seconds = 0.5F;
    BlendCurve curve = BlendCurve::EaseInOut;
    bool position = true;
    bool rotation = true;
    bool lens = true;
};

/// One contribution to a player's camera.
struct StackEntry {
    RigHandle rig;
    ContributionKind kind = ContributionKind::Base;
    /// Higher wins. A cinematic sits above gameplay; a debug override sits above everything.
    i32 priority = 0;
    /// What this contribution blends toward, in [0, 1]. Usually one; a volume at partial influence
    /// is the case it exists for.
    f32 target_weight = 1.0F;
    BlendPolicy blend_in;
    BlendPolicy blend_out;
};

/// An entry's identity within a stack. Not a handle: it is a position in one player's stack, it
/// never outlives the stack, and giving it a generational handle would suggest otherwise.
using StackEntryId = u32;
inline constexpr StackEntryId kInvalidStackEntry = 0;

/// What one entry did to the result. Filled by `blend()`; see the header comment.
struct StackContribution {
    StackEntryId id = kInvalidStackEntry;
    RigHandle rig;
    ContributionKind kind = ContributionKind::Base;
    i32 priority = 0;
    /// The weight the blend actually used, after the curve and the elapsed time.
    f32 weight = 0.0F;
    /// How far this entry moved the accumulated result, in metres and radians.
    f32 position_delta = 0.0F;
    f32 rotation_delta_radians = 0.0F;
    f32 fov_delta_radians = 0.0F;
};

/// One local player's camera stack.
///
/// NOT a singleton and not global: `camera-system` forbids "Minimap, reflection, split-screen, and
/// cinematic views forced through one global camera singleton", and four local players own four of
/// these.
class CameraStack {
public:
    explicit CameraStack(Allocator& allocator) noexcept : entries_(allocator) {}

    /// Push a contribution. Returns its id.
    [[nodiscard]] Expected<StackEntryId, Error> push(const StackEntry& entry) noexcept;

    /// Begin releasing a contribution: it blends out over `blend_out` and is removed when it
    /// reaches zero. Releasing rather than removing is what makes "a cinematic takes over and
    /// returns" a blend in both directions.
    [[nodiscard]] Status release(StackEntryId id) noexcept;

    /// Remove immediately, with no blend out. For a rig that is being destroyed.
    [[nodiscard]] Status remove(StackEntryId id) noexcept;

    /// Advance every entry's blend by `delta_seconds` and drop the ones that finished blending out.
    void advance(f32 delta_seconds) noexcept;

    /// The entries, lowest priority first — the order `blend()` requires its input in.
    ///
    /// Indexed rather than handed out as a span: the runtime half of an entry (its elapsed blend,
    /// whether it is releasing) is not the authored half, and returning a span of the authored half
    /// alone would need a second array kept in step with this one.
    [[nodiscard]] usize size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] const StackEntry& entry_at(usize index) const noexcept;
    [[nodiscard]] StackEntryId id_at(usize index) const noexcept;
    /// The effective weight of the entry at `index`, after its curve.
    [[nodiscard]] f32 weight_at(usize index) const noexcept;

    /// Blend `evaluated` — one evaluated camera per entry, in `entries()` order — into `out`.
    ///
    /// `report`, when given, receives one `StackContribution` per entry. Fails when `evaluated`
    /// does not have exactly one entry per contribution, because a caller that lost track of which
    /// camera belongs to which entry would otherwise blend the wrong pose at the right weight and
    /// nothing would look wrong enough to notice.
    [[nodiscard]] Status blend(Span<const EvaluatedCamera> evaluated, EvaluatedCamera& out,
                               Array<StackContribution>* report) const noexcept;

private:
    struct Record {
        StackEntry entry;
        StackEntryId id = kInvalidStackEntry;
        /// Monotonic, so two entries at one priority keep their push order. See the header comment.
        u32 sequence = 0;
        f32 elapsed = 0.0F;
        bool releasing = false;
        /// The weight the entry had when `release()` was called, so a contribution released mid
        /// blend-in fades from where it was rather than from full.
        f32 release_from = 0.0F;
    };

    [[nodiscard]] const Record* find(StackEntryId id) const noexcept;
    [[nodiscard]] Record* find(StackEntryId id) noexcept;
    [[nodiscard]] static f32 effective_weight(const Record& record) noexcept;

    Array<Record> entries_;
    StackEntryId next_id_ = 1;
    u32 next_sequence_ = 0;
};

}  // namespace cy::camera
