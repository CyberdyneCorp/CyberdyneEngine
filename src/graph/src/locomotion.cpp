// The four-state locomotion graph: the nodes it writes, and the parameter block a host drives.
//
// See locomotion.h for why a builder exists, for the three decisions in the machine it writes, and
// for what "die does not loop" means once the graph is a compiled program.

#include <cy/graph/locomotion.h>

#include <cmath>

namespace cy::graph::pose {

namespace {

/// THE NODE KEYS ARE PART OF THE CONTRACT, not an implementation detail.
///
/// `compile_pose()` numbers states by ascending node key and makes state 0 the entry, so idle's key
/// has to be the lowest one in the graph for idle to be the entry state. Ten apart, with the clip
/// node one above its state, leaves room to author a blend space or an additive layer under a state
/// later without renumbering anything — and a renumbering would change every state index a saved
/// `PoseInstance` refers to.
[[nodiscard]] constexpr NodeKey state_key(LocomotionState state) noexcept {
    return static_cast<NodeKey>(static_cast<u32>(state) + 1U) * 10U;
}

[[nodiscard]] constexpr NodeKey clip_key(LocomotionState state) noexcept {
    return state_key(state) + 1U;
}

[[nodiscard]] Literal number(f32 value) noexcept {
    Literal literal;
    literal.type = Name::intern("float");
    literal.value = Immediate::scalar(value);
    return literal;
}

[[nodiscard]] Literal integer(u32 value) noexcept {
    Literal literal;
    literal.type = Name::intern("int");
    literal.value.mask = value;
    return literal;
}

[[nodiscard]] Literal boolean(bool value) noexcept {
    Literal literal;
    literal.type = Name::intern("bool");
    literal.value.mask = value ? 1U : 0U;
    return literal;
}

[[nodiscard]] Literal named(Name value) noexcept {
    Literal literal;
    literal.type = Name::intern("name");
    literal.text = value;
    return literal;
}

/// One authored transition. The table below is the whole state machine, and it is a table so that a
/// reviewer counts seven edges rather than reading seven blocks of wiring.
struct Edge {
    NodeKey key;
    LocomotionState from;
    LocomotionState to;
    f32 duration;
    u16 priority;
    /// "none" or "higher_priority", as lower_pose.cpp spells them.
    const char* interruption;
};

[[nodiscard]] const LocomotionClip& clip_of(const LocomotionSpec& spec,
                                            LocomotionState state) noexcept {
    switch (state) {
        case LocomotionState::Walk:
            return spec.walk;
        case LocomotionState::Run:
            return spec.run;
        case LocomotionState::Die:
            return spec.die;
        case LocomotionState::Idle:
        case LocomotionState::Count:
            break;
    }
    return spec.idle;
}

/// A locomotion edge: it can be outranked, and death is the only thing that outranks it.
[[nodiscard]] Edge locomotion_edge(NodeKey key, LocomotionState from, LocomotionState to,
                                   f32 duration) noexcept {
    return Edge{key, from, to, duration, kLocomotionPriority, "higher_priority"};
}

/// A death edge: it outranks everything and admits no interruption, which is the two halves of
/// decision 2 in locomotion.h.
[[nodiscard]] Edge death_edge(NodeKey key, LocomotionState from, f32 duration) noexcept {
    return Edge{key, from, LocomotionState::Die, duration, kDeathPriority, "none"};
}

/// The seven edges, keyed so that a source state's edges are contiguous and in the order this table
/// lists them — which is the order `PoseState::first_transition` will run over.
void fill_edges(const LocomotionSpec& spec, Edge (&edges)[7]) noexcept {
    edges[0] =
        locomotion_edge(100, LocomotionState::Idle, LocomotionState::Walk, spec.idle_to_walk);
    edges[1] = death_edge(101, LocomotionState::Idle, spec.to_die);
    edges[2] = locomotion_edge(110, LocomotionState::Walk, LocomotionState::Run, spec.walk_to_run);
    edges[3] =
        locomotion_edge(111, LocomotionState::Walk, LocomotionState::Idle, spec.walk_to_idle);
    edges[4] = death_edge(112, LocomotionState::Walk, spec.to_die);
    edges[5] = locomotion_edge(120, LocomotionState::Run, LocomotionState::Walk, spec.run_to_walk);
    edges[6] = death_edge(121, LocomotionState::Run, spec.to_die);
}

[[nodiscard]] Status check_spec(const LocomotionSpec& spec, const Edge (&edges)[7]) noexcept {
    for (u32 index = 0; index < kLocomotionStateCount; ++index) {
        if (clip_of(spec, static_cast<LocomotionState>(index)).clip.is_empty()) {
            return make_unexpected(Error{
                ErrorCode::InvalidArgument,
                "every locomotion state names a clip; an unnamed one would compile to a reference "
                "no clip table can be matched against",
                index});
        }
    }
    if (spec.die.looping) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument,
                  "the death state is terminal, and a looping death clip returns the character to "
                  "the moment it was killed for as long as the entity exists",
                  0});
    }
    for (const Edge& edge : edges) {
        if (!(edge.duration > 0.0F)) {
            // A zero duration is a CUT to lower_pose.cpp: `advance` moves straight to the target
            // state and the two trees are never evaluated together. Refused here rather than
            // written, because the pop it produces is silent and a defaulted field is easy to miss.
            return make_unexpected(
                Error{ErrorCode::InvalidArgument,
                      "a locomotion transition blends; a duration of zero is a cut, and a cut "
                      "between two locomotion poses pops",
                      static_cast<i64>(edge.key)});
        }
    }
    return ok();
}

