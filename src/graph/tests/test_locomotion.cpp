// The four-state locomotion program: what it compiles to, and what it does when a host drives it.
//
// The subject is a `PoseProgram` that an engine can produce before an artist has authored anything
// — four imported clips and a machine to sequence them — and the cases here are about the three
// properties that make it a locomotion machine rather than four clips in a list:
//
//   * A SCRIPTED SEQUENCE OF REQUESTS LANDS WHERE IT SHOULD, including the one that does not: a
//     request for idle from the run is refused by the machine's shape, not honoured by accident.
//   * A TRANSITION BLENDS. `animation-and-skinning` requires transitions to carry a blend duration,
//     and lower_pose.cpp reads a zero duration as a cut, so "it blends" has to be measured as more
//     than one frame with both trees evaluated and a weight that moves between them.
//   * DEATH IS TERMINAL AND DOES NOT LOOP. Two separate facts — no outgoing transition, and a clip
//     whose clock clamps instead of wrapping — and a case for each.
//
// The clips are the four Mixamo files the import bridge was built against, with their measured
// durations, so the numbers here are the numbers a cook would actually carry.

#include <cy/core/memory/system_allocator.h>
#include <cy/graph/locomotion.h>
#include <cy/test/test.h>

#include <utility>

using namespace cy;
using namespace cy::graph;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Scripting);
}

constexpr u32 kJoints = 8;
constexpr f32 kFrame = 1.0F / 60.0F;

[[nodiscard]] constexpr u16 index_of(pose::LocomotionState state) noexcept {
    return static_cast<u16>(state);
}

[[nodiscard]] pose::LocomotionSpec mixamo_spec() noexcept {
    pose::LocomotionSpec spec;
    spec.name = Name::intern("mixamo_locomotion");
    spec.idle = {Name::intern("Breathing Idle"), 9.9333F, true};
    spec.walk = {Name::intern("Walking"), 1.0333F, true};
    spec.run = {Name::intern("Running"), 0.6333F, true};
    // The one that does not loop. A death clip that wrapped would kill the character once a second.
    spec.die = {Name::intern("Dying Backwards"), 4.6F, false};
    return spec;
}

/// Writes a constant per clip into channel zero of every joint it is asked for, so a blended pose
/// reads as a number between the two states' constants and the blend weight is observable.
class LocomotionSampler final : public pose::PoseSampler {
public:
    void sample(const pose::ClipRef& clip, f32 time, const pose::JointMask& mask,
                Span<f32> out) override {
        ++samples;
        const f32 value = value_of(clip.name);
        if (clip.name == Name::intern("Dying Backwards")) {
            death_time = time;
        }
        for (u32 joint = 0; joint < kJoints; ++joint) {
            if (mask.test(joint)) {
                out[static_cast<usize>(joint) * pose::kChannelsPerJoint] = value;
            }
        }
    }
    void reference(const pose::JointMask& /*mask*/, Span<f32> /*out*/) override { ++references; }
    void solve_ik(u16 /*chain*/, const pose::JointMask& /*mask*/, Span<f32> /*pose*/) override {}

    /// One, two, three, four, in state order: a pose reading 1.5 is halfway from idle into the
    /// walk, which is the whole point of the constants.
    [[nodiscard]] static f32 value_of(Name clip) noexcept {
        if (clip == Name::intern("Breathing Idle")) {
            return 1.0F;
        }
        if (clip == Name::intern("Walking")) {
            return 2.0F;
        }
        if (clip == Name::intern("Running")) {
            return 3.0F;
        }
        return 4.0F;
    }

    u32 samples = 0;
    u32 references = 0;
    f32 death_time = -1.0F;
};

/// The machine and everything needed to drive it. One per case, because a `PoseInstance` that had
/// been through another case's script is not a fixture, it is a coincidence.
struct Rig {
    explicit Rig(pose::PoseProgram&& compiled) noexcept
        : program(std::move(compiled)), driver(allocator()), out(allocator()) {}

    pose::PoseProgram program;
    pose::LocomotionDriver driver;
    pose::PoseInstance instance;
    Array<f32> out;

    [[nodiscard]] bool prepare() noexcept {
        return driver.bind(program).has_value() &&
               out.resize(static_cast<usize>(kJoints) * pose::kChannelsPerJoint).has_value();
    }

    /// Advance `frames` frames at sixty hertz, keeping the clip clocks the way a host would.
    void tick(u32 frames) noexcept {
        for (u32 frame = 0; frame < frames; ++frame) {
            pose::advance(program, instance, driver.parameters(), kFrame);
            driver.follow(program, instance);
        }
    }

