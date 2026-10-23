// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_MENUS_SCRIPT_H
#define CY_SAMPLE_MENUS_SCRIPT_H
// script.h — the scripted player: what a person with a gamepad, a keyboard and a mouse would do,
// frame by frame, read from a text file and played through the host's hand.
//
//     # frame  verb     arguments
//     30       tap      gamepad/dpadDown          press, and release on the next frame
//     40       hold     keyboard/e 3              press, and release three frames later
//     60       point    #slot-0                   the pointer to a widget's centre
//     61       point    640 360                   ... or to a point in the reference resolution
//     62       press    mouse/left                down, until a `release`
//     70       release  mouse/left
//     90       shot     settings-graphics         the renderer writes the frame as this name
//     95       report                             print the host's observation now
//     99       edit     ui/main_menu.cyui Menus Screens   replace a word in a project file, as a
//                                                         save in the editor does
//
// Blank lines and `#` comments are skipped; frames must not go backwards. A line that does not
// read is refused at load with its number.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <string>
#include <vector>

namespace sample::menus {

class MenusHost;

struct ScriptStep {
    cy::u64 frame = 0;
    std::string verb;
    std::string target;
    cy::f32 x = 0.0F;
    cy::f32 y = 0.0F;
    cy::u32 hold = 1;
    /// `edit`: the word replaced, and what replaces it.
    std::string find;
    std::string replace;
};

class Script {
public:
    /// `project` is where `edit` finds its files.
    [[nodiscard]] cy::Status load(const char* path, std::string project) noexcept;
    /// Play every step due at the host's current frame. `shots` receives the names of the frames
    /// to write, and `reports` is set when one asked for the observation.
    [[nodiscard]] cy::Status play(MenusHost& host, std::vector<std::string>& shots,
                                  bool& report) noexcept;
    [[nodiscard]] cy::u64 last_frame() const noexcept;

private:
    [[nodiscard]] cy::Status step(MenusHost& host, const ScriptStep& line,
                                  std::vector<std::string>& shots, bool& report) noexcept;

    [[nodiscard]] cy::Status edit(const ScriptStep& line) const noexcept;

    std::vector<ScriptStep> steps_;
    std::string project_;
    /// Releases due later: the frame and the control.
    std::vector<ScriptStep> releases_;
    std::size_t next_ = 0;
};

}  // namespace sample::menus

#endif  // CY_SAMPLE_MENUS_SCRIPT_H
