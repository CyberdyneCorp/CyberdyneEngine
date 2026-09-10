#ifndef CY_AUDIO_TIERS_H
#define CY_AUDIO_TIERS_H
// Importance, simulation tiers, and voice virtualisation. M8.b task 10.2.
//
// `audio`: "Every playing source SHALL be scored each frame and assigned a simulation tier, so that
// the cost of audio is bounded by CONFIGURATION rather than by content." That sentence is the whole
// design: the tier budgets are numbers a project sets, and eight thousand sources cost what the
// budgets say rather than what the level designer placed.
//
// --- THE FOUR TIERS, AND WHAT EACH COSTS ---------------------------------------------------------
//
//   FullAcoustic  propagation, reflections, geometry occlusion, HRTF
//   Spatialised   panning, distance attenuation, filter-based occlusion, a reverb send
//   Simple        stereo or mono mixing with distance attenuation
//   Virtual       the playback position advances and nothing is mixed
//
// --- HYSTERESIS IS NOT AN OPTIMISATION -----------------------------------------------------------
//
// "WHEN a source hovers at a tier boundary THEN hysteresis SHALL prevent per-frame tier flapping,
// and any change SHALL be cross-faded." A source that changes tier every frame is audible as a
// flutter, so a promotion needs to beat the threshold by a margin and a demotion needs to fall
// below it by one — and `TierState::seconds_in_tier` holds a source in place for a minimum time as
// well.
//
// --- VIRTUAL IS NOT STOPPED ----------------------------------------------------------------------
//
// "A source that is inaudible or beyond budget SHALL be virtualised rather than stopped: its
// playback position SHALL continue to advance, but no mixing or DSP SHALL be performed for it." So
// `advance_virtual` moves the position and does nothing else, and a source that becomes audible
// again resumes where it would have been — "WHEN the listener leaves and later re-enters a looping
// ambience's range THEN it SHALL resume at the position it would have reached, not restart."

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>

