// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_PLAY_PAUSE_H
#define CY_SAMPLE_PLAY_PAUSE_H
// pause.h — when a played game is paused, and when the mouse is the game's.
//
// Two things pause it: the player (Esc, a pad's Start) and the window losing the keyboard's focus —
// an alt-tab, a notification that took it. Both stop the simulation and hand the cursor back. Only
// the player resumes it: the window getting its focus back leaves the game paused with its menu up,
// so a player who clicked back into the window is not dropped into a fight mid-click.
//
// The mouse is HELD — relative mode, the cursor hidden and the motion raw — exactly while the game
// runs and the window has the focus; any other time it is the desktop's or the pause menu's.

#include <cy/core/base/types.h>

namespace cy::play {

enum class PauseCause : u8 {
    None = 0,
    /// The player asked: Esc or Start.
    Player,
    /// The window lost the keyboard's focus.
    Focus,
};

class PauseState {
public:
    /// The player's toggle: pauses a running game, resumes a paused one.
    void toggle() noexcept;
    /// The pause menu's resume.
    void resume() noexcept;
    void focus_lost() noexcept;
    void focus_gained() noexcept;

    [[nodiscard]] bool paused() const noexcept { return cause_ != PauseCause::None; }
    [[nodiscard]] PauseCause cause() const noexcept { return cause_; }
    [[nodiscard]] bool focused() const noexcept { return focused_; }
    /// Whether the mouse should be held in the window now.
    [[nodiscard]] bool hold_mouse() const noexcept { return focused_ && !paused(); }
    /// Pauses since the start, and those a lost focus caused.
    [[nodiscard]] u32 pauses() const noexcept { return pauses_; }
    [[nodiscard]] u32 focus_pauses() const noexcept { return focus_pauses_; }

private:
    void pause(PauseCause cause) noexcept;

    PauseCause cause_ = PauseCause::None;
    bool focused_ = true;
    u32 pauses_ = 0;
    u32 focus_pauses_ = 0;
};

}  // namespace cy::play

#endif  // CY_SAMPLE_PLAY_PAUSE_H
