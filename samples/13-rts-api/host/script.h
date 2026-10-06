// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_API_SCRIPT_H
#define CY_SAMPLE_RTS_API_SCRIPT_H
// script.h — a scripted player: a hand on the keyboard and mouse, and eyes on the screen.
//
// It is not the game and decides nothing the game decides. It presses keys and moves the pointer
// through `InputServer::inject`, which records the events as synthetic, and it aims a click the way
// a person does: by looking at where a thing is on screen (`RtsHost::project`, through the same
// camera adapter the game reads). What the click MEANS — which unit is selected, where it is sent,
// whether it plays a sound on arrival — is decided by the Swift behaviour and read back afterwards.
//
// THE TIMELINE (one fixed tick per frame, 60 per second):
//
//   frame   0       pointer to the middle of the window
//   frame   1-30    hold D                          the camera pans right with the keyboard
//   frame  40-69    pointer against the left edge   the camera pans left with the mouse
//   frame  80-82    aim at the second unit, left click
//   frame  90-92    aim at a ground point, right click
//   frame 360-361   press B                         the build key
//   frame 420       end
//
// The scene is the one Swift builds in `Commander.onCreate`: two workers. "The second unit" is the
// second navigation agent in entity order, which is the second one the game spawned.

#include <cy/core/base/expected.h>
#include <cy/core/math/vec.h>

#include "rts_host.h"

namespace sample::rts {

/// What the player saw, for the report.
struct Findings {
    /// The camera's x as placed by the game, after the keyboard pan, and after the edge pan.
    cy::f32 camera_start = 0.0F;
    cy::f32 camera_after_keys = 0.0F;
    cy::f32 camera_after_edge = 0.0F;
    /// The unit the player clicked, and the one it did not.
    CyEntity clicked = CY_ENTITY_NULL;
    CyEntity bystander = CY_ENTITY_NULL;
    cy::Vec3 bystander_start{};
    /// The ground point the player right-clicked.
    cy::Vec3 target{};
    /// The first frame after which the clicked unit's navigation status was ARRIVED; zero if never.
    cy::u64 arrived_frame = 0;
    /// The most voices the audio server had active after any frame.
    cy::u32 peak_voices = 0;
    /// Aims that failed because the thing was not on screen or did not exist.
    cy::u32 missed_aims = 0;
    /// The HUD's Build button was found in the interface and aimed at.
    bool button_found = false;
};

class Player {
public:
    static constexpr cy::u64 kFrames = 420;

    explicit Player(RtsHost& host) noexcept : host_(&host) {}

    /// Inject this frame's events. Before `RtsHost::frame()`.
    [[nodiscard]] cy::Status before_frame(cy::u64 frame) noexcept;
    /// Look at the result. After `RtsHost::frame()`.
    void after_frame(cy::u64 frame) noexcept;

    [[nodiscard]] const Findings& findings() const noexcept { return findings_; }

private:
    [[nodiscard]] cy::Status camera_steps(cy::u64 frame) noexcept;
    [[nodiscard]] cy::Status order_steps(cy::u64 frame) noexcept;
    [[nodiscard]] cy::Status button_steps(cy::u64 frame) noexcept;
    [[nodiscard]] cy::Status aim_at(cy::Vec3 point) noexcept;
    [[nodiscard]] cy::Status aim_at_second_unit() noexcept;

    RtsHost* host_;
    Findings findings_;
};

}  // namespace sample::rts

#endif  // CY_SAMPLE_RTS_API_SCRIPT_H