namespace cy::audio {

/// The four tiers, most expensive first. The order is the demotion order.
enum class SimulationTier : u8 {
    FullAcoustic = 0,
    Spatialised = 1,
    Simple = 2,
    Virtual = 3,
    Count = 4,
};

[[nodiscard]] const char* simulation_tier_name(SimulationTier tier) noexcept;

/// What a source presents to the scorer. Every field is one the requirement names: "distance to the
/// listener, emitter volume and priority, listener orientation, sound category, gameplay importance
/// flags, current occlusion estimate, and the source's tier in the previous frame."
struct SourceScoring {
    u64 source = 0;
    Vec3 position;
    /// The source's own gain, before distance.
    f32 volume = 1.0F;
    /// The designer's priority. Multiplies the score.
    f32 priority = 1.0F;
    /// A category weight — dialogue above ambience — supplied by the project's category table.
    f32 category_weight = 1.0F;
    /// [0, 1], the current estimate. A fully occluded source matters less.
    f32 occlusion = 0.0F;
    /// Gameplay's own flag: a source the game says matters, whatever the arithmetic says.
    bool gameplay_important = false;
    /// The floor a source may not be demoted below. "Gameplay SHALL be able to pin a source to a
    /// minimum tier, so narratively critical audio is not demoted."
    SimulationTier minimum_tier = SimulationTier::Virtual;
    /// Its tier last frame, which hysteresis reads.
    SimulationTier previous_tier = SimulationTier::Virtual;
    /// How long it has been in that tier.
    f32 seconds_in_tier = 0.0F;
    /// False until this source has been assigned once. HYSTERESIS DOES NOT APPLY TO A SOURCE THAT
    /// HAS NEVER BEEN ASSIGNED: a sound that has just started takes the tier its importance earns,
    /// and holding it at `Virtual` for a quarter of a second because it "changed tier" would make
    /// every new sound late. `assign_tiers` sets it.
    bool assigned = false;
};

/// The listener the scoring is against.
struct ScoringListener {
    Vec3 position;
    Vec3 forward{0.0F, 0.0F, -1.0F};
    /// Beyond this a source scores zero however loud it is.
    f32 audible_distance = 150.0F;
};

/// The project's tier budgets. "Each tier SHALL have a configurable budget as a maximum source
/// count; sources beyond a tier's budget SHALL be demoted to the next tier by importance rank."
struct TierBudgets {
    u32 full_acoustic = 8;
    u32 spatialised = 64;
    u32 simple = 256;
    /// How much better a source must score to be PROMOTED, as a fraction. Zero would flap.
    f32 promotion_margin = 0.15F;
    /// Seconds a source must spend in a tier before it may change again.
    f32 minimum_dwell = 0.25F;
    /// Seconds a tier change cross-fades over. "any change SHALL be cross-faded."
    f32 crossfade_seconds = 0.08F;
};

/// One source's assignment.
struct TierAssignment {
    u64 source = 0;
    SimulationTier tier = SimulationTier::Virtual;
    f32 score = 0.0F;
    /// True when the tier changed this frame, so the caller cross-fades.
    bool changed = false;
    /// True when the source was held by hysteresis rather than by its score — the diagnostic that
    /// distinguishes "stable" from "stuck".
    bool held = false;
};

struct TierReport {
    u32 scored = 0;
    u32 counts[static_cast<usize>(SimulationTier::Count)] = {};
    u32 promotions = 0;
    u32 demotions = 0;
    u32 held_by_hysteresis = 0;
    u32 pinned = 0;
};

/// A source's importance, in [0, 1].
///
/// Distance dominates, orientation and category weight it, occlusion reduces it, and a gameplay
/// flag lifts it — the requirement's own list, with the arithmetic in one place so a project tuning
/// it has one function to read.
[[nodiscard]] f32 score_source(const SourceScoring& source,
                               const ScoringListener& listener) noexcept;

/// Score and assign tiers for a frame.
///
/// `sources` is updated in place: each entry's `previous_tier` and `seconds_in_tier` are advanced,
/// so the caller keeps one array and this function maintains its own hysteresis state.
[[nodiscard]] Status assign_tiers(Span<SourceScoring> sources, const ScoringListener& listener,
                                  const TierBudgets& budgets, f32 dt, Array<TierAssignment>& out,
                                  TierReport& report) noexcept;

// --- Virtualisation
// ---------------------------------------------------------------------------------

/// A virtualised voice: a position that keeps moving and nothing else.
struct VirtualVoice {
    u64 source = 0;
    /// Seconds into the clip.
    f64 position = 0.0;
    /// The clip's length in seconds. Zero means "not looping and unknown", which advances forever.
    f64 length = 0.0;
    bool looping = false;
    f32 pitch = 1.0F;
    /// Set when gameplay stopped it. "Sources explicitly stopped by gameplay SHALL be stopped, not
    /// virtualised" — so a stopped voice is not in this list at all, and this flag is how a caller
    /// that reuses the array marks one.
    bool stopped = false;
};

/// Advance a virtualised voice. THE WHOLE COST OF A VIRTUAL SOURCE, and it is deliberately this
/// small: "their per-frame cost SHALL be limited to advancing a position and scoring importance."
void advance_virtual(VirtualVoice& voice, f32 dt) noexcept;

/// What a voice resuming from virtual needs.
struct ResumePlan {
    /// Where to start playing, in seconds.
    f64 position = 0.0;
    /// Seconds to fade in over. "fading in over a short configurable time."
    f32 fade_in = 0.05F;
    /// False when the voice had stopped rather than been virtualised.
    bool resumable = false;
};

/// The plan for bringing a virtualised voice back.
[[nodiscard]] ResumePlan plan_resume(const VirtualVoice& voice, f32 fade_in_seconds) noexcept;

}  // namespace cy::audio

#endif  // CY_AUDIO_TIERS_H
