// SPDX-License-Identifier: MIT
#pragma once
// cy/game_backend/graph_behaviours.h — gameplay graphs, run on scene entities through the same
// engine services a Swift game calls. Issue #29, visual scripting.
//
// WHAT IT IS. The host side of an event graph (`cy/graph/event_script.h`): it compiles a `.cyscript`
// source once per graph, binds every name the program uses ONCE, and then runs one shared program
// over a dense array of instances — one `ScriptState` per entity, never a machine per entity.
// `update()` is the one system: it moves every unit that has an order, then resumes every instance
// whose wait its host has satisfied.
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

#include <cy/abi/game/audio.h>
#include <cy/core/base/expected.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/ownership.h>
#include <cy/core/values/name.h>
#include <cy/ecs/entity.h>
#include <cy/graph/event_script.h>

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

/// One cue a graph played: which entity, which cue, on which tick and where.
struct GraphCuePlayed {
    ecs::Entity entity;
    Name cue;
    u64 tick = 0;
    Vec3 position;
};

/// Compiled gameplay graphs and their instances. See the header comment.
class GraphBehaviours final : private graph::script::ScriptHost {
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

    /// One fixed step: move every unit with an order, then resume every satisfied wait.
    [[nodiscard]] Status update(f32 dt) noexcept;

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

    /// One compiled graph. Heap-held, because its native program points at its script program.
    struct LoadedGraph {
        LoadedGraph(Allocator& allocator, Name graph_name,
                    graph::script::EventProgram&& compiled) noexcept;

        Name name;
        graph::script::EventProgram program;
        graph::script::NativeProgram native;
        GraphBackend backend = GraphBackend::Bytecode;
        /// By external index, then by suspend-point index: resolved once, at load.
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

    [[nodiscard]] Status bind(LoadedGraph& loaded, const graph::Graph& source,
                              graph::DiagnosticSink& sink) noexcept;
    [[nodiscard]] const Binding& binding_of(const graph::script::ExternalRef& external) const noexcept;

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
};

}  // namespace cy::game_backend
