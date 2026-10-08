// SPDX-License-Identifier: MIT
// The `animation.*` operations. See cy/editor/animation_service.h.

#include <cy/editor/animation_service.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/graph/text.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "graph_wire.h"
#include "service_wire.h"

namespace cy::editor {
namespace {

namespace pose = graph::pose;
using Out = wire::Writer;
using graph::NodeKey;

/// The catalogue's property control kinds, numbered as the material catalogue numbers them.
enum class ControlKind : u8 { Text = 0, Bool, Scalar, Vector, Enumeration, Asset };

/// Where a property's choices come from.
enum class Choices : u8 { None, Clips, Interruption };

struct PropertySpec {
    std::string_view node;
    std::string_view name;
    ControlKind kind;
    std::string_view fallback;
    /// The literal type the value is written at in the graph's text.
    std::string_view literal;
    Choices choices;
    std::string_view tooltip;
};

constexpr PropertySpec kProperties[] = {
    {"pose.clip", "clip", ControlKind::Enumeration, "", "name", Choices::Clips,
     "The clip this node samples, by the name the character's clip table gives it."},
    {"pose.clip", "duration", ControlKind::Scalar, "1", "float", Choices::None,
     "The clip's length in seconds, as the program records it. The preview plays the clip's own "
     "length."},
    {"pose.clip", "loop", ControlKind::Bool, "true", "bool", Choices::None,
     "Whether the clip's clock wraps. A clip that does not loop holds its last frame."},
    {"pose.clip", "time_parameter", ControlKind::Text, "", "name", Choices::None,
     "The parameter that carries this clip's clock. Empty: a clock of its own."},
    {"pose.clip", "events", ControlKind::Text, "", "name", Choices::None,
     "The clip's events, `name@seconds` separated by `;`. Placed on the timeline."},
    {"pose.state", "name", ControlKind::Text, "state", "name", Choices::None,
     "The state's name, which a game asks Animator.play for."},
    {"pose.transition", "condition", ControlKind::Text, "", "name", Choices::None,
     "The parameter that opens the transition while it is not zero."},
    {"pose.transition", "duration", ControlKind::Scalar, "0.25", "float", Choices::None,
     "The blend's length in seconds. An authored transition blends: zero is refused."},
    {"pose.transition", "priority", ControlKind::Scalar, "0", "int", Choices::None,
     "Which of two open transitions is taken, and what may interrupt a blend."},
    {"pose.transition", "interruption", ControlKind::Enumeration, "none", "name",
     Choices::Interruption, "What may take over while this blend runs."},
    {"pose.blend", "weight_parameter", ControlKind::Text, "", "name", Choices::None,
     "The parameter that weights b against a, from 0 to 1."},
    {"pose.blend_mask", "weight_parameter", ControlKind::Text, "", "name", Choices::None,
     "The parameter that weights b against a, from 0 to 1."},
    {"pose.blend_mask", "mask_first", ControlKind::Scalar, "0", "int", Choices::None,
     "The first joint b contributes."},
    {"pose.blend_mask", "mask_count", ControlKind::Scalar, "0", "int", Choices::None,
     "How many joints b contributes; zero for every joint."},
    {"pose.additive", "weight_parameter", ControlKind::Text, "", "name", Choices::None,
     "The parameter that weights the additive pose."},
    {"pose.layer", "weight_parameter", ControlKind::Text, "", "name", Choices::None,
     "The parameter that weights the layer."},
    {"pose.layer", "mask_first", ControlKind::Scalar, "0", "int", Choices::None,
     "The first joint the layer covers."},
    {"pose.layer", "mask_count", ControlKind::Scalar, "0", "int", Choices::None,
     "How many joints the layer covers; zero for every joint."},
    {"pose.ik", "chain", ControlKind::Scalar, "0", "int", Choices::None,
     "The two-bone chain the character's rig solves."},
};

constexpr std::string_view kInterruptions[] = {"none", "higher_priority", "any"};

constexpr u32 kCatalogueSchema = 3;
constexpr u32 kCatalogueVersion = 1;

[[nodiscard]] Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Editor);
}

