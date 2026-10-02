// SPDX-License-Identifier: MIT
#pragma once
// cy/game_backend/graph_behaviours.h — gameplay graphs, run on scene entities through the same
// engine services a Swift game calls. Issue #29, visual scripting.
//
// WHAT IT IS. The host side of an event graph (`cy/graph/event_script.h`): it compiles a
// `.cyscript` source once per graph, binds every name the program uses ONCE, and then runs one
// shared program over a dense array of instances — one `ScriptState` per entity, never a machine
// per entity. `update()` is the one system: it moves every unit that has an order, then resumes
// every instance whose wait its host has satisfied.
//
// WHAT A GRAPH CAN NAME. `gameplay_graph_externals()` is the whole vocabulary, with its metadata,
// and the compiler refuses anything else on the node that names it:
//
//   unit.move_to(x, z)   call   start moving towards a ground point at the unit's speed
//   unit.set_speed(m/s)  call   the speed the next steps move at (default 3 m/s)
//   unit.stop()          call   stop where it is
//   event.x/.y/.z        query  the arguments of the event that started this handler
//   unit.x/.z            query  where the unit is
//   unit.arrived         wait   satisfied once the unit's move has finished
//   cue.<name>           event  play the project cue `<name>` at the unit's position
//
// THE SAME SERVICES AS SWIFT. A `cue.` event plays through `abi::game::AudioBackend::play`, the
// adapter a Swift behaviour's `Audio.play(_:at:)` reaches, so a graph-authored unit and a
// Swift-authored one are heard through one mixer and can be compared call for call. Movement writes
// the entity's `LocalTransform`, as a Swift behaviour writing `world.setFloat` does; the arithmetic
// is stated once, in `step_towards`, and a Swift port must use the same expression to agree.
//
// BYTECODE OR NATIVE, PER GRAPH. `GraphBackend` picks the back end when a graph is loaded. Both
// read the same `ScriptState`, and `integration.game_backend_graph` runs one graph on each and
// requires the same moves, the same cues, at the same ticks.
//
// THE PLAY DEBUGGER (#84 stage 3, #29). `set_debugging(true)` swaps every graph's program for its
// debug-instrumented copy (`cy/graph/script_debug.h`): the same code with a probe at each node
// boundary, so nothing is interpreted and nothing else changes. Breakpoints name a graph, a node
// and optionally one entity; `debug_pause` breaks at the next node any instance runs; a step runs
// the paused instance to its next node (`Into`) or its next node on an execution chain (`Over`).
// Every node an instance runs is appended to a bounded trace, the panel's execution highlighting.
//
// WHAT A BREAK PAUSES IS THE WHOLE SIMULATION TICK, as a Blueprint breakpoint stops the game
// thread. The instance stops before its node with its registers live; the rest of the tick's graph
// work — later instances' resumes, later handlers of the same raise — is held, in order, and
// `debug_continue` / `debug_step` run it on from exactly there. While `paused()`, `update` and
// `raise` are refused, so nothing else advances: a host pauses its physics and its other systems
// with it (the hosted runtime pauses its `PlaySession`). A run that breaks and continues therefore
// makes exactly the moves, cues and state a run without the debugger makes, tick for tick —
// `integration.game_backend_graph` holds that.
//
// HOT RELOAD (#84 stage 2). `reload` compiles a graph's new source, binds it and checks that every
// instance can move to it (`cy/graph/script_reload.h`: variables by declaring node, added at their
// defaults, removed dropped, a changed type refused on its node). A refused reload changes
// nothing; an accepted one is applied at the next tick boundary — the start of the next `update` —
// to every instance of that graph at once, and the graph's `generation` moves on.

#include <cy/abi/game/audio.h>
#include <cy/core/base/expected.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/ownership.h>
#include <cy/core/values/name.h>
#include <cy/ecs/entity.h>
#include <cy/graph/event_script.h>
#include <cy/graph/script_debug.h>

#include <string_view>

namespace cy::scene {
class SceneTree;
}  // namespace cy::scene

