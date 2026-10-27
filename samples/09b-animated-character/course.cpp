// SPDX-License-Identifier: MIT
// The course the character walks with its feet placed. See course.h.

#include "course.h"

#include <cy/animation/cooked.h>
#include <cy/core/math/scalar.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/graph/cybergraph.h>
#include <cy/graph/lower_pose.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace cy::sample::character {
namespace {

// THE COURSE, along +z and two metres wide. Six steps of a domestic stair — 32 cm deep and 16 cm
// high — up to a landing, then a ramp down at 12 degrees back to the floor.
constexpr f32 kHalfWidth = 1.0F;
constexpr u32 kSteps = 6;
constexpr f32 kStairStart = 2.0F;
constexpr f32 kTread = 0.32F;
constexpr f32 kRiser = 0.16F;
constexpr f32 kLandingEnd = 5.4F;
constexpr f32 kTop = kRiser * static_cast<f32>(kSteps);
constexpr f32 kRampDegrees = 12.0F;
constexpr f32 kSlab = 0.08F;

[[nodiscard]] f32 ramp_tangent() noexcept {
    return std::tan(kRampDegrees * 0.0174532925F);
}

[[nodiscard]] f32 ramp_end() noexcept {
    return kLandingEnd + (kTop / ramp_tangent());
}

[[nodiscard]] Transform box(Vec3 centre, Vec3 size) noexcept {
    Transform placement = Transform::from_translation(centre);
    placement.scale = size;
    return placement;
}

graph::Literal named(const char* text) noexcept {
    graph::Literal literal;
    literal.type = Name::intern("name");
    literal.text = Name::intern(text);
    return literal;
}

graph::Literal scalar(f32 value) noexcept {
    graph::Literal literal;
    literal.type = Name::intern("float");
    literal.value.x = value;
    return literal;
}

/// The text of a skeleton's joint name, as a graph property spells it.
[[nodiscard]] std::string joint_name(const animation::Skeleton& skeleton, u16 joint) noexcept {
    const std::string_view text = skeleton.joints()[joint].name.text();
    return {text.data(), text.size()};
}

}  // namespace

f32 CourseGround::height(f32 x, f32 z, Vec3* normal) noexcept {
    if (normal != nullptr) {
        *normal = Vec3{0.0F, 1.0F, 0.0F};
    }
    if (std::fabs(x) > kHalfWidth || z < kStairStart) {
        return 0.0F;
    }
    const f32 stair_end = kStairStart + (kTread * static_cast<f32>(kSteps));
    if (z < stair_end) {
        const auto step = static_cast<u32>((z - kStairStart) / kTread);
        return kRiser * static_cast<f32>(math::min(step, kSteps - 1U) + 1U);
    }
    if (z < kLandingEnd) {
        return kTop;
    }
    if (z < ramp_end()) {
        if (normal != nullptr) {
            const f32 angle = kRampDegrees * 0.0174532925F;
            *normal = Vec3{0.0F, std::cos(angle), std::sin(angle)};
        }
        return kTop - ((z - kLandingEnd) * ramp_tangent());
    }
    return 0.0F;
}

animation::GroundHit CourseGround::cast_down(const animation::GroundQuery& query) const noexcept {
    animation::GroundHit hit;
    Vec3 normal;
    const f32 ground = height(query.origin.x, query.origin.z, &normal);
    const f32 drop = query.origin.y - ground;
    if (drop < 0.0F || drop > query.length) {
        return hit;
    }
    hit.hit = true;
    hit.point = Vec3{query.origin.x, ground, query.origin.z};
    hit.normal = normal;
    return hit;
}

void course_boxes(Array<CourseBox>& out) noexcept {
    out.clear();
    for (u32 step = 0; step < kSteps; ++step) {
        const f32 top = kRiser * static_cast<f32>(step + 1);
        const f32 front = kStairStart + (kTread * static_cast<f32>(step));
        CourseBox entry;
        entry.placement = box(Vec3{0.0F, top * 0.5F, front + (kTread * 0.5F)},
                              Vec3{kHalfWidth * 2.0F, top, kTread});
        entry.material = step & 1U;
        (void)out.push_back(entry);
    }
    const f32 landing_front = kStairStart + (kTread * static_cast<f32>(kSteps));
    CourseBox landing;
    landing.placement = box(Vec3{0.0F, kTop * 0.5F, 0.5F * (landing_front + kLandingEnd)},
                            Vec3{kHalfWidth * 2.0F, kTop, kLandingEnd - landing_front});
    landing.material = 0;
    (void)out.push_back(landing);

    // THE RAMP: a slab turned about x, its top face the plane `height()` answers for.
    const f32 angle = kRampDegrees * 0.0174532925F;
    const f32 run = ramp_end() - kLandingEnd;
    const Vec3 up{0.0F, std::cos(angle), std::sin(angle)};
    const Vec3 top_centre{0.0F, kTop * 0.5F, kLandingEnd + (run * 0.5F)};
    CourseBox ramp;
    ramp.placement = box(top_centre - (up * (kSlab * 0.5F)),
                         Vec3{kHalfWidth * 2.0F, kSlab, run / std::cos(angle)});
    ramp.placement.rotation = Quat::from_axis_angle(Vec3{1.0F, 0.0F, 0.0F}, angle);
    ramp.material = 1;
    (void)out.push_back(ramp);
}

