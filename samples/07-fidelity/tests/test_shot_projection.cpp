// SPDX-License-Identifier: MIT
// The shot's view-projection is the same bits in every build profile.
// `unit.fidelity_shot_projection`.
//
// WHY THIS EXISTS. M11.c's fourth close found `render.virtual_geometry_shaded` red in the RELEASE
// profile only: 2 of 57,600 texels, worst channel delta 10 at (297, 14), both off any high-contrast
// edge. Debug, dev and profile matched the committed reference bit for bit. At both texels the
// hardware rasteriser had given the pixel to the other triangle of a shared edge, because the
// frame's `world_to_clip` differed from dev's by one or two ulps in every element that carries the
// focal length. The camera was identical.
//
// The cause was the tangent of the field of view. In dev `render_frames` calls
// `perspective_reversed_z_infinite`, which calls glibc's `tanf` at run time. In release, link-time
// optimisation specialised `render_frames` for the suite's constant options, inlined the
// projection and let GCC fold `tan(0.5236)` at compile time through MPFR. glibc 2.39 answers
// 0x1.279a76p-1 and MPFR answers 0x1.279a74p-1. Neither the shading nor the rasteriser was at
// fault.
//
// WHAT IS ASSERTED. The matrix the frame renders with equals the one built from a field of view
// read at run time, which is what every profile but release already computed and what the
// committed reference was photographed with. The expected side reads the field of view through a
// volatile for the same reason `shot_world_to_clip` does: that is the part under test.
//
// PROVED RED. With `shot_world_to_clip` passing `kShotFovY` straight to the projection, this case
// fails in the release profile (`build/m11c-rel-vg`, GCC 13.3, glibc 2.39). At t = 0 and t = 0.5,
// 7 of the 16 elements differ, all in the x and y rows, which are the only two the focal length
// enters. At t = 0, element (1, 1) is 0x3fdd89e2 where the run-time field of view gives
// 0x3fdd89e0. The same mutation turns `render.virtual_geometry_shaded` red again with the original
// numbers (2 differing, 2 off edge, worst delta 10 at (297, 14)). The case passes in dev either
// way, because dev never folds the call. The defect lives in the release profile, and that is a
// profile `m1:four-profiles` runs this suite in.
//
// It needs no device and cooks nothing. `camera_at` and `camera_target` do not read the scene, so
// an empty one is enough.

#include <cy/test/test.h>

#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>

#include "frame.h"
#include "scene.h"

#include <cstdio>
#include <cstring>

namespace {

using cy::f32;
using cy::Mat4;
using cy::u32;
using cy::sample::fidelity::Scene;

/// The golden suite's viewport, 320x180, and the capture recipe's, 1280x720. Both are 16:9, so it
/// is one aspect, and it is spelled as the division `render_frames` performs.
constexpr u32 kWidth = 320;
constexpr u32 kHeight = 180;

/// The same product `shot_world_to_clip` forms, with the field of view read at run time.
Mat4 expected_world_to_clip(const Scene& scene, f32 t, f32 aspect) noexcept {
    const volatile f32 fov_y = cy::sample::fidelity::kShotFovY;
    const Mat4 view =
        cy::look_at(cy::sample::fidelity::camera_at(scene, t),
                    cy::sample::fidelity::camera_target(scene, t), cy::Vec3{0.0F, 1.0F, 0.0F});
    return cy::perspective_reversed_z_infinite(fov_y, aspect, 0.05F) * view;
}

u32 bits(f32 value) noexcept {
    u32 out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

}  // namespace

CY_TEST_CASE("the shot's world-to-clip is the matrix a run-time field of view gives") {
    Scene scene(cy::system_allocator(cy::MemoryDomain::Renderer));
    const f32 aspect = static_cast<f32>(kWidth) / static_cast<f32>(kHeight);

    // t = 0 is the golden frame; 0.5 and 1 are the first and last frames outside the hall.
    for (const f32 t : {0.0F, 0.5F, 1.0F}) {
        const Mat4 frame = cy::sample::fidelity::shot_world_to_clip(scene, t, aspect);
        const Mat4 expected = expected_world_to_clip(scene, t, aspect);
        u32 differing = 0;
        for (u32 column = 0; column < 4U; ++column) {
            for (u32 row = 0; row < 4U; ++row) {
                const f32 got = frame.at(row, column);
                const f32 want = expected.at(row, column);
                if (bits(got) != bits(want)) {
                    ++differing;
                    std::fprintf(stderr, "t=%.2f (%u, %u): frame 0x%08x, run-time fov 0x%08x\n",
                                 static_cast<double>(t), row, column, bits(got), bits(want));
                }
            }
        }
        CY_CHECK(differing == 0U);
    }
}
