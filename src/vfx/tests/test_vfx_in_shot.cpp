// PARTICLES IN THE ARTEFACT'S FRAME — the half of the claim every machine can settle. M11.c task
// 6.3, and `m11c:vfx-in-the-shot`.
//
// ================================================================================================
// WHAT THIS CASE IS FOR, AND WHAT IT DELIBERATELY IS NOT
// ================================================================================================
//
// The criterion is one sentence: *"particles are in the artefact's frame. An art-directed shot with
// no particles in it does not exercise `vfx-system`."* Two things have to be true for that, and
// only one of them needs a graphics device:
//
//   * the shot's effect PRODUCES a population, and that population is WHERE THE CAMERA IS LOOKING.
//     That is arithmetic — a simulation, a publication and a projection — and this case does it on
//     every machine, including the ones with no GPU.
//   * the frame DRAWS it. That needs a device, and it is `render.vfx`'s case of the same name,
//     which photographs the field and compares it against a committed reference.
//
// Splitting them is `m11c:material-table-is-nameable` / `m11c:material-texture-is-bound`'s
// arrangement, and it is here for the same reason: a device-only claim is a claim nobody can check
// on the machine they are reviewing on, and a device-free claim on its own cannot tell a published
// record from a drawn one.
//
// ================================================================================================
// IT PROJECTS RATHER THAN ASSUMING
// ================================================================================================
//
// "In the frame" is not "exists". A field of embers placed behind the camera, or thirty metres past
// the far pillar, is a field that simulates perfectly and photographs as nothing — and it would
// satisfy any assertion phrased as `particles > 0`. So this case builds THE ARTEFACT'S OWN camera
// out of `embers.h`'s constants — which `Shot::read` refuses to disagree with — projects every
// published record through it, and counts the ones whose clip coordinates are inside the volume.

#include "embers.h"

#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstdio>

using namespace cy;
using namespace cy::sample::beauty;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

/// The artefact's aspect: 1920x1080. The still is rendered at twice that and box-filtered down, so
/// the framing is the same number.
inline constexpr f32 kShotAspect = 1920.0F / 1080.0F;

/// `shot.cyshot` states the field of view HORIZONTALLY — a 50 mm lens on full frame — and the
/// projection wants it vertically. The same conversion `Stage::render_from` does, spelled the same
/// way so the two cannot disagree about the framing.
[[nodiscard]] f32 shot_fov_y() noexcept {
    constexpr f32 kHalfDegreesToRadians = 3.14159265358979F / 360.0F;
    return 2.0F * std::atan(std::tan(kShotFovDegrees * kHalfDegreesToRadians) / kShotAspect);
}

/// The artefact's view-projection, camera-relative: the camera at the origin, looking down the
/// offset to its target. Everything `publish_sprites` hands back is in exactly that space.
[[nodiscard]] Mat4 shot_view_projection() noexcept {
    const Vec3 forward{kShotTarget.x - kShotEye.x, kShotTarget.y - kShotEye.y,
                       kShotTarget.z - kShotEye.z};
    const Mat4 view = look_at(Vec3{0.0F, 0.0F, 0.0F}, forward, Vec3{0.0F, 1.0F, 0.0F});
    // The far plane `samples/12-beauty/stage.cpp` uses. Reversed-Z, so `w` is the view depth and a
    // point in front of the camera has a positive one.
    const Mat4 projection = perspective_reversed_z(shot_fov_y(), kShotAspect, kShotNearPlane,
                                                   600.0F);
    return projection * view;
}

/// Is this record inside the frame the artefact photographs? Clip-space, before the divide, which
/// is the only test that is correct for a point behind the camera.
[[nodiscard]] bool inside_frame(const Mat4& view_projection,
                               const rendering::particles::ParticleInstance& record) noexcept {
    const Vec4 clip = view_projection * Vec4{record.position[0], record.position[1],
                                             record.position[2], 1.0F};
    if (clip.w <= 0.0F) {
        return false;
    }
    return clip.x >= -clip.w && clip.x <= clip.w && clip.y >= -clip.w && clip.y <= clip.w &&
           clip.z >= 0.0F && clip.z <= clip.w;
}

}  // namespace

