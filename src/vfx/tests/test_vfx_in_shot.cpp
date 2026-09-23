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
#include <numbers>

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
    constexpr f32 kHalfDegreesToRadians = std::numbers::pi_v<f32> / 360.0F;
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
    const Mat4 projection =
        perspective_reversed_z(shot_fov_y(), kShotAspect, kShotNearPlane, 600.0F);
    return projection * view;
}

/// Is this record inside the frame the artefact photographs? Clip-space, before the divide, which
/// is the only test that is correct for a point behind the camera.
[[nodiscard]] bool inside_frame(const Mat4& view_projection,
                                const rendering::particles::ParticleInstance& record) noexcept {
    const Vec4 clip =
        view_projection * Vec4{record.position[0], record.position[1], record.position[2], 1.0F};
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
    // A RADIANCE, AND THE FLOOR IS THE ONE THIS EFFECT ALREADY FAILED. `emission` is folded into
    // the colour at publication, so the brightest record is `kEmberBirthRadiance`: measured
    // 35 811.6. The first version of this effect emitted 2 600, which is BELOW this shot's own sky
    // (about 1 800, 3 200, 5 000 of linear radiance — `embers.h` records the probe that measured
    // it), and the premultiplied blend therefore SUBTRACTED: the motes photographed as dirt on the
    // lens. A floor of "order one" would not have caught that and a floor of 10 000 does, because
    // it is above the sky in every channel.
    CY_CHECK_GT(brightest, 10'000.0F);
    // AND THE CLAIM ITSELF: the air is in front of the camera, not behind it and not past the far
    // pillar. Measured: 901 of 996, which is 90%. The floor is three fifths, because the emitters
    // are volumes and the motes at the edges of the slab legitimately fall outside a 42-degree
    // lens — but a field that had drifted behind the camera, or been placed past the far plane,
    // would fall under it at once.
    CY_CHECK_GT(framed, (published * 3U) / 5U);
}

CY_TEST_CASE("particles are in the assembled frame: every mote's trail is its own and in shot") {
    // THE TRAILS the published still draws behind every mote — `vfx-system`'s `Trail` renderer,
    // composited by `StripRenderer` — and the half of them every machine can check. `render.vfx`
    // photographs them; this case says they are the right SHAPE: one strip a mote, beginning where
    // the mote is drawn, as long as the mote's last third of a second and no longer, and framed.
    EmberField field(allocator());
    CY_REQUIRE(field.build().has_value());
    CY_REQUIRE(field.settle(kShotEye).has_value());

    const Span<const rendering::particles::StripVertex> trails = field.trails();
    const rendering::particles::StripReport counted = rendering::particles::count_strips(trails);
    std::fprintf(stderr,
                 "the shot's trails: %u vertices in %u strip(s), %u segment(s); %u mote(s) "
                 "published; %u trail(s) derived, %u dropped\n",
                 counted.vertices, counted.strips, counted.segments, field.published().particles,
                 field.trailed().primitives, field.trailed().base.dropped);
    CY_CHECK_EQ(field.trailed().base.dropped, 0U);
    CY_CHECK_EQ(counted.strips, field.trailed().primitives);
    // Every mote that has lived through the whole trail window has a full trail; the youngest
    // third of a second's spawns have shorter ones or none. So most motes, never more than all.
    CY_CHECK_GT(counted.strips, (field.published().particles * 4U) / 5U);
    CY_CHECK_LE(counted.strips, field.published().particles);
    CY_CHECK_LE(counted.vertices, counted.strips * kEmberTrailHistory);

    // ONE MOTE'S TRAIL, NOT TWO MOTES JOINED. Every vertex of a strip is within the distance the
    // head's mote can have flown in the window — at most 0.6 m/s for 0.4 s is 0.24 m, and the
    // bound is a generous 0.5 m. The two defects this rung fixed each broke it by metres: a
    // history keyed by slot alone joined the three emitters' motes across the courtyard, and a
    // slot re-used between two trail publications joined a new mote to where the old one died.
    f32 longest = 0.0F;
    u32 strip = ~0U;
    Vec3 head{};
    for (const rendering::particles::StripVertex& vertex : trails) {
        if (vertex.strip != strip) {
            strip = vertex.strip;
            head = Vec3{vertex.position[0], vertex.position[1], vertex.position[2]};
            continue;
        }
        const f32 dx = vertex.position[0] - head.x;
        const f32 dy = vertex.position[1] - head.y;
        const f32 dz = vertex.position[2] - head.z;
        const f32 distance = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
        longest = distance > longest ? distance : longest;
    }
    std::fprintf(stderr, "the shot's trails: the longest reaches %.3f m behind its mote\n",
                 static_cast<double>(longest));
    CY_CHECK_GT(longest, 0.01F);
    CY_CHECK_LT(longest, 0.5F);

    // FRAMED: the heads are where the motes are, so the same projection that counts motes counts
    // trails, and the same three-fifths floor applies.
    const Mat4 view_projection = shot_view_projection();
    u32 framed = 0;
    strip = ~0U;
    for (const rendering::particles::StripVertex& vertex : trails) {
        if (vertex.strip == strip) {
            continue;
        }
        strip = vertex.strip;
        rendering::particles::ParticleInstance probe;
        for (u32 component = 0; component < 3U; ++component) {
            probe.position[component] = vertex.position[component];
        }
        framed += inside_frame(view_projection, probe) ? 1U : 0U;
    }
    std::fprintf(stderr, "the shot's trails: %u of %u inside the artefact's frame\n", framed,
                 counted.strips);
    CY_CHECK_GT(framed, (counted.strips * 3U) / 5U);
}
