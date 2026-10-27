// SPDX-License-Identifier: MIT
// Foot placement through the per-frame system: a graph's `pose.foot_ik` node bound by joint name,
// the ground the system is handed, the entity's world placement, off as the frame before, and the
// same bytes from a job system as from one thread. Issue #92.
//
// INTEGRATION: each case runs a simulation over tens of ticks.

#include <cy/animation/animation_system.h>
#include <cy/core/jobs/job_system.h>
#include <cy/graph/cybergraph.h>
#include <cy/graph/lower_pose.h>
#include <cy/runtime/simulation.h>
#include <cy/scene/components.h>
#include <cy/scene/tree.h>
#include <cy/test/test.h>

#include "legs_fixture.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

using namespace cy;
using namespace cy::animation;
using namespace cy::animation::testing;

namespace {

[[nodiscard]] graph::Literal text(const char* value) noexcept {
    graph::Literal literal;
    literal.type = Name::intern("name");
    literal.text = Name::intern(value);
    return literal;
}

/// A clip that holds the legs' reference pose: one constant pelvis key, so it samples something.
[[nodiscard]] Status build_stand(Clip& clip) noexcept {
    clip.set_name(Name::intern("stand"));
    clip.set_duration(1.0F);
    clip.set_loop_mode(LoopMode::Loop);
    Expected<u32, Error> pelvis =
        clip.add_joint_track(TrackKind::Translation, kPelvis, Interpolation::Linear);
    if (!pelvis) {
        return Status{make_unexpected(pelvis.error())};
    }
    for (const f32 time : {0.0F, 1.0F}) {
        if (Status added = clip.add_key(*pelvis, time, Vec4{0.0F, 0.96F, 0.0F, 0.0F}); !added) {
            return added;
        }
    }
    return clip.compress(CompressionSettings{});
}

/// A program: the stand clip, through foot placement when `planted`, into one state.
[[nodiscard]] Expected<PoseProgram, Error> compile_stand(bool planted) noexcept {
    graph::NodeRegistry registry(allocator());
    if (Status registered = graph::pose::register_pose_nodes(registry); !registered) {
        return make_unexpected(registered.error());
    }
    graph::Graph graph(allocator(), Name::intern("stand"));
    Status built = graph.add_node(1, Name::intern("pose.clip"));
    built = built ? graph.set_property(1, Name::intern("clip"), text("stand")) : built;
    built = built ? graph.add_node(9, Name::intern("pose.state")) : built;
    if (planted) {
        built = built ? graph.add_node(2, Name::intern("pose.foot_ik")) : built;
        for (const auto& [property, joint] :
             {std::pair{"pelvis", "pelvis"}, std::pair{"hip_l", "l_hip"},
              std::pair{"knee_l", "l_knee"}, std::pair{"ankle_l", "l_ankle"},
              std::pair{"hip_r", "r_hip"}, std::pair{"knee_r", "r_knee"},
              std::pair{"ankle_r", "r_ankle"}, std::pair{"weight_parameter", "feet"}}) {
            built = built ? graph.set_property(2, Name::intern(property), text(joint)) : built;
        }
        graph::Literal still;
        still.type = Name::intern("float");
        still.value.x = 0.0F;
        built = built ? graph.set_property(2, Name::intern("smoothing"), still) : built;
        built = built ? graph.connect(1, Name::intern("pose"), 2, Name::intern("a")) : built;
        built = built ? graph.connect(2, Name::intern("pose"), 9, Name::intern("pose")) : built;
    } else {
        built = built ? graph.connect(1, Name::intern("pose"), 9, Name::intern("pose")) : built;
    }
    if (!built) {
        return make_unexpected(built.error());
    }
    graph.resolve(registry);
    graph::DiagnosticSink sink(allocator());
    return graph::pose::compile_pose(graph, registry, kLegJoints, sink);
}

/// One character on one rig in a simulation with a scene tree, placed at `placement`.
struct Stage {
    explicit Stage(bool planted)
        : skeleton(allocator()),
          clip(allocator()),
          program(allocator()),
          rig(allocator()),
          simulation(allocator(), config()) {
        CY_REQUIRE(build_legs(skeleton).has_value());
        CY_REQUIRE(build_stand(clip).has_value());
        Expected<PoseProgram, Error> compiled = compile_stand(planted);
        CY_REQUIRE(compiled.has_value());
        program = std::move(*compiled);
        const Clip* clips[] = {&clip};
        CY_REQUIRE(rig.bind(skeleton, program, Span<const Clip* const>(clips, 1)).has_value());
        CY_REQUIRE_EQ(rig.unresolved_joints(), 0U);
    }

