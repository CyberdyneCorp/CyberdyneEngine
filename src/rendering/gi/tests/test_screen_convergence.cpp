// SPDX-License-Identifier: MIT
// After a light moves, the frame converges on the new light within a stated frame count: the
// convergence bar of the per-pixel resolve and its denoiser (issue #35, stage 4), on the host.
//
// SMOKE, AND WHY THIS CASE LEFT `integration.render_gi_screen`. The claim is a frame count, so
// every frame is a whole one — the system's update, the resolve and the denoiser — and the case
// needs two converged scenes before it can count: the room under the moved light from the start, as
// the reference, and the room under the old light, as the start. That is ninety-one full frames. It
// spent 1098 ms of CPU against the integration budget of 1000 on `test linux-x86_64` (CI run
// 38534736174) and 4836 ms against Debug's 4000 in `profiles` (CI run 38506194364); measured on the
// workstation, 620 ms, of which the GI update was 281, the resolve 135 and the denoiser 169. None
// of that is work the claim can drop, so the case is in the suite whose budget fits it.
//
// WHAT IT NO LONGER PAYS FOR is the specular signal. Only the diffuse image is compared, and the
// denoiser keeps each signal's history apart, so both scenes run with `specular_enabled` off: the
// same diffuse frames, the same 11-frame answer, 50 ms of the denoiser less.

#include <cy/test/test.h>

#include <cy/rendering/gi/system.h>

#include "screen_scene.h"
#include "support.h"

#include <cstdio>
#include <vector>

namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;
using namespace cy::rendering::gi;  // NOLINT(google-build-using-namespace)

using gi_support::image_difference;
using gi_support::make_room;
using gi_support::ScreenScene;

}  // namespace

CY_TEST_CASE(
    "after a light moves, the frame converges on the new light within a stated frame count") {
    // The scene converged under one light, the light moved across the room, and the region filed
    // as invalidated with the scene and with the resolve's history, as a renderer does. Measured
    // against the same scene converged under the moved light from the start.
    const Vec3 moved{2.4F, 1.2F, -2.6F};
    ScreenScene settled;
    settled.specular_enabled = false;
    make_room(settled);
    settled.lights[0].position = moved;
    settled.run(0, 48);
    const std::vector<Vec3> target = settled.copy_diffuse();

    ScreenScene scene;
    scene.specular_enabled = false;
    make_room(scene);
    scene.run(0, 32);
    const f32 before = image_difference(scene.copy_diffuse(), target, scene.view.depth);
    const cy::Aabb room = cy::Aabb::from_center_extents(
        Vec3{0.0F, 0.0F, 0.0F},
        Vec3{gi_support::kRoomX + 0.5F, gi_support::kRoomY + 0.5F, gi_support::kRoomZ + 0.5F});
    scene.lights[0].position = moved;
    scene.system.scene().invalidate(room, InvalidationCause::LightChanged, scene.lights[0].id, 32);
    scene.invalidated.push_back(room);
    constexpr f32 kWithin = 0.10F;
    u32 frames = 0;
    f32 difference = before;
    for (u64 frame = 32; frame < 32 + 40; ++frame) {
        scene.step(frame);
        frames += 1;
        difference = image_difference(scene.copy_diffuse(), target, scene.view.depth);
        if (difference <= kWithin) {
            break;
        }
    }
    std::fprintf(stderr,
                 "a light moved 4.9 m: %.3f from the new light's converged frame before, within "
                 "%.2f after %u frames (%.3f)\n",
                 static_cast<double>(before), static_cast<double>(kWithin), frames,
                 static_cast<double>(difference));
    // MEASURED: 11 frames.
    CY_CHECK_GT(before, 0.3F);
    CY_CHECK_LE(difference, kWithin);
    CY_CHECK_LE(frames, 14U);
}