[[nodiscard]] Span<const AnimationClipInfo> clips_of(
    const AnimationPreviewRuntime* preview) noexcept {
    return preview != nullptr ? preview->clips() : Span<const AnimationClipInfo>{};
}

[[nodiscard]] const AnimationClipInfo* find_clip(const AnimationPreviewRuntime* preview,
                                                 Name name) noexcept {
    for (const AnimationClipInfo& clip : clips_of(preview)) {
        if (clip.name == name) {
            return &clip;
        }
    }
    return nullptr;
}

void encode_choices(Out& out, const PropertySpec& property,
                    const AnimationPreviewRuntime* preview) noexcept {
    if (property.choices == Choices::Interruption) {
        out.u32v(static_cast<u32>(std::size(kInterruptions)));
        for (const std::string_view rule : kInterruptions) {
            out.text(rule);
        }
        return;
    }
    if (property.choices == Choices::Clips) {
        out.u32v(static_cast<u32>(clips_of(preview).size()));
        for (const AnimationClipInfo& clip : clips_of(preview)) {
            out.text(clip.name.text());
        }
        return;
    }
    out.u32v(0);
}

void encode_property(Out& out, const PropertySpec& property, u32 identity,
                     const AnimationPreviewRuntime* preview) noexcept {
    char semantic[32] = {};
    (void)std::snprintf(semantic, sizeof(semantic), "literal:%.*s",
                        static_cast<int>(property.literal.size()), property.literal.data());
    const bool integral = property.literal == "int";
    // Without a character there is nothing to choose from, and a choice of nothing would refuse
    // every clip: the name is free text, and the compile says what is missing.
    const bool clips = property.choices == Choices::Clips;
    const bool free = clips && clips_of(preview).empty();
    const ControlKind kind = free ? ControlKind::Text : property.kind;
    std::string_view fallback = property.fallback;
    if (clips && !free) {
        fallback = clips_of(preview)[0].name.text();
    }
    out.u32v(identity).u8v(static_cast<u8>(kind)).text(property.name);
    out.text(fallback).text(property.tooltip).text(semantic).text("");
    encode_choices(out, property, preview);
    out.text("").text("animation-and-skinning").u64v(0).u8v(0);
    // A whole number is zero or more, in steps of one: the compiler reads it unsigned.
    out.u8v(integral ? 0x5U : 0x0U).f64v(0.0).f64v(0.0).f64v(integral ? 1.0 : 0.0);
}

void encode_node(Out& out, const graph::NodeType& type,
                 const AnimationPreviewRuntime* preview) noexcept {
    out.u32v(wire::type_identity(type.name().text())).u32v(type.version());
    out.text(type.name().text()).u8v(0);
    out.u32v(static_cast<u32>(type.pins().size()));
    u32 pin_identity = 0;
    for (const graph::PinDesc& pin : type.pins()) {
        out.u32v(++pin_identity).u8v(static_cast<u8>(pin.direction));
        out.text(pin.name.text()).text(pin.type.text());
    }
    u32 count = 0;
    for (const PropertySpec& property : kProperties) {
        count += property.node == type.name().text() ? 1U : 0U;
    }
    out.u32v(count);
    u32 property_identity = 0;
    for (const PropertySpec& property : kProperties) {
        if (property.node == type.name().text()) {
            encode_property(out, property, ++property_identity, preview);
        }
    }
}

// --- Reading the authored graph ------------------------------------------------------------------

[[nodiscard]] NodeKey source_of(const graph::Graph& graph, NodeKey node, Name pin) noexcept {
    NodeKey source = graph::kInvalidNodeKey;
    for (const graph::Link& link : graph.links()) {
        if (link.to == node && link.to_pin == pin) {
            source = link.from;
        }
    }
    return source;
}

[[nodiscard]] Name text_property(const graph::Graph& graph, NodeKey node, const char* name) {
    const graph::Literal* literal = graph.property(node, Name::intern(name));
    return literal != nullptr ? literal->text : Name{};
}