[[nodiscard]] Status add_state(Graph& graph, const LocomotionSpec& spec,
                               LocomotionState state) noexcept {
    const LocomotionClip& clip = clip_of(spec, state);
    const NodeKey clip_node = clip_key(state);
    const NodeKey state_node = state_key(state);
    if (Status added = graph.add_node(clip_node, Name::intern("pose.clip")); !added) {
        return added;
    }
    if (Status set = graph.set_property(clip_node, Name::intern("clip"), named(clip.clip)); !set) {
        return set;
    }
    if (Status set = graph.set_property(clip_node, Name::intern("duration"), number(clip.duration));
        !set) {
        return set;
    }
    if (Status set = graph.set_property(clip_node, Name::intern("loop"), boolean(clip.looping));
        !set) {
        return set;
    }
    if (Status set = graph.set_property(clip_node, Name::intern("time_parameter"),
                                        named(locomotion_clock(state)));
        !set) {
        return set;
    }
    if (Status added = graph.add_node(state_node, Name::intern("pose.state")); !added) {
        return added;
    }
    if (Status set = graph.set_property(state_node, Name::intern("name"),
                                        named(Name::intern(locomotion_state_name(state))));
        !set) {
        return set;
    }
    return graph.connect(clip_node, Name::intern("pose"), state_node, Name::intern("pose"));
}

[[nodiscard]] Status add_edge(Graph& graph, const Edge& edge) noexcept {
    if (Status added = graph.add_node(edge.key, Name::intern("pose.transition")); !added) {
        return added;
    }
    if (Status set = graph.set_property(edge.key, Name::intern("condition"),
                                        named(locomotion_request(edge.to)));
        !set) {
        return set;
    }
    if (Status set = graph.set_property(edge.key, Name::intern("duration"), number(edge.duration));
        !set) {
        return set;
    }
    if (Status set = graph.set_property(edge.key, Name::intern("priority"), integer(edge.priority));
        !set) {
        return set;
    }
    if (Status set = graph.set_property(edge.key, Name::intern("interruption"),
                                        named(Name::intern(edge.interruption)));
        !set) {
        return set;
    }
    // The wires are state -> transition, on the `from` and `to` pins: lower_pose.cpp reads the
    // links rather than a property, so a transition between two states is a thing an editor can
    // draw.
    if (Status wired = graph.connect(state_key(edge.from), Name::intern("pose"), edge.key,
                                     Name::intern("from"));
        !wired) {
        return wired;
    }
    return graph.connect(state_key(edge.to), Name::intern("pose"), edge.key, Name::intern("to"));
}

[[nodiscard]] Expected<u16, Error> parameter_index(const PoseProgram& program,
                                                   Name parameter) noexcept {
    const Span<const Name> parameters = program.parameters();
    for (usize index = 0; index < parameters.size(); ++index) {
        if (parameters[index] == parameter) {
            return static_cast<u16>(index);
        }
    }
    return make_unexpected(Error{ErrorCode::NotFound,
                                 "this program does not carry one of the locomotion parameters, "
                                 "which is what a program built elsewhere looks like from here",
                                 0});
}

}  // namespace

const char* locomotion_state_name(LocomotionState state) noexcept {
    switch (state) {
        case LocomotionState::Idle:
            return "idle";
        case LocomotionState::Walk:
            return "walk";
        case LocomotionState::Run:
            return "run";
        case LocomotionState::Die:
            return "die";
        case LocomotionState::Count:
            break;
    }
    return "";
}

Name locomotion_request(LocomotionState state) noexcept {
    switch (state) {
        case LocomotionState::Idle:
            return Name::intern("request_idle");
        case LocomotionState::Walk:
            return Name::intern("request_walk");
        case LocomotionState::Run:
            return Name::intern("request_run");
        case LocomotionState::Die:
            return Name::intern("request_die");
        case LocomotionState::Count:
            break;
    }
    return Name{};
}

Name locomotion_clock(LocomotionState state) noexcept {
    switch (state) {
        case LocomotionState::Idle:
            return Name::intern("clock_idle");
        case LocomotionState::Walk:
            return Name::intern("clock_walk");
        case LocomotionState::Run:
            return Name::intern("clock_run");
        case LocomotionState::Die:
            return Name::intern("clock_die");
        case LocomotionState::Count:
            break;
    }
    return Name{};
}