    [[nodiscard]] f32 evaluate(LocomotionSampler& sampler,
                               pose::EvaluationReport& report) noexcept {
        const Status ran =
            pose::evaluate(program, instance, driver.parameters(), sampler, out.span(), report);
        return ran ? out[0] : -1.0F;
    }
};

[[nodiscard]] Expected<pose::PoseProgram, Error> compile(
    const pose::LocomotionSpec& spec) noexcept {
    DiagnosticSink sink(allocator());
    auto program = pose::compile_locomotion(allocator(), spec, kJoints, sink);
    if (program && sink.errors() != 0) {
        return make_unexpected(Error{ErrorCode::Internal,
                                     "the locomotion graph reported a compile diagnostic",
                                     sink.errors()});
    }
    return program;
}

[[nodiscard]] const pose::ClipRef* clip_of(const pose::PoseProgram& program,
                                           pose::LocomotionState state) noexcept {
    const u16 index = index_of(state);
    if (index >= program.states().size()) {
        return nullptr;
    }
    const pose::PoseValue root = program.states()[index].root;
    if (root >= program.code().size()) {
        return nullptr;
    }
    const pose::PoseInstruction& instruction = program.code()[root];
    if (instruction.op != pose::PoseOp::SampleClip || instruction.clip >= program.clips().size()) {
        return nullptr;
    }
    return &program.clips()[instruction.clip];
}

}  // namespace

CY_TEST_CASE("graph_locomotion: four states compile, idle is the entry, and die is terminal") {
    auto compiled = compile(mixamo_spec());
    CY_REQUIRE(compiled.has_value());
    const pose::PoseProgram& program = compiled.value();

    CY_REQUIRE_EQ(program.states().size(), 4U);
    // The enumeration's values ARE the compiled indices. This is an agreement between the builder's
    // node keys and the compiler's "states are numbered by ascending key", and every other case
    // here leans on it.
    CY_CHECK_EQ(program.states()[0].name, Name::intern("idle"));
    CY_CHECK_EQ(program.states()[1].name, Name::intern("walk"));
    CY_CHECK_EQ(program.states()[2].name, Name::intern("run"));
    CY_CHECK_EQ(program.states()[3].name, Name::intern("die"));
    CY_CHECK_EQ(program.entry_state(), index_of(pose::LocomotionState::Idle));

    // idle: walk and die. walk: run, idle and die. run: walk and die. die: NOTHING.
    CY_CHECK_EQ(program.states()[index_of(pose::LocomotionState::Idle)].transition_count, 2U);
    CY_CHECK_EQ(program.states()[index_of(pose::LocomotionState::Walk)].transition_count, 3U);
    CY_CHECK_EQ(program.states()[index_of(pose::LocomotionState::Run)].transition_count, 2U);
    CY_CHECK_EQ(program.states()[index_of(pose::LocomotionState::Die)].transition_count, 0U);

    // Every state samples its own clip, and every transition blends rather than cutting.
    CY_REQUIRE_EQ(program.clips().size(), 4U);
    for (u32 index = 0; index < pose::kLocomotionStateCount; ++index) {
        const pose::ClipRef* clip = clip_of(program, static_cast<pose::LocomotionState>(index));
        CY_REQUIRE(clip != nullptr);
        CY_CHECK_GT(clip->duration, 0.0F);
    }
    CY_CHECK_EQ(clip_of(program, pose::LocomotionState::Run)->name, Name::intern("Running"));
    for (const pose::Transition& transition : program.transitions()) {
        CY_CHECK_GT(transition.duration, 0.0F);
    }
}

CY_TEST_CASE("graph_locomotion: a scripted sequence of state requests lands where expected") {
    auto compiled = compile(mixamo_spec());
    CY_REQUIRE(compiled.has_value());
    Rig rig(std::move(compiled.value()));
    CY_REQUIRE(rig.prepare());
    CY_CHECK_EQ(rig.instance.state, index_of(pose::LocomotionState::Idle));

    rig.driver.request(pose::LocomotionState::Walk);
    rig.tick(30);
    CY_CHECK_EQ(rig.instance.state, index_of(pose::LocomotionState::Walk));
    CY_CHECK_EQ(rig.instance.transition, 0xFFFFU);

    rig.driver.request(pose::LocomotionState::Run);
    rig.tick(30);
    CY_CHECK_EQ(rig.instance.state, index_of(pose::LocomotionState::Run));

    // THERE IS NO run -> idle EDGE. A character decelerates through the walk, so a request the
    // machine has no edge for is simply not honoured — held for a full second here, which is three
    // times the longest blend in the graph.
    rig.driver.request(pose::LocomotionState::Idle);
    rig.tick(60);
    CY_CHECK_EQ(rig.instance.state, index_of(pose::LocomotionState::Run));

    rig.driver.request(pose::LocomotionState::Walk);
    rig.tick(30);
    CY_CHECK_EQ(rig.instance.state, index_of(pose::LocomotionState::Walk));
    rig.driver.request(pose::LocomotionState::Idle);
    rig.tick(30);
    CY_CHECK_EQ(rig.instance.state, index_of(pose::LocomotionState::Idle));

    rig.driver.request(pose::LocomotionState::Die);
    rig.tick(40);
    CY_CHECK_EQ(rig.instance.state, index_of(pose::LocomotionState::Die));
}

