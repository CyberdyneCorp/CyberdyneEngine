// SPDX-License-Identifier: MIT
// Irradiance volumes: capture, sampling, visibility and the update policy.
// `integration.render_gi_volume`.
//
// Every case captures real rays through a real scene — `BoxProxyScene`, or the engine's own GI room
// through `IlluminationSystem`'s world tracer and surface cache — and none of them hand the volume
// a radiance the test wrote. Tolerances are stated where they are used, with the measured value.

#include <cy/test/test.h>

#include <cy/rendering/gi/irradiance_volume.h>
#include <cy/rendering/gi/proxy_scene.h>
#include <cy/rendering/gi/system.h>

#include "support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

namespace {

using cy::Aabb;
using cy::f32;
using cy::f64;
using cy::u32;
using cy::u64;
using cy::Vec3;
using namespace cy::rendering::gi;  // NOLINT(google-build-using-namespace)

constexpr Vec3 kUp{0.0F, 1.0F, 0.0F};
constexpr Vec3 kWhite{0.75F, 0.75F, 0.75F};
constexpr Vec3 kRed{0.85F, 0.08F, 0.06F};

Aabb box(Vec3 lo, Vec3 hi) noexcept {
    return Aabb::from_min_max(lo, hi);
}

SkyTerm uniform_sky(f32 value) noexcept {
    SkyTerm sky;
    sky.zenith = Vec3{value, value, value};
    sky.horizon = sky.zenith;
    sky.ground = sky.zenith;
    sky.intensity = 1.0F;
    return sky;
}

/// The sun: from above and, by default, from +x, travelling toward -x. Its z component is zero, so
/// a wall facing +z receives no direct light at all and shows the ambient term alone.
GiLight sun(f32 travel_x = -0.6F) noexcept {
    GiLight light;
    light.directional = true;
    light.direction = cy::normalize(Vec3{travel_x, -0.8F, 0.0F});
    light.colour = Vec3{1.0F, 1.0F, 1.0F};
    light.intensity = 1.0F;
    light.id = 1;
    return light;
}

/// An open-topped corner: a white floor, a red wall facing +x at x = -3.5, and a white back wall
/// facing +z at z = -5.5 that the sun does not reach, running 21 m to x = 17.5.
struct Corner {
    BoxProxyScene scene;
    GiLight light = sun();
    IrradianceVolume volume;

    Corner() {
        CY_REQUIRE(scene.add(ProxyBox{box({-6.0F, -1.0F, -6.0F}, {18.0F, 0.0F, 6.0F}), kWhite})
                       .has_value());
        CY_REQUIRE(
            scene.add(ProxyBox{box({-4.0F, 0.0F, -6.0F}, {-3.5F, 4.0F, 6.0F}), kRed}).has_value());
        CY_REQUIRE(scene.add(ProxyBox{box({-3.5F, 0.0F, -6.0F}, {17.5F, 4.0F, -5.5F}), kWhite})
                       .has_value());
        scene.set_lights({&light, 1});
        IrradianceVolumeSettings settings;
        settings.origin = Vec3{-3.25F, 0.25F, -5.25F};
        settings.spacing_metres = 1.0F;
        settings.count_x = 16;
        settings.count_y = 4;
        settings.count_z = 9;
        CY_REQUIRE(volume.configure(settings).has_value());
    }