Status build_locomotion_graph(Graph& graph, const LocomotionSpec& spec) noexcept {
    Edge edges[7] = {};
    fill_edges(spec, edges);
    if (Status checked = check_spec(spec, edges); !checked) {
        return checked;
    }
    if (!spec.name.is_empty()) {
        graph.set_name(spec.name);
    }
    // States first and in enumeration order, so the node keys ascend the way the state indices do.
    for (u32 index = 0; index < kLocomotionStateCount; ++index) {
        if (Status added = add_state(graph, spec, static_cast<LocomotionState>(index)); !added) {
            return added;
        }
    }
    for (const Edge& edge : edges) {
        if (Status added = add_edge(graph, edge); !added) {
            return added;
        }
    }
    return ok();
}

Expected<PoseProgram, Error> compile_locomotion(Allocator& allocator, const LocomotionSpec& spec,
                                                u32 joint_count, DiagnosticSink& sink) noexcept {
    NodeRegistry registry(allocator);
    if (Status registered = register_pose_nodes(registry); !registered) {
        return make_unexpected(registered.error());
    }
    Graph graph(allocator, Name::intern("locomotion"));
    if (Status built = build_locomotion_graph(graph, spec); !built) {
        return make_unexpected(built.error());
    }
    graph.resolve(registry);
    // The graph dies with this call and the program does not: a `PoseProgram`'s arrays hold the
    // allocator the graph was given, never the graph.
    return compile_pose(graph, registry, joint_count, sink);
}

f32 clip_time(const ClipRef& clip, f32 elapsed) noexcept {
    if (!(elapsed > 0.0F) || !(clip.duration > 0.0F)) {
        return 0.0F;
    }
    if (!clip.looping) {
        return elapsed < clip.duration ? elapsed : clip.duration;
    }
    const f32 wrapped = elapsed - (std::floor(elapsed / clip.duration) * clip.duration);
    // Rounding can land exactly on the duration, which is the first frame of the next cycle rather
    // than one past the last frame of this one.
    return wrapped < clip.duration ? wrapped : 0.0F;
}

LocomotionDriver::LocomotionDriver(Allocator& allocator) noexcept : values_(allocator) {}

Status LocomotionDriver::bind(const PoseProgram& program) noexcept {
    bound_ = false;
    if (Status sized = values_.resize(program.parameters().size()); !sized) {
        return sized;
    }
    for (f32& value : values_) {
        value = 0.0F;
    }
    for (u32 index = 0; index < kLocomotionStateCount; ++index) {
        const auto state = static_cast<LocomotionState>(index);
        auto request = parameter_index(program, locomotion_request(state));
        if (!request) {
            return make_unexpected(request.error());
        }
        auto clock = parameter_index(program, locomotion_clock(state));
        if (!clock) {
            return make_unexpected(clock.error());
        }
        requests_[index] = request.value();
        clocks_[index] = clock.value();
    }
    bound_ = true;
    return ok();
}

void LocomotionDriver::request(LocomotionState state) noexcept {
    clear_requests();
    if (bound_ && static_cast<u32>(state) < kLocomotionStateCount) {
        values_[requests_[static_cast<u32>(state)]] = 1.0F;
    }
}

void LocomotionDriver::clear_requests() noexcept {
    if (!bound_) {
        return;
    }
    for (const u16 index : requests_) {
        values_[index] = 0.0F;
    }
}

void LocomotionDriver::set_clock(LocomotionState state, f32 seconds) noexcept {
    if (bound_ && static_cast<u32>(state) < kLocomotionStateCount) {
        values_[clocks_[static_cast<u32>(state)]] = seconds;
    }
}

f32 LocomotionDriver::clock(LocomotionState state) const noexcept {
    if (!bound_ || static_cast<u32>(state) >= kLocomotionStateCount) {
        return 0.0F;
    }
    return values_[clocks_[static_cast<u32>(state)]];
}

bool LocomotionDriver::requesting(LocomotionState state) const noexcept {
    if (!bound_ || static_cast<u32>(state) >= kLocomotionStateCount) {
        return false;
    }
    return values_[requests_[static_cast<u32>(state)]] != 0.0F;
}

void LocomotionDriver::follow(const PoseProgram& program, const PoseInstance& instance) noexcept {
    write_clock(program, instance.state, instance.state_time);
    if (instance.transition != 0xFFFFU) {
        // The incoming state starts at its own first frame, not at the clock of the state being
        // left: a walk blending into a run does not begin the run a second and a half in.
        write_clock(program, instance.target, instance.blend_elapsed);
    }
}

void LocomotionDriver::write_clock(const PoseProgram& program, u16 state_index,
                                   f32 elapsed) noexcept {
    if (!bound_ || state_index >= program.states().size() || state_index >= kLocomotionStateCount) {
        return;
    }
    const PoseValue root = program.states()[state_index].root;
    if (root == kNoPoseValue || root >= program.code().size()) {
        return;
    }
    const PoseInstruction& instruction = program.code()[root];
    if (instruction.op != PoseOp::SampleClip || instruction.clip >= program.clips().size()) {
        return;
    }
    set_clock(static_cast<LocomotionState>(state_index),
              clip_time(program.clips()[instruction.clip], elapsed));
}

}  // namespace cy::graph::pose
