// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_THIRD_PERSON_SCRIPT_H
#define CY_SAMPLE_THIRD_PERSON_SCRIPT_H
// script.h — the scripted player: a hand on the keyboard, and nothing else.
//
// It presses and releases keys through `InputServer::inject`, the door a recorded replay uses; what
// a key MEANS — which way the hero goes, how fast, when it may jump, where the camera turns — is
// the Swift game's. One fixed tick a frame, 60 a second; the hero starts on the courtyard's
// flagstones south of the stairs, facing them, and the camera behind it:
//
//   frame    45-519    W             walk north: up the eight steps, onto the platform
//   frame   380        space (tap)   a jump on the platform, walking
//   frame   540-735    right arrow   turn the camera once round the hero and a quarter more:
//                                    past the pillars, and into the north wall at the platform's
//                                    back, which pulls it in; it ends looking east
//   frame   750-1129   W             walk east, the way the camera looks, toward the ramp
//   frame   800-1039   left shift    run: down the ramp, through the courtyard's east gap, up
//                                    the hill
//   frame   960        space (tap)   a jump, running onto the hill
//   frame  1135-1198   left arrow    stand on the slope and turn the camera to look back down
//   frame  1200        end

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include "tps_host.h"

namespace sample::tps {

class Player {
public:
    static constexpr cy::u64 kFrames = 1200;

    explicit Player(ThirdPersonHost& host) noexcept : host_(&host) {}

    /// Inject this frame's key edges. Before `ThirdPersonHost::frame()`.
    [[nodiscard]] cy::Status before_frame(cy::u64 frame) noexcept;

private:
    ThirdPersonHost* host_;
};

}  // namespace sample::tps

#endif  // CY_SAMPLE_THIRD_PERSON_SCRIPT_H