    [[nodiscard]] VolumeCaptureContext context() const noexcept {
        VolumeCaptureContext capture;
        capture.tracer = &scene;
        capture.radiance = &scene;
        capture.sky = uniform_sky(0.05F);
        return capture;
    }
};

f32 redness(Vec3 radiance) noexcept {
    return radiance.x / std::max(radiance.y, 1.0e-6F);
}

f32 luminance(Vec3 radiance) noexcept {
    return (0.2126F * radiance.x) + (0.7152F * radiance.y) + (0.0722F * radiance.z);
}

/// THE CPU REFERENCE: the radiance arriving at `position`, integrated over a stratified
/// latitude-longitude grid of the whole sphere with its solid-angle weights — a different set of
/// directions from the capture's Fibonacci set, and its own quadrature. It gives two answers for a
/// normal: the EXACT cosine-weighted mean, and the same field's SH L1 projection by quadrature,
/// evaluated with the L1 irradiance convolution.
class ReferenceProbe {
public:
    ReferenceProbe(const BoxProxyScene& scene, const SkyTerm& sky, Vec3 position) {
        constexpr u32 kTheta = 96;
        constexpr u32 kPhi = 192;
        const f32 pi = std::numbers::pi_v<f32>;
        for (u32 row = 0; row < kTheta; ++row) {
            const f32 theta = (static_cast<f32>(row) + 0.5F) * pi / kTheta;
            const f32 solid = std::sin(theta) * (pi / kTheta) * (2.0F * pi / kPhi);
            for (u32 column = 0; column < kPhi; ++column) {
                const f32 phi = (static_cast<f32>(column) + 0.5F) * 2.0F * pi / kPhi;
                const Vec3 direction{std::sin(theta) * std::cos(phi), std::cos(theta),
                                     std::sin(theta) * std::sin(phi)};
                const Vec3 radiance = arriving(scene, sky, position, direction);
                samples_.push_back(Sample{direction, radiance, solid});
                const f32 basis[4] = {0.282095F, 0.488603F * direction.y, 0.488603F * direction.z,
                                      0.488603F * direction.x};
                for (u32 coefficient = 0; coefficient < 4U; ++coefficient) {
                    payload_[(coefficient * 3U) + 0U] += radiance.x * basis[coefficient] * solid;
                    payload_[(coefficient * 3U) + 1U] += radiance.y * basis[coefficient] * solid;
                    payload_[(coefficient * 3U) + 2U] += radiance.z * basis[coefficient] * solid;
                }
            }
        }
    }

    [[nodiscard]] Vec3 exact(Vec3 normal) const {
        Vec3 total{0.0F, 0.0F, 0.0F};
        for (const Sample& sample : samples_) {
            const f32 cosine = cy::dot(sample.direction, normal);
            if (cosine > 0.0F) {
                total = total + (sample.radiance * (cosine * sample.solid));
            }
        }
        return total / std::numbers::pi_v<f32>;
    }

    [[nodiscard]] Vec3 projected(Vec3 normal) const {
        return decode_payload(ProbeEncoding::SphericalHarmonicsL1, payload_, normal);
    }

private:
    struct Sample {
        Vec3 direction;
        Vec3 radiance;
        f32 solid;
    };

    static Vec3 arriving(const BoxProxyScene& scene, const SkyTerm& sky, Vec3 position,
                         Vec3 direction) {
        SceneHit hit;
        if (!scene.trace(position + (direction * 1.0e-3F), direction, 40.0F, hit)) {
            return sky.radiance(direction);
        }
        Vec3 radiance{0.0F, 0.0F, 0.0F};
        u32 age = 0;
        if (cy::dot(hit.normal, direction) < 0.0F) {
            (void)scene.radiance_at(hit.position, hit.normal, radiance, age);
        }
        return radiance;
    }

    std::vector<Sample> samples_;
    f32 payload_[12] = {};
};

}  // namespace

