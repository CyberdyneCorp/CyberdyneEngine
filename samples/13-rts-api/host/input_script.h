// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_API_INPUT_SCRIPT_H
#define CY_SAMPLE_RTS_API_INPUT_SCRIPT_H
// input_script.h — a person's hand, written down: `--play --input <file>`.
//
// The tests and the captures of `--play` are driven by a script rather than by a person, and the
// script is replayed THROUGH THE PLATFORM — every line becomes real SDL events pushed into SDL's
// queue (`cy::Sdl3EventInjector`), read by the same `Sdl3InputSource` a keyboard and a mouse are
// read by — so what is tested is the door a person's input comes through, not the game's code. The
// game decides what each click means exactly as it does for a person.
//
// One event a line, at the frame it happens on; `#` starts a comment:
//
//   <frame> key <name> down|up       a key: a-z, 0-9, escape, space, enter, tab, shift, ctrl, alt,
//                                    left, right, up, down
//   <frame> move <x> <y>             the pointer to (x, y) in the VIEW's pixels (1280 by 720)
//   <frame> press <button>           left, right or middle, where the pointer is
//   <frame> release <button>
//   <frame> wheel <notches>          positive away from the player: zoom in
//   <frame> aim unit <i> [dx dy]     the pointer onto battle unit i's chest, where the game's
//   camera
//                                    shows it now — the way a person aims, with their eyes — offset
//                                    by (dx, dy) view pixels
//   <frame> aim field <x> <z>        the pointer onto that point of the battlefield (its metres)
//   <frame> aim ui <name>            the pointer onto the middle of the interface element or widget
//                                    named so, as the last layout placed it
//   <frame> resize <w> <h>           the window is resized to w by h, as a person dragging its
//                                    corner would (ignored with no window)
//   <frame> quit                     the session ends, as if the window were closed
//
// Coordinates are the view's, whatever the window's size: the driver converts them into the
// window's own before pushing them, through the letterbox the platform's pointer mapping inverts.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/servers/input/types.h>

#include <string>
#include <vector>

namespace sample::rts {

struct ScriptEvent {
    enum class Kind : cy::u8 {
        Key,
        Move,
        Press,
        Release,
        Wheel,
        AimUnit,
        AimField,
        AimUi,
        Resize,
        Quit
    };

    cy::u64 frame = 0;
    Kind kind = Kind::Move;
    cy::input::Key key = cy::input::Key::Unknown;
    cy::input::MouseControl button = cy::input::MouseControl::Unknown;
    bool down = false;
    /// Move: the point. Wheel: notches in `x`. AimUnit: the offset. AimField: the field point.
    /// Resize: the window's new size.
    cy::f32 x = 0.0F;
    cy::f32 y = 0.0F;
    /// AimUnit: the unit.
    cy::u32 unit = 0;
    /// AimUi: the element's name.
    std::string name;
};

/// The events of `path`, in frame order. Names the line it refuses.
[[nodiscard]] cy::Status read_input_script(const std::string& path, std::vector<ScriptEvent>& out,
                                           std::string& detail) noexcept;

/// One line, for the parser's own test.
[[nodiscard]] bool parse_script_line(const std::string& line, ScriptEvent& out,
                                     bool& blank) noexcept;

}  // namespace sample::rts

#endif  // CY_SAMPLE_RTS_API_INPUT_SCRIPT_H
