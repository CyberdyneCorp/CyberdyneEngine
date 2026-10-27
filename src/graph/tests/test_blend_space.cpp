// SPDX-License-Identifier: MIT
// Blend spaces in the animation IR: the `pose.blend1d` and `pose.blend2d` nodes, their weights, the
// cook-time triangulation, the refusals, and the foot placement and aim nodes' tables. Issue #92.
//
// INTEGRATION, beside the other lowering cases: each compiles a graph.

#include <cy/core/memory/system_allocator.h>
#include <cy/graph/lower_pose.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string_view>
#include <utility>

using namespace cy;
using namespace cy::graph;

namespace {

[[nodiscard]] Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Animation);
}

[[nodiscard]] Literal text(const char* value) noexcept {
    Literal literal;
    literal.type = Name::intern("name");
    literal.text = Name::intern(value);
    return literal;
}

[[nodiscard]] Literal at(f32 x, f32 y = 0.0F) noexcept {
    Literal literal;
    literal.type = Name::intern("vec2");
    literal.value.x = x;
    literal.value.y = y;
    return literal;
}

[[nodiscard]] Literal flag(bool value) noexcept {
    Literal literal;
    literal.type = Name::intern("bool");
    literal.value.mask = value ? 1U : 0U;
    return literal;
}

struct Sample {
    const char* clip = "";
    f32 x = 0.0F;
    f32 y = 0.0F;
};

/// One state whose pose is a blend space over clips, one per sample, wired to `s0`, `s1`, ... in
/// the order given. Node 100 is the space; clips are 1.., the state is 200.
class SpaceGraph {
public:
    SpaceGraph(bool planar, std::initializer_list<Sample> samples) noexcept
        : registry_(allocator()), graph_(allocator(), Name::intern("space")), sink_(allocator()) {
        keep(pose::register_pose_nodes(registry_));
        keep(graph_.add_node(100, Name::intern(planar ? "pose.blend2d" : "pose.blend1d")));
        keep(graph_.set_property(100, Name::intern("x_parameter"), text("speed")));
        if (planar) {
            keep(graph_.set_property(100, Name::intern("y_parameter"), text("direction")));
        }
        u32 index = 0;
        for (const Sample& sample : samples) {
            const NodeKey clip = index + 1;
            char pin[8] = {};
            (void)std::snprintf(pin, sizeof(pin), "s%u", index);
            keep(graph_.add_node(clip, Name::intern("pose.clip")));
            keep(graph_.set_property(clip, Name::intern("clip"), text(sample.clip)));
            keep(graph_.connect(clip, Name::intern("pose"), 100, Name::intern(pin)));
            keep(graph_.set_property(100, Name::intern(pin), at(sample.x, sample.y)));
            ++index;
        }
        keep(graph_.add_node(200, Name::intern("pose.state")));
        keep(graph_.connect(100, Name::intern("pose"), 200, Name::intern("pose")));
    }

    void prop(NodeKey node, const char* name, const Literal& value) noexcept {
        keep(graph_.set_property(node, Name::intern(name), value));
    }

    [[nodiscard]] Expected<pose::PoseProgram, Error> compile() noexcept {
        graph_.resolve(registry_);
        return pose::compile_pose(graph_, registry_, 8, sink_);
    }

    [[nodiscard]] const DiagnosticSink& sink() const noexcept { return sink_; }
    [[nodiscard]] bool built() const noexcept { return status_.has_value(); }
    [[nodiscard]] Graph& graph() noexcept { return graph_; }

private:
    void keep(const Status& step) noexcept {
        if (status_ && !step) {
            status_ = step;
        }
    }

    NodeRegistry registry_;
    Graph graph_;
    DiagnosticSink sink_;
    Status status_ = ok();
};

