// SPDX-License-Identifier: MIT
// Illumination classes of the things that move, through the composed system.
//
// `rendering-global-illumination` — "Illumination classification of dynamic objects", its three
// scenarios ("A projectile does not invalidate the world", "A vehicle does", "Crowd is
// approximated"), the per-object override, and what a FastDynamic proxy still contributes:
// occlusion of the indirect light under it, measured against the path tracer with the same body
// in the field, and its presence in a reflection.

#include <cy/test/test.h>

#include <cy/core/math/matrix.h>
#include <cy/rendering/gi/bake.h>
#include <cy/rendering/gi/classification.h>
#include <cy/rendering/gi/system.h>

#include "rooms.h"
#include "support.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <vector>

namespace {

using cy::Aabb;
using cy::f32;
using cy::f64;
using cy::Mat4;
using cy::Sphere;
using cy::u32;
using cy::u64;
using cy::usize;
using cy::Vec3;
using namespace cy::rendering::gi;  // NOLINT(google-build-using-namespace)

[[nodiscard]] u32 total_invalidations(const IlluminationSystem& system) noexcept {
    u32 total = 0;
    for (u32 cause = 0; cause < static_cast<u32>(InvalidationCause::Count); ++cause) {
        total += system.scene().invalidation_count(static_cast<InvalidationCause>(cause));
    }
    return total;
}

/// A box's cards: one per face, a metre apart, facing out. What a cook gives a rigid prop.
[[nodiscard]] std::vector<Surfel> box_cards(Vec3 half, Vec3 albedo) {
    std::vector<Surfel> cards;
    const Vec3 normals[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                             Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
    for (const Vec3 normal : normals) {
        Surfel card;
        card.position = Vec3{normal.x * half.x, normal.y * half.y, normal.z * half.z};
        card.normal = normal;
        card.albedo = albedo;
        card.area = 4.0F * half.x * half.z;
        cards.push_back(card);
    }
    return cards;
}

/// The cosine-weighted fraction of the hemisphere a sphere covers, by brute force over a fine
/// latitude-longitude grid: the reference `sphere_occlusion`'s closed form is held to.
[[nodiscard]] f32 occlusion_by_quadrature(const Sphere& sphere, Vec3 position, Vec3 normal) {
    Vec3 tangent = cy::normalize(
        cy::cross(normal, std::fabs(normal.y) < 0.9F ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
    const Vec3 bitangent = cy::cross(normal, tangent);
    constexpr u32 kTheta = 400;
    constexpr u32 kPhi = 800;
    double covered = 0.0;
    double total = 0.0;
    for (u32 i = 0; i < kTheta; ++i) {
        const double theta = (static_cast<double>(i) + 0.5) / kTheta * (std::numbers::pi * 0.5);
        for (u32 j = 0; j < kPhi; ++j) {
            const double phi = (static_cast<double>(j) + 0.5) / kPhi * (2.0 * std::numbers::pi);
            const double weight = std::cos(theta) * std::sin(theta);
            const Vec3 direction = (normal * static_cast<f32>(std::cos(theta))) +
                                   (tangent * static_cast<f32>(std::sin(theta) * std::cos(phi))) +
                                   (bitangent * static_cast<f32>(std::sin(theta) * std::sin(phi)));
            total += weight;
            if (sphere_hit_distance(sphere, position, direction) >= 0.0F) {
                covered += weight;
            }
        }
    }
    return static_cast<f32>(covered / total);
}

/// A sphere as triangles, for the hardware tier: a latitude-longitude mesh fine enough that its
/// faceting is a fraction of a percent of the occlusion it casts.
struct SphereMesh {
    std::vector<Vec3> positions;
    std::vector<u32> indices;

    explicit SphereMesh(f32 radius) {
        constexpr u32 kRings = 24;
        constexpr u32 kSegments = 48;
        for (u32 ring = 0; ring <= kRings; ++ring) {
            const f32 polar = cy::math::kPi * static_cast<f32>(ring) / static_cast<f32>(kRings);
            for (u32 segment = 0; segment <= kSegments; ++segment) {
                const f32 azimuth =
                    2.0F * cy::math::kPi * static_cast<f32>(segment) / static_cast<f32>(kSegments);
                positions.push_back(Vec3{std::sin(polar) * std::cos(azimuth), std::cos(polar),
                                         std::sin(polar) * std::sin(azimuth)} *
                                    radius);
            }
        }
        for (u32 ring = 0; ring < kRings; ++ring) {
            for (u32 segment = 0; segment < kSegments; ++segment) {
                const u32 a = (ring * (kSegments + 1U)) + segment;
                const u32 b = a + kSegments + 1U;
                indices.insert(indices.end(), {a, b, a + 1U, a + 1U, b, b + 1U});
            }
        }
    }

    [[nodiscard]] cy::rendering::rt::TriangleGeometry geometry() const noexcept {
        cy::rendering::rt::TriangleGeometry triangles;
        triangles.positions = {positions.data(), positions.size()};
        triangles.indices = {indices.data(), indices.size()};
        return triangles;
    }
};

/// Cards over a sphere's surface, for the reference body: the path tracer resolves a hit on it to
/// one of these and so to the proxy's albedo.
[[nodiscard]] std::vector<Surfel> sphere_cards(Vec3 centre, f32 radius, Vec3 albedo) {
    std::vector<Surfel> cards;
    constexpr u32 kRings = 6;
    constexpr u32 kSegments = 12;
    const f32 area = 4.0F * cy::math::kPi * radius * radius / static_cast<f32>(kRings * kSegments);
    for (u32 ring = 0; ring < kRings; ++ring) {
        const f32 polar =
            cy::math::kPi * (static_cast<f32>(ring) + 0.5F) / static_cast<f32>(kRings);
        for (u32 segment = 0; segment < kSegments; ++segment) {
            const f32 azimuth =
                2.0F * cy::math::kPi * static_cast<f32>(segment) / static_cast<f32>(kSegments);
            const Vec3 normal{std::sin(polar) * std::cos(azimuth), std::cos(polar),
                              std::sin(polar) * std::sin(azimuth)};
            Surfel card;
            card.position = centre + (normal * radius);
            card.normal = normal;
            card.albedo = albedo;
            card.area = area;
            card.instance_id = 50;
            cards.push_back(card);
        }
    }
    return cards;
}

}  // namespace

CY_TEST_CASE("classes derive from mobility and size, and an override wins") {
    const ClassificationSettings settings;
    CY_CHECK(classify_illumination(ObjectMobility::Static, 0.01F, settings) ==
             IlluminationClass::Static);
    CY_CHECK(classify_illumination(ObjectMobility::Static, 40.0F, settings) ==
             IlluminationClass::Static);
    CY_CHECK(classify_illumination(ObjectMobility::Movable, 0.05F, settings) ==
             IlluminationClass::TransientIgnored);
    CY_CHECK(classify_illumination(ObjectMobility::Movable, 1.8F, settings) ==
             IlluminationClass::FastDynamic);
    CY_CHECK(classify_illumination(ObjectMobility::Movable, 4.5F, settings) ==
             IlluminationClass::SlowDynamic);
    // The boundaries are where the settings put them.
    CY_CHECK(classify_illumination(ObjectMobility::Movable, settings.transient_max_size_metres,
                                   settings) == IlluminationClass::FastDynamic);
    CY_CHECK(classify_illumination(ObjectMobility::Movable, settings.slow_dynamic_min_size_metres,
                                   settings) == IlluminationClass::SlowDynamic);

    gi_rooms::LitRoom room;
    GiObjectDesc crate;
    crate.id = 40;
    crate.mobility = ObjectMobility::Movable;
    crate.local_bounds = Aabb::from_center_extents(Vec3{}, Vec3{0.02F, 0.02F, 0.02F});
    crate.has_class_override = true;
    crate.class_override = IlluminationClass::Static;
    CY_REQUIRE(room.system.add_object(crate, 1).has_value());
    CY_CHECK(room.system.object(40)->klass == IlluminationClass::Static);
    // The same body without the override is a projectile.
    crate.id = 41;
    crate.has_class_override = false;
    CY_REQUIRE(room.system.add_object(crate, 1).has_value());
    CY_CHECK(room.system.object(41)->klass == IlluminationClass::TransientIgnored);
    // An id is one object.
    CY_CHECK_FALSE(room.system.add_object(crate, 1).has_value());
}

CY_TEST_CASE("a projectile crossing the room invalidates nothing") {
    // Against a control: the same room, the same frames, no projectile. The field keeps solving
    // bricks as the frames go by in both, so what is compared is the two rooms, not a frame with
    // the one before it.
    gi_rooms::LitRoom room;
    gi_rooms::LitRoom control;
    u64 frame = room.converge();
    (void)control.converge();
    const u32 before = total_invalidations(room.system);

    GiObjectDesc bullet;
    bullet.id = 7;
    bullet.mobility = ObjectMobility::Movable;
    bullet.local_bounds = Aabb::from_center_extents(Vec3{}, Vec3{0.01F, 0.01F, 0.03F});
    bullet.transform = Mat4::from_translation(Vec3{-3.5F, 0.0F, -3.0F});
    const std::vector<Surfel> cards = box_cards(Vec3{0.01F, 0.01F, 0.03F}, Vec3{0.8F, 0.7F, 0.2F});
    bullet.surfels = {cards.data(), cards.size()};
    CY_REQUIRE(room.system.add_object(bullet, frame).has_value());
    CY_CHECK(room.system.object(7)->klass == IlluminationClass::TransientIgnored);

    u32 serviced = 0;
    for (u32 step = 0; step < 30; ++step, ++frame) {
        const f32 x = -3.5F + (0.23F * static_cast<f32>(step));
        CY_REQUIRE(room.system.move_object(7, Mat4::from_translation(Vec3{x, 0.0F, -3.0F}), frame)
                       .has_value());
        const IlluminationFrameReport report = room.system.update(room.context(frame));
        (void)control.system.update(control.context(frame));
        serviced += report.invalidations_serviced;
        CY_CHECK_EQ(report.objects.objects[static_cast<u32>(IlluminationClass::TransientIgnored)],
                    1U);
    }
    CY_CHECK_EQ(serviced, 0U);
    CY_CHECK_EQ(total_invalidations(room.system), before);
    CY_CHECK_EQ(room.system.field().generation(), control.system.field().generation());
    CY_CHECK_EQ(room.system.radiance().diagnostics().last_update.probes_updated,
                control.system.radiance().diagnostics().last_update.probes_updated);
    CY_CHECK_FALSE(room.system.scene().cell_resident(object_cell_id(7)));
}

CY_TEST_CASE("a vehicle participates, with its invalidation amortised rather than every frame") {
    gi_rooms::LitRoom room;
    u64 frame = room.converge();
    const ClassificationSettings& classes = room.system.settings().classification;

    const Vec3 half{2.2F, 0.8F, 1.0F};
    gi_support::BoxField body{half, 0.5F};
    const std::vector<Surfel> cards = box_cards(half, Vec3{0.7F, 0.1F, 0.1F});
    GiObjectDesc car;
    car.id = 9;
    car.mobility = ObjectMobility::Movable;
    car.local_bounds = Aabb::from_center_extents(Vec3{}, half);
    car.transform = Mat4::from_translation(Vec3{-1.5F, -1.2F, -2.0F});
    const AssetDistanceField field = body.asset();
    car.field = &field;
    car.surfels = {cards.data(), cards.size()};
    CY_REQUIRE(room.system.add_object(car, frame).has_value());
    CY_CHECK(room.system.object(9)->klass == IlluminationClass::SlowDynamic);
    // It participates: its cards are in the GI scene, as a cell of its own.
    CY_CHECK(room.system.scene().cell_resident(object_cell_id(9)));
    (void)room.system.update(room.context(frame++));

    constexpr u32 kFrames = 32;
    u32 records = 0;
    u32 flushes = 0;
    u32 coalesced = 0;
    u32 probes = 0;
    const u32 before = total_invalidations(room.system);
    const u64 first_move = frame - 1;
    for (u32 step = 0; step < kFrames; ++step, ++frame) {
        const f32 x = -1.5F + (0.1F * static_cast<f32>(step + 1U));
        CY_REQUIRE(room.system.move_object(9, Mat4::from_translation(Vec3{x, -1.2F, -2.0F}), frame)
                       .has_value());
        const IlluminationFrameReport report = room.system.update(room.context(frame));
        records += report.invalidations_serviced;
        flushes += report.objects.flushes;
        coalesced += report.objects.moves_coalesced;
        probes += report.invalidated_probes;
    }
    // Every frame moved it; at most one flush per interval, each one eviction and one ingestion.
    const u32 most = (kFrames / classes.slow_dynamic_interval_frames) + 1U;
    CY_TEST_MESSAGE("vehicle: " << kFrames << " moves, " << flushes << " flushes, " << records
                                << " invalidation records, " << probes << " probes invalidated");
    CY_CHECK_EQ(coalesced, kFrames);
    CY_CHECK_GE(flushes, kFrames / classes.slow_dynamic_interval_frames);
    CY_CHECK_LE(flushes, most);
    CY_CHECK_EQ(records, 2U * flushes);
    CY_CHECK_EQ(total_invalidations(room.system) - before, 2U * flushes);
    CY_CHECK_GT(probes, 0U);
    // Each record the moves filed names the vehicle.
    u32 named = 0;
    for (const InvalidationRecord& record : room.system.recent_invalidations()) {
        if (record.frame > first_move) {
            CY_CHECK_EQ(record.source_id, object_cell_id(9));
            named += 1;
        }
    }
    CY_CHECK_EQ(named, records);
    // The scene holds it where the last flush put it, not where it started.
    const GiObject& vehicle = *room.system.object(9);
    const Vec3 committed = cy::transform_point(vehicle.committed_transform, Vec3{});
    CY_CHECK_GT(committed.x,
                -1.5F + (0.1F * static_cast<f32>(kFrames - classes.slow_dynamic_interval_frames)));
    const u32 roof = room.system.scene().nearest_surfel(committed + Vec3{0.0F, half.y, 0.0F},
                                                        Vec3{0.0F, 1.0F, 0.0F}, 0.05F);
    CY_CHECK(roof != GiScene::kInvalidSurfel);
}

CY_TEST_CASE("a static object invalidates on every change") {
    gi_rooms::LitRoom room;
    u64 frame = room.converge();
    const std::vector<Surfel> cards = box_cards(Vec3{0.3F, 0.3F, 0.3F}, Vec3{0.5F, 0.5F, 0.5F});
    GiObjectDesc statue;
    statue.id = 3;
    statue.local_bounds = Aabb::from_center_extents(Vec3{}, Vec3{0.3F, 0.3F, 0.3F});
    statue.transform = Mat4::from_translation(Vec3{2.0F, -1.7F, 2.0F});
    statue.surfels = {cards.data(), cards.size()};
    CY_REQUIRE(room.system.add_object(statue, frame).has_value());
    (void)room.system.update(room.context(frame++));
    for (u32 move = 0; move < 3; ++move, ++frame) {
        CY_REQUIRE(room.system
                       .move_object(3,
                                    Mat4::from_translation(Vec3{
                                        2.0F - (0.5F * static_cast<f32>(move + 1)), -1.7F, 2.0F}),
                                    frame)
                       .has_value());
        const IlluminationFrameReport report = room.system.update(room.context(frame));
        CY_CHECK_EQ(report.invalidations_serviced, 2U);
    }
}

CY_TEST_CASE(
    "a character occludes the light under it as the path tracer does, invalidating nothing") {
    // The reference: the same body as a solid sphere in the field, carded in the proxy's albedo,
    // traced by the path tracer — so it both blocks light and reflects some back. The measured: the
    // room without it in the field and the body as a FastDynamic proxy. The quantity both report is
    // the indirect light at points on the floor under and beside the body, over the same light
    // without the body.
    constexpr f32 kRadius = 0.6F;
    const Vec3 character_albedo{0.5F, 0.5F, 0.5F};
    const Vec3 centre{0.0F, -gi_support::kRoomY + 0.9F, 2.0F};

    gi_rooms::LitRoom room;
    u64 frame = room.converge();
    const u32 before = total_invalidations(room.system);
    std::vector<Vec3> points;
    points.reserve(5);
    for (u32 index = 0; index < 5; ++index) {
        points.push_back(Vec3{0.25F * static_cast<f32>(index), -gi_support::kRoomY + 0.02F, 2.0F});
    }
    std::vector<f32> open;
    open.reserve(points.size());
    for (const Vec3& point : points) {
        open.push_back(cy::length(
            room.system.indirect_diffuse(point, Vec3{0.0F, 1.0F, 0.0F}, SurfaceProperties{})
                .radiance));
    }

    GiObjectDesc character;
    character.id = 11;
    character.mobility = ObjectMobility::Movable;
    // A body whose volume is the sphere's, so the coarse proxy is exactly that sphere.
    const f32 side = std::cbrt(4.0F / 3.0F * cy::math::kPi) * kRadius;
    character.local_bounds = Aabb::from_center_extents(Vec3{}, Vec3{side, side, side} * 0.5F);
    character.transform = Mat4::from_translation(centre);
    character.proxy_albedo = character_albedo;
    CY_REQUIRE(room.system.add_object(character, frame).has_value());
    CY_CHECK(room.system.object(11)->klass == IlluminationClass::FastDynamic);
    const IlluminationFrameReport report = room.system.update(room.context(frame++));
    CY_CHECK_EQ(report.invalidations_serviced, 0U);
    CY_CHECK_EQ(report.objects.proxy_spheres, 1U);
    CY_CHECK_EQ(total_invalidations(room.system), before);
    CY_CHECK_NEAR(room.system.proxy_spheres()[0].radius, kRadius, 1.0e-4F);

    // The reference room: the same light and cards, traced as exact triangles through the
    // hardware tier — the room, and for the blocked trace the body as a sphere of triangles carded
    // in the proxy's albedo. Exact geometry, because the field's half-voxel hit tolerance would
    // fatten a 0.6 m body by a fifth and the reference would measure that.
    IlluminationSettings exact = gi_support::room_settings();
    exact.ray_tracing.device_supports_ray_tracing = true;
    exact.ray_tracing.budget.max_builds_per_frame = 8;
    gi_rooms::LitRoom reference(false, exact);
    const gi_support::RoomTriangles walls(false);
    CY_REQUIRE(reference.system.acceleration()
                   .declare_geometry(1, cy::rendering::rt::adapt_static_mesh(walls.geometry(), 0))
                   .has_value());
    cy::rendering::rt::InstanceDescriptor walls_instance;
    walls_instance.geometry = 1;
    walls_instance.instance_id = 1;
    CY_REQUIRE(reference.system.acceleration().add_instance(walls_instance).has_value());
    (void)reference.converge();
    CY_REQUIRE(reference.system.world_tracer().tier() == RadianceSource::HardwareTrace);
    BakeSettings bake;
    bake.bounces = 3;
    bake.samples = 512;
    PathTracer open_tracer(reference.system.world_tracer(), reference.system.scene(),
                           {reference.lights.data(), reference.lights.size()},
                           reference.system.sky_term(), &reference.system.world_tracer());
    const SphereMesh solid{kRadius};
    const std::vector<Surfel> body = sphere_cards(centre, kRadius, character_albedo);
    std::vector<f32> open_reference;
    open_reference.reserve(points.size());
    for (const Vec3& point : points) {
        open_reference.push_back(
            cy::length(open_tracer.irradiance(point, Vec3{0.0F, 1.0F, 0.0F}, bake)));
    }
    CY_REQUIRE(reference.system.acceleration()
                   .declare_geometry(2, cy::rendering::rt::adapt_static_mesh(solid.geometry(), 0))
                   .has_value());
    cy::rendering::rt::InstanceDescriptor body_instance;
    body_instance.geometry = 2;
    body_instance.instance_id = 2;
    body_instance.transform = Mat4::from_translation(centre);
    CY_REQUIRE(reference.system.acceleration().add_instance(body_instance).has_value());
    CY_REQUIRE(reference.system.scene()
                   .ingest_cell(50, Aabb::from_center_extents(centre, Vec3{1.0F, 1.0F, 1.0F}),
                                {body.data(), body.size()}, 100)
                   .has_value());
    (void)reference.system.update(reference.context(100));
    PathTracer blocked_tracer(reference.system.world_tracer(), reference.system.scene(),
                              {reference.lights.data(), reference.lights.size()},
                              reference.system.sky_term(), &reference.system.world_tracer());
    f64 error = 0.0;
    for (usize index = 0; index < points.size(); ++index) {
        const f32 shaded = cy::length(
            room.system.indirect_diffuse(points[index], Vec3{0.0F, 1.0F, 0.0F}, SurfaceProperties{})
                .radiance);
        const f32 traced =
            cy::length(blocked_tracer.irradiance(points[index], Vec3{0.0F, 1.0F, 0.0F}, bake));
        const f32 measured = shaded / open[index];
        const f32 expected = traced / open_reference[index];
        error += static_cast<f64>(std::fabs(measured - expected));
        std::printf("character occlusion at x=%.2f: proxy keeps %.3f, path tracer %.3f\n",
                    static_cast<double>(points[index].x), static_cast<double>(measured),
                    static_cast<double>(expected));
        // Under the body it is darker; the proxy took light away and nothing came back.
        CY_CHECK_LT(measured, 1.0F);
    }
    error /= static_cast<f64>(points.size());
    std::printf("character occlusion: mean |proxy - path tracer| %.4f of the open light\n", error);
    CY_CHECK_LT(error, 0.08);
    // And the reflection presence: a mirror on the floor looking straight up at it sees the body.
    // Against a twin room advanced the same frames without it — a FastDynamic object changes no
    // cache, so the twin's caches are the room's and the difference is the proxy's alone.
    gi_rooms::LitRoom twin;
    (void)twin.converge();
    (void)twin.system.update(twin.context(frame - 1));
    TraceBudget budget;
    const ResolveResult with = room.system.indirect_specular(
        points[0], Vec3{0.0F, 1.0F, 0.0F}, Vec3{0.0F, -1.0F, 0.0F}, 0.0F, 1, budget);
    TraceBudget budget_without;
    const ResolveResult without = twin.system.indirect_specular(
        points[0], Vec3{0.0F, 1.0F, 0.0F}, Vec3{0.0F, -1.0F, 0.0F}, 0.0F, 1, budget_without);
    // The twin is the room without the body, bit for bit, wherever the body is not involved.
    for (u32 handle = 0; handle < room.system.radiance().probes().size(); ++handle) {
        const cy::Span<const f32> ours = room.system.radiance().probe_payload(handle);
        const cy::Span<const f32> theirs = twin.system.radiance().probe_payload(handle);
        CY_REQUIRE_EQ(ours.size(), theirs.size());
        CY_CHECK(std::memcmp(ours.data(), theirs.data(), ours.size() * sizeof(f32)) == 0);
    }
    std::printf("reflection presence: with the body %.4f, without %.4f\n",
                static_cast<double>(cy::length(with.radiance)),
                static_cast<double>(cy::length(without.radiance)));
    CY_CHECK_GT(cy::length(with.radiance - without.radiance), 1.0e-3F);
}

CY_TEST_CASE("a distant crowd uses coarse proxies, and a near character its deformed one") {
    gi_rooms::LitRoom room;
    u64 frame = room.converge();
    // Forty skinned members sixty metres out, and one beside the camera.
    for (u32 member = 0; member < 40; ++member) {
        GiObjectDesc walker;
        walker.id = 100 + member;
        walker.mobility = ObjectMobility::Movable;
        walker.skinned = true;
        walker.local_bounds = Aabb::from_center_extents(Vec3{}, Vec3{0.3F, 0.9F, 0.3F});
        walker.transform =
            Mat4::from_translation(Vec3{-20.0F + static_cast<f32>(member), 0.0F, -60.0F});
        CY_REQUIRE(room.system.add_object(walker, frame).has_value());
        const Sphere bones[3] = {
            Sphere{Vec3{-20.0F + static_cast<f32>(member), 0.5F, -60.0F}, 0.2F},
            Sphere{Vec3{-20.0F + static_cast<f32>(member), 0.0F, -60.0F}, 0.25F},
            Sphere{Vec3{-20.0F + static_cast<f32>(member), -0.6F, -60.0F}, 0.2F}};
        CY_REQUIRE(room.system.pose_object(100 + member, {bones, 3}).has_value());
    }
    GiObjectDesc hero;
    hero.id = 99;
    hero.mobility = ObjectMobility::Movable;
    hero.skinned = true;
    hero.local_bounds = Aabb::from_center_extents(Vec3{}, Vec3{0.3F, 0.9F, 0.3F});
    hero.transform = Mat4::from_translation(Vec3{1.0F, -1.1F, 1.0F});
    CY_REQUIRE(room.system.add_object(hero, frame).has_value());
    const Sphere bones[4] = {
        Sphere{Vec3{1.0F, -0.4F, 1.0F}, 0.15F}, Sphere{Vec3{1.0F, -0.9F, 1.0F}, 0.25F},
        Sphere{Vec3{0.9F, -1.5F, 1.0F}, 0.12F}, Sphere{Vec3{1.1F, -1.5F, 1.0F}, 0.12F}};
    CY_REQUIRE(room.system.pose_object(99, {bones, 4}).has_value());

    const IlluminationFrameReport report = room.system.update(room.context(frame));
    CY_CHECK_EQ(report.objects.coarse_proxies, 40U);
    CY_CHECK_EQ(report.objects.deformed_proxies, 1U);
    // One sphere per member of the crowd and the hero's four bones.
    CY_CHECK_EQ(report.objects.proxy_spheres, 44U);
    CY_CHECK(room.system.object(99)->fidelity == ProxyFidelity::Deformed);
    CY_CHECK(room.system.object(100)->fidelity == ProxyFidelity::Coarse);
    // Importance moves the line: the same distance, a hero's importance, a deformed proxy.
    CY_CHECK(select_proxy_fidelity(60.0F, 4.0F, room.system.settings().classification) ==
             ProxyFidelity::Deformed);
    CY_CHECK(select_proxy_fidelity(60.0F, 1.0F, room.system.settings().classification) ==
             ProxyFidelity::Coarse);
}

CY_TEST_CASE(
    "one sphere's occlusion is its cosine-weighted solid angle, above and across the horizon") {
    const Vec3 up{0.0F, 1.0F, 0.0F};
    const Sphere cases[] = {
        Sphere{Vec3{0.0F, 2.0F, 0.0F}, 0.5F},    // overhead
        Sphere{Vec3{1.5F, 1.0F, 0.0F}, 0.6F},    // to one side, above the horizon
        Sphere{Vec3{1.0F, 0.1F, 0.0F}, 0.5F},    // straddling the horizon
        Sphere{Vec3{0.8F, -0.2F, 0.3F}, 0.45F},  // mostly below it
    };
    for (const Sphere& sphere : cases) {
        const f32 closed = sphere_occlusion(sphere, Vec3{}, up);
        const f32 numeric = occlusion_by_quadrature(sphere, Vec3{}, up);
        std::printf("sphere occlusion: closed form %.5f, quadrature %.5f\n",
                    static_cast<double>(closed), static_cast<double>(numeric));
        CY_CHECK_NEAR(closed, numeric, 2.0e-3F);
    }
    CY_CHECK_EQ(sphere_occlusion(Sphere{Vec3{0.0F, -3.0F, 0.0F}, 0.5F}, Vec3{}, up), 0.0F);
    CY_CHECK_EQ(sphere_occlusion(Sphere{Vec3{0.0F, 0.1F, 0.0F}, 0.5F}, Vec3{}, up), 1.0F);
}
