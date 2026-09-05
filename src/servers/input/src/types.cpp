// The vocabulary's spellings, and the cook-time control-path reader. Task 4.1.2.
//
// Every `*_name()` here returns the enumerator's own spelling rather than a prose description: a
// diagnostic that says `Gamepad` can be grepped for in this tree, and one that says "a game
// controller" cannot.

#include <cy/servers/input/types.h>

#include <cy/core/base/assert.h>

namespace cy::input {
namespace {

/// `"keyboard/w"` split at the slash. Returns false when there is no slash, which is the only shape
/// error a path can have that is worth distinguishing.
bool split_path(std::string_view path, std::string_view& device, std::string_view& control) {
    const usize slash = path.find('/');
    if (slash == std::string_view::npos) {
        return false;
    }
    device = path.substr(0, slash);
    control = path.substr(slash + 1);
    return true;
}

struct NamedControl {
    const char* text;
    u16 code;
};

// The tables are the *only* place a control's authored spelling lives, so a rename is one edit and
// `control_path()` can be its exact inverse rather than a second list that drifts.

constexpr NamedControl kKeys[] = {
    {"a", static_cast<u16>(Key::A)},
    {"b", static_cast<u16>(Key::B)},
    {"c", static_cast<u16>(Key::C)},
    {"d", static_cast<u16>(Key::D)},
    {"e", static_cast<u16>(Key::E)},
    {"f", static_cast<u16>(Key::F)},
    {"g", static_cast<u16>(Key::G)},
    {"h", static_cast<u16>(Key::H)},
    {"i", static_cast<u16>(Key::I)},
    {"j", static_cast<u16>(Key::J)},
    {"k", static_cast<u16>(Key::K)},
    {"l", static_cast<u16>(Key::L)},
    {"m", static_cast<u16>(Key::M)},
    {"n", static_cast<u16>(Key::N)},
    {"o", static_cast<u16>(Key::O)},
    {"p", static_cast<u16>(Key::P)},
    {"q", static_cast<u16>(Key::Q)},
    {"r", static_cast<u16>(Key::R)},
    {"s", static_cast<u16>(Key::S)},
    {"t", static_cast<u16>(Key::T)},
    {"u", static_cast<u16>(Key::U)},
    {"v", static_cast<u16>(Key::V)},
    {"w", static_cast<u16>(Key::W)},
    {"x", static_cast<u16>(Key::X)},
    {"y", static_cast<u16>(Key::Y)},
    {"z", static_cast<u16>(Key::Z)},
    {"space", static_cast<u16>(Key::Space)},
    {"enter", static_cast<u16>(Key::Enter)},
    {"escape", static_cast<u16>(Key::Escape)},
    {"tab", static_cast<u16>(Key::Tab)},
    {"backspace", static_cast<u16>(Key::Backspace)},
    {"leftShift", static_cast<u16>(Key::LeftShift)},
    {"rightShift", static_cast<u16>(Key::RightShift)},
    {"leftControl", static_cast<u16>(Key::LeftControl)},
    {"rightControl", static_cast<u16>(Key::RightControl)},
    {"leftAlt", static_cast<u16>(Key::LeftAlt)},
    {"rightAlt", static_cast<u16>(Key::RightAlt)},
    {"left", static_cast<u16>(Key::Left)},
    {"right", static_cast<u16>(Key::Right)},
    {"up", static_cast<u16>(Key::Up)},
    {"down", static_cast<u16>(Key::Down)},
};

constexpr NamedControl kMouse[] = {
    {"left", static_cast<u16>(MouseControl::Left)},
    {"right", static_cast<u16>(MouseControl::Right)},
    {"middle", static_cast<u16>(MouseControl::Middle)},
    {"extra1", static_cast<u16>(MouseControl::Extra1)},
    {"extra2", static_cast<u16>(MouseControl::Extra2)},
    {"moveX", static_cast<u16>(MouseControl::MoveX)},
    {"moveY", static_cast<u16>(MouseControl::MoveY)},
    {"wheel", static_cast<u16>(MouseControl::Wheel)},
    {"wheelX", static_cast<u16>(MouseControl::WheelX)},
    {"positionX", static_cast<u16>(MouseControl::PositionX)},
    {"positionY", static_cast<u16>(MouseControl::PositionY)},
};

constexpr NamedControl kGamepad[] = {
    {"south", static_cast<u16>(GamepadControl::South)},
    {"east", static_cast<u16>(GamepadControl::East)},
    {"west", static_cast<u16>(GamepadControl::West)},
    {"north", static_cast<u16>(GamepadControl::North)},
    {"back", static_cast<u16>(GamepadControl::Back)},
    {"guide", static_cast<u16>(GamepadControl::Guide)},
    {"start", static_cast<u16>(GamepadControl::Start)},
    {"leftStick", static_cast<u16>(GamepadControl::LeftStick)},
    {"rightStick", static_cast<u16>(GamepadControl::RightStick)},
    {"leftShoulder", static_cast<u16>(GamepadControl::LeftShoulder)},
    {"rightShoulder", static_cast<u16>(GamepadControl::RightShoulder)},
    {"dpadUp", static_cast<u16>(GamepadControl::DpadUp)},
    {"dpadDown", static_cast<u16>(GamepadControl::DpadDown)},
    {"dpadLeft", static_cast<u16>(GamepadControl::DpadLeft)},
    {"dpadRight", static_cast<u16>(GamepadControl::DpadRight)},
    {"leftStickX", static_cast<u16>(GamepadControl::LeftStickX)},
    {"leftStickY", static_cast<u16>(GamepadControl::LeftStickY)},
    {"rightStickX", static_cast<u16>(GamepadControl::RightStickX)},
    {"rightStickY", static_cast<u16>(GamepadControl::RightStickY)},
    {"leftTrigger", static_cast<u16>(GamepadControl::LeftTrigger)},
    {"rightTrigger", static_cast<u16>(GamepadControl::RightTrigger)},
};

struct ControlTable {
    const NamedControl* entries;
    usize count;
};

ControlTable table_for(DeviceKind kind) {
    switch (kind) {
        case DeviceKind::Keyboard:
            return ControlTable{kKeys, sizeof(kKeys) / sizeof(kKeys[0])};
        case DeviceKind::Mouse:
            return ControlTable{kMouse, sizeof(kMouse) / sizeof(kMouse[0])};
        case DeviceKind::Gamepad:
            return ControlTable{kGamepad, sizeof(kGamepad) / sizeof(kGamepad[0])};
        default:
            return ControlTable{nullptr, 0};
    }
}

DeviceKind device_kind_from_path(std::string_view text) {
    if (text == "keyboard") {
        return DeviceKind::Keyboard;
    }
    if (text == "mouse") {
        return DeviceKind::Mouse;
    }
    if (text == "gamepad") {
        return DeviceKind::Gamepad;
    }
    if (text == "touch") {
        return DeviceKind::Touch;
    }
    if (text == "wheel") {
        return DeviceKind::Wheel;
    }
    if (text == "tracked") {
        return DeviceKind::Tracked;
    }
    if (text == "virtual") {
        return DeviceKind::Virtual;
    }
    return DeviceKind::Unknown;
}

}  // namespace

const char* device_kind_name(DeviceKind kind) noexcept {
    switch (kind) {
        case DeviceKind::Unknown:
            return "Unknown";
        case DeviceKind::Keyboard:
            return "Keyboard";
        case DeviceKind::Mouse:
            return "Mouse";
        case DeviceKind::Gamepad:
            return "Gamepad";
        case DeviceKind::Touch:
            return "Touch";
        case DeviceKind::Wheel:
            return "Wheel";
        case DeviceKind::Tracked:
            return "Tracked";
        case DeviceKind::Virtual:
            return "Virtual";
        case DeviceKind::Count:
            break;
    }
    return "Unknown";
}

const char* action_value_type_name(ActionValueType type) noexcept {
    switch (type) {
        case ActionValueType::Digital:
            return "Digital";
        case ActionValueType::Scalar:
            return "Scalar";
        case ActionValueType::Axis2:
            return "Axis2";
        case ActionValueType::Axis3:
            return "Axis3";
        case ActionValueType::Pose:
            return "Pose";
        case ActionValueType::Count:
            break;
    }
    return "Digital";
}

const char* interpretation_name(Interpretation interpretation) noexcept {
    switch (interpretation) {
        case Interpretation::Absolute:
            return "Absolute";
        case Interpretation::Delta:
            return "Delta";
        case Interpretation::Rate:
            return "Rate";
    }
    return "Absolute";
}

const char* trigger_phase_name(TriggerPhase phase) noexcept {
    switch (phase) {
        case TriggerPhase::Idle:
            return "Idle";
        case TriggerPhase::Started:
            return "Started";
        case TriggerPhase::Ongoing:
            return "Ongoing";
        case TriggerPhase::Triggered:
            return "Triggered";
        case TriggerPhase::Completed:
            return "Completed";
        case TriggerPhase::Cancelled:
            return "Cancelled";
        case TriggerPhase::Count:
            break;
    }
    return "Idle";
}

const char* trigger_kind_name(TriggerKind kind) noexcept {
    switch (kind) {
        case TriggerKind::Down:
            return "Down";
        case TriggerKind::Pressed:
            return "Pressed";
        case TriggerKind::Released:
            return "Released";
        case TriggerKind::Hold:
            return "Hold";
        case TriggerKind::Tap:
            return "Tap";
        case TriggerKind::DoubleTap:
            return "DoubleTap";
        case TriggerKind::Threshold:
            return "Threshold";
        case TriggerKind::Chord:
            return "Chord";
        case TriggerKind::Sequence:
            return "Sequence";
        case TriggerKind::Pulse:
            return "Pulse";
        case TriggerKind::Count:
            break;
    }
    return "Down";
}

const char* event_source_name(EventSource source) noexcept {
    switch (source) {
        case EventSource::Physical:
            return "Physical";
        case EventSource::Synthetic:
            return "Synthetic";
        case EventSource::Remote:
            return "Remote";
        case EventSource::Replay:
            return "Replay";
        case EventSource::Count:
            break;
    }
    return "Physical";
}

const char* scheme_kind_name(SchemeKind kind) noexcept {
    switch (kind) {
        case SchemeKind::KeyboardMouse:
            return "KeyboardMouse";
        case SchemeKind::Gamepad:
            return "Gamepad";
        case SchemeKind::Touch:
            return "Touch";
        case SchemeKind::Wheel:
            return "Wheel";
        case SchemeKind::Project:
            return "Project";
        case SchemeKind::Count:
            break;
    }
    return "Project";
}

namespace cook {

Control parse_control_path(std::string_view path) noexcept {
    std::string_view device_text;
    std::string_view control_text;
    if (!split_path(path, device_text, control_text)) {
        return Control{};
    }
    const DeviceKind kind = device_kind_from_path(device_text);
    const ControlTable table = table_for(kind);
    for (usize index = 0; index < table.count; ++index) {
        if (control_text == table.entries[index].text) {
            return Control{kind, table.entries[index].code};
        }
    }
    return Control{};
}

const char* control_path(Control control) noexcept {
    const ControlTable table = table_for(control.kind);
    for (usize index = 0; index < table.count; ++index) {
        if (table.entries[index].code == control.code) {
            return table.entries[index].text;
        }
    }
    return "";
}

}  // namespace cook

}  // namespace cy::input