CY_TEST_CASE("under a uniform sky every probe holds exactly that sky, in the frame's units") {
    BoxProxyScene empty;
    IrradianceVolume volume;
    IrradianceVolumeSettings settings;
    settings.count_x = 2;
    settings.count_y = 2;
    settings.count_z = 2;
    CY_REQUIRE(volume.configure(settings).has_value());
    VolumeCaptureContext context;
    context.tracer = &empty;
    context.radiance = &empty;
    context.sky = uniform_sky(0.4F);
    (void)volume.capture_all(context);
    // THE UNIT: a uniform sky of radiance L gives back L for every normal, which is what makes the
    // volume a drop-in for the frame's flat ambient where nothing but sky is visible.
    f32 worst = 0.0F;
    const Vec3 normals[] = {kUp, Vec3{0.0F, -1.0F, 0.0F}, Vec3{1.0F, 0.0F, 0.0F},
                            cy::normalize(Vec3{1.0F, 1.0F, -1.0F})};
    for (u32 probe = 0; probe < volume.probe_count(); ++probe) {
        for (const Vec3 normal : normals) {
            worst = std::max(worst, std::fabs(volume.probe_radiance(probe, normal).y - 0.4F));
        }
    }
    std::fprintf(stderr, "uniform sky: worst probe error %.3g of 0.4\n",
                 static_cast<double>(worst));
    // MEASURED 1.2e-4: the Fibonacci set's residual first moment. 1e-2 is 2.5% of the value.
    CY_CHECK_LT(worst, 1.0e-2F);
}

CY_TEST_CASE("a probe's irradiance agrees with a CPU reference integration of the same scene") {
    Corner corner;
    const VolumeCaptureContext context = corner.context();
    (void)corner.volume.capture_all(context);

    const Vec3 normals[] = {kUp,
                            Vec3{0.0F, -1.0F, 0.0F},
                            Vec3{1.0F, 0.0F, 0.0F},
                            Vec3{-1.0F, 0.0F, 0.0F},
                            Vec3{0.0F, 0.0F, 1.0F},
                            Vec3{0.0F, 0.0F, -1.0F},
                            cy::normalize(Vec3{1.0F, 1.0F, 1.0F}),
                            cy::normalize(Vec3{-1.0F, 1.0F, -1.0F}),
                            cy::normalize(Vec3{-1.0F, -1.0F, 1.0F}),
                            cy::normalize(Vec3{1.0F, -1.0F, -1.0F})};
    // Three probes: beside the red wall, in the corner, and out in the open.
    const u32 probes[] = {corner.volume.probe_index(0, 0, 4), corner.volume.probe_index(0, 1, 0),
                          corner.volume.probe_index(8, 2, 7)};
    f32 worst_projection = 0.0F;
    f32 worst_exact = 0.0F;
    for (const u32 index : probes) {
        const VolumeProbe& probe = corner.volume.probe(index);
        CY_CHECK_GT(probe.validity, 0.0F);
        const ReferenceProbe reference(corner.scene, context.sky, probe.position);
        // Errors are stated against the probe's mean irradiance over the normals, so a normal that
        // faces a dark wall is not held to a relative error its tiny value cannot carry.
        f32 scale = 0.0F;
        for (const Vec3 normal : normals) {
            scale += luminance(reference.exact(normal));
        }
        scale /= static_cast<f32>(std::size(normals));
        for (const Vec3 normal : normals) {
            const Vec3 answer = corner.volume.probe_radiance(index, normal);
            const f32 projection =
                cy::max_component(cy::cwise_abs(answer - reference.projected(normal)));
            const f32 exact = cy::max_component(cy::cwise_abs(answer - reference.exact(normal)));
            worst_projection = std::max(worst_projection, projection / scale);
            worst_exact = std::max(worst_exact, exact / scale);
        }
    }
    std::fprintf(stderr,
                 "reference integration, worst error of the probe's mean irradiance: %.3g against "
                 "the reference's own L1 projection, %.3g against the exact cosine integral\n",
                 static_cast<double>(worst_projection), static_cast<double>(worst_exact));
    // THE CAPTURE: 256 Fibonacci rays against 18 432 quadrature cells, projected onto the same four
    // functions. MEASURED 0.044 at the default 256 rays and 0.0093 at 1024: the residual is the
    // capture's own sampling of the hard edges where walls meet sky, and it falls as rays are
    // added.
    CY_CHECK_LT(worst_projection, 0.06F);
    // THE ENCODING: SH L1 cannot hold the second-order structure of a field that is bright above
    // (sky) and below (a sunlit floor) and darker at the horizon. MEASURED 0.249 (0.233 at 1024
    // rays, so not sampling): that truncation is the difference from the exact integral, and the
    // bound records it so a regression past it shows.
    CY_CHECK_LT(worst_exact, 0.3F);
}