CY_TEST_CASE("particles are in the assembled frame: the shot's air simulates and frames") {
    EmberField field(allocator());
    CY_REQUIRE(field.build().has_value());

    // The cook is the shipping one — `compile_system`, the shipping node library, the shipping
    // precision selection. A cook that fell back to something else would make every number below a
    // number about a different program.
    CY_CHECK_EQ(field.cooked().emitters.size(), 1U);
    CY_CHECK_GT(field.cooked().kernels, 0U);
    // NOT A BISECTION BUILD: every optimisation pass ran. A cook with a pass switched off is a cook
    // of a different program, and the field below would be a field nobody ships.
    CY_CHECK_FALSE(field.cooked().bisection_build);

    CY_REQUIRE(field.settle(kShotEye).has_value());

    // THE POPULATION, AS A BAND AROUND A MEASURED NUMBER. Six seconds of thirty-Hertz spawning at
    // two a step against a four-and-a-half to seven second life, three instances of it: 996 motes
    // on the machine this was written on, and the simulation is a hash of the particle index so
    // that is the number every machine gets.
    //
    // A band and not `> 0`: an effect degraded to a handful of motes simulates, publishes and
    // passes any assertion phrased as "some". A band and not equality: the arithmetic is f32 and a
    // different rounding at a lifetime boundary legitimately moves the count by a mote.
    const u32 published = field.published().particles;
    std::fprintf(stderr, "the shot's air: %u mote(s) published, %u dropped, %u emitter(s)\n",
                 published, field.published().dropped, field.published().emitters);
    CY_CHECK_EQ(field.published().emitters, kEmberEmitterCount);
    CY_CHECK_GT(published, 900U);
    CY_CHECK_LT(published, 1100U);
    // The ring is bigger than the field, so a drop is the effect outgrowing its own budget rather
    // than the harness being small.
    CY_CHECK_EQ(field.published().dropped, 0U);

    // THE FRAMING. Every record projected through the artefact's own camera.
    const Mat4 view_projection = shot_view_projection();
    u32 framed = 0;
    u32 drawable = 0;
    f32 brightest = 0.0F;
    for (const rendering::particles::ParticleInstance& record : field.records()) {
        if (record.size > 0.0F && record.color[3] > 0.0F) {
            ++drawable;
        }
        brightest = record.color[0] > brightest ? record.color[0] : brightest;
        if (inside_frame(view_projection, record)) {
            ++framed;
        }
    }
    std::fprintf(stderr,
                 "the shot's air: %u of %u mote(s) inside the artefact's frame, %u drawable, "
                 "brightest red %.1f\n",
                 framed, published, drawable, static_cast<double>(brightest));

    // A MOTE WITH NO SIZE IS DRAWN AND COVERS NOTHING, which is what `ParticleInstance` says a dead
    // slot should cost. Every published record here is a live one, so every one of them has both.
    CY_CHECK_EQ(drawable, published);
    // A RADIANCE. `emission` is folded into the colour at publication, so a record whose red
    // channel is order-one is a record the tone curve will photograph as black — which is how an
    // effect can be in the frame and invisible.
    CY_CHECK_GT(brightest, 100.0F);
    // AND THE CLAIM ITSELF: the air is in front of the camera, not behind it and not past the far
    // pillar. Measured: 901 of 996, which is 90%. The floor is three fifths, because the emitters
    // are volumes and the motes at the edges of the slab legitimately fall outside a 42-degree
    // lens — but a field that had drifted behind the camera, or been placed past the far plane,
    // would fall under it at once.
    CY_CHECK_GT(framed, (published * 3U) / 5U);
}
