// SPDX-License-Identifier: MIT
// Gameplay graphs on scene entities. See cy/game_backend/graph_behaviours.h.

#include <cy/game_backend/graph_behaviours.h>

#include <cy/abi/cy_abi.h>
#include <cy/core/math/transform.h>
#include <cy/graph/text.h>
#include <cy/scene/node.h>
#include <cy/scene/tree.h>

#include <cmath>
#include <utility>

namespace cy::game_backend {
namespace {

namespace script = graph::script;
using graph::Capability;
using script::ExternalDecl;
using script::ExternalKind;

constexpr ExternalDecl kExternals[] = {
    {"unit.move_to", ExternalKind::Call, 2, false, Capability::WriteWorld,
     "Move the unit towards the ground point (arg0 = x, arg1 = z) at its speed."},
    {"unit.set_speed", ExternalKind::Call, 1, false, Capability::WriteWorld,
     "Set the speed, in metres per second, the unit's next steps move at."},
    {"unit.stop", ExternalKind::Call, 0, false, Capability::WriteWorld,
     "Stop the unit where it is."},
    {"event.x", ExternalKind::Query, 0, false, Capability::ReadWorld,
     "The first argument of the event that started this handler."},
    {"event.y", ExternalKind::Query, 0, false, Capability::ReadWorld,
     "The second argument of the event that started this handler."},
    {"event.z", ExternalKind::Query, 0, false, Capability::ReadWorld,
     "The third argument of the event that started this handler."},
    {"unit.x", ExternalKind::Query, 0, false, Capability::ReadWorld,
     "Where the unit is along x, in metres."},
    {"unit.z", ExternalKind::Query, 0, false, Capability::ReadWorld,
     "Where the unit is along z, in metres."},
    {"unit.arrived", ExternalKind::Wait, 0, false, Capability::ReadWorld,
     "Satisfied once the unit's current move has finished."},
    {"cue.", ExternalKind::Event, 0, true, Capability::Audio,
     "Play the project cue named after `cue.` at the unit's position."},
};

constexpr std::string_view kCuePrefix = "cue.";

/// The exact external names and the verb each binds to. `cue.` is the one family.
struct Named {
    std::string_view name;
    u8 verb;
};

[[nodiscard]] f32 argument(Span<const script::Value> arguments, usize index) noexcept {
    return index < arguments.size() ? arguments[index].x : 0.0F;
}

/// The node that names `name` as its `property`, for a diagnostic about a binding.
[[nodiscard]] graph::NodeKey naming_node(const graph::Graph& graph, Name name,
                                         std::string_view property) noexcept {
    for (const graph::GraphNode& node : graph.nodes()) {
        const graph::Literal* literal = graph.property(node.key, Name::intern(property));
        if (literal != nullptr && literal->text == name) {
            return node.key;
        }
    }
    return graph::kInvalidNodeKey;
}

void report(graph::DiagnosticSink& sink, const char* code, graph::NodeKey node,
            const char* message, Name detail) noexcept {
    graph::Diagnostic diagnostic;
    diagnostic.code = code;
    diagnostic.node = node;
    diagnostic.message = message;
    diagnostic.detail = detail;
    sink.report(diagnostic);
}

}  // namespace

Span<const ExternalDecl> gameplay_graph_externals() noexcept {
    return {kExternals, sizeof(kExternals) / sizeof(kExternals[0])};
}

Status register_gameplay_graph_nodes(graph::NodeRegistry& registry) noexcept {
    return script::register_script_nodes(registry);
}

bool step_towards(Vec3& position, f32 target_x, f32 target_z, f32 speed, f32 dt) noexcept {
    const f32 dx = target_x - position.x;
    const f32 dz = target_z - position.z;
    const f32 distance = std::sqrt((dx * dx) + (dz * dz));
    const f32 step = speed * dt;
    if (distance <= step) {
        position.x = target_x;
        position.z = target_z;
        return true;
    }
    position.x += dx / distance * step;
    position.z += dz / distance * step;
    return false;
}

const char* graph_instance_status_name(GraphInstanceStatus status) noexcept {
    switch (status) {
        case GraphInstanceStatus::Idle:
            return "idle";
        case GraphInstanceStatus::Waiting:
            return "waiting";
        case GraphInstanceStatus::Failed:
            return "failed";
    }
    return "?";
}

GraphBehaviours::LoadedGraph::LoadedGraph(Allocator& allocator, Name graph_name,
                                          script::EventProgram&& compiled) noexcept
    : name(graph_name),
      program(std::move(compiled)),
      native(allocator),
      externals(allocator),
      waits(allocator) {}

GraphBehaviours::Instance::Instance(Allocator& allocator, u32 graph_index, ecs::Entity bound,
                                    const script::ScriptProgram& program) noexcept
    : graph(graph_index), entity(bound), state(allocator, program) {}

GraphBehaviours::GraphBehaviours(Allocator& allocator) noexcept
    : allocator_(&allocator),
      registry_(allocator),
      graphs_(allocator),
      instances_(allocator),
      cues_(allocator) {}

GraphBehaviours::~GraphBehaviours() {
    stop();
}

void GraphBehaviours::start(scene::SceneTree& tree, abi::game::AudioBackend* audio) noexcept {
    stop();
    tree_ = &tree;
    audio_ = audio;
}

void GraphBehaviours::stop() noexcept {
    instances_.clear();
    graphs_.clear();
    cues_.clear();
    current_ = nullptr;
    tree_ = nullptr;
    audio_ = nullptr;
    tick_ = 0;
}

Expected<u32, Error> GraphBehaviours::load(Name name, std::string_view source,
                                           GraphBackend backend,
                                           graph::DiagnosticSink& sink) noexcept {
    if (!registered_) {
        if (Status registered = register_gameplay_graph_nodes(registry_); !registered) {
            return make_unexpected(registered.error());
        }
        registered_ = true;
    }
    Expected<graph::Graph, Error> parsed =
        graph::parse_graph(source, &registry_, *allocator_, sink);
    if (!parsed) {
        report(sink, "script.source.invalid", graph::kInvalidNodeKey, parsed.error().message,
               name);
        return make_unexpected(parsed.error());
    }
    Expected<script::EventProgram, Error> compiled = script::compile_event_graph(
        *parsed, registry_, gameplay_graph_externals(), sink);
    if (!compiled) {
        return make_unexpected(compiled.error());
    }
    Expected<UniquePtr<LoadedGraph>, Error> loaded =
        make_unique<LoadedGraph>(*allocator_, *allocator_, name, std::move(*compiled));
    if (!loaded) {
        return make_unexpected(loaded.error());
    }
    LoadedGraph& graph = **loaded;
    graph.backend = backend;
    if (backend == GraphBackend::Native) {
        Expected<script::NativeProgram, Error> native =
            script::compile_native(graph.program.program(), *allocator_);
        if (!native) {
            return make_unexpected(native.error());
        }
        graph.native = std::move(*native);
    }
    if (Status bound = bind(graph, *parsed, sink); !bound) {
        return make_unexpected(bound.error());
    }
    const auto index = static_cast<u32>(graphs_.size());
    if (Status pushed = graphs_.push_back(std::move(*loaded)); !pushed) {
        return make_unexpected(pushed.error());
    }
    return index;
}

Status GraphBehaviours::bind(LoadedGraph& loaded, const graph::Graph& source,
                             graph::DiagnosticSink& sink) noexcept {
    // EVERY NAME IS RESOLVED HERE, ONCE. At run time an external is an index into `externals` and
    // a cue is the adapter's handle: `visual-scripting` forbids a name lookup in execution.
    constexpr Named kNamed[] = {
        {"unit.move_to", static_cast<u8>(Verb::MoveTo)}, {"unit.set_speed", static_cast<u8>(Verb::SetSpeed)},
        {"unit.stop", static_cast<u8>(Verb::Stop)},      {"event.x", static_cast<u8>(Verb::EventX)},
        {"event.y", static_cast<u8>(Verb::EventY)},      {"event.z", static_cast<u8>(Verb::EventZ)},
        {"unit.x", static_cast<u8>(Verb::UnitX)},        {"unit.z", static_cast<u8>(Verb::UnitZ)},
    };
    bool refused = false;
    for (const script::ExternalRef& external : loaded.program.program().externals()) {
        Binding binding;
        const std::string_view text = external.name.text();
        bool named = false;
        for (const Named& entry : kNamed) {
            if (entry.name == text) {
                binding.verb = static_cast<Verb>(entry.verb);
                named = true;
            }
        }
        if (!named && text.starts_with(kCuePrefix)) {
            binding.verb = Verb::Cue;
            binding.cue_name = Name::intern(text.substr(kCuePrefix.size()));
            const graph::NodeKey node = naming_node(source, external.name, "event");
            if (audio_ == nullptr) {
                report(sink, "script.cue.no-audio", node,
                       "this graph plays a cue and this host has no audio", binding.cue_name);
                refused = true;
            } else if (audio_->find_cue(binding.cue_name.c_str(), binding.cue) != CY_RESULT_OK) {
                report(sink, "script.cue.unknown", node,
                       "this cue is not one of the project's cues", binding.cue_name);
                refused = true;
            }
        }
        if (Status pushed = loaded.externals.push_back(binding); !pushed) {
            return pushed;
        }
    }
    for (const script::SuspendPoint& point : loaded.program.program().suspends()) {
        // `compile_event_graph` admits only declared waits, and `unit.arrived` is the one there is.
        Binding binding;
        binding.verb = Verb::Arrived;
        (void)point;
        if (Status pushed = loaded.waits.push_back(binding); !pushed) {
            return pushed;
        }
    }
    if (refused) {
        return fail(ErrorCode::NotFound, "a graph names a cue this host cannot play");
    }
    return ok();
}

Status GraphBehaviours::attach(u32 graph, ecs::Entity entity) noexcept {
    if (graph >= graphs_.size()) {
        return fail(ErrorCode::InvalidArgument, "attach names a graph that was not loaded");
    }
    for (const Instance& existing : instances_) {
        if (existing.graph == graph && existing.entity == entity) {
            return fail(ErrorCode::AlreadyExists, "this entity already runs this graph");
        }
    }
    return instances_.push_back(
        Instance(*allocator_, graph, entity, graphs_[graph]->program.program()));
}

Expected<u32, Error> GraphBehaviours::raise(ecs::Entity entity, Name event,
                                            Span<const f32> arguments) noexcept {
    u32 started = 0;
    for (Instance& instance : instances_) {
        if (instance.entity != entity) {
            continue;
        }
        const script::EventHandler* handler = graphs_[instance.graph]->program.handler(event);
        if (handler == nullptr) {
            continue;
        }
        for (usize lane = 0; lane < 3; ++lane) {
            instance.arguments[lane] = lane < arguments.size() ? arguments[lane] : 0.0F;
        }
        instance.runs += 1;
        if (Status ran = run(instance, handler->block); !ran) {
            return make_unexpected(ran.error());
        }
        ++started;
    }
    return started;
}

Status GraphBehaviours::update(f32 dt) noexcept {
    ++tick_;
    for (Instance& instance : instances_) {
        if (!instance.moving) {
            continue;
        }
        Vec3 position = position_of(instance.entity);
        instance.moving =
            !step_towards(position, instance.target_x, instance.target_z, instance.speed, dt);
        if (Status placed = place(instance.entity, position); !placed) {
            return placed;
        }
    }
    // ONE PASS OVER THE INSTANCES, AFTER EVERY MOVE: the scheduler asks each wait's host, and only
    // a satisfied wait costs a resumed program.
    for (Instance& instance : instances_) {
        const LoadedGraph& loaded = *graphs_[instance.graph];
        const script::SuspendPoint* point = script::waiting_at(loaded.program.program(), instance.state);
        if (point == nullptr) {
            continue;
        }
        current_ = &instance;
        const bool ready = wait_satisfied(*point);
        current_ = nullptr;
        if (ready) {
            if (Status ran = run(instance, script::kNoBlock); !ran) {
                return ran;
            }
        }
    }
    return ok();
}

Status GraphBehaviours::run(Instance& instance, script::BlockId start) noexcept {
    const LoadedGraph& loaded = *graphs_[instance.graph];
    const bool native = loaded.backend == GraphBackend::Native;
    current_ = &instance;
    Expected<script::RunOutcome, Error> outcome = script::RunOutcome::Finished;
    if (start == script::kNoBlock) {
        outcome = native ? script::execute_native(loaded.native, instance.state, *this)
                         : script::execute(loaded.program.program(), instance.state, *this);
    } else {
        outcome = native
                      ? script::execute_native_from(loaded.native, instance.state, *this, start)
                      : script::execute_from(loaded.program.program(), instance.state, *this,
                                             start);
    }
    current_ = nullptr;
    // A HANDLER THAT FAILS FAILS ITSELF, NOT THE SESSION: the instance says why and answers the
    // next event, as a Swift behaviour that throws is reported and keeps its entity.
    instance.problem = "";
    if (!outcome) {
        instance.status = GraphInstanceStatus::Failed;
        instance.problem = outcome.error().message;
        return ok();
    }
    switch (*outcome) {
        case script::RunOutcome::Finished:
            instance.status = GraphInstanceStatus::Idle;
            break;
        case script::RunOutcome::Suspended:
            instance.status = instance.state.suspended() ? GraphInstanceStatus::Waiting
                                                         : GraphInstanceStatus::Idle;
            break;
        case script::RunOutcome::BudgetExhausted:
            instance.status = GraphInstanceStatus::Failed;
            instance.problem = "the handler ran past its instruction budget";
            break;
    }
    return ok();
}

Vec3 GraphBehaviours::position_of(ecs::Entity entity) const noexcept {
    if (tree_ == nullptr) {
        return {};
    }
    const scene::Node node = tree_->node(entity);
    return node.valid() ? node.local_transform().translation : Vec3{};
}

Status GraphBehaviours::place(ecs::Entity entity, Vec3 position) const noexcept {
    if (tree_ == nullptr) {
        return fail(ErrorCode::Unavailable, "graph behaviours were not started on a scene tree");
    }
    const scene::Node node = tree_->node(entity);
    if (!node.valid()) {
        return fail(ErrorCode::NotFound, "a graph's unit is not a scene node");
    }
    Transform transform = node.local_transform();
    transform.translation = position;
    return node.set_local_transform(transform);
}

GraphInstanceView GraphBehaviours::instance(u32 index) const noexcept {
    GraphInstanceView view;
    if (index >= instances_.size()) {
        return view;
    }
    const Instance& instance = instances_[index];
    const LoadedGraph& loaded = *graphs_[instance.graph];
    view.entity = instance.entity;
    view.graph = loaded.name;
    view.status = instance.status;
    if (const script::SuspendPoint* point =
            script::waiting_at(loaded.program.program(), instance.state);
        point != nullptr) {
        view.waiting = point->reason;
    }
    view.position = position_of(instance.entity);
    view.moving = instance.moving;
    view.runs = instance.runs;
    view.problem = instance.problem;
    return view;
}

const script::EventProgram* GraphBehaviours::program(u32 graph) const noexcept {
    return graph < graphs_.size() ? &graphs_[graph]->program : nullptr;
}

// --- The host calls a running handler makes -------------------------------------------------------

const GraphBehaviours::Binding& GraphBehaviours::binding_of(
    const script::ExternalRef& external) const noexcept {
    static const Binding kUnbound{};
    if (current_ == nullptr) {
        return kUnbound;
    }
    const LoadedGraph& loaded = *graphs_[current_->graph];
    const Span<const script::ExternalRef> table = loaded.program.program().externals();
    const auto index = static_cast<usize>(&external - table.data());
    return index < loaded.externals.size() ? loaded.externals[index] : kUnbound;
}

script::Value GraphBehaviours::call(const script::ExternalRef& callee,
                                    Span<const script::Value> arguments) noexcept {
    if (current_ == nullptr) {
        return {};
    }
    Instance& instance = *current_;
    switch (binding_of(callee).verb) {
        case Verb::MoveTo: {
            instance.target_x = argument(arguments, 0);
            instance.target_z = argument(arguments, 1);
            instance.moving = true;
            const Vec3 here = position_of(instance.entity);
            const f32 dx = instance.target_x - here.x;
            const f32 dz = instance.target_z - here.z;
            return script::Value::from_float(std::sqrt((dx * dx) + (dz * dz)));
        }
        case Verb::SetSpeed: {
            const f32 speed = argument(arguments, 0);
            instance.speed = speed > 0.0F ? speed : 0.0F;
            return script::Value::from_float(instance.speed);
        }
        case Verb::Stop:
            instance.moving = false;
            return {};
        default:
            return {};
    }
}

script::Value GraphBehaviours::query(const script::ExternalRef& query,
                                     Span<const script::Value> /*arguments*/) noexcept {
    if (current_ == nullptr) {
        return {};
    }
    const Instance& instance = *current_;
    switch (binding_of(query).verb) {
        case Verb::EventX:
            return script::Value::from_float(instance.arguments[0]);
        case Verb::EventY:
            return script::Value::from_float(instance.arguments[1]);
        case Verb::EventZ:
            return script::Value::from_float(instance.arguments[2]);
        case Verb::UnitX:
            return script::Value::from_float(position_of(instance.entity).x);
        case Verb::UnitZ:
            return script::Value::from_float(position_of(instance.entity).z);
        default:
            return {};
    }
}

void GraphBehaviours::emit_event(const script::ExternalRef& event,
                                 Span<const script::Value> /*arguments*/) noexcept {
    if (current_ == nullptr || audio_ == nullptr) {
        return;
    }
    const Binding& binding = binding_of(event);
    if (binding.verb != Verb::Cue) {
        return;
    }
    const Vec3 position = position_of(current_->entity);
    CyAudioPlay play{};
    play.struct_size = sizeof(CyAudioPlay);
    play.flags = CY_AUDIO_PLAY_SPATIAL;
    play.cue = binding.cue;
    play.position[0] = position.x;
    play.position[1] = position.y;
    play.position[2] = position.z;
    CyAudioVoice voice = 0;
    (void)audio_->play(play, voice);
    (void)cues_.push_back(GraphCuePlayed{current_->entity, binding.cue_name, tick_, position});
}

void GraphBehaviours::emit_command(const script::ExternalRef& /*command*/,
                                   Span<const script::Value> /*arguments*/) noexcept {
    // No command is declared, so `compile_event_graph` refuses every program that emits one.
}

script::Value GraphBehaviours::get_field(const script::ExternalRef& /*field*/,
                                         const script::Value& /*subject*/) noexcept {
    return {};  // No field is declared; see `emit_command`.
}

void GraphBehaviours::set_field(const script::ExternalRef& /*field*/,
                                const script::Value& /*subject*/,
                                const script::Value& /*value*/) noexcept {}

bool GraphBehaviours::wait_satisfied(const script::SuspendPoint& point) noexcept {
    if (current_ == nullptr) {
        return false;
    }
    const LoadedGraph& loaded = *graphs_[current_->graph];
    const Span<const script::SuspendPoint> table = loaded.program.program().suspends();
    const auto index = static_cast<usize>(&point - table.data());
    if (index >= loaded.waits.size()) {
        return false;
    }
    return loaded.waits[index].verb == Verb::Arrived && !current_->moving;
}

}  // namespace cy::game_backend
