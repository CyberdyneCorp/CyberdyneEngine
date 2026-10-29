// SPDX-License-Identifier: MIT
// pause.cpp — see pause.h.

#include "pause.h"

namespace cy::play {

void PauseState::pause(PauseCause cause) noexcept {
    if (paused()) {
        return;
    }
    cause_ = cause;
    ++pauses_;
    focus_pauses_ += cause == PauseCause::Focus ? 1U : 0U;
}

void PauseState::toggle() noexcept {
    if (paused()) {
        resume();
    } else {
        pause(PauseCause::Player);
    }
}

void PauseState::resume() noexcept {
    cause_ = PauseCause::None;
}

void PauseState::focus_lost() noexcept {
    focused_ = false;
    pause(PauseCause::Focus);
}

void PauseState::focus_gained() noexcept {
    focused_ = true;
}

}  // namespace cy::play
