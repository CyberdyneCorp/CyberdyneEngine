#pragma once
// The four-state locomotion animation graph, authored in code and compiled at cook time.
//
// ================================================================================================
// WHY A BUILDER EXISTS AT ALL, AND WHY IT LIVES HERE
// ================================================================================================
//
// `animation-and-skinning` is categorical about where a state machine may be interpreted: "the
// runtime SHALL contain no graph compiler". `compile_pose()` in lower_pose.h is therefore the only
// way a `PoseProgram` is ever populated — its one mutator, `PoseProgramAccess`, is private to
// lower_pose.cpp, so a program constructed anywhere else is permanently empty.
//
// That leaves a gap between "an artist authored a graph in the editor" and "a character animates",
// which is everything the engine wants to do BEFORE the editor can save an animation graph asset:
// an imported Mixamo rig with four clips and nothing to drive them. This header closes it. It
// writes the `pose.clip` / `pose.state` / `pose.transition` nodes that lower_pose.cpp understands
// into a `Graph`, and hands that graph to `compile_pose()`. Nothing here is a second compiler and
// nothing here runs in a frame: the output is the same `PoseProgram` the editor's own graph would
// produce, and the same `advance()` / `evaluate()` read it.
//
// It lives in src/graph/ rather than beside the importer because the node vocabulary it writes is
// lower_pose.cpp's, and a builder that had to be kept in step with a compiler it could not see
// would drift the first time a property was renamed.
//
// ================================================================================================
// THE STATE MACHINE THIS BUILDS, AND THE THREE DECISIONS IN IT
// ================================================================================================
//
//        request_walk           request_run
//   idle ------------> walk ---------------> run
//        <-----------       <---------------
//        request_idle           request_walk
//
//   idle, walk and run each ------ request_die -----> die  (terminal)
//
// 1. THERE IS NO run -> idle EDGE. A character decelerates through the walk, because the
// alternative
//    is a two-hundred-millisecond blend from a full sprint to a standing pose and it reads as the
//    run being deleted. A host that asks for idle from the run gets nothing until it has asked for
//    the walk first, and `LocomotionDriver` makes that a request rather than a silent failure.
//
// 2. DEATH OUTRANKS LOCOMOTION AND CANNOT BE OUTRANKED. `animation-and-skinning`: "WHEN a
//    higher-priority transition becomes valid mid-blend and interruption is allowed THEN it SHALL
//    take over". The three locomotion edges carry `kLocomotionPriority` and allow a higher-priority
//    interruption; the three death edges carry `kDeathPriority` and allow none. So dying interrupts
//    a walk that is still blending in, and nothing interrupts dying.
//
// 3. EVERY TRANSITION BLENDS. `build_locomotion_graph` REFUSES a blend duration of zero rather than
//    writing it, because lower_pose.cpp reads a zero duration as a cut — it moves the instance to
//    the target state in one `advance()` and never evaluates the two trees together. A cut between
//    two locomotion poses is a visible pop, and a builder that let one through by leaving a field
//    at its default would produce that pop silently.
//
// ================================================================================================
// WHAT "die DOES NOT LOOP" MEANS IN A COMPILED PROGRAM
// ================================================================================================
//
// Two independent things, and the builder is responsible for both:
//
//   * THE STATE IS TERMINAL. `die` is authored with no outgoing transition, so `advance()` has
//     nothing to consider once the instance is in it, whatever the host goes on to request.
//   * THE CLIP DOES NOT WRAP. `ClipRef::looping` is false, which `clip_time()` below reads to clamp
//     the clock at the last frame instead of wrapping it to zero. Without this the character would
//     die, and die again, and again, for as long as the entity existed.
//
// The second one is why `pose.clip` now carries a `loop` property: see lower_pose.cpp.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/graph/cybergraph.h>
#include <cy/graph/lower_pose.h>

namespace cy::graph::pose {

/// The four states, in the order the compiled program numbers them.
///
/// `compile_pose()` numbers states by ascending node key and makes state 0 the entry, so this
/// enumeration's values ARE the compiled state indices — `build_locomotion_graph` assigns the keys
/// to make that true, and `idle` is the entry state because it has the lowest one. A test asserts
/// it rather than trusting it, because the relationship is an agreement between two files.
enum class LocomotionState : u16 {
    Idle = 0,
    Walk,
    Run,
    Die,
    Count,
};

inline constexpr u32 kLocomotionStateCount = static_cast<u32>(LocomotionState::Count);

/// `idle`, `walk`, `run`, `die`. Never null; the empty string for a value outside the enumeration.
[[nodiscard]] const char* locomotion_state_name(LocomotionState state) noexcept;

/// What the three locomotion transitions carry, and what the three death transitions carry.
/// `advance()` compares these directly: a candidate may interrupt a running transition only when it
/// outranks it, so the gap between them is the whole of decision 2 above.
inline constexpr u16 kLocomotionPriority = 1;
inline constexpr u16 kDeathPriority = 9;

/// One clip as the program will reference it.
struct LocomotionClip {
    /// The cooked clip's name. `AnimationRig::bind()` matches its clip table against this, so it is
    /// the importer's sub-asset name and not a file path.
    Name clip;
    /// Seconds. The host needs it to wrap or clamp the clock; the evaluator never reads it.
    f32 duration = 1.0F;
    /// Whether the clock wraps at `duration`. False on `die`, and `build_locomotion_graph` refuses
    /// a spec that says otherwise.
    bool looping = true;
};

/// Everything the four-state graph needs that is not structural.
///
/// The blend durations are named for the edge they belong to rather than collected in an array,
/// because a locomotion machine's feel lives in exactly these five numbers and a reviewer should be
/// able to read them without counting indices. They are seconds.
struct LocomotionSpec {
    /// The graph's name, which becomes the program's. Empty means `locomotion`.
    Name name;

