#ifndef CY_CAMERA_ASSIST_H
#define CY_CAMERA_ASSIST_H
// Aim assistance, and the director camera. M8.b task 7.3.
//
// --- ASSISTANCE IS A MODIFIER BETWEEN INPUT AND INTENT -------------------------------------------
//
// `camera-system`: "Aim assistance — magnetism, slowdown near targets, and snapping — SHALL be
// implemented as modifiers between input and camera intent, informed by gameplay spatial queries.
// Assistance SHALL be configurable and shall be part of the accessibility surface, not a hidden
// constant. Assistance SHALL NOT be implemented in device drivers or platform input handling, and
// SHALL be observable in diagnostics so its effect can be measured."
//
// Four things follow, and each is visible in the signature below: `apply_aim_assist()` takes a raw
// look and returns a modified one — it never touches a camera; the candidates are passed IN,
// because the spatial query is gameplay's and a camera module that issued one would be deciding a
// gameplay policy; every constant is in `AimAssistSettings`, which a settings screen owns; and the
// report is an out-parameter rather than an optional, so the measurement exists whether or not
// anybody is looking at it.
//
// --- THE DIRECTOR SCORES, IT DOES NOT CUT --------------------------------------------------------
//
// "an optional director camera that scores candidate viewpoints and selects or blends among them
// ... Director scoring SHALL consider visibility of the action, target importance, activity, and
// shot repetition, and SHALL be replaceable per project", and its scenario asks for a BLEND: "the
// selection SHALL blend rather than cut unnecessarily".
//
// So `score_shots()` returns an ordering and `DirectorState::select()` applies hysteresis and a
// minimum shot duration before changing its mind. The repetition term is what stops a director
// oscillating between two equally good shots, and it is a decaying memory per shot rather than a
// blacklist — a shot used a minute ago should be available again.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>

namespace cy::camera {

/// A candidate the gameplay spatial query offered. Never produced here.
struct AimCandidate {
    /// Unit direction from the player toward the candidate, in the same space as the view.
    Vec3 direction{0.0F, 0.0F, -1.0F};
    f32 distance = 0.0F;
    /// How much the project wants this candidate helped. A weight, not a boolean, so a low-priority
    /// target near the reticle does not steal assistance from the one behind it.
    f32 weight = 1.0F;
};

/// Every constant assistance has. Part of the accessibility surface, and therefore data.
struct AimAssistSettings {
    /// How strongly the look is bent toward a candidate, per second.
    f32 magnetism = 0.0F;
    /// Look scaling while a candidate is within `slowdown_radians`, in (0, 1].
    f32 slowdown = 1.0F;
    f32 slowdown_radians = 0.15F;
    /// The angular window a candidate must be inside to be helped at all.
    f32 capture_radians = 0.25F;
    /// Snap the look to rest when the candidate is very close to the reticle.
    bool snap = false;
};

/// What assistance did. "observable in diagnostics so its effect can be measured".
struct AimAssistReport {
    static constexpr u32 kNoCandidate = 0xFFFFFFFFU;

    Vec2 raw_look;
    Vec2 assisted_look;
    u32 candidate = kNoCandidate;
    f32 candidate_angle = 0.0F;
    bool active = false;
};

/// Bend a look toward the best candidate. Input in, input out; no camera is reachable from here.
[[nodiscard]] Vec2 apply_aim_assist(const AimAssistSettings& settings, Vec2 raw_look,
                                    Vec3 view_direction, Span<const AimCandidate> candidates,
                                    f32 dt, AimAssistReport& report) noexcept;

// --- The director
// ----------------------------------------------------------------------------------

/// One viewpoint the director may choose.
struct ShotCandidate {
    Name name;
    /// [0, 1]: how much of the action this viewpoint can see. Supplied by the caller from the same
    /// batched occlusion queries the rigs use — a director that cast its own rays would be the
    /// scattered-cast pattern the specification forbids, wearing a different hat.
    f32 visibility = 1.0F;
    /// [0, 1]: how important what it frames is.
    f32 importance = 0.5F;
    /// [0, 1]: how much is happening in it.
    f32 activity = 0.5F;
};

/// A scored shot, in the order the director prefers them.
struct ShotScore {
    Name name;
    f32 score = 0.0F;
    /// The repetition penalty applied, so a developer can see WHY a better-looking shot lost.
    f32 repetition_penalty = 0.0F;
};

/// The weights a project may replace. "SHALL be replaceable per project" — as data, because a
/// virtual scorer would make the director an object to subclass, which is what the specification's
/// first requirement forbids for cameras generally.
struct DirectorWeights {
    f32 visibility = 0.5F;
    f32 importance = 0.3F;
    f32 activity = 0.2F;
    /// How much a recently used shot is penalised, at the moment it is released.
    f32 repetition = 0.4F;
    /// Seconds over which that penalty decays to nothing.
    f32 repetition_half_life = 20.0F;
    /// A new shot must beat the current one by this much before the director changes its mind.
    f32 hysteresis = 0.08F;
    /// And the current shot is held at least this long, whatever the scores say.
    f32 minimum_shot_seconds = 2.0F;
};

/// The director's memory. Small and fixed: a director that grew an array per candidate would
/// allocate in the frame it is supposed to be cheap in.
class DirectorState {
public:
    static constexpr usize kRemembered = 8;

    explicit DirectorState(Allocator& allocator) noexcept;

    /// Score every candidate, newest scores in `out`, best first.
    [[nodiscard]] Status score_shots(Span<const ShotCandidate> candidates,
                                     const DirectorWeights& weights,
                                     Array<ShotScore>& out) const noexcept;

    /// Advance the shot clock and the repetition memory.
    void advance(f32 dt) noexcept;

    /// Choose. Returns the shot in effect after the decision, which may be the one already running:
    /// hysteresis and the minimum duration are applied here rather than by the caller, so two
    /// callers cannot apply them differently.
    [[nodiscard]] Name select(Span<const ShotScore> scores,
                              const DirectorWeights& weights) noexcept;

    [[nodiscard]] Name current() const noexcept { return current_; }
    [[nodiscard]] f32 shot_seconds() const noexcept { return shot_seconds_; }
    /// True when the last `select()` changed the shot — which is the caller's cue to BLEND, not to
    /// cut. "the selection SHALL blend rather than cut unnecessarily."
    [[nodiscard]] bool changed() const noexcept { return changed_; }

private:
    struct Memory {
        Name shot;
        f32 penalty = 0.0F;
    };

    [[nodiscard]] f32 penalty_of(Name shot) const noexcept;
    void remember(Name shot, f32 penalty) noexcept;

    Memory memory_[kRemembered];
    Name current_;
    f32 shot_seconds_ = 0.0F;
    f32 decay_half_life_ = 20.0F;
    bool changed_ = false;
};

}  // namespace cy::camera

#endif  // CY_CAMERA_ASSIST_H
