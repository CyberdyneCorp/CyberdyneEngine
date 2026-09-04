// Camera-relative rendering, and a scene a million units from the origin. Tasks 5.2 and 5.3.
//
// design.md §3: "**Camera-relative rendering lands with the first draw**, not when precision
// breaks. Positions reach the GPU relative to the camera, and the test is a scene one million units
// from the origin rendering without visible jitter. Retrofitting this at M6, when world partition
// puts real content at real distances, means revisiting every shader and every transform path that
// assumed world space."
//
// ================================================================================================
// WHAT "WITHOUT VISIBLE JITTER" IS, AS A NUMBER
// ================================================================================================
//
// Jitter is not a look, it is a DIFFERENCE between two frames that should be identical. So the test
// renders the same geometry twice — once at the origin and once a million units out, with the
// camera moved with it — and asserts the two images are IDENTICAL, texel for texel and depth for
// depth. Nothing is within a tolerance: camera-relative rendering means the GPU sees the same small
// numbers both times, so it must produce the same bits.
//
// The control matters as much as the assertion. The same case computes what the world-space path
// would have sent — the same positions without the subtraction — and asserts that those are NOT
// representable: at a million units an `f32` resolves to about 0.06, so two vertices 1 cm apart
// collapse onto each other. That is the failure this whole arrangement exists to prevent, and
// without the control the main assertion would pass just as happily in an engine that had no
// precision problem to solve.

#include "probe.h"

#include <cy/core/math/projection.h>

#include <cmath>

namespace {

using cy::render_test::ProbeFixture;
using cy::render_test::ProbeImage;
using cy::render_test::ProbeTriangle;

/// A world position in `f64`.
///
/// THE WHOLE POINT OF THE CASE IS THAT WORLD COORDINATES ARE NOT `f32`. A scene authored, streamed
/// and simulated a million units out holds its coordinates in something that can represent them;
/// the renderer's job is to get them to the GPU as small numbers. Materialising an `f32` world
/// position anywhere in between is exactly the defect being tested for, so this file never has one
/// except in the control, where it is the point.
struct WorldPoint {
    cy::f64 x = 0.0;
    cy::f64 y = 0.0;
    cy::f64 z = 0.0;
};

/// A small scene: three triangles at different depths, with centimetre-scale offsets between them —
/// detail that survives only if the numbers reaching the GPU are small.
void build_scene(WorldPoint origin, WorldPoint (&out)[3][3]) noexcept {
    const cy::f64 depths[3] = {3.0, 5.0, 8.0};
    for (cy::u32 index = 0; index < 3; ++index) {
        const cy::f64 z = -depths[index];
        const cy::f64 shift = static_cast<cy::f64>(index) * 0.01;  // one centimetre apart
        out[index][0] = WorldPoint{origin.x - 1.0 + shift, origin.y - 1.0, origin.z + z};
        out[index][1] = WorldPoint{origin.x + 1.0 + shift, origin.y - 1.0, origin.z + z};
        out[index][2] = WorldPoint{origin.x + shift, origin.y + 1.0, origin.z + z};
    }
}

/// The subtraction the renderer makes before anything reaches the GPU, in `f64` — which is the half
/// a shader cannot do for itself. `cy::rendering::build_gpu_light` does exactly this for a light.
cy::Vec3 relative_to(WorldPoint world, WorldPoint camera) noexcept {
    return cy::Vec3{static_cast<cy::f32>(world.x - camera.x),
                    static_cast<cy::f32>(world.y - camera.y),
                    static_cast<cy::f32>(world.z - camera.z)};
}

/// What the world-space path would have sent: the world position, rounded to `f32`.
cy::Vec3 as_world_f32(WorldPoint world) noexcept {
    return cy::Vec3{static_cast<cy::f32>(world.x), static_cast<cy::f32>(world.y),
                    static_cast<cy::f32>(world.z)};
}

/// Turn a scene into probe triangles, relative to a camera.
void to_triangles(const WorldPoint (&scene)[3][3], WorldPoint camera,
                  ProbeTriangle (&out)[3]) noexcept {
    const cy::f32 reds[3] = {1.0F, 0.6F, 0.3F};
    for (cy::u32 triangle = 0; triangle < 3; ++triangle) {
        for (cy::u32 vertex = 0; vertex < 3; ++vertex) {
            out[triangle].vertices[vertex] = relative_to(scene[triangle][vertex], camera);
        }
        out[triangle].color[0] = reds[triangle];
        out[triangle].color[1] = 0.0F;
        out[triangle].color[2] = 0.0F;
        out[triangle].color[3] = 1.0F;
    }
}

cy::Mat4 projection() noexcept {
    return cy::perspective_reversed_z(1.0471975512F, 1.0F, 0.1F, 100.0F);
}

constexpr cy::f64 kMillion = 1000000.0;

}  // namespace