[[nodiscard]] bool float_property(const graph::Graph& graph, NodeKey node, const char* name,
                                  f32& out) noexcept {
    const graph::Literal* literal = graph.property(node, Name::intern(name));
    if (literal == nullptr) {
        return false;
    }
    out = literal->value.x;
    return true;
}

void report(graph::DiagnosticSink& sink, graph::Severity severity, const char* code, NodeKey node,
            const char* message, Name detail = {}) noexcept {
    graph::Diagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.code = code;
    diagnostic.node = node;
    diagnostic.message = message;
    diagnostic.detail = detail;
    sink.report(diagnostic);
}

[[nodiscard]] bool is(const graph::GraphNode& node, const char* type) noexcept {
    return node.type == Name::intern(type);
}

void check_transition(const graph::Graph& graph, const graph::GraphNode& node,
                      graph::DiagnosticSink& sink) noexcept {
    const NodeKey from = source_of(graph, node.key, Name::intern("from"));
    const NodeKey to = source_of(graph, node.key, Name::intern("to"));
    const graph::GraphNode* source = graph.find_node(from);
    const graph::GraphNode* target = graph.find_node(to);
    if (source == nullptr || target == nullptr || !is(*source, "pose.state") ||
        !is(*target, "pose.state")) {
        report(sink, graph::Severity::Error, "animation.transition.unwired", node.key,
               "a transition leaves one state and enters another: wire a state's `state` output "
               "into both `from` and `to`");
    }
    f32 duration = 0.0F;
    if (!float_property(graph, node.key, "duration", duration) || !(duration > 0.0F)) {
        report(sink, graph::Severity::Error, "animation.transition.cut", node.key,
               "an authored transition blends: a duration that is not positive is a cut, which "
               "pops the pose. A game that wants a cut asks Animator.play for one");
    }
    if (text_property(graph, node.key, "condition").is_empty()) {
        report(sink, graph::Severity::Warning, "animation.transition.no-condition", node.key,
               "this transition names no condition parameter, so nothing ever opens it");
    }
}

void check_state(const graph::Graph& graph, const graph::GraphNode& node,
                 graph::DiagnosticSink& sink) noexcept {
    if (source_of(graph, node.key, Name::intern("pose")) == graph::kInvalidNodeKey) {
        report(sink, graph::Severity::Warning, "animation.state.empty", node.key,
               "nothing is wired into this state's pose, so it holds the reference pose");
    }
}

void check_events(const graph::GraphNode& node, std::string_view events, Name clip,
                  const AnimationClipInfo* known, graph::DiagnosticSink& sink,
                  Array<AnimationClipEvent>* out) noexcept {
    Array<AnimationClipEvent> parsed(allocator());
    if (!parse_animation_events(events, clip, parsed)) {
        report(sink, graph::Severity::Error, "animation.event.malformed", node.key,
               "a clip's events are `name@seconds` items separated by `;`");
        return;
    }
    for (const AnimationClipEvent& event : parsed) {
        if (event.time < 0.0F || (known != nullptr && event.time > known->duration)) {
            report(sink, graph::Severity::Error, "animation.event.outside", node.key,
                   "this event lies outside its clip", event.event);
        }
    }
    if (out != nullptr) {
        (void)out->append(parsed.span());
    }
}

/// Clip nodes are visited in key order; the first node naming a clip owns its events.
[[nodiscard]] bool events_owned(Span<const Name> owners, Name clip) noexcept {
    return std::ranges::any_of(owners, [clip](const Name owner) { return owner == clip; });
}

void check_clip(const graph::Graph& graph, const graph::GraphNode& node,
                const AnimationPreviewRuntime* preview, graph::DiagnosticSink& sink,
                Array<Name>& owners, Array<AnimationClipEvent>* events) noexcept {
    const Name clip = text_property(graph, node.key, "clip");
    const AnimationClipInfo* known = find_clip(preview, clip);
    if (preview != nullptr && known == nullptr) {
        report(sink, graph::Severity::Error, "animation.clip.unknown", node.key,
               "the preview character has no clip of this name", clip);
    }
    const Name text = text_property(graph, node.key, "events");
    if (events_owned(owners.span(), clip)) {
        if (!text.is_empty()) {
            report(sink, graph::Severity::Warning, "animation.event.shadowed", node.key,
                   "another node of this clip already gives its events; the first one is kept",
                   clip);
        }
        return;
    }
    (void)owners.push_back(clip);
    check_events(node, text.text(), clip, known, sink, events);
}