/// The weight of each sample at the parameters, by the CLIP each sample plays, so a case reads
/// "walk 0.5" rather than a sample index the compiler's sort chose.
struct Weights {
    f32 value[pose::kMaxBlendSamples] = {};
    const pose::PoseProgram* program = nullptr;

    [[nodiscard]] f32 of(const char* clip) const noexcept {
        const pose::BlendSpace& space = program->blend_spaces()[0];
        for (u32 index = 0; index < space.sample_count; ++index) {
            const pose::PoseValue sample =
                program->blend_samples()[space.first_sample + index].value;
            const pose::ClipRef& ref = program->clips()[program->code()[sample].clip];
            if (ref.name == Name::intern(clip)) {
                return value[index];
            }
        }
        return -1.0F;
    }
};

[[nodiscard]] Weights weights_at(const pose::PoseProgram& program, f32 x, f32 y = 0.0F) noexcept {
    Weights weights;
    weights.program = &program;
    f32 parameters[16] = {};
    for (usize index = 0; index < program.parameters().size(); ++index) {
        if (program.parameters()[index] == Name::intern("speed")) {
            parameters[index] = x;
        } else if (program.parameters()[index] == Name::intern("direction")) {
            parameters[index] = y;
        }
    }
    (void)pose::blend_space_weights(program, 0,
                                    Span<const f32>(parameters, program.parameters().size()),
                                    Span<f32>(weights.value, pose::kMaxBlendSamples));
    return weights;
}

[[nodiscard]] bool reported(const DiagnosticSink& sink, std::string_view code) noexcept {
    return std::ranges::any_of(sink.entries(), [code](const Diagnostic& diagnostic) {
        return std::string_view(diagnostic.code) == code;
    });
}

constexpr f32 kExact = 1e-6F;

}  // namespace

CY_TEST_CASE("graph_pose: a one-dimensional blend space interpolates the two samples around it") {
    // Authored out of order: the compiler sorts along the axis.
    SpaceGraph space(false, {{"run", 4.0F}, {"idle", 0.0F}, {"walk", 1.5F}});
    CY_REQUIRE(space.built());
    auto program = space.compile();
    CY_REQUIRE(program.has_value());
    CY_CHECK_EQ(space.sink().errors(), 0U);
    CY_REQUIRE_EQ(program->blend_spaces().size(), 1U);
    const pose::BlendSpace& compiled = program->blend_spaces()[0];
    CY_CHECK_EQ(compiled.dimensions, 1U);
    CY_CHECK_EQ(compiled.sample_count, 3U);
    CY_CHECK_EQ(program->blend_samples()[0].x, 0.0F);
    CY_CHECK_EQ(program->blend_samples()[2].x, 4.0F);

    const Weights between = weights_at(*program, 0.75F);
    CY_CHECK(std::fabs(between.of("idle") - 0.5F) < kExact);
    CY_CHECK(std::fabs(between.of("walk") - 0.5F) < kExact);
    CY_CHECK_EQ(between.of("run"), 0.0F);

    const Weights upper = weights_at(*program, 2.75F);
    CY_CHECK(std::fabs(upper.of("walk") - 0.5F) < kExact);
    CY_CHECK(std::fabs(upper.of("run") - 0.5F) < kExact);

    // Outside the samples the space clamps to its end rather than extrapolating a negative weight.
    CY_CHECK_EQ(weights_at(*program, -3.0F).of("idle"), 1.0F);
    CY_CHECK_EQ(weights_at(*program, 9.0F).of("run"), 1.0F);
    CY_CHECK_EQ(weights_at(*program, 1.5F).of("walk"), 1.0F);
}