CY_TEST_CASE("a red wall bleeds onto the white surfaces near it and not onto those far away") {
    Corner corner;
    (void)corner.volume.capture_all(corner.context());
    const Vec3 flat{0.05F, 0.05F, 0.05F};
    const Vec3 facing{0.0F, 0.0F, 1.0F};

    // The unlit back wall, 0.5 m from the red wall and 14 m from it; and the floor at the same two
    // distances.
    const Vec3 wall_near = corner.volume.ambient(Vec3{-3.0F, 1.0F, -5.5F}, facing, flat);
    const Vec3 wall_far = corner.volume.ambient(Vec3{10.5F, 1.0F, -5.5F}, facing, flat);
    const Vec3 floor_near = corner.volume.ambient(Vec3{-3.0F, 0.0F, -2.0F}, kUp, flat);
    const Vec3 floor_far = corner.volume.ambient(Vec3{10.5F, 0.0F, -2.0F}, kUp, flat);
    std::fprintf(stderr,
                 "redness (red/green) of the ambient: back wall near %.3f far %.3f, floor near "
                 "%.3f far %.3f; the flat ambient's is %.3f\n",
                 static_cast<double>(redness(wall_near)), static_cast<double>(redness(wall_far)),
                 static_cast<double>(redness(floor_near)), static_cast<double>(redness(floor_far)),
                 static_cast<double>(redness(flat)));
    CY_CHECK_GT(redness(wall_near), 1.5F);
    CY_CHECK_GT(redness(floor_near), 1.5F);
    CY_CHECK_LT(redness(wall_far), 1.15F);
    CY_CHECK_LT(redness(floor_far), 1.15F);
}

CY_TEST_CASE("a wall the sun never reaches is lit by bounce, where the flat ambient is uniform") {
    Corner corner;
    (void)corner.volume.capture_all(corner.context());
    const Vec3 flat{0.05F, 0.05F, 0.05F};
    const Vec3 facing{0.0F, 0.0F, 1.0F};
    f32 lowest = INFINITY;
    f32 highest = 0.0F;
    f32 bottom = 0.0F;
    f32 top = 0.0F;
    for (u32 row = 0; row < 4U; ++row) {
        for (u32 column = 0; column < 8U; ++column) {
            const Vec3 point{-2.5F + static_cast<f32>(column), 0.25F + static_cast<f32>(row),
                             -5.5F};
            const f32 value = luminance(corner.volume.ambient(point, facing, flat));
            lowest = std::min(lowest, value);
            highest = std::max(highest, value);
            bottom += row == 0U ? value : 0.0F;
            top += row == 3U ? value : 0.0F;
        }
    }
    std::fprintf(stderr,
                 "unlit back wall: ambient %.4f to %.4f (flat %.4f); bottom row %.4f, top row "
                 "%.4f\n",
                 static_cast<double>(lowest), static_cast<double>(highest),
                 static_cast<double>(luminance(flat)), static_cast<double>(bottom / 8.0F),
                 static_cast<double>(top / 8.0F));
    // Lit by the sunlit floor in front of it: brighter than sky alone everywhere, brightest at its
    // foot where the floor fills most of its view, and not uniform.
    CY_CHECK_GT(lowest, luminance(flat));
    CY_CHECK_GT(bottom, top);
    CY_CHECK_GT(highest / lowest, 1.2F);
}

