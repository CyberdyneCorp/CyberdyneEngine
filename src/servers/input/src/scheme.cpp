// Control schemes and active-scheme detection. Task 4.1.5.

#include <cy/servers/input/scheme.h>

#include <cmath>

namespace cy::input {

ControlScheme builtin_scheme(SchemeKind kind) noexcept {
    ControlScheme scheme;
    scheme.kind = kind;
    switch (kind) {
        case SchemeKind::KeyboardMouse:
            scheme.name = CY_NAME("KeyboardMouse");
            scheme.required_devices = device_mask(DeviceKind::Keyboard);
            // The mouse is optional on purpose: a keyboard-only player is on this scheme, and a
            // scheme that required both would report "no scheme satisfied" on a machine that can
            // obviously play.
            scheme.optional_devices = device_mask(DeviceKind::Mouse);
            break;
        case SchemeKind::Gamepad:
            scheme.name = CY_NAME("Gamepad");
            scheme.required_devices = device_mask(DeviceKind::Gamepad);
            break;
        case SchemeKind::Touch:
            scheme.name = CY_NAME("Touch");
            scheme.required_devices = device_mask(DeviceKind::Touch);
            break;
        case SchemeKind::Wheel:
            scheme.name = CY_NAME("Wheel");
            scheme.required_devices = device_mask(DeviceKind::Wheel);
            scheme.optional_devices = device_mask(DeviceKind::Gamepad);
            break;
        case SchemeKind::Project:
        case SchemeKind::Count:
            scheme.name = CY_NAME("Project");
            break;
    }
    return scheme;
}

void SchemeDetector::set_active(SchemeKind scheme, Nanoseconds timestamp) noexcept {
    active_ = scheme;
    candidate_ = scheme;
    candidate_since_ = timestamp;
    changed_at_ = timestamp;
}

bool SchemeDetector::observe(DeviceKind kind, f32 value, Nanoseconds timestamp) noexcept {
    // GUARD ONE: significance. An idle stick reporting 0.03 because the spring is tired is not
    // evidence that the player picked up the controller. Discarded before it is counted, so it
    // cannot even become a candidate.
    if (std::fabs(value) < significance_) {
        return false;
    }
    const SchemeKind observed = scheme_of(kind);
    if (observed == SchemeKind::Project) {
        return false;
    }
    if (observed == active_) {
        // Activity on the active scheme resets the candidate, which is what stops a stray mouse
        // nudge from accumulating dwell time across a whole minute of controller play.
        candidate_ = active_;
        candidate_since_ = timestamp;
        return false;
    }
    if (observed != candidate_) {
        candidate_ = observed;
        candidate_since_ = timestamp;
        return false;
    }
    // GUARD TWO: hysteresis. One significant sample is not a change of intent; the candidate has to
    // hold the lead for the dwell time. See scheme.h — the two guards answer different questions
    // and dropping either one leaves a real failure in place.
    const auto elapsed = static_cast<f32>(timestamp - candidate_since_) * 1e-9F;
    if (elapsed < hysteresis_seconds_) {
        return false;
    }
    active_ = observed;
    changed_at_ = timestamp;
    return true;
}

}  // namespace cy::input
