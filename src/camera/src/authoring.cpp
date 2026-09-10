// CyberGraph to a camera rig definition. M8.b task 7.3.

#include <cy/camera/authoring.h>

#include <string_view>

namespace cy::camera {
namespace {

/// The rig vocabulary, in the order a rig graph is usually written. `rig.*` and not `camera.*`: the
/// header says why, and `lens` and `output` are the two names that would otherwise collide with the
/// expression vocabulary `cy::graph::camera` registers.
struct RigNodeSpec {
    const char* type;
    RigNodeKind kind;
};

constexpr RigNodeSpec kSpecs[] = {
    {"rig.target", RigNodeKind::Target},       {"rig.follow", RigNodeKind::Follow},
    {"rig.orbit", RigNodeKind::Orbit},         {"rig.offset", RigNodeKind::Offset},
    {"rig.look_at", RigNodeKind::LookAt},      {"rig.lens", RigNodeKind::Lens},
    {"rig.noise", RigNodeKind::Noise},         {"rig.constraint", RigNodeKind::Constraint},
    {"rig.collision", RigNodeKind::Collision}, {"rig.output", RigNodeKind::Output},
};

[[nodiscard]] const RigNodeSpec* spec_of(Name type) noexcept {
    for (const RigNodeSpec& spec : kSpecs) {
        if (type == Name::intern(spec.type)) {
            return &spec;
        }
    }
    return nullptr;
}

[[nodiscard]] graph::PinDesc pin(const char* name, const char* type,
                                 graph::PinDirection direction) noexcept {
    graph::PinDesc desc;
    desc.name = Name::intern(name);
    desc.type = Name::intern(type);
    desc.direction = direction;
    return desc;
}

/// One scalar property, or the fallback when the author did not set it.
[[nodiscard]] f32 number_of(const graph::Graph& source, graph::NodeKey key, const char* property,
                            f32 fallback) noexcept {
    const graph::Literal* literal = source.property(key, Name::intern(property));
    return (literal == nullptr) ? fallback : literal->value.x;
}

[[nodiscard]] bool flag_of(const graph::Graph& source, graph::NodeKey key, const char* property,
                           bool fallback) noexcept {
    const graph::Literal* literal = source.property(key, Name::intern(property));
    return (literal == nullptr) ? fallback : (literal->value.mask != 0U);
}

[[nodiscard]] Vec3 vector_of(const graph::Graph& source, graph::NodeKey key, const char* property,
                             Vec3 fallback) noexcept {
    const graph::Literal* literal = source.property(key, Name::intern(property));
    if (literal == nullptr) {
        return fallback;
    }
    return Vec3{literal->value.x, literal->value.y, literal->value.z};
}

void report(graph::DiagnosticSink& sink, graph::NodeKey node, const char* message,
            Name detail = Name{}) noexcept {
    graph::Diagnostic diagnostic;
    diagnostic.severity = graph::Severity::Error;
    diagnostic.node = node;
    diagnostic.message = message;
    diagnostic.detail = detail;
    sink.report(diagnostic);
}

/// Read a node's authored parameters into the descriptor its kind uses. One function per kind would
/// be ten functions that each read three properties; the switch keeps the mapping in one place
/// where a reader can see that every kind's properties are named consistently.
void read_parameters(const graph::Graph& source, const graph::GraphNode& node,
                     RigNodeDesc& desc) noexcept {
    switch (desc.kind) {
        case RigNodeKind::Target:
            desc.target.use_bounds_center = flag_of(source, node.key, "use_bounds_center", false);
            desc.target.anchor_offset = vector_of(source, node.key, "anchor_offset", Vec3{});
            desc.target.anchor_half_life = number_of(source, node.key, "anchor_half_life", 0.0F);
            break;
        case RigNodeKind::Follow:
            desc.follow.offset = vector_of(source, node.key, "offset", Vec3{0.0F, 2.0F, 6.0F});
            desc.follow.position_half_life = number_of(source, node.key, "half_life", 0.12F);
            desc.follow.dead_zone = number_of(source, node.key, "dead_zone", 0.0F);
            break;
        case RigNodeKind::Orbit:
            desc.orbit.yaw_scale = number_of(source, node.key, "yaw_scale", 1.0F);
            desc.orbit.pitch_scale = number_of(source, node.key, "pitch_scale", 1.0F);
            desc.orbit.min_pitch_radians = number_of(source, node.key, "min_pitch", -1.4F);
            desc.orbit.max_pitch_radians = number_of(source, node.key, "max_pitch", 1.4F);
            desc.orbit.near_distance = number_of(source, node.key, "near_distance", 3.0F);
            desc.orbit.far_distance = number_of(source, node.key, "far_distance", 12.0F);
            desc.orbit.distance_half_life = number_of(source, node.key, "half_life", 0.1F);
            break;
        case RigNodeKind::Offset:
            desc.offset.offset = vector_of(source, node.key, "offset", Vec3{});
            desc.offset.mirrored = flag_of(source, node.key, "mirrored", false);
            break;
        case RigNodeKind::LookAt:
            desc.look_at.aim_offset = vector_of(source, node.key, "aim_offset", Vec3{});
            desc.look_at.rotation_half_life = number_of(source, node.key, "half_life", 0.08F);
            desc.look_at.level_horizon = flag_of(source, node.key, "level_horizon", true);
            break;
        case RigNodeKind::Lens:
            desc.lens.near_value = number_of(source, node.key, "near_value", 1.0471975512F);
            desc.lens.far_value = number_of(source, node.key, "far_value", 1.0471975512F);
            desc.lens.near_plane = number_of(source, node.key, "near_plane", 0.1F);
            desc.lens.far_plane = number_of(source, node.key, "far_plane", 0.0F);
            desc.lens.half_life = number_of(source, node.key, "half_life", 0.08F);
            break;
        case RigNodeKind::Noise:
            desc.noise.amplitude = number_of(source, node.key, "amplitude", 1.0F);
            desc.noise.position_scale = number_of(source, node.key, "position_scale", 0.05F);
            desc.noise.rotation_scale = number_of(source, node.key, "rotation_scale", 0.01F);
            break;
        case RigNodeKind::Constraint:
            desc.constraint.min_distance = number_of(source, node.key, "min_distance", 0.0F);
            desc.constraint.max_distance = number_of(source, node.key, "max_distance", 0.0F);
            desc.constraint.region_center = vector_of(source, node.key, "region_center", Vec3{});
            desc.constraint.region_extents = vector_of(source, node.key, "region_extents", Vec3{});
            break;
        case RigNodeKind::Collision:
            desc.collision.probe_radius = number_of(source, node.key, "probe_radius", 0.25F);
            desc.collision.recovery_half_life = number_of(source, node.key, "half_life", 0.25F);
            desc.collision.occlusion_samples = static_cast<u8>(
                static_cast<u32>(number_of(source, node.key, "occlusion_samples", 1.0F)));
            break;
        case RigNodeKind::Output:
        case RigNodeKind::Custom:
        case RigNodeKind::Count:
            break;
    }
}

}  // namespace

Status register_rig_nodes(graph::NodeRegistry& registry) noexcept {
    // Every rig node has one input and one output, and the chain from `rig.target` to `rig.output`
    // IS the composition. `camera-system`: "composed rather than inherited".
    const graph::PinDesc pins[] = {pin("in", "rig", graph::PinDirection::Input),
                                   pin("out", "rig", graph::PinDirection::Output)};
    for (const RigNodeSpec& spec : kSpecs) {
        graph::NodeTypeDesc desc;
        desc.name = Name::intern(spec.type);
        desc.plugin = Name::intern("cy.camera");
        desc.version = 1;
        desc.pure = true;
        desc.determinism = graph::Determinism::Deterministic;
        // A target has an output and no input; the output node has an input and no output;
        // everything between has both, which is what makes the chain a chain.
        if (spec.kind == RigNodeKind::Target) {
            desc.pins = Span<const graph::PinDesc>(pins + 1, 1);
        } else if (spec.kind == RigNodeKind::Output) {
            desc.pins = Span<const graph::PinDesc>(pins, 1);
        } else {
            desc.pins = Span<const graph::PinDesc>(pins, 2);
        }
        if (Status added = registry.register_type(desc); !added) {
            return added;
        }
    }
    return ok();
}

Expected<RigDefinition, Error> rig_definition_from_graph(const graph::Graph& source,
                                                         const graph::NodeRegistry& registry,
                                                         Allocator& allocator,
                                                         graph::DiagnosticSink& sink) noexcept {
    (void)registry;
    RigDefinition definition(allocator);
    definition.name = source.name();

    u32 outputs = 0;
    for (const graph::GraphNode& node : source.nodes()) {
        const RigNodeSpec* spec = spec_of(node.type);
        if (spec == nullptr) {
            report(sink, node.key, "this node is not a camera rig node", node.type);
            return make_unexpected(
                Error{ErrorCode::InvalidArgument, "a rig graph holds rig nodes", 0});
        }
        if (node.muted) {
            // AN AUTHOR'S MUTE LOWERS TO NOTHING rather than being deleted from the graph — the
            // property `visual-scripting` asks of every consumer, and here it means the node simply
            // does not reach the definition.
            continue;
        }
        if (spec->kind == RigNodeKind::Output) {
            ++outputs;
        }

        RigNodeDesc desc;
        // A rig node's identity is an AUTHORED NAME, because that is what the compiled program's
        // trace reports and what a designer reads in the inspector. It is a property rather than a
        // derivation from the node key: a key is stable across an edit, and a name is stable across
        // a re-author, and it is the second one a rig's diagnostics are read in.
        const graph::Literal* id = source.property(node.key, Name::intern("id"));
        if (id == nullptr || id->text.is_empty()) {
            report(sink, node.key, "a rig node needs an `id` property: the trace reports by name");
            return make_unexpected(
                Error{ErrorCode::InvalidArgument, "every rig node needs an id", 0});
        }
        desc.id = id->text;
        desc.kind = spec->kind;
        read_parameters(source, node, desc);

        // The wire is the composition: a node's `in` pin names the node before it.
        Array<graph::Link> inputs(allocator);
        if (Status found = source.inputs_of(node.key, Name::intern("in"), inputs); !found) {
            return make_unexpected(found.error());
        }
        if (!inputs.empty()) {
            const graph::GraphNode* upstream = source.find_node(inputs[inputs.size() - 1].from);
            if (upstream != nullptr) {
                const graph::Literal* upstream_id =
                    source.property(upstream->key, Name::intern("id"));
                if (upstream_id == nullptr || upstream_id->text.is_empty()) {
                    report(sink, upstream->key, "a rig node needs an `id` property");
                    return make_unexpected(
                        Error{ErrorCode::InvalidArgument, "every rig node needs an id", 0});
                }
                desc.input = upstream_id->text;
            }
        } else if (spec->kind != RigNodeKind::Target) {
            report(sink, node.key, "this rig node has no input: a rig is a chain from a target");
            return make_unexpected(Error{ErrorCode::InvalidArgument,
                                         "every rig node but the target needs an input", 0});
        }

        if (Status pushed = definition.nodes.push_back(desc); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    if (outputs != 1) {
        report(sink, graph::kInvalidNodeKey,
               "a rig graph needs exactly one `rig.output`: it is the node every path reaches");
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a rig graph needs exactly one output", 0});
    }
    return definition;
}

}  // namespace cy::camera