    [[nodiscard]] static runtime::SimulationConfig config() {
        runtime::SimulationConfig settings;
        settings.world_name = "feet";
        settings.clock.mode = determinism::TickMode::FixedStep;
        settings.clock.fixed_ticks_per_frame = 1;
        return settings;
    }

    void open(const Transform& placement) {
        CY_REQUIRE(simulation.initialize().has_value());
        Expected<ecs::ComponentTypeId, Error> registered = register_animator(simulation.world());
        CY_REQUIRE(registered.has_value());
        animator = *registered;
        system = std::make_unique<AnimationSystem>(allocator(), simulation.world(), animator,
                                                   simulation.tree());
        CY_REQUIRE(system->add_rig(rig).has_value());
        CY_REQUIRE(system->install(simulation.schedule(), simulation.clock()).has_value());
        CY_REQUIRE(simulation.finalize_registration().has_value());
        scene::SceneTree* tree = simulation.tree();
        if (tree == nullptr) {
            std::abort();
        }
        Expected<scene::Node, Error> node = tree->create_node(Name::intern("hero"), tree->root());
        CY_REQUIRE(node.has_value());
        CY_REQUIRE(node->set_local_transform(placement).has_value());
        entity = node->entity();
        Animator settings;
        settings.rig = 0;
        CY_REQUIRE(simulation.world().add(entity, animator, &settings).has_value());
        // The instance exists from the first sync: parameters are set on it after that.
        CY_REQUIRE(system->sync().has_value());
    }

    void frames(u32 count, jobs::JobSystem* jobs = nullptr) {
        for (u32 frame = 0; frame < count; ++frame) {
            const determinism::FrameTicks ticks = simulation.begin_frame(0);
            for (u32 tick = 0; tick < ticks.ticks; ++tick) {
                CY_REQUIRE(simulation.step(jobs).has_value());
            }
            CY_REQUIRE(simulation.frame(ticks.alpha, jobs).has_value());
            CY_REQUIRE(system->last_error().has_value());
        }
    }

    [[nodiscard]] Span<const Mat4> matrices() const noexcept {
        return system->poses().current(system->pose_of(entity));
    }

    [[nodiscard]] Vec3 joint_world(const char* joint, const Transform& placement) const {
        Expected<Mat4, Error> model = system->joint_model_matrix(entity, Name::intern(joint));
        CY_REQUIRE(model.has_value());
        const Vec4 column = model->columns[3];
        return placement.transform_point(Vec3{column.x, column.y, column.z});
    }

    Skeleton skeleton;
    Clip clip;
    PoseProgram program;
    AnimationRig rig;
    runtime::Simulation simulation;
    ecs::ComponentTypeId animator = ecs::kInvalidComponent;
    std::unique_ptr<AnimationSystem> system;
    ecs::Entity entity = ecs::kNoEntity;
};

constexpr f32 kDegree = 0.0174532925F;

}  // namespace

