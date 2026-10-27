// SPDX-License-Identifier: MIT
// The grounds foot placement is handed: a physics world and a terrain. Issue #92.
//
// INTEGRATION: each case makes a reference physics world, or a terrain query over a source written
// here, and probes it.

#include <cy/animation/foot_placement.h>
#include <cy/animation/ground_probes.h>
#include <cy/animation/ik.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/servers/physics/reference/server.h>
#include <cy/test/test.h>

#include <cmath>

using namespace cy;
using namespace cy::animation;

namespace {

[[nodiscard]] Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Physics);
}

/// A reference physics world with a floor, and the bodies a case adds.
struct Physics {
    physics::PhysicsServer* server = nullptr;
    physics::WorldHandle world;

    Physics() noexcept {
        Expected<physics::PhysicsServer*, Error> made =
            physics::reference::create_server(allocator());
        CY_REQUIRE(made.has_value());
        server = *made;
        CY_REQUIRE(server->initialize().has_value());
        physics::WorldDescription description;
        description.name = Name::intern("ground");
        description.body_capacity = 16;
        description.body_pair_capacity = 64;
        description.contact_constraint_capacity = 64;
        Expected<physics::WorldHandle, Error> created = server->create_world(description);
        CY_REQUIRE(created.has_value());
        world = *created;
    }
    ~Physics() {
        server->shutdown();
        physics::reference::destroy_server(server, allocator());
    }
    Physics(const Physics&) = delete;
    Physics& operator=(const Physics&) = delete;

    void add(const physics::ShapeDescription& shape, const Transform& placement,
             physics::UserData user = 0, bool trigger = false) const noexcept {
        Expected<physics::ShapeHandle, Error> made = server->create_shape(shape);
        CY_REQUIRE(made.has_value());
        physics::ColliderDescription collider;
        collider.shape = *made;
        collider.is_trigger = trigger;
        physics::BodyDescription body;
        body.motion = physics::MotionType::Static;
        body.transform = placement;
        body.colliders = &collider;
        body.collider_count = 1;
        body.user_data = user;
        CY_REQUIRE(server->create_body(world, body).has_value());
    }

    void floor() const noexcept {
        physics::ShapeDescription plane;
        plane.type = physics::ShapeType::Plane;
        plane.plane = Plane::from_point_normal(Vec3{}, Vec3{0.0F, 1.0F, 0.0F});
        add(plane, Transform{});
    }

    void box(Vec3 centre, Vec3 half, physics::UserData user = 0,
             bool trigger = false) const noexcept {
        physics::ShapeDescription shape;
        shape.type = physics::ShapeType::Box;
        shape.half_extents = half;
        add(shape, Transform::from_translation(centre), user, trigger);
    }
};

/// A terrain source: a plane rising along +x at `slope`, with a hole where x > 5.
class SlopeSource final : public terrain::SurfaceSource {
public:
    explicit SlopeSource(f32 slope) noexcept : slope_(slope) {}
    [[nodiscard]] terrain::Representation representation() const noexcept override {
        return terrain::Representation::Heightfield;
    }
    [[nodiscard]] u32 column(f64 x, f64 /*z*/,
                             Span<terrain::SurfaceSample> out) const noexcept override {
        if (out.empty()) {
            return 0;
        }
        terrain::SurfaceSample sample;
        sample.height = static_cast<f32>(x) * std::tan(slope_);
        sample.normal = Vec3{-std::sin(slope_), std::cos(slope_), 0.0F};
        sample.resolved = x <= 5.0;
        sample.hole = x > 5.0;
        out[0] = sample;
        return 1;
    }

private:
    f32 slope_;
};

}  // namespace

CY_TEST_CASE("animation ground: a physics probe finds the first solid surface under a foot") {
    Physics physics;
    physics.floor();
    // A 20 cm step, the character's own capsule-sized box, and a trigger volume above the floor.
    physics.box(Vec3{1.0F, 0.1F, 0.0F}, Vec3{0.5F, 0.1F, 0.5F});
    physics.box(Vec3{-1.0F, 0.5F, 0.0F}, Vec3{0.3F, 0.5F, 0.3F}, 77);
    physics.box(Vec3{-3.0F, 0.5F, 0.0F}, Vec3{0.3F, 0.3F, 0.3F}, 0, true);
    const PhysicsGroundProbe probe(*physics.server, physics.world);

    GroundQuery query;
    query.origin = Vec3{1.0F, 0.8F, 0.0F};
    query.length = 1.5F;
    GroundHit hit = probe.cast_down(query);
    CY_REQUIRE(hit.hit);
    CY_CHECK_NEAR(hit.point.y, 0.2F, 1e-4F);
    CY_CHECK_NEAR(hit.normal.y, 1.0F, 1e-4F);

    // The asking character's own body is not ground: the floor under it is.
    query.origin = Vec3{-1.0F, 1.2F, 0.0F};
    query.owner = 77;
    hit = probe.cast_down(query);
    CY_REQUIRE(hit.hit);
    CY_CHECK_NEAR(hit.point.y, 0.0F, 1e-4F);
    query.owner = 0;
    CY_CHECK_NEAR(probe.cast_down(query).point.y, 1.0F, 1e-4F);

    // A trigger is not ground either, and a ray too short for the floor finds nothing.
    query.origin = Vec3{-3.0F, 1.2F, 0.0F};
    CY_CHECK_NEAR(probe.cast_down(query).point.y, 0.0F, 1e-4F);
    query.length = 0.5F;
    query.origin = Vec3{3.0F, 1.2F, 0.0F};
    CY_CHECK_FALSE(probe.cast_down(query).hit);
}

