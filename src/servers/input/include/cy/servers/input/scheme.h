#pragma once
// Control schemes and active-scheme detection. Task 4.1.5.
//
// `input-and-actions` — "Control schemes and device detection": the engine defines control schemes,
// each declaring required and optional devices; "The active scheme SHALL be detected from recent
// meaningful input, with **hysteresis and a significance threshold**, so that a noisy device or an
// incidental mouse movement does not flip prompts back and forth."
//
// --- THE TWO GUARDS ARE NOT THE SAME GUARD -------------------------------------------------------
//
// They are often written as one and then only one of the two failure modes is fixed.
//
//   * The **significance threshold** answers "was this input meaningful?" — a stick resting at 0.03
//     because the spring is tired, a mouse that moved one pixel because the desk was bumped. Below
//     the threshold the sample is not evidence of anything and is discarded before it is counted.
//   * The **hysteresis** answers "is this a change of intent?" — a player using a controller who
//     brushes the mouse once. One significant sample is not a switch; the detector requires the new
//     scheme to stay ahead for a dwell time before the prompts change.
//
// Drop the first and an idle controller keeps the prompts on gamepad forever. Drop the second and a
// single significant sample flips them mid-sentence. The requirement's two scenarios are exactly
// these two cases, and the detector is tested against both.

#include <cy/core/base/types.h>
#include <cy/core/values/name.h>
#include <cy/servers/input/types.h>

namespace cy::input {

/// One scheme, as declared. `required` and `optional` are masks over `DeviceKind`.
struct ControlScheme {
    Name name;
    SchemeKind kind = SchemeKind::KeyboardMouse;
    u16 required_devices = 0;
    u16 optional_devices = 0;

    [[nodiscard]] constexpr bool satisfied_by(u16 present_devices) const noexcept {
        return (present_devices & required_devices) == required_devices;
    }
};

[[nodiscard]] constexpr u16 device_mask(DeviceKind kind) noexcept {
    return static_cast<u16>(1U << static_cast<u16>(kind));
}

/// The four schemes the engine defines. A project adds its own with `SchemeKind::Project`.
[[nodiscard]] ControlScheme builtin_scheme(SchemeKind kind) noexcept;

/// Decides which scheme the player is currently using.
///
/// One instance per input user — the whole point of an input user is that two people at one machine
/// may be on different schemes, and a process-wide detector would make that impossible.
class SchemeDetector {
public:
    /// A sample below this magnitude is not evidence. Digital controls always report 1 and are
    /// always significant; the threshold exists for axes.
    void set_significance(f32 magnitude) noexcept { significance_ = magnitude; }
    /// How long a candidate scheme must lead before the active scheme changes.
    void set_hysteresis(f32 seconds) noexcept { hysteresis_seconds_ = seconds; }

    [[nodiscard]] f32 significance() const noexcept { return significance_; }
    [[nodiscard]] f32 hysteresis_seconds() const noexcept { return hysteresis_seconds_; }

    /// Offer one device event. Returns true when the active scheme changed as a result.
    bool observe(DeviceKind kind, f32 value, Nanoseconds timestamp) noexcept;

    [[nodiscard]] SchemeKind active() const noexcept { return active_; }
    /// When the active scheme last changed. What an interface animates from.
    [[nodiscard]] Nanoseconds changed_at() const noexcept { return changed_at_; }

    /// Force the scheme — a game that starts on a title screen with a declared scheme, or a test.
    void set_active(SchemeKind scheme, Nanoseconds timestamp) noexcept;

    /// The candidate currently leading, and since when. Reported by the inspector so that "why have
    /// the prompts not changed" has an answer other than "wait".
    [[nodiscard]] SchemeKind candidate() const noexcept { return candidate_; }
    [[nodiscard]] Nanoseconds candidate_since() const noexcept { return candidate_since_; }

private:
    SchemeKind active_ = SchemeKind::KeyboardMouse;
    SchemeKind candidate_ = SchemeKind::KeyboardMouse;
    Nanoseconds candidate_since_ = 0;
    Nanoseconds changed_at_ = 0;
    f32 significance_ = 0.2F;
    f32 hysteresis_seconds_ = 0.25F;
};

}  // namespace cy::input