/// The checks a compiled program cannot carry. Collects every clip's events into `events`.
void check_authoring(const graph::Graph& graph, const AnimationPreviewRuntime* preview,
                     graph::DiagnosticSink& sink, Array<AnimationClipEvent>* events) noexcept {
    Array<const graph::GraphNode*> ordered(allocator());
    for (const graph::GraphNode& node : graph.nodes()) {
        (void)ordered.push_back(&node);
    }
    for (usize outer = 1; outer < ordered.size(); ++outer) {
        for (usize inner = outer; inner > 0 && ordered[inner - 1]->key > ordered[inner]->key;
             --inner) {
            const graph::GraphNode* swap = ordered[inner - 1];
            ordered[inner - 1] = ordered[inner];
            ordered[inner] = swap;
        }
    }
    Array<Name> owners(allocator());
    for (const graph::GraphNode* node : ordered) {
        if (is(*node, "pose.transition")) {
            check_transition(graph, *node, sink);
        } else if (is(*node, "pose.state")) {
            check_state(graph, *node, sink);
        } else if (is(*node, "pose.clip")) {
            check_clip(graph, *node, preview, sink, owners, events);
        }
    }
}

[[nodiscard]] bool has_error(const graph::DiagnosticSink& sink) noexcept {
    return std::ranges::any_of(sink.entries(), [](const graph::Diagnostic& diagnostic) {
        return diagnostic.severity == graph::Severity::Error;
    });
}

/// A parsed, checked and compiled graph: what both `animation.compile` and a preview start from.
struct Compiled {
    explicit Compiled(Allocator& memory) noexcept : sink(memory), events(memory), program(memory) {}

    graph::DiagnosticSink sink;
    Array<AnimationClipEvent> events;
    u64 semantic = 0;
    bool parsed = false;
    bool compiled = false;
    pose::PoseProgram program;
    /// The graph, for a preview's focus.
    Expected<graph::Graph, Error> graph = make_unexpected(Error{});
};

void compile(const AnimationPreviewRuntime* preview, std::string_view source,
             Compiled& out) noexcept {
    graph::NodeRegistry registry(allocator());
    if (Status registered = pose::register_pose_nodes(registry); !registered) {
        report(out.sink, graph::Severity::Error, "animation.source.invalid", 0,
               "the pose vocabulary could not be registered");
        return;
    }
    out.graph = graph::parse_graph(source, &registry, allocator(), out.sink);
    if (!out.graph) {
        report(out.sink, graph::Severity::Error, "animation.source.invalid", 0,
               "the graph's text does not parse");
        return;
    }
    out.parsed = true;
    out.semantic = out.graph->semantic_digest();
    (void)graph::validate(*out.graph, registry, nullptr, out.sink);
    check_authoring(*out.graph, preview, out.sink, &out.events);
    const u32 joints = preview != nullptr ? preview->joint_count() : pose::kMaxJoints;
    Expected<pose::PoseProgram, Error> program =
        pose::compile_pose(*out.graph, registry, joints, out.sink);
    if (!program) {
        report(out.sink, graph::Severity::Error, "animation.graph.invalid", 0,
               "the graph does not compile: an animation graph needs at least one state");
        return;
    }
    out.program = std::move(*program);
    out.compiled = !has_error(out.sink);
}

[[nodiscard]] bool is_clock(const pose::PoseProgram& program, u16 parameter) noexcept {
    return std::ranges::any_of(program.code(), [parameter](
                                                   const pose::PoseInstruction& instruction) {
        return instruction.op == pose::PoseOp::SampleClip && instruction.time_param == parameter;
    });
}