f32 controller_height(f32 z) noexcept {
    // A capsule of about half a metre's footprint: the ground averaged over it.
    constexpr u32 kSamples = 9;
    constexpr f32 kFootprint = 0.5F;
    f32 total = 0.0F;
    for (u32 sample = 0; sample < kSamples; ++sample) {
        const f32 offset =
            ((static_cast<f32>(sample) / static_cast<f32>(kSamples - 1)) - 0.5F) * kFootprint;
        total += CourseGround::height(0.0F, z + offset);
    }
    return total / static_cast<f32>(kSamples);
}

Status cook_course_program(const animation::Skeleton& skeleton,
                           const animation::SkeletonProfile& humanoid, Array<u8>& out) noexcept {
    using animation::HumanoidJoint;
    const HumanoidJoint legs[] = {HumanoidJoint::Hips,          HumanoidJoint::LeftUpperLeg,
                                  HumanoidJoint::LeftLowerLeg,  HumanoidJoint::LeftFoot,
                                  HumanoidJoint::RightUpperLeg, HumanoidJoint::RightLowerLeg,
                                  HumanoidJoint::RightFoot};
    const char* properties[] = {"pelvis", "hip_l",  "knee_l", "ankle_l",
                                "hip_r",  "knee_r", "ankle_r"};
    Allocator& memory = system_allocator(MemoryDomain::Animation);
    graph::NodeRegistry registry(memory);
    if (Status registered = graph::pose::register_pose_nodes(registry); !registered) {
        return registered;
    }
    graph::Graph graph(memory, Name::intern("course"));
    Status built = graph.add_node(1, Name::intern("pose.clip"));
    built = built ? graph.set_property(1, Name::intern("clip"), named("walk")) : built;
    built = built ? graph.add_node(2, Name::intern("pose.foot_ik")) : built;
    for (u32 index = 0; index < 7 && built; ++index) {
        const u16 joint = humanoid.resolve(legs[index]);
        if (joint == animation::kInvalidJoint) {
            return fail(ErrorCode::NotFound, "the character's humanoid profile has no legs");
        }
        const std::string name = joint_name(skeleton, joint);
        built = graph.set_property(2, Name::intern(properties[index]), named(name.c_str()));
    }
    // The ankle's height over the sole is the bind pose's: a T-pose stands on the floor.
    const u16 ankle = humanoid.resolve(HumanoidJoint::LeftFoot);
    const f32 ankle_height = skeleton.bind_model()[ankle].translation.y;
    built =
        built ? graph.set_property(2, Name::intern("ankle_height"), scalar(ankle_height)) : built;
    built = built ? graph.set_property(2, Name::intern("weight_parameter"), named("feet")) : built;
    built = built ? graph.set_property(2, Name::intern("smoothing"), scalar(0.06F)) : built;
    built = built ? graph.add_node(3, Name::intern("pose.state")) : built;
    built = built ? graph.set_property(3, Name::intern("name"), named("walk")) : built;
    built = built ? graph.connect(1, Name::intern("pose"), 2, Name::intern("a")) : built;
    built = built ? graph.connect(2, Name::intern("pose"), 3, Name::intern("pose")) : built;
    if (!built) {
        return built;
    }
    graph.resolve(registry);
    graph::DiagnosticSink sink(memory);
    Expected<graph::pose::PoseProgram, Error> program =
        graph::pose::compile_pose(graph, registry, skeleton.joint_count(), sink);
    if (!program) {
        return Status{make_unexpected(program.error())};
    }
    if (sink.errors() != 0) {
        return fail(ErrorCode::InvalidArgument, "the course's program did not compile");
    }
    out.clear();
    return animation::encode_program(*program, out);
}

}  // namespace cy::sample::character