namespace cy::game_backend {

/// Everything a gameplay graph may call, read, emit or wait for, with its metadata.
[[nodiscard]] Span<const graph::script::ExternalDecl> gameplay_graph_externals() noexcept;

/// The node types a gameplay graph compiles from: `cy::graph`'s script vocabulary.
[[nodiscard]] Status register_gameplay_graph_nodes(graph::NodeRegistry& registry) noexcept;

/// What a gameplay graph is granted when the editor creates one.
inline constexpr graph::Capability kGameplayGraphCapabilities =
    graph::Capability::ReadWorld | graph::Capability::WriteWorld | graph::Capability::Audio;

/// The speed a unit moves at until a graph sets another, in metres per second.
inline constexpr f32 kDefaultUnitSpeed = 3.0F;

/// One step of a unit's move: towards `target` in the ground plane, `speed * dt` metres, arriving
/// exactly on it when the step would reach it. The one statement of the arithmetic; answers whether
/// the unit arrived. `y` is untouched.
[[nodiscard]] bool step_towards(Vec3& position, f32 target_x, f32 target_z, f32 speed,
                                f32 dt) noexcept;

/// Which back end runs a graph's program.
enum class GraphBackend : u8 { Bytecode = 0, Native };

/// Where an instance is.
enum class GraphInstanceStatus : u8 {
    /// No handler is running or waiting.
    Idle = 0,
    /// A handler is suspended on a wait its host has not satisfied yet.
    Waiting,
    /// A handler failed; `problem` says how. The instance answers the next event again.
    Failed,
};

[[nodiscard]] const char* graph_instance_status_name(GraphInstanceStatus status) noexcept;

/// One instance, as a tool reads it.
struct GraphInstanceView {
    ecs::Entity entity;
    Name graph;
    GraphInstanceStatus status = GraphInstanceStatus::Idle;
    /// The wait's reason while `Waiting`.
    Name waiting;
    Vec3 position;
    bool moving = false;
    /// Handlers started.
    u32 runs = 0;
    /// Why the last handler failed; empty otherwise.
    const char* problem = "";
};

/// How a paused instance continues.
enum class GraphStep : u8 {
    /// To the very next node it runs, a data node included.
    Into = 0,
    /// To the next node on an execution chain: the data nodes feeding it run without stopping.
    Over,
};

/// A breakpoint: a node of a graph, for every instance or for one entity's.
struct GraphBreakpoint {
    Name graph;
    graph::NodeKey node = graph::kInvalidNodeKey;
    /// The one entity it stops for; a null entity stops every instance of the graph.
    ecs::Entity entity;
};

/// Why the simulation is paused.
enum class GraphPauseReason : u8 { Breakpoint = 0, Step, Pause };

[[nodiscard]] const char* graph_pause_reason_name(GraphPauseReason reason) noexcept;

/// Where the debugger stopped: one instance, before one node.
struct GraphPauseView {
    bool paused = false;
    GraphPauseReason reason = GraphPauseReason::Breakpoint;
    u32 instance = 0;
    ecs::Entity entity;
    Name graph;
    graph::NodeKey node = graph::kInvalidNodeKey;
    /// The simulation tick the break happened in.
    u64 tick = 0;
};

/// One node an instance ran, in execution order.
struct GraphTraceEntry {
    /// Monotonic across the session; a reader keeps the last it saw.
    u64 sequence = 0;
    u64 tick = 0;
    u32 instance = 0;
    ecs::Entity entity;
    Name graph;
    graph::NodeKey node = graph::kInvalidNodeKey;
};

/// How many trace entries are kept: the oldest is dropped beyond it.
inline constexpr u32 kGraphTraceCapacity = 64;

/// One graph variable's value in one instance.
struct GraphVariableView {
    graph::NodeKey id = graph::kInvalidNodeKey;
    Name name;
    graph::script::ValueKind kind = graph::script::ValueKind::Float;
    graph::script::Value value;
};

/// What the last applied reload did.
struct GraphReloadReport {
    Name graph;
    /// The graph's generation after the reload: 1 for the program Play started with.
    u32 generation = 1;
    u32 instances = 0;
    u32 kept = 0;
    u32 added = 0;
    u32 dropped = 0;
    u32 waits_kept = 0;
    u32 waits_dropped = 0;
};

/// One cue a graph played: which entity, which cue, on which tick and where.
struct GraphCuePlayed {
    ecs::Entity entity;
    Name cue;
    u64 tick = 0;
    Vec3 position;
};

/// Compiled gameplay graphs and their instances. See the header comment.
class GraphBehaviours final : private graph::script::ScriptHost,
                              private graph::script::ScriptDebugHook {
public:
    explicit GraphBehaviours(Allocator& allocator) noexcept;
    ~GraphBehaviours() override;

    GraphBehaviours(const GraphBehaviours&) = delete;
    GraphBehaviours& operator=(const GraphBehaviours&) = delete;
    GraphBehaviours(GraphBehaviours&&) = delete;
    GraphBehaviours& operator=(GraphBehaviours&&) = delete;

    /// Bind to a play session's scene tree and, optionally, the audio adapter cues play through.
    /// Clears every graph and instance from a previous session.
    void start(scene::SceneTree& tree, abi::game::AudioBackend* audio) noexcept;
    /// Forget every graph and instance.
    void stop() noexcept;

    /// Parse and compile one `.cyscript` source and bind its names. Compile problems are reported
    /// through `sink`, node by node; a cue the audio adapter does not know is reported on the node
    /// that plays it. Answers the graph's index.
    [[nodiscard]] Expected<u32, Error> load(Name name, std::string_view source,
                                            GraphBackend backend,
                                            graph::DiagnosticSink& sink) noexcept;

    /// Run graph `graph` on `entity`. One instance per entity and graph.
    [[nodiscard]] Status attach(u32 graph, ecs::Entity entity) noexcept;

    /// Start `event`'s handler on every instance attached to `entity`, with up to three arguments.
    /// A waiting handler is replaced. Answers how many handlers started; zero when no graph on the
    /// entity answers the event.
    [[nodiscard]] Expected<u32, Error> raise(ecs::Entity entity, Name event,
                                             Span<const f32> arguments) noexcept;

    /// One fixed step: apply a staged reload, move every unit with an order, then resume every
    /// satisfied wait. Refused while the debugger holds the simulation paused.
    [[nodiscard]] Status update(f32 dt) noexcept;

    // --- Hot reload ------------------------------------------------------------------------------

    /// Compile `source` as the new program of the loaded graph `name` and check that every running
    /// instance can move to it. Problems are reported through `sink`, on their nodes, and refuse
    /// the reload with the old program left running. An accepted reload is staged and applied at
    /// the next tick boundary; staging another before then replaces it. Answers the generation the
    /// graph will have.
    [[nodiscard]] Expected<u32, Error> reload(Name name, std::string_view source,
                                              graph::DiagnosticSink& sink) noexcept;
    /// Whether a reload is staged and not yet applied.
    [[nodiscard]] bool reload_pending() const noexcept;
    /// The last reload that was applied.
    [[nodiscard]] const GraphReloadReport& last_reload() const noexcept { return last_reload_; }
    /// A loaded graph's program generation: 1 until it is first reloaded.
    [[nodiscard]] u32 generation(u32 graph) const noexcept;
    /// The index of the loaded graph `name`, or `graph_count()`.
    [[nodiscard]] u32 find_graph(Name name) const noexcept;

    // --- The Play debugger -----------------------------------------------------------------------

    /// Run every graph's debug-instrumented program (true) or its plain one (false), from the
    /// next handler or resume on. Refused (`Unsupported`) where the debugger is compiled out, and
    /// refused while paused.
    [[nodiscard]] Status set_debugging(bool enabled) noexcept;
    [[nodiscard]] bool debugging() const noexcept { return debugging_; }

    /// Add or remove a breakpoint. `entity` null stops every instance of `graph`.
    [[nodiscard]] Status set_breakpoint(Name graph, graph::NodeKey node, ecs::Entity entity,
                                        bool enabled) noexcept;
    void clear_breakpoints() noexcept { breakpoints_.clear(); }
    [[nodiscard]] Span<const GraphBreakpoint> breakpoints() const noexcept {
        return breakpoints_.span();
    }

    /// Break at the next node any instance runs.
    [[nodiscard]] Status debug_pause() noexcept;
    /// Run the held tick on from the paused node until the next breakpoint, or to its end.
    [[nodiscard]] Status debug_continue() noexcept;
    /// Run the paused instance to its next node (`Into`) or next execution node (`Over`). The step
    /// stays armed for that instance until it gets there, a later tick if need be; a breakpoint
    /// another instance reaches first still stops there.
    [[nodiscard]] Status debug_step(GraphStep step) noexcept;

    /// Whether the debugger is holding the simulation in the middle of a tick.
    [[nodiscard]] bool paused() const noexcept { return pause_.paused; }
    [[nodiscard]] const GraphPauseView& pause_view() const noexcept { return pause_; }

    /// The trace, oldest first: at most `kGraphTraceCapacity` entries.
    [[nodiscard]] u32 trace_count() const noexcept;
    [[nodiscard]] GraphTraceEntry trace_entry(u32 index) const noexcept;

    /// A pin's value in one instance, through the running program's debug map (see
    /// `graph::script::read_pin`). A paused instance is read in the block it is paused in.
    [[nodiscard]] graph::script::PinReading watch_pin(u32 instance, graph::NodeKey node,
                                                      Name pin) const noexcept;
    /// One instance's graph variables, in declaration order.
    [[nodiscard]] u32 variable_count(u32 instance) const noexcept;
    [[nodiscard]] GraphVariableView variable(u32 instance, u32 index) const noexcept;

    [[nodiscard]] u64 tick() const noexcept { return tick_; }
    [[nodiscard]] u32 graph_count() const noexcept { return static_cast<u32>(graphs_.size()); }
    [[nodiscard]] u32 instance_count() const noexcept {
        return static_cast<u32>(instances_.size());
    }
    [[nodiscard]] GraphInstanceView instance(u32 index) const noexcept;
    /// The compiled program a loaded graph runs, for a tool and a test.
    [[nodiscard]] const graph::script::EventProgram* program(u32 graph) const noexcept;
    /// Every cue played since `start`, in order.
    [[nodiscard]] Span<const GraphCuePlayed> cues() const noexcept { return cues_.span(); }

private:
    /// What a program's external or wait was bound to when the graph was loaded.
    enum class Verb : u8 {
        MoveTo = 0,
        SetSpeed,
        Stop,
        EventX,
        EventY,
        EventZ,
        UnitX,
        UnitZ,
        Arrived,
        Cue,
    };

    struct Binding {
        Verb verb = Verb::Stop;
        CyAudioCue cue = 0;
        Name cue_name;
    };

    /// One compiled graph. Heap-held, because its native programs point at its script programs.
    struct LoadedGraph {
        LoadedGraph(Allocator& allocator, Name graph_name,
                    graph::script::EventProgram&& compiled) noexcept;

        Name name;
        graph::script::EventProgram program;
        graph::script::NativeProgram native;
        /// The debug-instrumented copy and its native compile, while the debugger is on.
        UniquePtr<graph::script::EventProgram> debug;
        graph::script::NativeProgram debug_native;
        GraphBackend backend = GraphBackend::Bytecode;
        u32 generation = 1;
        /// By external index, then by suspend-point index: resolved once, at load. The debug copy
        /// has the same tables in the same order, so one binding serves both.
        Array<Binding> externals;
        Array<Binding> waits;
    };

    /// One entity running one graph: its registers, its order, and the event that started it.
    struct Instance {
        Instance(Allocator& allocator, u32 graph_index, ecs::Entity bound,
                 const graph::script::ScriptProgram& program) noexcept;

        u32 graph = 0;
        ecs::Entity entity;
        graph::script::ScriptState state;
        GraphInstanceStatus status = GraphInstanceStatus::Idle;
        f32 arguments[3] = {};
        f32 target_x = 0.0F;
        f32 target_z = 0.0F;
        f32 speed = kDefaultUnitSpeed;
        bool moving = false;
        u32 runs = 0;
        const char* problem = "";
    };

    /// Where a tick's graph work stopped for the debugger, so it runs on from exactly there.
    enum class Phase : u8 { None = 0, Raise, Resume };
    struct Interrupted {
        Phase phase = Phase::None;
        u32 instance = 0;
        ecs::Entity entity;
        Name event;
        f32 arguments[3] = {};
    };

    [[nodiscard]] Expected<UniquePtr<LoadedGraph>, Error> compile(
        Name name, std::string_view source, GraphBackend backend,
        graph::DiagnosticSink& sink) noexcept;
    [[nodiscard]] Status instrument(LoadedGraph& loaded) noexcept;
    [[nodiscard]] Status bind(LoadedGraph& loaded, const graph::Graph& source,
                              graph::DiagnosticSink& sink) noexcept;
    [[nodiscard]] Status apply_reloads() noexcept;
    /// The program an instance of `loaded` runs right now: the debug copy while debugging.
    [[nodiscard]] const graph::script::ScriptProgram& active(
        const LoadedGraph& loaded) const noexcept;
    /// Start handlers on `entity`'s instances from `first` on; answers how many started.
    [[nodiscard]] Expected<u32, Error> raise_from(u32 first, ecs::Entity entity, Name event,
                                                  Span<const f32> arguments) noexcept;
    /// Resume every satisfied wait from instance `first` on.
    [[nodiscard]] Status resume_from(u32 first) noexcept;
    /// Run the held work on after the paused instance finishes its handler.
    [[nodiscard]] Status run_on() noexcept;
    [[nodiscard]] u32 index_of(const Instance& instance) const noexcept;
    [[nodiscard]] const Binding& binding_of(
        const graph::script::ExternalRef& external) const noexcept;

    [[nodiscard]] Status run(Instance& instance, graph::script::BlockId start) noexcept;
    [[nodiscard]] Vec3 position_of(ecs::Entity entity) const noexcept;
    [[nodiscard]] Status place(ecs::Entity entity, Vec3 position) const noexcept;

    graph::script::Value call(const graph::script::ExternalRef& callee,
                              Span<const graph::script::Value> arguments) noexcept override;
    graph::script::Value query(const graph::script::ExternalRef& query,
                               Span<const graph::script::Value> arguments) noexcept override;
    void emit_event(const graph::script::ExternalRef& event,
                    Span<const graph::script::Value> arguments) noexcept override;
    void emit_command(const graph::script::ExternalRef& command,
                      Span<const graph::script::Value> arguments) noexcept override;
    graph::script::Value get_field(const graph::script::ExternalRef& field,
                                   const graph::script::Value& subject) noexcept override;
    void set_field(const graph::script::ExternalRef& field, const graph::script::Value& subject,
                   const graph::script::Value& value) noexcept override;
    [[nodiscard]] bool wait_satisfied(const graph::script::SuspendPoint& point) noexcept override;
    [[nodiscard]] graph::script::DebugVerdict on_probe(
        const graph::script::ProbeSite& site,
        const graph::script::ScriptState& state) noexcept override;

    Allocator* allocator_;
    graph::NodeRegistry registry_;
    bool registered_ = false;
    scene::SceneTree* tree_ = nullptr;
    abi::game::AudioBackend* audio_ = nullptr;
    Array<UniquePtr<LoadedGraph>> graphs_;
    Array<Instance> instances_;
    Array<GraphCuePlayed> cues_;
    /// The instance whose handler is running, for the host calls it makes.
    Instance* current_ = nullptr;
    u64 tick_ = 0;

    /// Reloads accepted and not yet applied, by graph index; null where none is staged.
    Array<UniquePtr<LoadedGraph>> staged_;
    GraphReloadReport last_reload_;

    bool debugging_ = false;
    Array<GraphBreakpoint> breakpoints_;
    bool pause_requested_ = false;
    /// An armed step: which instance, and how it continues.
    bool stepping_ = false;
    GraphStep step_ = GraphStep::Into;
    u32 step_instance_ = 0;
    GraphPauseView pause_;
    Interrupted interrupted_;
    /// A ring of the last `kGraphTraceCapacity` nodes run.
    Array<GraphTraceEntry> trace_;
    u32 trace_head_ = 0;
    u64 trace_sequence_ = 0;
};

}  // namespace cy::game_backend