CY_TEST_CASE("graph_pose: a two-dimensional blend space weights a point barycentrically") {
    // Speed forward on y, lateral on x: a centre and four directions.
    SpaceGraph space(true, {{"idle", 0.0F, 0.0F},
                            {"forward", 0.0F, 1.0F},
                            {"right", 1.0F, 0.0F},
                            {"left", -1.0F, 0.0F},
                            {"back", 0.0F, -1.0F}});
    CY_REQUIRE(space.built());
    auto program = space.compile();
    CY_REQUIRE(program.has_value());
    CY_CHECK_EQ(space.sink().errors(), 0U);
    const pose::BlendSpace& compiled = program->blend_spaces()[0];
    CY_CHECK_EQ(compiled.dimensions, 2U);
    // Five points with one inside the hull of the other four: four triangles.
    CY_CHECK_EQ(compiled.triangle_count, 4U);

    // (0.25, 0.5) lies in the triangle idle-forward-right, at barycentric (0.25, 0.5, 0.25).
    const Weights inside = weights_at(*program, 0.25F, 0.5F);
    CY_CHECK(std::fabs(inside.of("idle") - 0.25F) < kExact);
    CY_CHECK(std::fabs(inside.of("forward") - 0.5F) < kExact);
    CY_CHECK(std::fabs(inside.of("right") - 0.25F) < kExact);
    CY_CHECK_EQ(inside.of("left"), 0.0F);
    CY_CHECK_EQ(inside.of("back"), 0.0F);

    // (-0.5, -0.25) is in idle-left-back: idle 0.25, left 0.5, back 0.25.
    const Weights behind = weights_at(*program, -0.5F, -0.25F);
    CY_CHECK(std::fabs(behind.of("idle") - 0.25F) < kExact);
    CY_CHECK(std::fabs(behind.of("left") - 0.5F) < kExact);
    CY_CHECK(std::fabs(behind.of("back") - 0.25F) < kExact);

    // Outside the hull the nearest point of its edge is weighted: (2, 2) projects onto the middle
    // of forward-right.
    const Weights outside = weights_at(*program, 2.0F, 2.0F);
    CY_CHECK(std::fabs(outside.of("forward") - 0.5F) < kExact);
    CY_CHECK(std::fabs(outside.of("right") - 0.5F) < kExact);
    CY_CHECK_EQ(outside.of("idle"), 0.0F);

    // A sample's own position is that sample alone, and every weighting sums to one.
    CY_CHECK(std::fabs(weights_at(*program, 0.0F, 1.0F).of("forward") - 1.0F) < kExact);
    for (const f32 x : {-0.9F, -0.3F, 0.0F, 0.4F, 0.8F}) {
        for (const f32 y : {-0.7F, 0.0F, 0.2F, 0.6F}) {
            const Weights any = weights_at(*program, x, y);
            f32 total = 0.0F;
            for (u32 index = 0; index < compiled.sample_count; ++index) {
                CY_CHECK_GE(any.value[index], 0.0F);
                total += any.value[index];
            }
            CY_CHECK(std::fabs(total - 1.0F) < 1e-5F);
        }
    }
}

CY_TEST_CASE("graph_pose: a locomotion grid's co-circular quads triangulate the same every time") {
    // A 3x3 grid: every cell's four corners lie on one circle, which an exact in-circle test
    // would split by rounding.
    const auto build = []() {
        return SpaceGraph(true, {{"a", -1.0F, -1.0F},
                                 {"b", 0.0F, -1.0F},
                                 {"c", 1.0F, -1.0F},
                                 {"d", -1.0F, 0.0F},
                                 {"e", 0.0F, 0.0F},
                                 {"f", 1.0F, 0.0F},
                                 {"g", -1.0F, 1.0F},
                                 {"h", 0.0F, 1.0F},
                                 {"i", 1.0F, 1.0F}});
    };
    SpaceGraph first = build();
    SpaceGraph second = build();
    auto one = first.compile();
    auto two = second.compile();
    CY_REQUIRE(one.has_value());
    CY_REQUIRE(two.has_value());
    // Four cells, two triangles each, covering the square.
    CY_REQUIRE_EQ(one->blend_spaces()[0].triangle_count, 8U);
    CY_CHECK_EQ(one->digest(), two->digest());
    for (u32 index = 0; index < 8; ++index) {
        for (u32 vertex = 0; vertex < 3; ++vertex) {
            CY_CHECK_EQ(one->blend_triangles()[index].sample[vertex],
                        two->blend_triangles()[index].sample[vertex]);
        }
    }
    // The centre of a cell is two samples' worth of each of its diagonal's ends, whichever
    // diagonal was chosen: the weights still sum to one and touch only that cell.
    const Weights cell = weights_at(*one, 0.5F, 0.5F);
    f32 total = cell.of("e") + cell.of("f") + cell.of("h") + cell.of("i");
    CY_CHECK(std::fabs(total - 1.0F) < 1e-5F);
    CY_CHECK_EQ(cell.of("a"), 0.0F);
}