CY_TEST_CASE("a probe inside geometry weighs nothing, and a probe beyond a wall does not leak") {
    // A closed hut with 0.5 m walls in a sunlit yard. Its inside sees no sky and no sun.
    BoxProxyScene scene;
    CY_REQUIRE(
        scene.add(ProxyBox{box({-6.0F, -1.0F, -6.0F}, {6.0F, 0.0F, 6.0F}), kWhite}).has_value());
    CY_REQUIRE(
        scene.add(ProxyBox{box({0.0F, 0.0F, 0.0F}, {0.5F, 3.0F, 4.0F}), kWhite}).has_value());
    CY_REQUIRE(
        scene.add(ProxyBox{box({3.5F, 0.0F, 0.0F}, {4.0F, 3.0F, 4.0F}), kWhite}).has_value());
    CY_REQUIRE(
        scene.add(ProxyBox{box({0.0F, 0.0F, 0.0F}, {4.0F, 3.0F, 0.5F}), kWhite}).has_value());
    CY_REQUIRE(
        scene.add(ProxyBox{box({0.0F, 0.0F, 3.5F}, {4.0F, 3.0F, 4.0F}), kWhite}).has_value());
    CY_REQUIRE(
        scene.add(ProxyBox{box({0.0F, 2.5F, 0.0F}, {4.0F, 3.0F, 4.0F}), kWhite}).has_value());
    // The sun from -x this time, so the yard side of the hut's left wall is lit and a probe beside
    // it sees a bright wall: exactly the probe whose light must not reach the inside.
    const GiLight light = sun(0.6F);
    scene.set_lights({&light, 1});

    IrradianceVolume volume;
    IrradianceVolumeSettings settings;
    // Probes along x at -0.35 (the yard, 0.35 m from the wall), 1.15 and 2.65 (inside), 4.15 (the
    // yard again); and a second grid whose x = 0.25 column is inside the wall itself.
    settings.origin = Vec3{-0.35F, 0.75F, 1.25F};
    settings.spacing_metres = 1.5F;
    settings.count_x = 4;
    settings.count_y = 2;
    settings.count_z = 2;
    CY_REQUIRE(volume.configure(settings).has_value());
    VolumeCaptureContext context;
    context.tracer = &scene;
    context.radiance = &scene;
    context.sky = uniform_sky(0.05F);
    (void)volume.capture_all(context);

    const Vec3 inward{1.0F, 0.0F, 0.0F};
    const Vec3 query{0.6F, 1.0F, 1.5F};
    // What a trilinear blend with no visibility term would give: the weights alone.
    const Vec3 grid = (query + (inward * settings.normal_offset_metres) - settings.origin) /
                      settings.spacing_metres;
    const f32 tx = grid.x;
    const f32 ty = grid.y;
    const f32 tz = grid.z;
    Vec3 naive{0.0F, 0.0F, 0.0F};
    for (u32 corner = 0; corner < 8U; ++corner) {
        const u32 x = corner & 1U;
        const u32 y = (corner >> 1U) & 1U;
        const u32 z = (corner >> 2U) & 1U;
        const f32 weight =
            (x != 0U ? tx : 1.0F - tx) * (y != 0U ? ty : 1.0F - ty) * (z != 0U ? tz : 1.0F - tz);
        naive = naive + (volume.probe_radiance(volume.probe_index(x, y, z), inward) * weight);
    }
    const Vec3 answer = volume.gather(query, inward);
    std::fprintf(stderr, "inside the hut by its wall: %.4g with visibility, %.4g without\n",
                 static_cast<double>(luminance(answer)), static_cast<double>(luminance(naive)));
    CY_CHECK_GT(luminance(naive), 0.02F);
    CY_CHECK_LT(luminance(answer), 0.05F * luminance(naive));

    // A probe inside the wall: every ray it casts leaves through the wall's inside.
    settings.origin = Vec3{0.25F, 0.75F, 1.25F};
    CY_REQUIRE(volume.configure(settings).has_value());
    const VolumeUpdateReport report = volume.capture_all(context);
    CY_CHECK_EQ(volume.probe(volume.probe_index(0, 0, 0)).validity, 0.0F);
    CY_CHECK_GT(volume.probe(volume.probe_index(1, 0, 0)).validity, 0.0F);
    CY_CHECK_EQ(report.invalid_probes, 4U);
}