[[nodiscard]] u32 events_of(Span<const AnimationClipEvent> events, Name clip) noexcept {
    u32 count = 0;
    for (const AnimationClipEvent& event : events) {
        count += event.clip == clip ? 1U : 0U;
    }
    return count;
}

void encode_program(Out& out, const Compiled& compiled,
                    const AnimationPreviewRuntime* preview) noexcept {
    const pose::PoseProgram& program = compiled.program;
    out.u32v(program.joint_count()).u32v(static_cast<u32>(program.code().size()));
    out.u32v(static_cast<u32>(program.states().size()));
    for (const pose::PoseState& state : program.states()) {
        out.text(state.name.text()).u64v(state.origin).u32v(state.transition_count);
    }
    out.u32v(static_cast<u32>(program.transitions().size()));
    for (u32 index = 0; index < program.states().size(); ++index) {
        const pose::PoseState& state = program.states()[index];
        for (u32 offset = 0; offset < state.transition_count; ++offset) {
            const pose::Transition& transition =
                program.transitions()[state.first_transition + offset];
            out.u64v(transition.origin).u32v(index).u32v(transition.target_state);
            out.text(program.parameters()[transition.condition_param].text());
            out.f32v(transition.duration).u32v(transition.priority);
            out.u8v(static_cast<u8>(transition.interruption));
        }
    }
    out.u32v(static_cast<u32>(program.clips().size()));
    for (const pose::ClipRef& clip : program.clips()) {
        out.text(clip.name.text()).f32v(clip.duration).u8v(clip.looping ? 1 : 0);
        out.u8v(find_clip(preview, clip.name) != nullptr ? 1 : 0);
        out.u32v(events_of(compiled.events.span(), clip.name));
    }
    out.u32v(static_cast<u32>(program.parameters().size()));
    for (u16 index = 0; index < program.parameters().size(); ++index) {
        out.text(program.parameters()[index].text()).u8v(is_clock(program, index) ? 1 : 0);
    }
}

[[nodiscard]] AnimationRefusal refused(const char* code, const char* detail) noexcept {
    return AnimationRefusal{code, detail};
}

[[nodiscard]] AnimationRefusal answered(const Status& status) noexcept {
    return status ? AnimationRefusal{}
                  : refused("animation.reply", "the reply could not be encoded");
}

/// The first error, for a refusal's detail. Diagnostic messages are static strings.
[[nodiscard]] const char* first_error(const graph::DiagnosticSink& sink) noexcept {
    for (const graph::Diagnostic& diagnostic : sink.entries()) {
        if (diagnostic.severity == graph::Severity::Error) {
            return diagnostic.message;
        }
    }
    return "the graph does not compile";
}

struct PreviewArguments {
    std::string_view source;
    AnimationPreviewRequest request;
};

[[nodiscard]] AnimationRefusal read_preview(Span<const u8> payload, PreviewArguments& out,
                                            Array<AnimationParameter>& parameters) noexcept {
    wire::Reader reader(payload);
    const u32 format = reader.read_u32();
    out.source = reader.read_text();
    out.request.focus = reader.read_u64();
    out.request.time = reader.read_f32();
    out.request.playing = reader.read_u8() != 0;
    const u32 count = reader.read_u32();
    constexpr u32 kMostParameters = 64;
    for (u32 index = 0; index < count && index < kMostParameters; ++index) {
        AnimationParameter parameter;
        parameter.name = Name::intern(reader.read_text());
        parameter.value = reader.read_f32();
        if (!parameters.push_back(parameter)) {
            return refused("animation.request.malformed", "out of memory");
        }
    }
    if (!reader.complete() || count > kMostParameters) {
        return refused("animation.request.malformed",
                       "a preview is format, source, focus, time, playing and at most 64 "
                       "(name, value) parameters");
    }
    if (format != kAnimationWireFormat) {
        return refused("animation.schema.unsupported", "this engine reads animation format 1");
    }
    if (!std::isfinite(out.request.time) || out.request.time < 0.0F) {
        return refused("animation.request.malformed", "a preview time is finite and not negative");
    }
    for (const AnimationParameter& parameter : parameters) {
        if (!std::isfinite(parameter.value)) {
            return refused("animation.request.malformed", "a parameter is not finite");
        }
    }
    out.request.parameters = parameters.span();
    return {};
}