CY_TEST_CASE("graph_pose: a blend space that cannot weight its samples is refused by name") {
    SpaceGraph line(true, {{"a", 0.0F, 0.0F}, {"b", 1.0F, 1.0F}, {"c", 2.0F, 2.0F}});
    (void)line.compile();
    CY_CHECK(reported(line.sink(), "pose.blend_space.degenerate"));

    SpaceGraph twins(false, {{"a", 1.0F}, {"b", 1.0F}});
    (void)twins.compile();
    CY_CHECK(reported(twins.sink(), "pose.blend_space.duplicate"));

    SpaceGraph unplaced(false, {{"a", 0.0F}});
    CY_REQUIRE(unplaced.graph().add_node(7, Name::intern("pose.clip")).has_value());
    CY_REQUIRE(
        unplaced.graph().connect(7, Name::intern("pose"), 100, Name::intern("s5")).has_value());
    (void)unplaced.compile();
    CY_CHECK(reported(unplaced.sink(), "pose.blend_space.position"));
}

CY_TEST_CASE("graph_pose: a synchronised space has a phase of its own, and the digest knows it") {
    SpaceGraph plain(false, {{"walk", 1.0F}, {"run", 3.0F}});
    SpaceGraph synced(false, {{"walk", 1.0F}, {"run", 3.0F}});
    synced.prop(100, "sync", flag(true));
    auto unsynced_program = plain.compile();
    auto synced_program = synced.compile();
    CY_REQUIRE(unsynced_program.has_value());
    CY_REQUIRE(synced_program.has_value());
    CY_CHECK_FALSE(unsynced_program->blend_spaces()[0].sync);
    CY_REQUIRE(synced_program->blend_spaces()[0].sync);
    const u16 phase = synced_program->blend_spaces()[0].phase_param;
    CY_CHECK_EQ(synced_program->parameters()[phase], Name::intern("phase.100"));
    CY_CHECK_NE(unsynced_program->digest(), synced_program->digest());
    // Moving one sample is a different program, with the same code, clips and parameters.
    SpaceGraph moved(false, {{"walk", 1.0F}, {"run", 3.5F}});
    auto moved_program = moved.compile();
    CY_REQUIRE(moved_program.has_value());
    CY_CHECK_EQ(moved_program->parameters().size(), unsynced_program->parameters().size());
    CY_CHECK_NE(moved_program->digest(), unsynced_program->digest());

    // Every sample is read wherever the space is read: dependency analysis reaches them all.
    for (u32 index = 0; index < 2; ++index) {
        const pose::PoseValue value = synced_program->blend_samples()[index].value;
        CY_CHECK_FALSE(synced_program->code()[value].required.empty());
    }
}