CY_TEST_CASE("the update policy captures within its budget, invalidated probes first") {
    Corner corner;
    IrradianceVolumeSettings settings = corner.volume.settings();
    settings.count_x = 4;
    settings.count_y = 4;
    settings.count_z = 4;
    settings.probes_per_update = 16;
    settings.rays_per_probe = 32;
    CY_REQUIRE(corner.volume.configure(settings).has_value());
    VolumeCaptureContext context = corner.context();

    // Nothing is lit by a probe that was never captured.
    CY_CHECK_EQ(corner.volume.queue_depth(), 64U);
    CY_CHECK_EQ(corner.volume.sample(Vec3{-2.0F, 1.0F, -4.0F}, kUp).weight, 0.0F);

    u32 updates = 0;
    while (corner.volume.queue_depth() > 0 && updates < 10) {
        context.frame = ++updates;
        const VolumeUpdateReport report = corner.volume.update(context);
        CY_CHECK_LE(report.probes_captured, 16U);
    }
    CY_CHECK_EQ(updates, 4U);
    // OnInvalidation: a static scene costs nothing after its bake.
    const u64 generation = corner.volume.generation();
    CY_CHECK_EQ(corner.volume.update(context).probes_captured, 0U);
    CY_CHECK_EQ(corner.volume.generation(), generation);

    // A change near the red wall queues the probes whose cells it touches, and only those.
    const u32 queued = corner.volume.invalidate(box({-3.5F, 0.0F, -5.0F}, {-3.0F, 1.0F, -4.5F}));
    CY_CHECK_GT(queued, 0U);
    CY_CHECK_LT(queued, 64U);
    context.frame = 100;
    const VolumeUpdateReport serviced = corner.volume.update(context);
    CY_CHECK_EQ(serviced.probes_captured, std::min(queued, 16U));
    for (u32 index = 0; index < corner.volume.probe_count(); ++index) {
        const VolumeProbe& probe = corner.volume.probe(index);
        const bool refreshed = probe.captured_frame == 100U;
        const bool near = probe.position.x <= -2.0F && probe.position.z <= -3.5F;
        // Refreshed implies near.
        CY_CHECK((!refreshed || near));
    }

    // Amortised: a light that changes is picked up with no invalidation at all, within
    // ceil(64 / 16) updates, and the result is the bake's.
    settings.policy = VolumeUpdatePolicy::Amortised;
    CY_REQUIRE(corner.volume.configure(settings).has_value());
    (void)corner.volume.capture_all(context);
    corner.light.intensity = 2.0F;
    for (u32 step = 0; step < 4U; ++step) {
        context.frame = 200 + step;
        CY_CHECK_EQ(corner.volume.update(context).probes_captured, 16U);
    }
    IrradianceVolume baked;
    CY_REQUIRE(baked.configure(settings).has_value());
    (void)baked.capture_all(context);
    f32 worst = 0.0F;
    for (u32 index = 0; index < baked.probe_count(); ++index) {
        worst = std::max(worst, std::fabs(corner.volume.probe_radiance(index, kUp).x -
                                          baked.probe_radiance(index, kUp).x));
    }
    CY_CHECK_LT(worst, 1.0e-6F);
}