[[nodiscard]] AnimationRefusal preview_set(AnimationPreviewRuntime* preview, Span<const u8> payload,
                                           Array<u8>& reply) noexcept {
    PreviewArguments arguments;
    Array<AnimationParameter> parameters(allocator());
    if (const AnimationRefusal bad = read_preview(payload, arguments, parameters); bad.refused()) {
        return bad;
    }
    if (preview == nullptr) {
        return refused("animation.preview.unavailable",
                       "the engine has no preview character: the host has none, or the build has "
                       "no animation");
    }
    Compiled compiled(allocator());
    compile(preview, arguments.source, compiled);
    if (!compiled.compiled) {
        return refused("animation.preview.uncompiled", first_error(compiled.sink));
    }
    if (arguments.request.focus != 0) {
        const graph::GraphNode* node = compiled.graph->find_node(arguments.request.focus);
        if (node == nullptr || !is(*node, "pose.clip")) {
            return refused("animation.preview.focus", "the focus is not a clip node of the graph");
        }
        arguments.request.focus_clip = text_property(*compiled.graph, node->key, "clip");
    }
    arguments.request.events = compiled.events.span();
    if (Status shown = preview->preview(std::move(compiled.program), arguments.request); !shown) {
        return refused("animation.preview.failed", shown.error().message);
    }
    return answered(encode_animation_state(preview, reply));
}

[[nodiscard]] AnimationRefusal compile_request(const AnimationPreviewRuntime* preview,
                                               Span<const u8> payload, Array<u8>& reply) noexcept {
    wire::Reader reader(payload);
    const u32 format = reader.read_u32();
    const std::string_view source = reader.read_text();
    if (!reader.complete()) {
        return refused("animation.request.malformed", "a compile is format and source");
    }
    if (format != kAnimationWireFormat) {
        return refused("animation.schema.unsupported", "this engine reads animation format 1");
    }
    return answered(encode_animation_compile(preview, source, reply));
}

[[nodiscard]] bool parse_seconds(std::string_view text, f32& out) noexcept {
    char buffer[32] = {};
    if (text.empty() || text.size() >= sizeof(buffer)) {
        return false;
    }
    std::memcpy(buffer, text.data(), text.size());
    char* end = nullptr;
    // strtof, not from_chars: AppleClang's libc++ has no floating-point from_chars.
    const f32 value = std::strtof(buffer, &end);
    if (end != buffer + text.size() || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

[[nodiscard]] std::string_view trimmed(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] bool event_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    return std::ranges::all_of(name, [](const char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '.' ||
               character == '-';
    });
}

}  // namespace