CY_TEST_CASE("graph_locomotion: a transition blends over more than one frame") {
    auto compiled = compile(mixamo_spec());
    CY_REQUIRE(compiled.has_value());
    Rig rig(std::move(compiled.value()));
    CY_REQUIRE(rig.prepare());

    LocomotionSampler sampler;
    rig.driver.request(pose::LocomotionState::Walk);

    u32 blending_frames = 0;
    u32 both_trees_frames = 0;
    f32 previous = 0.0F;
    f32 first_weighted = -1.0F;
    f32 last_weighted = -1.0F;
    for (u32 frame = 0; frame < 120 && rig.instance.state != index_of(pose::LocomotionState::Walk);
         ++frame) {
        rig.tick(1);
        if (rig.instance.transition == 0xFFFFU) {
            continue;
        }
        ++blending_frames;
        pose::EvaluationReport report;
        const f32 value = rig.evaluate(sampler, report);
        // BOTH trees are walked while a transition is in flight, and nothing else is: the run and
        // the death states cost nothing while the character is walking off the idle.
        if (report.clips_sampled == 2U) {
            ++both_trees_frames;
        }
        // Idle is 1.0 and walk is 2.0, so the pose is the blend weight plus one, and it only ever
        // moves towards the walk.
        CY_CHECK_GE(value, previous);
        CY_CHECK_GE(value, 1.0F);
        CY_CHECK_LE(value, 2.0F);
        previous = value;
        if (value > 1.0F && value < 2.0F) {
            last_weighted = value;
            if (first_weighted < 0.0F) {
                first_weighted = value;
            }
        }
    }

    // "More than one frame" is the claim and the point: a cut would spend zero frames blending and
    // land in the walk on the first advance. The exact count is 0.20 s at sixty hertz, which is
    // twelve or thirteen frames depending on where the accumulated float lands — asserting the
    // range rather than the number keeps the case about the blend and not about rounding.
    CY_CHECK_GT(blending_frames, 1U);
    CY_CHECK_GE(blending_frames, 10U);
    CY_CHECK_LE(blending_frames, 14U);
    CY_CHECK_EQ(both_trees_frames, blending_frames);
    // The weight actually moved: at least two distinct intermediate poses, neither endpoint.
    CY_CHECK_GT(first_weighted, 1.0F);
    CY_CHECK_GT(last_weighted, first_weighted);

    CY_CHECK_EQ(rig.instance.state, index_of(pose::LocomotionState::Walk));
    pose::EvaluationReport settled;
    CY_CHECK_NEAR(rig.evaluate(sampler, settled), 2.0F, 1e-5F);
    // Settled, only the active state's tree is walked.
    CY_CHECK_EQ(settled.clips_sampled, 1U);
}

CY_TEST_CASE("graph_locomotion: death interrupts a blend in flight, and nothing interrupts death") {
    auto compiled = compile(mixamo_spec());
    CY_REQUIRE(compiled.has_value());
    Rig rig(std::move(compiled.value()));
    CY_REQUIRE(rig.prepare());

    rig.driver.request(pose::LocomotionState::Walk);
    rig.tick(3);
    CY_REQUIRE_NE(rig.instance.transition, 0xFFFFU);
    CY_CHECK_EQ(rig.instance.target, index_of(pose::LocomotionState::Walk));
    CY_CHECK_EQ(rig.program.transitions()[rig.instance.transition].priority,
                pose::kLocomotionPriority);

    // `animation-and-skinning`: "WHEN a higher-priority transition becomes valid mid-blend and
    // interruption is allowed THEN it SHALL take over". The character is killed halfway into the
    // walk and the walk is abandoned.
    rig.driver.request(pose::LocomotionState::Die);
    rig.tick(1);
    CY_REQUIRE_NE(rig.instance.transition, 0xFFFFU);
    CY_CHECK_EQ(rig.instance.target, index_of(pose::LocomotionState::Die));
    CY_CHECK_EQ(rig.program.transitions()[rig.instance.transition].priority, pose::kDeathPriority);
    CY_CHECK_EQ(rig.program.transitions()[rig.instance.transition].interruption,
                pose::Interruption::None);

    // And the other way round: the death blend allows no interruption, so a host that keeps asking
    // for the walk is asking a question the machine has stopped listening to.
    rig.driver.request(pose::LocomotionState::Walk);
    for (u32 frame = 0; frame < 30 && rig.instance.transition != 0xFFFFU; ++frame) {
        rig.tick(1);
        const bool still_dying = rig.instance.target == 0xFFFFU ||
                                 rig.instance.target == index_of(pose::LocomotionState::Die);
        CY_CHECK(still_dying);
    }
    CY_CHECK_EQ(rig.instance.state, index_of(pose::LocomotionState::Die));
}