CY_TEST_CASE("graph_pose: the reference evaluator blends a space's samples by their weights") {
    SpaceGraph space(false, {{"idle", 0.0F}, {"walk", 2.0F}, {"run", 4.0F}});
    auto program = space.compile();
    CY_REQUIRE(program.has_value());

    // Each clip writes its own index + 1 into every channel, so the blend is readable.
    class Sampler final : public pose::PoseSampler {
    public:
        explicit Sampler(const pose::PoseProgram& program) noexcept : program_(&program) {}
        void sample(const pose::ClipRef& clip, f32 /*time*/, const pose::JointMask& mask,
                    Span<f32> out) override {
            ++sampled;
            f32 value = 0.0F;
            for (usize index = 0; index < program_->clips().size(); ++index) {
                if (program_->clips()[index].name == clip.name) {
                    value = static_cast<f32>(index + 1);
                }
            }
            for (u32 joint = 0; joint < 8; ++joint) {
                if (mask.test(joint)) {
                    for (u32 channel = 0; channel < pose::kChannelsPerJoint; ++channel) {
                        out[(joint * pose::kChannelsPerJoint) + channel] = value;
                    }
                }
            }
        }
        void reference(const pose::JointMask& /*mask*/, Span<f32> /*out*/) override {}
        void solve_ik(u16 /*chain*/, const pose::JointMask& /*mask*/, Span<f32> /*pose*/) override {
        }
        u32 sampled = 0;

    private:
        const pose::PoseProgram* program_;
    };

    Array<f32> out(allocator());
    CY_REQUIRE(out.resize(static_cast<usize>(8) * pose::kChannelsPerJoint).has_value());
    Array<f32> parameters(allocator());
    CY_REQUIRE(parameters.resize(program->parameters().size()).has_value());
    for (usize index = 0; index < program->parameters().size(); ++index) {
        parameters[index] = program->parameters()[index] == Name::intern("speed") ? 3.0F : 0.0F;
    }
    pose::PoseInstance instance;
    Sampler sampler(*program);
    pose::EvaluationReport report;
    CY_REQUIRE(pose::evaluate(*program, instance, parameters.span(), sampler, out.span(), report)
                   .has_value());
    // Speed 3 is half walk, half run; idle has no weight and is not sampled.
    CY_CHECK_EQ(sampler.sampled, 2U);
    const f32 walk = static_cast<f32>(program->code()[program->blend_samples()[1].value].clip + 1);
    const f32 run = static_cast<f32>(program->code()[program->blend_samples()[2].value].clip + 1);
    CY_CHECK(std::fabs(out[0] - (0.5F * (walk + run))) < kExact);
}

