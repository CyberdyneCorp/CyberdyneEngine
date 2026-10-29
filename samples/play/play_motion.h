// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_PLAY_PLAY_MOTION_H
#define CY_SAMPLE_PLAY_PLAY_MOTION_H
// play_motion.h — where a moving thing is DRAWN when a frame falls between two ticks.
//
// `CyTime::interpolation` is how far past the last tick the frame is, in [0, 1). The thing is
// carried forward from where the last tick left it by its motion over that tick: the step from
// `before` to `now`, times `ahead`. A frame on its tick — `ahead` zero, every frame of a scripted
// run — is `now` exactly, which is what keeps a scripted run's pictures and digests what they
// were. Header-only and over the math alone, so a game host uses it whether or not the play host is
// in the build.

#include <cy/core/math/quat.h>
#include <cy/core/math/transform.h>
#include <cy/core/math/vec.h>

namespace cy::play {

/// `now`, carried forward by `ahead` of the step that took `before` to it.
[[nodiscard]] inline Vec3 carried_forward(Vec3 before, Vec3 now, f32 ahead) noexcept {
    if (ahead <= 0.0F) {
        return now;
    }
    return now + ((now - before) * ahead);
}

/// The same for a placement: its turn carried along the same arc.
[[nodiscard]] inline Transform carried_forward(const Transform& before, const Transform& now,
                                               f32 ahead) noexcept {
    if (ahead <= 0.0F) {
        return now;
    }
    Transform out = now;
    out.translation = carried_forward(before.translation, now.translation, ahead);
    out.rotation = nlerp(before.rotation, now.rotation, 1.0F + ahead);
    return out;
}

}  // namespace cy::play

#endif  // CY_SAMPLE_PLAY_PLAY_MOTION_H
