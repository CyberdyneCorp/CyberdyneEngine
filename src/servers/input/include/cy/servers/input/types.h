#pragma once
// The vocabulary of CyberInput: devices, controls, action values, interpretations and phases.
// Tasks 4.1.1 to 4.1.3.
//
// `input-and-actions` draws two boundaries and this file sits between them. Below it the platform
// layer produces *normalised, timestamped device events*; above it `gameplay-framework` consumes
// *commands*. Nothing here names SDL, a scan code from a particular operating system, or an entity.
//
// --- WHY THE CONTROL IS A PAIR AND NOT A STRING --------------------------------------------------
//
// "Runtime SHALL perform no string lookup, path parsing, or asset search in the evaluation path"
// and "control paths SHALL NOT be parsed at runtime". A `Control` is therefore `{DeviceKind, u16}`
// — two integers, comparable in one 32-bit compare. Authoring writes `"keyboard/w"`; cooking turns
// it into `Control{DeviceKind::Keyboard, key_code(Key::W)}`; the evaluation path only ever sees the
// pair. `cook::parse_control_path()` in this header is the *only* function that reads a path, it is
// declared in a namespace named for when it runs, and no file under src/servers/input/ calls it
// outside a cook or a rebinding flow.
//
// --- WHY INTERPRETATION IS ON THE BINDING --------------------------------------------------------
//
// `input-and-actions`: "A binding SHALL declare its interpretation: delta (mouse motion), absolute
// (a position), or rate (a stick deflection). Consumers SHALL integrate accordingly, and a delta
// SHALL NOT be scaled by frame time."
//
// The specification calls this "the most common input defect in shipped games and nearly invisible
// until someone changes frame rate". It is invisible because both spellings look identical at 60Hz
// with a sensitivity constant tuned to hide it. `apply_time_step()` below is the one place the
// engine turns a value into a per-step displacement, and it takes the interpretation, so the defect
// has no place left to live: a delta returns unchanged and a rate is multiplied.

#include <cy/core/base/types.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/vec.h>

#include <string_view>

namespace cy::input {

// --- Devices -------------------------------------------------------------------------------------

/// The classes of device the engine knows how to normalise. A device that is none of these is
/// `Virtual` — synthetic, remote, replay or artificial-intelligence sources all report as one, and
/// `EventSource` says which.
enum class DeviceKind : u8 {
    Unknown = 0,
    Keyboard,
    Mouse,
    Gamepad,
    Touch,
    Wheel,
    /// A tracked device with a pose: a hand controller, a headset, a tracker.
    Tracked,
    /// Not a physical device. Synthetic input, remote input and replay all arrive on one of these.
    Virtual,
    Count,
};

const char* device_kind_name(DeviceKind kind) noexcept;

/// A control on a device, already resolved. Two integers, never a path.
///
/// `code` is the device class's own numbering — `Key` for a keyboard, `MouseControl` for a mouse,
/// `GamepadControl` for a gamepad — so that one binding works for every device of that class and a
/// user who swaps controllers keeps their bindings.
struct Control {
    DeviceKind kind = DeviceKind::Unknown;
    u16 code = 0;

    friend constexpr bool operator==(Control a, Control b) noexcept {
        return a.kind == b.kind && a.code == b.code;
    }
    friend constexpr bool operator!=(Control a, Control b) noexcept { return !(a == b); }

    [[nodiscard]] constexpr bool is_valid() const noexcept { return kind != DeviceKind::Unknown; }