CY_TEST_CASE("graph_pose: foot placement and aim nodes carry their joints by name") {
    NodeRegistry registry(allocator());
    CY_REQUIRE(pose::register_pose_nodes(registry).has_value());
    Graph graph(allocator(), Name::intern("planted"));
    CY_REQUIRE(graph.add_node(1, Name::intern("pose.clip")).has_value());
    CY_REQUIRE(graph.set_property(1, Name::intern("clip"), text("walk")).has_value());
    CY_REQUIRE(graph.add_node(2, Name::intern("pose.foot_ik")).has_value());
    for (const auto& [property, joint] :
         {std::pair{"pelvis", "hips"}, std::pair{"hip_l", "l_hip"}, std::pair{"knee_l", "l_knee"},
          std::pair{"ankle_l", "l_ankle"}, std::pair{"hip_r", "r_hip"},
          std::pair{"knee_r", "r_knee"}, std::pair{"ankle_r", "r_ankle"},
          std::pair{"weight_parameter", "feet"}}) {
        CY_REQUIRE(graph.set_property(2, Name::intern(property), text(joint)).has_value());
    }
    CY_REQUIRE(graph.add_node(3, Name::intern("pose.aim")).has_value());
    CY_REQUIRE(
        graph.set_property(3, Name::intern("chain"), text("spine, chest ,head")).has_value());
    CY_REQUIRE(graph.set_property(3, Name::intern("target_parameter"), text("look")).has_value());
    CY_REQUIRE(graph.add_node(4, Name::intern("pose.state")).has_value());
    CY_REQUIRE(graph.connect(1, Name::intern("pose"), 2, Name::intern("a")).has_value());
    CY_REQUIRE(graph.connect(2, Name::intern("pose"), 3, Name::intern("a")).has_value());
    CY_REQUIRE(graph.connect(3, Name::intern("pose"), 4, Name::intern("pose")).has_value());
    graph.resolve(registry);
    DiagnosticSink sink(allocator());
    auto program = pose::compile_pose(graph, registry, 8, sink);
    CY_REQUIRE(program.has_value());
    CY_CHECK_EQ(sink.errors(), 0U);
    CY_REQUIRE_EQ(program->foot_placements().size(), 1U);
    CY_CHECK_EQ(program->foot_placements()[0].pelvis, Name::intern("hips"));
    CY_CHECK_EQ(program->foot_placements()[0].leg[1][2], Name::intern("r_ankle"));
    CY_REQUIRE_EQ(program->aims().size(), 1U);
    const pose::AimRef& aim = program->aims()[0];
    CY_CHECK_EQ(aim.joint_count, 3U);
    CY_CHECK_EQ(aim.joints[1], Name::intern("chest"));
    CY_CHECK_EQ(program->parameters()[aim.target_param[0]], Name::intern("look.x"));
    CY_CHECK_EQ(program->parameters()[aim.target_param[2]], Name::intern("look.z"));

    // A foot placement that does not name its legs is refused, not bound to nothing.
    Graph broken(allocator(), Name::intern("broken"));
    CY_REQUIRE(broken.add_node(1, Name::intern("pose.clip")).has_value());
    CY_REQUIRE(broken.add_node(2, Name::intern("pose.foot_ik")).has_value());
    CY_REQUIRE(broken.add_node(4, Name::intern("pose.state")).has_value());
    CY_REQUIRE(broken.connect(1, Name::intern("pose"), 2, Name::intern("a")).has_value());
    CY_REQUIRE(broken.connect(2, Name::intern("pose"), 4, Name::intern("pose")).has_value());
    broken.resolve(registry);
    DiagnosticSink refused(allocator());
    (void)pose::compile_pose(broken, registry, 8, refused);
    CY_CHECK(reported(refused, "pose.foot_ik.joints"));
}

CY_TEST_CASE("graph_pose: a cooked blend space that names what it does not have is refused") {
    SpaceGraph space(true, {{"a", 0.0F, 0.0F}, {"b", 1.0F, 0.0F}, {"c", 0.0F, 1.0F}});
    auto program = space.compile();
    CY_REQUIRE(program.has_value());

    pose::PoseProgramParts parts;
    parts.name = program->name();
    parts.code = program->code();
    parts.states = program->states();
    parts.transitions = program->transitions();
    parts.clips = program->clips();
    parts.masks = program->masks();
    parts.parameters = program->parameters();
    parts.joint_count = program->joint_count();
    parts.blend_spaces = program->blend_spaces();
    parts.blend_samples = program->blend_samples();
    parts.blend_triangles = program->blend_triangles();
    CY_CHECK(pose::assemble_pose_program(allocator(), parts).has_value());

    // A triangle naming a fourth sample of a three-sample space.
    pose::BlendTriangle wrong = program->blend_triangles()[0];
    wrong.sample[2] = 3;
    parts.blend_triangles = Span<const pose::BlendTriangle>(&wrong, 1);
    CY_CHECK_FALSE(pose::assemble_pose_program(allocator(), parts).has_value());

    // A sample reading a pose computed after the space.
    parts.blend_triangles = program->blend_triangles();
    pose::BlendSample samples[3] = {program->blend_samples()[0], program->blend_samples()[1],
                                    program->blend_samples()[2]};
    samples[1].value = static_cast<pose::PoseValue>(program->code().size() - 1);
    parts.blend_samples = Span<const pose::BlendSample>(samples, 3);
    CY_CHECK_FALSE(pose::assemble_pose_program(allocator(), parts).has_value());
}
