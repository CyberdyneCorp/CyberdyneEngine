// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_FIRST_PERSON_SCRIPT_H
#define CY_SAMPLE_FIRST_PERSON_SCRIPT_H
// script.h — the scripted player: a keyboard and a mouse, pressed and moved by a timeline.
//
// It plays the arena the way a person would, through the input server and nothing else: it walks
// north, crouches under the beam and lets go of the crouch while still under it, sprints, jumps,
// then turns the mouse onto each thing it means to shoot and pulls the trigger. It aims WITH ITS
// EYES — the eye the host draws and the look the camera holds — and fires only once its own ray
// along the weapon's aim lands on what it is aiming at, so a target that strafes is followed, not
// missed by a fixed timeline. It knows nothing about what a shot does; the game decides that.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>

#include "fps_host.h"

namespace sample::fps {

/// What the script did, for the report.
struct ScriptFindings {
    cy::u32 trigger_pulls = 0;
    /// Aims abandoned because the mark was gone or never came under the sights.
    cy::u32 abandoned = 0;
    /// The frame each aiming task finished, or zero.
    cy::u64 finished[8] = {};
    cy::u32 tasks = 0;
};

class ScriptedPlayer {
public:
    /// Frames the session runs: twelve seconds at sixty.
    static constexpr cy::u64 kFrames = 720;

    explicit ScriptedPlayer(FpsHost& host) noexcept : host_(&host) {}

    /// This frame's input, before the host runs it.
    [[nodiscard]] cy::Status before_frame(cy::u64 frame) noexcept;
    [[nodiscard]] const ScriptFindings& findings() const noexcept { return findings_; }

private:
    enum class Mark : cy::u8 { Target, Prop, Point };
    struct Task {
        Mark mark = Mark::Point;
        cy::u32 index = 0;
        cy::Vec3 point{0.0F, 0.0F, 0.0F};
        cy::u32 shots = 1;
        bool sights = false;
    };
    enum class Phase : cy::u8 { Aim, Settle, Fire, Recover };

    [[nodiscard]] cy::Status walk(cy::u64 frame) noexcept;
    [[nodiscard]] cy::Status shoot(cy::u64 frame) noexcept;
    /// One frame of the task's phase: aim, settle, pull the trigger, let the kick settle.
    [[nodiscard]] cy::Status advance(const Task& task) noexcept;
    /// Move the mouse toward the mark, or call the aim done.
    [[nodiscard]] cy::Status aim(const Task& task) noexcept;
    [[nodiscard]] cy::Vec3 aim_point(const Task& task) const noexcept;
    [[nodiscard]] bool on_mark(const Task& task) const noexcept;
    [[nodiscard]] bool mark_gone(const Task& task) const noexcept;
    [[nodiscard]] cy::Status next_task(cy::u64 frame) noexcept;

    FpsHost* host_;
    ScriptFindings findings_;
    cy::u32 task_ = 0;
    cy::u32 fired_ = 0;
    Phase phase_ = Phase::Aim;
    cy::u32 phase_frames_ = 0;
    cy::u32 task_frames_ = 0;
    bool trigger_down_ = false;
    bool sights_down_ = false;
};

}  // namespace sample::fps

#endif  // CY_SAMPLE_FIRST_PERSON_SCRIPT_H
