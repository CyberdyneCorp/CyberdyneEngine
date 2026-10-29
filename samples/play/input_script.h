// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_PLAY_INPUT_SCRIPT_H
#define CY_SAMPLE_PLAY_INPUT_SCRIPT_H
// input_script.h — a person's hands, written down: platform events at given frames, pushed onto
// SDL's queue through `cy::Sdl3EventInjector`.
//
// What a capture and a play-mode test drive the game with. The events take the path a person's
// take — SDL's queue, the input source's translation, the display server's focus events, the input
// server, the game's actions — so a script exercises the play host, not a side door into the game.
//
//     # frame  what
//     30       key w down              a key, by the input layer's name (`keyboard/<name>`)
//     30       mouse move 120 -40      relative motion, window pixels, +y down (SDL's)
//     31       mouse to 640 360        the cursor to a point (a pause menu's button)
//     40       mouse left down         a button: left, right, middle, extra1, extra2
//     50       focus lost              the window loses, or gains, the keyboard's focus
//     55       window 1600 900         the window resized, as a person dragging its corner would
//     60       pad attach              a virtual gamepad, then its axes and buttons by the input
//     61       pad axis leftStickY 1   layer's names, in the engine's convention (stick +y up)
//     62       pad button south down
//     900      quit                    end the run
//
// A frame with several lines runs them in order. Frames are the play host's own count, so with
// a fixed clock a script is reproducible frame for frame.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/servers/input/types.h>

#include <string>
#include <string_view>
#include <vector>

namespace cy {
class DisplayServer;
class Sdl3EventInjector;
}  // namespace cy

namespace cy::play {

enum class ScriptVerb : u8 {
    Key,
    MouseMove,
    MouseTo,
    MouseButton,
    Focus,
    PadAttach,
    PadAxis,
    PadButton,
    Window,
    Quit,
};

struct ScriptEvent {
    u64 frame = 0;
    ScriptVerb verb = ScriptVerb::Quit;
    /// A key, a mouse button or a pad control.
    input::Control control;
    bool down = false;
    f32 x = 0.0F;
    f32 y = 0.0F;
};

/// Where the injected events go.
struct ScriptTarget {
    Sdl3EventInjector* inject = nullptr;
    /// SDL's id for the window a focus line addresses.
    u32 native_window = 0;
    /// The display server and the window a `window` line resizes.
    DisplayServer* display = nullptr;
    u32 window = 0;
};

class InputScript {
public:
    /// Parse a script; the error names the line.
    [[nodiscard]] Status parse(std::string_view text) noexcept;
    [[nodiscard]] Status load(const char* path) noexcept;

    /// Push every event of `frame`. True when the script asked the run to end at this frame.
    [[nodiscard]] Expected<bool, Error> run(u64 frame, const ScriptTarget& target) noexcept;

    [[nodiscard]] usize size() const noexcept { return events_.size(); }
    [[nodiscard]] bool empty() const noexcept { return events_.empty(); }
    /// The cursor where the script last put it.
    [[nodiscard]] f32 cursor_x() const noexcept { return cursor_x_; }
    [[nodiscard]] f32 cursor_y() const noexcept { return cursor_y_; }

private:
    [[nodiscard]] Status push(const ScriptEvent& event, const ScriptTarget& target) noexcept;

    std::vector<ScriptEvent> events_;
    usize next_ = 0;
    std::string error_;
    f32 cursor_x_ = 640.0F;
    f32 cursor_y_ = 360.0F;
};

}  // namespace cy::play

#endif  // CY_SAMPLE_PLAY_INPUT_SCRIPT_H