CY_TEST_CASE("animation_system: foot placement plants a character's feet on a slope it stands on") {
    // Rising 15 degrees towards world +x, and the character turned 30 degrees about the vertical,
    // so the solve has to take its ankles through the entity's placement to find the ground.
    const f32 slope = 15.0F * kDegree;
    const Vec3 normal{-std::sin(slope), std::cos(slope), 0.0F};
    const PlaneGround ground(Vec3{0.0F, 0.0F, 0.0F}, normal);
    Transform placement;
    placement.rotation = Quat::from_axis_angle(Vec3{0.0F, 1.0F, 0.0F}, 30.0F * kDegree);

    Stage stage(true);
    stage.open(placement);
    stage.system->set_ground_probe(&ground);
    CY_REQUIRE(stage.system->set_parameter(stage.entity, Name::intern("feet"), 1.0F).has_value());
    stage.frames(3);

    Expected<FootPlacementReport, Error> report = stage.system->foot_placement(stage.entity);
    CY_REQUIRE(report.has_value());
    CY_CHECK(report->solved);
    CY_CHECK(report->hit[0]);
    CY_CHECK(report->hit[1]);
    CY_CHECK_LT(report->pelvis, -0.01F);
    for (const char* ankle : {"l_ankle", "r_ankle"}) {
        const Vec3 at = stage.joint_world(ankle, placement);
        const f32 above = (at.y - (std::tan(slope) * at.x)) * std::cos(slope);
        CY_CHECK_LT(std::fabs(above - kAnkleHeight), 2e-3F);
    }
}

CY_TEST_CASE("animation_system: foot placement off is the frame before, bit for bit") {
    const PlaneGround ground(Vec3{0.0F, 0.0F, 0.0F}, Vec3{-std::sin(0.3F), std::cos(0.3F), 0.0F});
    const Transform placement;

    Stage plain(false);
    plain.open(placement);
    plain.frames(4);

    // The node in the graph at zero weight, with ground to stand on.
    Stage weightless(true);
    weightless.open(placement);
    weightless.system->set_ground_probe(&ground);
    weightless.frames(4);

    // The node at full weight with no ground handed to the system.
    Stage groundless(true);
    groundless.open(placement);
    CY_REQUIRE(groundless.system->set_parameter(groundless.entity, Name::intern("feet"), 1.0F)
                   .has_value());
    groundless.frames(4);

    const usize bytes = sizeof(Mat4) * kLegJoints;
    CY_REQUIRE_EQ(plain.matrices().size(), static_cast<usize>(kLegJoints));
    CY_CHECK(same_bits(plain.matrices().data(), weightless.matrices().data(), bytes));
    CY_CHECK(same_bits(plain.matrices().data(), groundless.matrices().data(), bytes));

    // And on: the same graph, weighted, is not the frame before.
    CY_REQUIRE(weightless.system->set_parameter(weightless.entity, Name::intern("feet"), 1.0F)
                   .has_value());
    weightless.frames(2);
    CY_CHECK(!same_bits(plain.matrices().data(), weightless.matrices().data(), bytes));
}

CY_TEST_CASE(
    "animation_system: planted feet are the same bytes from job workers as from one thread") {
    const StepGround stairs(0.0F, 0.0F, 0.17F);
    Transform placement;
    placement.translation = Vec3{0.02F, 0.0F, 0.0F};

    Stage serial(true);
    serial.open(placement);
    serial.system->set_ground_probe(&stairs);
    CY_REQUIRE(serial.system->set_parameter(serial.entity, Name::intern("feet"), 1.0F).has_value());
    serial.frames(6);

    jobs::JobSystem workers;
    jobs::JobSystemConfig settings;
    settings.worker_count = 3;
    CY_REQUIRE(workers.start(settings).has_value());
    Stage parallel(true);
    parallel.open(placement);
    parallel.system->set_ground_probe(&stairs);
    CY_REQUIRE(
        parallel.system->set_parameter(parallel.entity, Name::intern("feet"), 1.0F).has_value());
    parallel.frames(6, &workers);
    workers.shutdown();

    CY_CHECK(
        same_bits(serial.matrices().data(), parallel.matrices().data(), sizeof(Mat4) * kLegJoints));
    Expected<FootPlacementReport, Error> report = serial.system->foot_placement(serial.entity);
    CY_REQUIRE(report.has_value());
    CY_CHECK_LT(std::fabs(report->offset[1] - 0.17F), 1e-5F);
}