CY_TEST_CASE("graph_locomotion: die is terminal and its clip does not loop") {
    auto compiled = compile(mixamo_spec());
    CY_REQUIRE(compiled.has_value());
    Rig rig(std::move(compiled.value()));
    CY_REQUIRE(rig.prepare());

    rig.driver.request(pose::LocomotionState::Die);
    rig.tick(30);
    CY_REQUIRE_EQ(rig.instance.state, index_of(pose::LocomotionState::Die));

    // TERMINAL: the state has no outgoing transition, so no request of any kind moves it. Each of
    // the four is held for two and a half seconds, which outlasts every blend in the graph eight
    // times over, and the four together outlast the death clip itself.
    for (u32 index = 0; index < pose::kLocomotionStateCount; ++index) {
        rig.driver.request(static_cast<pose::LocomotionState>(index));
        rig.tick(150);
        CY_CHECK_EQ(rig.instance.state, index_of(pose::LocomotionState::Die));
        CY_CHECK_EQ(rig.instance.transition, 0xFFFFU);
    }

    // DOES NOT LOOP: ten seconds into a 4.6-second death, the clock stands at the last frame rather
    // than having wrapped back into the fall. `ClipRef::looping` is what `clip_time` reads, and it
    // is false here only because the graph authored it so.
    const pose::ClipRef* death = clip_of(rig.program, pose::LocomotionState::Die);
    CY_REQUIRE(death != nullptr);
    CY_CHECK_FALSE(death->looping);
    CY_CHECK_GT(rig.instance.state_time, death->duration);
    CY_CHECK_NEAR(rig.driver.clock(pose::LocomotionState::Die), death->duration, 1e-3F);

    LocomotionSampler sampler;
    pose::EvaluationReport report;
    CY_CHECK_NEAR(rig.evaluate(sampler, report), 4.0F, 1e-5F);
    CY_CHECK_NEAR(sampler.death_time, death->duration, 1e-3F);

    // The contrast, in the same program: the three locomotion clips wrap, and the idle's clock a
    // whole cycle and a half in is half a cycle, not a cycle and a half.
    const pose::ClipRef* idle = clip_of(rig.program, pose::LocomotionState::Idle);
    CY_REQUIRE(idle != nullptr);
    CY_CHECK(idle->looping);
    CY_CHECK_NEAR(pose::clip_time(*idle, idle->duration * 1.5F), idle->duration * 0.5F, 1e-3F);
    CY_CHECK_NEAR(pose::clip_time(*death, death->duration * 1.5F), death->duration, 1e-3F);
}

CY_TEST_CASE("graph_locomotion: the builder refuses a cut, a looping death, and a nameless clip") {
    Graph graph(allocator(), Name::intern("rejected"));

    pose::LocomotionSpec cut = mixamo_spec();
    cut.idle_to_walk = 0.0F;
    auto refused_cut = pose::build_locomotion_graph(graph, cut);
    CY_REQUIRE_FALSE(refused_cut.has_value());
    CY_CHECK_EQ(refused_cut.error().code, ErrorCode::InvalidArgument);

    pose::LocomotionSpec looping_death = mixamo_spec();
    looping_death.die.looping = true;
    auto refused_loop = pose::build_locomotion_graph(graph, looping_death);
    CY_REQUIRE_FALSE(refused_loop.has_value());
    CY_CHECK_EQ(refused_loop.error().code, ErrorCode::InvalidArgument);

    pose::LocomotionSpec nameless = mixamo_spec();
    nameless.run.clip = Name{};
    auto refused_name = pose::build_locomotion_graph(graph, nameless);
    CY_REQUIRE_FALSE(refused_name.has_value());
    CY_CHECK_EQ(refused_name.error().code, ErrorCode::InvalidArgument);

    // Nothing was written on the way to any of those refusals.
    CY_CHECK_EQ(graph.nodes().size(), 0U);
}