CY_TEST_CASE("a query at a probe is that probe, and between two it is their blend") {
    // One emissive panel off to one side, so the probes differ; no sun.
    BoxProxyScene scene;
    ProxyBox panel{box({4.0F, -2.0F, -2.0F}, {4.5F, 2.0F, 2.0F}), Vec3{}, Vec3{3.0F, 2.0F, 1.0F}};
    CY_REQUIRE(scene.add(panel).has_value());
    IrradianceVolume volume;
    IrradianceVolumeSettings settings;
    settings.origin = Vec3{-1.0F, -1.0F, -1.0F};
    settings.spacing_metres = 2.0F;
    settings.count_x = 2;
    settings.count_y = 2;
    settings.count_z = 2;
    settings.normal_offset_metres = 0.0F;
    CY_REQUIRE(volume.configure(settings).has_value());
    VolumeCaptureContext context;
    context.tracer = &scene;
    context.radiance = &scene;
    context.sky = uniform_sky(0.0F);
    (void)volume.capture_all(context);

    // At a probe, only that probe has trilinear weight.
    const u32 near = volume.probe_index(0, 0, 0);
    const Vec3 at = volume.gather(volume.probe(near).position, kUp);
    const Vec3 expected = volume.probe_radiance(near, kUp);
    CY_CHECK_NEAR(at.x, expected.x, 1.0e-5F);
    CY_CHECK_NEAR(at.z, expected.z, 1.0e-5F);

    // Halfway between probe (0,0,0) and (1,0,0), with the normal across the line joining them so
    // both are equally in front: the mean of the two.
    const u32 far = volume.probe_index(1, 0, 0);
    const Vec3 midway = volume.gather(Vec3{0.0F, -1.0F, -1.0F}, kUp);
    const Vec3 mean = (volume.probe_radiance(near, kUp) + volume.probe_radiance(far, kUp)) * 0.5F;
    CY_CHECK_NEAR(midway.x, mean.x, 1.0e-4F * std::max(1.0F, mean.x));
    CY_CHECK_GT(volume.probe_radiance(far, kUp).x, volume.probe_radiance(near, kUp).x);

    // Outside the volume the coverage falls to zero over one cell and the flat ambient returns.
    const Vec3 flat{0.1F, 0.1F, 0.1F};
    CY_CHECK_EQ(volume.sample(Vec3{-1.0F, -1.0F, -6.0F}, kUp).coverage, 0.0F);
    const Vec3 beyond = volume.ambient(Vec3{-1.0F, -1.0F, -6.0F}, kUp, flat);
    CY_CHECK_EQ(beyond.x, flat.x);
    CY_CHECK_NEAR(volume.sample(Vec3{-1.0F, -1.0F, -2.0F}, kUp).coverage, 0.5F, 1.0e-5F);
}

CY_TEST_CASE("the packed texels are the probes the shader reads") {
    Corner corner;
    (void)corner.volume.capture_all(corner.context());
    const VolumeTextureLayout layout = corner.volume.texture_layout();
    CY_CHECK_EQ(layout.width, 16U * kVolumeTexelsPerProbe);
    CY_CHECK_EQ(layout.height, 4U * 9U);
    std::vector<f32> texels(static_cast<std::size_t>(layout.width) * layout.height * 4U);
    CY_REQUIRE(corner.volume.pack_texels({texels.data(), texels.size()}).has_value());
    // Probe (3, 2, 5): the red channel's four coefficients, then validity and the +x distance.
    const u32 x = 3;
    const u32 y = 2;
    const u32 z = 5;
    const VolumeProbe& probe = corner.volume.probe(corner.volume.probe_index(x, y, z));
    const std::size_t row = y + (std::size_t{4} * z);
    const std::size_t column = std::size_t{x} * kVolumeTexelsPerProbe;
    const f32* first = texels.data() + (((row * layout.width) + column) * 4U);
    for (std::size_t coefficient = 0; coefficient < 4U; ++coefficient) {
        CY_CHECK_EQ(first[coefficient] * layout.coefficient_scale, probe.payload[coefficient * 3U]);
        CY_CHECK_EQ(first[8U + coefficient] * layout.coefficient_scale,
                    probe.payload[(coefficient * 3U) + 2U]);
    }
    CY_CHECK_EQ(first[12], probe.validity);
    CY_CHECK_EQ(first[13], probe.axis_distance[0]);
    CY_CHECK_EQ(first[18], probe.axis_distance[5]);
    std::vector<f32> wrong(texels.size() - 4U);
    CY_CHECK_FALSE(corner.volume.pack_texels({wrong.data(), wrong.size()}).has_value());
}