    /// A single integer for a table index. The evaluation path buckets bindings by this.
    [[nodiscard]] constexpr u32 key() const noexcept {
        return (static_cast<u32>(kind) << 16U) | static_cast<u32>(code);
    }
};

/// The keyboard's control numbering. A subset — what a character sample and a rebinding flow need —
/// chosen so that the letters and digits are contiguous and a backend's mapping table is a switch
/// rather than a hash.
///
/// This is deliberately **not** an operating-system scan code. `input-and-actions` forbids gameplay
/// depending on platform key codes, and the way to make that structural is for the engine's own
/// numbering to be the only one above the platform layer.
///
/// EVERY value is written out rather than only the ones that jump. These numbers are what a cooked
/// binding and a saved rebinding profile store, so an enumerator inserted in the middle would
/// silently renumber every key after it and repoint every profile on disk. Spelling them all makes
/// that an edit somebody has to make on purpose.
enum class Key : u16 {
    Unknown = 0,
    // Letters, contiguous, so a backend maps a range with one subtraction.
    A = 1,
    B = 2,
    C = 3,
    D = 4,
    E = 5,
    F = 6,
    G = 7,
    H = 8,
    I = 9,
    J = 10,
    K = 11,
    L = 12,
    M = 13,
    N = 14,
    O = 15,
    P = 16,
    Q = 17,
    R = 18,
    S = 19,
    T = 20,
    U = 21,
    V = 22,
    W = 23,
    X = 24,
    Y = 25,
    Z = 26,
    // Digits, contiguous, for the same reason.
    Num0 = 40,
    Num1 = 41,
    Num2 = 42,
    Num3 = 43,
    Num4 = 44,
    Num5 = 45,
    Num6 = 46,
    Num7 = 47,
    Num8 = 48,
    Num9 = 49,
    Space = 60,
    Enter = 61,
    Escape = 62,
    Tab = 63,
    Backspace = 64,
    LeftShift = 65,
    RightShift = 66,
    LeftControl = 67,
    RightControl = 68,
    LeftAlt = 69,
    RightAlt = 70,
    Left = 80,
    Right = 81,
    Up = 82,
    Down = 83,
    F1 = 100,
    F2 = 101,
    F3 = 102,
    F4 = 103,
    F5 = 104,
    F6 = 105,
    F7 = 106,
    F8 = 107,
    F9 = 108,
    F10 = 109,
    F11 = 110,
    F12 = 111,
    Count = 112,
};

/// The mouse's control numbering. Buttons are digital; `MoveX`/`MoveY`/`Wheel` are **deltas** and a
/// binding that reads one declares `Interpretation::Delta`.
enum class MouseControl : u16 {
    Unknown = 0,
    Left,
    Right,
    Middle,
    Extra1,
    Extra2,
    MoveX,
    MoveY,
    Wheel,
    WheelX,
    /// The pointer's absolute position, in window coordinates.
    PositionX,
    PositionY,
    Count,
};

/// The gamepad's control numbering, in the vendor-neutral naming `input-and-actions` implies by
/// forbidding button indices: `South` rather than `A` or `Cross`.
enum class GamepadControl : u16 {
    Unknown = 0,
    South,
    East,
    West,
    North,
    Back,
    Guide,
    Start,
    LeftStick,
    RightStick,
    LeftShoulder,
    RightShoulder,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    LeftStickX,
    LeftStickY,
    RightStickX,
    RightStickY,
    LeftTrigger,
    RightTrigger,
    Count,
};

[[nodiscard]] constexpr Control key_control(Key key) noexcept {
    return Control{DeviceKind::Keyboard, static_cast<u16>(key)};
}
[[nodiscard]] constexpr Control mouse_control(MouseControl control) noexcept {
    return Control{DeviceKind::Mouse, static_cast<u16>(control)};
}
[[nodiscard]] constexpr Control gamepad_control(GamepadControl control) noexcept {
    return Control{DeviceKind::Gamepad, static_cast<u16>(control)};
}

/// The largest `code` any device class issues. Sizes the per-device control state array, so a
/// backend cannot report a control the server has nowhere to put.
inline constexpr u16 kMaxControlCode = 128;

static_assert(static_cast<u16>(Key::Count) <= kMaxControlCode);
static_assert(static_cast<u16>(MouseControl::Count) <= kMaxControlCode);
static_assert(static_cast<u16>(GamepadControl::Count) <= kMaxControlCode);

// --- Actions -----------------------------------------------------------------------------------

/// What an action carries. `Pose` exists from the outset — `input-and-actions` requires it "so that
/// tracked devices can be added without changing the action model", and adding a value type later
/// changes every record that stores one.
enum class ActionValueType : u8 {
    Digital = 0,
    Scalar,
    Axis2,
    Axis3,
    Pose,
    Count,
};

const char* action_value_type_name(ActionValueType type) noexcept;

/// The value an action carries, in the one shape every value type fits.
///
/// `rotation` is meaningful only for `Pose`; the other four leave it at identity. Storing one type
/// rather than a variant is what keeps `ActionState` trivially copyable and the evaluation path
/// free of a branch on the value type per binding.
struct ActionValue {
    Vec3 axis;
    Quat rotation;