CY_TEST_CASE("a scene one million units out renders identically to the same scene at the origin") {
    ProbeFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    CY_REQUIRE(fixture.prepare().has_value());

    // The scene at the origin, with the camera at the origin.
    const WorldPoint origin{0.0, 0.0, 0.0};
    WorldPoint near_scene[3][3];
    build_scene(origin, near_scene);
    ProbeTriangle at_origin[3];
    to_triangles(near_scene, origin, at_origin);

    // The same scene a million units out, with the camera moved with it. Every position is made
    // relative to that camera in `f64` before it reaches the GPU, so the vertex buffer holds the
    // same small numbers as the first case — which is the entire mechanism.
    const WorldPoint far_camera{1000000.0, 1000000.0, 1000000.0};
    WorldPoint far_scene[3][3];
    build_scene(far_camera, far_scene);
    ProbeTriangle far_relative[3];
    to_triangles(far_scene, far_camera, far_relative);

    ProbeImage near_image(fixture.allocator());
    ProbeImage far_image(fixture.allocator());
    CY_REQUIRE(fixture.render(cy::Span<const ProbeTriangle>(at_origin, 3), projection(), near_image)
                   .has_value());
    CY_REQUIRE(
        fixture.render(cy::Span<const ProbeTriangle>(far_relative, 3), projection(), far_image)
            .has_value());

    // IDENTICAL, not close. The GPU saw the same numbers, so it must have produced the same bits.
    cy::u32 differing_texels = 0;
    cy::u32 differing_depths = 0;
    for (cy::u32 index = 0; index < cy::render_test::kProbeTexels; ++index) {
        if (near_image.color[index] != far_image.color[index]) {
            ++differing_texels;
        }
        if (near_image.depth[index] != far_image.depth[index]) {
            ++differing_depths;
        }
    }
    CY_CHECK_EQ(differing_texels, 0U);
    CY_CHECK_EQ(differing_depths, 0U);

    // And something was actually drawn, so the comparison is not two empty images agreeing.
    cy::u32 lit = 0;
    for (cy::u32 index = 0; index < cy::render_test::kProbeTexels; ++index) {
        lit += near_image.color[index] != 0xFF000000U ? 1U : 0U;
    }
    CY_CHECK_GT(lit, 100U);
    CY_CHECK_EQ(fixture.device().statistics().validation_errors, 0U);
}

CY_TEST_CASE("the world-space path loses the detail that the camera-relative one keeps") {
    // THE CONTROL. Without it the case above would pass in an engine with no precision problem to
    // solve, and would say nothing about why the subtraction is there.
    //
    // No device needed: this is arithmetic about what an `f32` can represent, and it is what makes
    // the identical-images assertion above meaningful.
    const WorldPoint camera{kMillion, kMillion, kMillion};
    WorldPoint scene[3][3];
    build_scene(camera, scene);

    // World space, rounded to `f32`: the centimetre offsets between the three triangles are gone.
    // At a million the spacing of `f32` is about 0.0625, so a one-centimetre shift rounds to
    // nothing.
    CY_CHECK_EQ(as_world_f32(scene[0][0]).x, as_world_f32(scene[1][0]).x);
    CY_CHECK_EQ(as_world_f32(scene[1][0]).x, as_world_f32(scene[2][0]).x);

    // Camera-relative: the same offsets survive exactly, because the subtraction happens in `f64`
    // and only the small result is rounded to `f32`.
    const cy::f32 first = relative_to(scene[0][0], camera).x;
    const cy::f32 second = relative_to(scene[1][0], camera).x;
    const cy::f32 third = relative_to(scene[2][0], camera).x;
    CY_CHECK_NE(first, second);
    CY_CHECK_NE(second, third);
    CY_CHECK_NEAR(second - first, 0.01F, 1e-6F);
    CY_CHECK_NEAR(third - second, 0.01F, 1e-6F);
}

CY_TEST_CASE("the same camera-relative frame renders bit-identically twice") {
    // Jitter a player would see is a difference between two frames that should be the same. This is
    // the floor under that: with the same camera and the same scene a million units out, two frames
    // must be identical bit for bit — no accumulation, no dependence on which frame index it is, no
    // rounding that varies.
    //
    // It is the device-side half of design.md §6, and it is what makes a golden image at this
    // distance reproducible rather than nearly reproducible.
    ProbeFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    CY_REQUIRE(fixture.prepare().has_value());

    const WorldPoint camera{kMillion, 0.0, 0.0};
    WorldPoint scene[3][3];
    build_scene(camera, scene);
    ProbeTriangle relative[3];
    to_triangles(scene, camera, relative);

    ProbeImage images[2] = {ProbeImage(fixture.allocator()), ProbeImage(fixture.allocator())};
    for (ProbeImage& image : images) {
        CY_REQUIRE(fixture.render(cy::Span<const ProbeTriangle>(relative, 3), projection(), image)
                       .has_value());
    }

    cy::u32 differing = 0;
    for (cy::u32 index = 0; index < cy::render_test::kProbeTexels; ++index) {
        differing += images[0].color[index] != images[1].color[index] ? 1U : 0U;
        differing += images[0].depth[index] != images[1].depth[index] ? 1U : 0U;
    }
    CY_CHECK_EQ(differing, 0U);
    CY_CHECK_EQ(fixture.device().statistics().validation_errors, 0U);
}