CY_TEST_CASE("animation ground: a terrain probe answers the surface under a foot, and no hole") {
    terrain::TerrainQuery terrain(allocator());
    const SlopeSource slope(0.25F);
    CY_REQUIRE(terrain.add_source(slope).has_value());
    const TerrainGroundProbe probe(terrain);

    GroundQuery query;
    query.origin = Vec3{2.0F, 1.5F, 0.0F};
    query.length = 2.0F;
    const GroundHit hit = probe.cast_down(query);
    CY_REQUIRE(hit.hit);
    CY_CHECK_NEAR(hit.point.y, 2.0F * std::tan(0.25F), 1e-5F);
    CY_CHECK_NEAR(hit.normal.x, -std::sin(0.25F), 1e-5F);

    // Out of reach, and over the hole.
    query.length = 0.5F;
    CY_CHECK_FALSE(probe.cast_down(query).hit);
    query.origin = Vec3{6.0F, 3.0F, 0.0F};
    query.length = 5.0F;
    CY_CHECK_FALSE(probe.cast_down(query).hit);
}

CY_TEST_CASE("animation ground: foot placement stands a pair of legs on a physics step") {
    Physics physics;
    physics.floor();
    // The right foot (x = +0.1) over a 15 cm step.
    physics.box(Vec3{0.55F, 0.075F, 0.0F}, Vec3{0.5F, 0.075F, 0.5F});
    const PhysicsGroundProbe probe(*physics.server, physics.world);

    Skeleton skeleton(allocator());
    const struct {
        const char* name = "";
        u16 parent = kInvalidJoint;
        Vec3 offset;
    } rows[] = {
        {"root", kInvalidJoint, Vec3{0.0F, 0.0F, 0.0F}}, {"pelvis", 0, Vec3{0.0F, 0.96F, 0.0F}},
        {"l_hip", 1, Vec3{-0.1F, -0.04F, 0.0F}},         {"l_knee", 2, Vec3{0.0F, -0.42F, 0.04F}},
        {"l_ankle", 3, Vec3{0.0F, -0.42F, -0.04F}},      {"r_hip", 1, Vec3{0.1F, -0.04F, 0.0F}},
        {"r_knee", 5, Vec3{0.0F, -0.42F, 0.04F}},        {"r_ankle", 6, Vec3{0.0F, -0.42F, -0.04F}},
    };
    for (const auto& row : rows) {
        CY_REQUIRE(skeleton
                       .add_joint(Name::intern(row.name), row.parent,
                                  Transform::from_translation(row.offset), kBoneLodLevels)
                       .has_value());
    }
    CY_REQUIRE(skeleton.finalize().has_value());
    Array<Transform> local(allocator());
    CY_REQUIRE(local.resize(8).has_value());
    skeleton.reference_pose(local.span());

    FootPlacementSetup setup;
    setup.pelvis = 1;
    setup.legs[0] = IkChain{2, 3, 4, 0, 0, Vec3{0.0F, 0.0F, 1.0F}};
    setup.legs[1] = IkChain{5, 6, 7, 0, 0, Vec3{0.0F, 0.0F, 1.0F}};
    setup.smoothing = 0.0F;
    FootPlacementState state;
    FootPlacementReport report;
    CY_REQUIRE(solve_foot_placement(skeleton, setup, FootPlacementContext{}, probe, 1.0F, state,
                                    local.span(), &report)
                   .has_value());
    CY_CHECK(report.hit[0]);
    CY_CHECK(report.hit[1]);
    CY_CHECK_NEAR(report.offset[1], 0.15F, 1e-4F);
    CY_CHECK_NEAR(joint_model(skeleton, 7, local.span()).translation.y, 0.15F + 0.08F, 1e-3F);
    CY_CHECK_NEAR(joint_model(skeleton, 4, local.span()).translation.y, 0.08F, 1e-3F);
}