CY_TEST_CASE("captured from the engine's GI room, and a dynamic object in Probe mode takes it") {
    // The GI suites' own room, through the composed system's world tracer and surface cache — the
    // seams the radiance cache gathers through — with its light in the middle so the red and blue
    // walls are lit alike.
    gi_support::RoomField field{Vec3{gi_support::kRoomX, gi_support::kRoomY, gi_support::kRoomZ}};
    std::vector<Surfel> surfels = gi_support::room_surfels(1.0F, false);
    std::vector<GiLight> lights = gi_support::room_lights();
    lights[0].position = Vec3{0.0F, 1.0F, 0.0F};
    IlluminationSystem system;
    CY_REQUIRE(system.configure(gi_support::room_settings()).has_value());
    CY_REQUIRE(system.field().place(1, field.asset(), cy::Mat4::identity()).has_value());
    const Aabb bounds = Aabb::from_center_extents(
        Vec3{0.0F, 0.0F, 0.0F},
        Vec3{gi_support::kRoomX + 1.0F, gi_support::kRoomY + 1.0F, gi_support::kRoomZ + 1.0F});
    CY_REQUIRE(
        system.scene().ingest_cell(1, bounds, {surfels.data(), surfels.size()}, 0).has_value());
    CY_REQUIRE(system.surfaces().allocate_from(system.scene(), bounds).has_value());
    system.surfaces().set_lookup_radius(1.2F);
    for (u32 frame = 0; frame < 24U; ++frame) {
        FrameContext frame_context;
        frame_context.lights = {lights.data(), lights.size()};
        frame_context.frame = frame;
        frame_context.measured_gi_ms = 1.0F;
        (void)system.update(frame_context);
    }

    IrradianceVolume volume;
    IrradianceVolumeSettings settings;
    settings.origin = Vec3{-3.0F, -1.5F, -3.0F};
    settings.spacing_metres = 1.5F;
    settings.count_x = 5;
    settings.count_y = 3;
    settings.count_z = 5;
    settings.rays_per_probe = 64;
    settings.max_ray_distance_metres = 24.0F;
    CY_REQUIRE(volume.configure(settings).has_value());
    VolumeCaptureContext context;
    context.tracer = &system.world_tracer();
    context.radiance = &system.surfaces();
    context.sky = system.sky_term();
    const VolumeUpdateReport report = volume.capture_all(context);
    CY_CHECK_GT(report.rays, 0U);

    const Vec3 beside_red = volume.gather(Vec3{-3.0F, -1.6F, 0.0F}, kUp);
    const Vec3 beside_blue = volume.gather(Vec3{3.0F, -1.6F, 0.0F}, kUp);
    std::fprintf(stderr,
                 "GI room floor: beside red (%.3g %.3g %.3g), beside blue (%.3g %.3g %.3g)\n",
                 static_cast<double>(beside_red.x), static_cast<double>(beside_red.y),
                 static_cast<double>(beside_red.z), static_cast<double>(beside_blue.x),
                 static_cast<double>(beside_blue.y), static_cast<double>(beside_blue.z));
    CY_CHECK_GT(beside_red.x - beside_red.z, 0.0F);
    CY_CHECK_GT(beside_blue.z - beside_blue.x, 0.0F);

    // "A character walks through a baked room": in Probe mode its indirect diffuse is the volume's
    // interpolated irradiance, and the dynamic caches are excluded rather than added to it.
    system.set_mode(GiMode::Probe);
    SurfaceProperties character;
    character.has_irradiance_volume = true;
    for (u32 step = 0; step < 5U; ++step) {
        const Vec3 position{-3.0F + (1.5F * static_cast<f32>(step)), -1.0F, 0.5F};
        character.irradiance_volume_radiance = volume.gather(position, kUp);
        const ResolveResult resolved = system.indirect_diffuse(position, kUp, character);
        CY_CHECK_NE(resolved.sources_used & source_bit(RadianceSource::IrradianceVolume), 0U);
        CY_CHECK_EQ(resolved.sources_used & source_bit(RadianceSource::RadianceCache), 0U);
        CY_CHECK_EQ(resolved.sources_used & source_bit(RadianceSource::Lightmap), 0U);
    }
}