CY_TEST_CASE("graph_pose: the program digest closes over the clips and the transition rules") {
    // A digest that does not close over the clip a `SampleClip` names, or over the rule a
    // transition carries, hashes two different machines the same — and a cook keyed on it serves
    // the walk where the sprint was asked for. An instruction records INDICES, and an index says
    // nothing about what it indexes.
    auto walking = compile(mixamo_spec());
    CY_REQUIRE(walking.has_value());

    pose::LocomotionSpec sprinting = mixamo_spec();
    sprinting.run.clip = Name::intern("Sprinting");
    auto sprint = compile(sprinting);
    CY_REQUIRE(sprint.has_value());
    CY_CHECK_NE(walking.value().digest(), sprint.value().digest());

    // Identical input, identical digest: the point of the hash is that it is a function of the
    // program and of nothing else.
    auto again = compile(mixamo_spec());
    CY_REQUIRE(again.has_value());
    CY_CHECK_EQ(walking.value().digest(), again.value().digest());

    // And the rule half, on a machine small enough to differ in exactly one property.
    const auto two_state =
        [](const char* interruption, u32 priority,
           const char* condition) noexcept -> Expected<pose::PoseProgram, Error> {
        NodeRegistry registry(allocator());
        if (Status registered = pose::register_pose_nodes(registry); !registered) {
            return make_unexpected(registered.error());
        }
        Graph graph(allocator(), Name::intern("pair"));
        Literal clip_name;
        clip_name.type = Name::intern("name");
        for (NodeKey key : {NodeKey{1}, NodeKey{2}}) {
            if (Status added = graph.add_node(key, Name::intern("pose.clip")); !added) {
                return make_unexpected(added.error());
            }
            clip_name.text = Name::intern(key == 1 ? "a" : "b");
            if (Status set = graph.set_property(key, Name::intern("clip"), clip_name); !set) {
                return make_unexpected(set.error());
            }
            const NodeKey state = key + 10;
            if (Status added = graph.add_node(state, Name::intern("pose.state")); !added) {
                return make_unexpected(added.error());
            }
            if (Status wired =
                    graph.connect(key, Name::intern("pose"), state, Name::intern("pose"));
                !wired) {
                return make_unexpected(wired.error());
            }
        }
        if (Status added = graph.add_node(30, Name::intern("pose.transition")); !added) {
            return make_unexpected(added.error());
        }
        Literal rule;
        rule.type = Name::intern("name");
        rule.text = Name::intern(interruption);
        if (Status set = graph.set_property(30, Name::intern("interruption"), rule); !set) {
            return make_unexpected(set.error());
        }
        rule.text = Name::intern(condition);
        if (Status set = graph.set_property(30, Name::intern("condition"), rule); !set) {
            return make_unexpected(set.error());
        }
        Literal number;
        number.type = Name::intern("int");
        number.value.mask = priority;
        if (Status set = graph.set_property(30, Name::intern("priority"), number); !set) {
            return make_unexpected(set.error());
        }
        Literal duration;
        duration.type = Name::intern("float");
        duration.value = Immediate::scalar(0.2F);
        if (Status set = graph.set_property(30, Name::intern("duration"), duration); !set) {
            return make_unexpected(set.error());
        }
        if (Status wired = graph.connect(11, Name::intern("pose"), 30, Name::intern("from"));
            !wired) {
            return make_unexpected(wired.error());
        }
        if (Status wired = graph.connect(12, Name::intern("pose"), 30, Name::intern("to"));
            !wired) {
            return make_unexpected(wired.error());
        }
        graph.resolve(registry);
        DiagnosticSink sink(allocator());
        return pose::compile_pose(graph, registry, kJoints, sink);
    };

    auto base = two_state("none", 1, "go");
    auto by_priority = two_state("none", 2, "go");
    auto by_interruption = two_state("any", 1, "go");
    auto by_condition = two_state("none", 1, "stop");
    CY_REQUIRE(base.has_value());
    CY_REQUIRE(by_priority.has_value());
    CY_REQUIRE(by_interruption.has_value());
    CY_REQUIRE(by_condition.has_value());
    CY_CHECK_NE(base.value().digest(), by_priority.value().digest());
    CY_CHECK_NE(base.value().digest(), by_interruption.value().digest());
    CY_CHECK_NE(base.value().digest(), by_condition.value().digest());
}