    [[nodiscard]] constexpr bool digital() const noexcept { return axis.x != 0.0F; }
    [[nodiscard]] constexpr f32 scalar() const noexcept { return axis.x; }
    [[nodiscard]] constexpr Vec2 axis2() const noexcept { return Vec2{axis.x, axis.y}; }
    [[nodiscard]] constexpr Vec3 axis3() const noexcept { return axis; }

    [[nodiscard]] static constexpr ActionValue from_scalar(f32 value) noexcept {
        return ActionValue{Vec3{value, 0.0F, 0.0F}, Quat{}};
    }
    [[nodiscard]] static constexpr ActionValue from_axis2(Vec2 value) noexcept {
        return ActionValue{Vec3{value.x, value.y, 0.0F}, Quat{}};
    }
};

/// How a binding's value should be read against time.
///
/// See the header comment. This is the one distinction that produces a defect nobody sees until the
/// frame rate changes.
enum class Interpretation : u8 {
    /// A position or a deflection that is already the value: a stick at 0.6, a trigger at 1.0.
    Absolute = 0,
    /// A displacement that already happened: a mouse motion, a wheel notch. **Never** multiplied by
    /// a time step — it is not a speed.
    Delta,
    /// A speed: a stick deflection driving a turn rate. Multiplied by the time step by whoever
    /// integrates it.
    Rate,
};

const char* interpretation_name(Interpretation interpretation) noexcept;

/// Turn one binding's value into the displacement for a step of `seconds`.
///
/// The whole point of the function is the `Delta` case returning `value` untouched. A caller that
/// multiplies every input by `dt` produces a mouse that is twice as sensitive at 30Hz as at 60Hz,
/// and it is nearly impossible to see in a manual test because the sensitivity slider hides it.
[[nodiscard]] constexpr Vec3 apply_time_step(Interpretation interpretation, Vec3 value,
                                             f32 seconds) noexcept {
    if (interpretation == Interpretation::Rate) {
        return Vec3{value.x * seconds, value.y * seconds, value.z * seconds};
    }
    return value;
}

// --- The action lifecycle ------------------------------------------------------------------------

/// The five phases `input-and-actions` names, plus the idle state between them.
///
/// `Cancelled` is the one that earns its place: a hold abandoned before its threshold reports
/// `Started` then `Cancelled` and never `Triggered`, which is what lets a game distinguish "the
/// player changed their mind" from "the player completed the hold".
enum class TriggerPhase : u8 {
    Idle = 0,
    Started,
    Ongoing,
    Triggered,
    Completed,
    Cancelled,
    Count,
};

const char* trigger_phase_name(TriggerPhase phase) noexcept;

/// What decides when an action becomes active.
enum class TriggerKind : u8 {
    /// Active for as long as the control is actuated. The default, and what a movement axis wants.
    Down = 0,
    /// The rising edge only.
    Pressed,
    /// The falling edge only.
    Released,
    /// Actuated continuously for `duration_seconds`; abandoned early reports `Cancelled`.
    Hold,
    /// Pressed and released inside `duration_seconds`.
    Tap,
    /// Two taps inside `duration_seconds` of each other.
    DoubleTap,
    /// The magnitude crossing `threshold` upwards.
    Threshold,
    /// Every component of a composite actuated at once.
    Chord,
    /// The composite's components actuated in order inside `duration_seconds`.
    Sequence,
    /// Repeats every `duration_seconds` while actuated.
    Pulse,
    Count,
};

const char* trigger_kind_name(TriggerKind kind) noexcept;

/// A trigger, flat and copyable. `duration_seconds` and `threshold` mean what the kind says they
/// mean; a kind that uses neither ignores both.
struct TriggerSpec {
    TriggerKind kind = TriggerKind::Down;
    /// Below this magnitude a control is not actuated. One number rather than a per-kind default,
    /// so that "the value was below the threshold" is one diagnostic reason and not several.
    f32 actuation_threshold = 0.5F;
    f32 duration_seconds = 0.0F;
    f32 threshold = 0.0F;
};

// --- Where an event came from --------------------------------------------------------------------

/// `input-and-actions`: "Synthetic and remote sources SHALL be marked as such, so diagnostics and
/// anti-cheat policies can distinguish them, and a shipping build SHALL be able to disable them."
///
/// Marked on the *event*, not on the device, because one virtual device may carry a replay on one
/// tick and a test's injection on the next, and a diagnostic that had to infer it from the device
/// would be inferring.
enum class EventSource : u8 {
    /// A real device, read by the platform layer.
    Physical = 0,
    /// Injected by code: a test, an automation harness, a tool.
    Synthetic,
    /// A device on another machine feeding this user.
    Remote,
    /// Replay playback.
    Replay,
    Count,
};

const char* event_source_name(EventSource source) noexcept;

// --- Control schemes -----------------------------------------------------------------------------

/// The schemes the engine defines. `Project` is the extension point: a project declares its own
/// with a `Name` and this enumerator, rather than the enumeration growing per game.
enum class SchemeKind : u8 {
    KeyboardMouse = 0,
    Gamepad,
    Touch,
    Wheel,
    Project,
    Count,
};

const char* scheme_kind_name(SchemeKind kind) noexcept;

/// Which schemes a binding belongs to, as a mask, so a binding may serve both keyboard and gamepad
/// prompts without being duplicated.
[[nodiscard]] constexpr u8 scheme_mask(SchemeKind kind) noexcept {
    return static_cast<u8>(1U << static_cast<u8>(kind));
}
inline constexpr u8 kAllSchemes = 0x1FU;

/// The scheme a control belongs to by construction. Used by rebinding to reject a gamepad control
/// offered for a keyboard override, and by the detector to attribute activity.
[[nodiscard]] constexpr SchemeKind scheme_of(DeviceKind kind) noexcept {
    switch (kind) {
        case DeviceKind::Gamepad:
            return SchemeKind::Gamepad;
        case DeviceKind::Touch:
            return SchemeKind::Touch;
        case DeviceKind::Wheel:
            return SchemeKind::Wheel;
        case DeviceKind::Keyboard:
        case DeviceKind::Mouse:
            return SchemeKind::KeyboardMouse;
        case DeviceKind::Tracked:
        case DeviceKind::Virtual:
        case DeviceKind::Unknown:
        case DeviceKind::Count:
            break;
    }
    return SchemeKind::Project;
}

// --- Cook time -----------------------------------------------------------------------------------

/// Everything in here runs at cook time, at load time, or inside an editor. **Nothing in the
/// evaluation path may call it**, and the namespace exists so that a reviewer can see the rule
/// being kept by reading a call site rather than by reasoning about one.
namespace cook {

/// `"keyboard/w"`, `"mouse/moveX"`, `"gamepad/leftStickX"` to a resolved `Control`.
///
/// Returns an invalid `Control` for anything it does not recognise; the caller reports the path,
/// because at cook time there is a path to report and at run time there is not.
[[nodiscard]] Control parse_control_path(std::string_view path) noexcept;

/// The control's own spelling — the half of the path after the slash — for a diagnostic and for an
/// editor's binding list. Never null; empty for a control no table names.
///
/// The device half is `device_kind_name()` lower-cased, and the two are kept apart rather than
/// joined here because joining them would need storage this function does not own. A caller that
/// wants the whole path formats `"%s/%s"` and owns the buffer.
[[nodiscard]] const char* control_path(Control control) noexcept;

}  // namespace cook

}  // namespace cy::input