bool parse_animation_events(std::string_view text, Name clip,
                            Array<AnimationClipEvent>& out) noexcept {
    Array<AnimationClipEvent> parsed(allocator());
    while (!trimmed(text).empty()) {
        const usize end = text.find(';');
        const std::string_view item = trimmed(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        const usize at = item.find('@');
        if (at == std::string_view::npos) {
            return false;
        }
        AnimationClipEvent event;
        event.clip = clip;
        const std::string_view name = trimmed(item.substr(0, at));
        if (!event_name(name) || !parse_seconds(trimmed(item.substr(at + 1)), event.time)) {
            return false;
        }
        event.event = Name::intern(name);
        if (!parsed.push_back(event)) {
            return false;
        }
    }
    return static_cast<bool>(out.append(parsed.span()));
}

u64 animation_pose_digest(Span<const Transform> pose) noexcept {
    u64 hash = 14695981039346656037ULL;
    for (const Transform& joint : pose) {
        const f32 lanes[] = {joint.translation.x, joint.translation.y, joint.translation.z,
                             joint.rotation.x,    joint.rotation.y,    joint.rotation.z,
                             joint.rotation.w,    joint.scale.x,       joint.scale.y,
                             joint.scale.z};
        for (const f32 lane : lanes) {
            u32 bits = 0;
            std::memcpy(&bits, &lane, sizeof(bits));
            for (u32 byte = 0; byte < 4; ++byte) {
                hash ^= (bits >> (byte * 8U)) & 0xFFU;
                hash *= 1099511628211ULL;
            }
        }
    }
    return hash;
}

Status encode_animation_catalogue(const AnimationPreviewRuntime* preview, Array<u8>& out) noexcept {
    graph::NodeRegistry registry(allocator());
    if (Status registered = pose::register_pose_nodes(registry); !registered) {
        return registered;
    }
    out.clear();
    Out writer(out);
    writer.u32v(kCatalogueSchema).u32v(kCatalogueVersion);
    writer.u32v(static_cast<u32>(registry.types().size()));
    for (const graph::NodeType& type : registry.types()) {
        encode_node(writer, type, preview);
    }
    return writer.status();
}

Status encode_animation_compile(const AnimationPreviewRuntime* preview, std::string_view source,
                                Array<u8>& out) noexcept {
    Compiled compiled(allocator());
    compile(preview, source, compiled);
    out.clear();
    Out writer(out);
    writer.u32v(kAnimationWireFormat).u8v(compiled.compiled ? 1 : 0).u64v(compiled.semantic);
    writer.u64v(compiled.compiled ? compiled.program.digest() : 0);
    if (compiled.compiled) {
        encode_program(writer, compiled, preview);
    } else {
        writer.u32v(0).u32v(0).u32v(0).u32v(0).u32v(0).u32v(0);
    }
    wire::encode_diagnostics(writer, compiled.sink);
    return writer.status();
}

Status encode_animation_state(const AnimationPreviewRuntime* preview, Array<u8>& out) noexcept {
    out.clear();
    Out writer(out);
    const AnimationPreviewState none{};
    const AnimationPreviewState& state = preview != nullptr ? preview->state() : none;
    writer.u32v(kAnimationWireFormat).u8v(state.active ? 1 : 0).u8v(state.playing ? 1 : 0);
    writer.u64v(state.focus).text(state.focus_clip.text()).f32v(state.time).f32v(state.length);
    writer.u32v(state.state).text(state.state_name.text());
    writer.u32v(state.target).text(state.target_name.text()).f32v(state.blend);
    writer.u64v(state.program_digest).u64v(state.pose_digest).u64v(state.generation);
    const Span<const Transform> pose =
        preview != nullptr && state.active ? preview->pose() : Span<const Transform>{};
    writer.u32v(static_cast<u32>(pose.size()));
    for (const Transform& joint : pose) {
        writer.vec3(joint.translation);
        writer.f32v(joint.rotation.x).f32v(joint.rotation.y).f32v(joint.rotation.z);
        writer.f32v(joint.rotation.w).vec3(joint.scale);
    }
    const Span<const AnimationFiredEvent> events =
        preview != nullptr ? preview->events() : Span<const AnimationFiredEvent>{};
    writer.u32v(static_cast<u32>(events.size()));
    for (const AnimationFiredEvent& event : events) {
        writer.u64v(event.sequence).text(event.name.text()).f32v(event.normalised_time);
        writer.f32v(event.at);
    }
    return writer.status();
}

AnimationRefusal answer_animation(AnimationPreviewRuntime* preview, std::string_view operation,
                                  Span<const u8> payload, Array<u8>& reply) noexcept {
    if (operation == "animation.catalogue.get") {
        return answered(encode_animation_catalogue(preview, reply));
    }
    if (operation == "animation.compile") {
        return compile_request(preview, payload, reply);
    }
    if (operation == "animation.preview.set") {
        return preview_set(preview, payload, reply);
    }
    if (operation == "animation.preview.get" || operation == "animation.preview.stop") {
        if (preview != nullptr && operation == "animation.preview.stop") {
            preview->stop();
        }
        return answered(encode_animation_state(preview, reply));
    }
    return refused("animation.operation.unsupported",
                   "this engine has no such animation operation");
}

}  // namespace cy::editor