    LocomotionClip idle;
    LocomotionClip walk;
    LocomotionClip run;
    /// `looping` must be false: a death that wraps is decision 3 of the header comment.
    LocomotionClip die;

    f32 idle_to_walk = 0.20F;
    f32 walk_to_run = 0.15F;
    /// Longer than the acceleration it undoes. Settling out of a run is where a short blend shows.
    f32 run_to_walk = 0.25F;
    f32 walk_to_idle = 0.25F;
    /// One blend length for all three death edges: the character is hit the same way whatever it
    /// was doing.
    f32 to_die = 0.30F;
};

/// The parameter a host raises to REQUEST `state`. Non-zero opens the transitions that lead there.
///
/// A request is not a command: the machine honours it only from a state that has an edge for it,
/// which is what makes the missing run -> idle edge a rule rather than a bug to work around.
[[nodiscard]] Name locomotion_request(LocomotionState state) noexcept;

/// The parameter carrying `state`'s clip clock, in seconds. `PoseOp::SampleClip` reads it and hands
/// it to the sampler; nothing in the program advances it, because a clock the program owned would
/// be a clock every character shared.
[[nodiscard]] Name locomotion_clock(LocomotionState state) noexcept;

/// Write the four-state machine into `graph`.
///
/// The graph must not already carry this builder's nodes; `Graph::add_node` refuses a key it
/// already holds, so a second call on one graph reports `AlreadyExists` rather than duplicating a
/// state.
///
/// Refuses, by name: a clip with no name, a blend duration that is not positive (decision 3), and a
/// looping death clip.
[[nodiscard]] Status build_locomotion_graph(Graph& graph, const LocomotionSpec& spec) noexcept;

/// Build the graph and compile it, registering the pose node types into a registry of its own.
///
/// The one call a cook makes. `joint_count` is the skeleton's, and is clamped to `kMaxJoints` by
/// the compiler; `sink` receives the compiler's node-precise diagnostics.
[[nodiscard]] Expected<PoseProgram, Error> compile_locomotion(Allocator& allocator,
                                                              const LocomotionSpec& spec,
                                                              u32 joint_count,
                                                              DiagnosticSink& sink) noexcept;

/// The time to sample `clip` at after `elapsed` seconds in its state.
///
/// A looping clip wraps; one that does not loop CLAMPS at its duration and stays there. This is the
/// only place the compiled `ClipRef::looping` is acted on, so a host that drives clocks itself
/// should call it rather than deciding separately.
[[nodiscard]] f32 clip_time(const ClipRef& clip, f32 elapsed) noexcept;

/// The parameter block a host drives, addressed by state rather than by index.
///
/// `PoseProgram::parameters()` is a flat array interned in compilation order, so every host would
/// otherwise resolve eight names by hand and get the answer wrong the first time an edge was added.
/// Bound once, then written per frame.
class LocomotionDriver {
public:
    explicit LocomotionDriver(Allocator& allocator) noexcept;

    /// Resolve every request and clock parameter against `program`. Reports `NotFound` when the
    /// program does not carry all eight, which is what a program built somewhere else looks like
    /// from here — and leaves the driver unbound, so a missed check writes nothing rather than
    /// writing parameter zero.
    [[nodiscard]] Status bind(const PoseProgram& program) noexcept;

    /// Raise `state`'s request and clear the other three.
    ///
    /// EXACTLY ONE, deliberately. Two requests raised at once are resolved by transition priority,
    /// and a host that leaned on that would be depending on a tie-break rather than on a decision.
    void request(LocomotionState state) noexcept;
    void clear_requests() noexcept;

    /// Drive the active state's clock — and, during a blend, the incoming state's — from the
    /// instance, wrapping or clamping each by its own clip's loop mode.
    void follow(const PoseProgram& program, const PoseInstance& instance) noexcept;

    void set_clock(LocomotionState state, f32 seconds) noexcept;
    [[nodiscard]] f32 clock(LocomotionState state) const noexcept;
    [[nodiscard]] bool requesting(LocomotionState state) const noexcept;

    [[nodiscard]] Span<const f32> parameters() const noexcept { return values_.span(); }

private:
    void write_clock(const PoseProgram& program, u16 state_index, f32 elapsed) noexcept;

    Array<f32> values_;
    u16 requests_[kLocomotionStateCount] = {};
    u16 clocks_[kLocomotionStateCount] = {};
    bool bound_ = false;
};

}  // namespace cy::graph::pose
