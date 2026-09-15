// Every stage of a material's lowering, readable. M11.c task 1.3.

#include <cy/rendering/material/stages.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace cy::rendering::material {

namespace {

/// The same appending primitives `tools/material/src/cook.cpp` uses, and for the same reason:
/// `-Wformat-nonliteral` is on for the whole tree, so a format string taken from a caller is not
/// available. Two shapes — a number and a float — is all a dump needs.
void put(Array<char>& out, std::string_view text) noexcept {
    for (const char character : text) {
        if (!out.push_back(character)) {
            return;
        }
    }
}

void put_u32(Array<char>& out, u32 value) noexcept {
    char buffer[16] = {};
    (void)std::snprintf(buffer, sizeof(buffer), "%u", value);
    put(out, buffer);
}

/// `%.9g`, which is the round-trip precision for a `float` and the same spelling
/// `src/graph/include/cy/graph/text.h` fixes for the engine's canonical graph text. A dump written
/// at fewer digits is a dump two materials can share.
void put_f32(Array<char>& out, f32 value) noexcept {
    char buffer[32] = {};
    (void)std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    put(out, buffer);
}

void put_immediate(Array<char>& out, const Immediate& value, ValueType type) noexcept {
    const u32 components = value_type_components(type);
    if (components == 0) {
        put(out, "-");
        return;
    }
    const f32 parts[4] = {value.x, value.y, value.z, value.w};
    put(out, "(");
    for (u32 index = 0; index < components && index < 4U; ++index) {
        if (index != 0) {
            put(out, ", ");
        }
        put_f32(out, parts[index]);
    }
    put(out, ")");
}

void put_node_id(Array<char>& out, NodeId id) noexcept {
    if (id == kInvalidNode) {
        put(out, "-");
        return;
    }
    put(out, "%");
    put_u32(out, id);
}

[[nodiscard]] u64 digest_of(const Array<char>& text) noexcept {
    return hash_bytes(kHashSeed, text.data(), text.size());
}

void put_declarations(Array<char>& out, Span<const ParameterDecl> parameters,
                      Span<const TextureDecl> textures) noexcept {
    for (const ParameterDecl& decl : parameters) {
        put(out, "  param ");
        put(out, decl.name.text());
        put(out, " : ");
        put(out, value_type_name(decl.type));
        put(out, " = ");
        put_immediate(out, decl.default_value, decl.type);
        put(out, decl.requested_static ? "  [author asked for static]\n" : "\n");
    }
    for (const TextureDecl& decl : textures) {
        put(out, "  texture ");
        put(out, decl.name.text());
        put(out, " average ");
        put_immediate(out, decl.average, ValueType::Vec4);
        put(out, decl.shadow_critical ? "  [shadow-critical]\n" : "\n");
    }
}

}  // namespace

const char* lowering_stage_name(LoweringStage stage) noexcept {
    switch (stage) {
        case LoweringStage::Graph:
            return "graph";
        case LoweringStage::AuthoredIr:
            return "ir";
        case LoweringStage::OptimisedIr:
            return "optimised-ir";
        case LoweringStage::GeneratedSlang:
            return "slang";
        case LoweringStage::CompiledProgram:
            return "compiled";
        case LoweringStage::Count:
            break;
    }
    return "?";
}

LoweringInspection::LoweringInspection(Allocator& allocator) noexcept : stages_(allocator) {
    for (u32 index = 0; index < kLoweringStageCount; ++index) {
        StageDump dump(allocator);
        dump.stage = static_cast<LoweringStage>(index);
        (void)stages_.push_back(std::move(dump));
    }
}

bool LoweringInspection::complete() const noexcept {
    return stages_.size() == kLoweringStageCount &&
           std::ranges::all_of(stages_,
                               [](const StageDump& dump) noexcept { return dump.available; });
}

u32 LoweringInspection::available() const noexcept {
    u32 count = 0;
    for (const StageDump& dump : stages_) {
        count += dump.available ? 1U : 0U;
    }
    return count;
}

Status dump_graph(const MaterialGraph& graph, Array<char>& out) noexcept {
    put(out, "graph ");
    put(out, graph.name().text());
    put(out, "\n");
    put_declarations(out, graph.parameters(), graph.textures());
    put(out, "  surface <- ");
    put_node_id(out, graph.surface_output());
    put(out, "\n  opacity <- ");
    put_node_id(out, graph.opacity_output());
    put(out, "\n");

    for (const GraphNode& node : graph.nodes()) {
        put(out, "  ");
        put_node_id(out, node.id);
        put(out, " ");
        put(out, graph_op_name(node.op));
        put(out, " : ");
        put(out, value_type_name(node.type));
        if (!node.symbol.is_empty()) {
            put(out, " '");
            put(out, node.symbol.text());
            put(out, "'");
        }
        if (node.op == GraphOp::Constant) {
            put(out, " = ");
            put_immediate(out, node.value, node.type);
        }
        // EVERY PORT, WIRED OR NOT. A dump that omitted the empty ones could not answer "which
        // input is not connected?", which is the first question an author asks of a graph.
        for (u8 port = 0; port < MaterialGraph::kMaxPorts; ++port) {
            const u32 source = graph.input(node.id, port);
            if (source == kInvalidNode) {
                continue;
            }
            put(out, " in");
            put_u32(out, port);
            put(out, "=");
            put_node_id(out, source);
        }
        if (node.muted) {
            put(out, " [muted]");
        }
        if (has_flag(node.flags, NodeFlags::BaseReflectance)) {
            put(out, " [base-reflectance]");
        }
        if (has_flag(node.flags, NodeFlags::OpacityCritical)) {
            put(out, " [opacity-critical]");
        }
        if (has_flag(node.flags, NodeFlags::Microdetail)) {
            put(out, " [microdetail]");
        }
        put(out, "\n");
    }
    return out.push_back('\0') ? ok()
                               : fail(ErrorCode::OutOfMemory, "the graph dump could not grow");
}

Status dump_module(const Module& module, Array<char>& out) noexcept {
    put(out, "module ");
    put(out, module.name().text());
    put(out, "  digest 0x");
    {
        char buffer[24] = {};
        (void)std::snprintf(buffer, sizeof(buffer), "%016llx",
                            static_cast<unsigned long long>(module.digest()));
        put(out, buffer);
    }
    put(out, "\n");
    put_declarations(out, module.parameters(), module.textures());
    put(out, "  surface <- ");
    put_node_id(out, module.surface());
    put(out, "\n  opacity <- ");
    put_node_id(out, module.opacity());
    put(out, "\n");

    const NodeId roots[] = {module.surface(), module.opacity()};
    Array<NodeId> order(module.allocator());
    if (Status walked = canonical_order(module, Span<const NodeId>(roots, 2), order); !walked) {
        return walked;
    }
    for (const NodeId id : order) {
        const Node& node = module.node(id);
        put(out, "  ");
        put_node_id(out, id);
        put(out, " ");
        put(out, op_name(node.op));
        put(out, " : ");
        put(out, value_type_name(node.type));
        if (!node.symbol.is_empty()) {
            put(out, " '");
            put(out, node.symbol.text());
            put(out, "'");
        }
        if (node.op == Op::Constant) {
            put(out, " = ");
            put_immediate(out, node.value, node.type);
        }
        for (const NodeId operand : module.operands(id)) {
            put(out, " ");
            put_node_id(out, operand);
        }
        const Span<const u32> origins = module.origins(id);
        if (!origins.empty()) {
            put(out, "  from");
            for (const u32 origin : origins) {
                put(out, " ");
                put_u32(out, origin);
            }
        }
        if (has_flag(module.flags(id), NodeFlags::Uniform)) {
            put(out, " [uniform]");
        }
        put(out, "\n");
    }
    // THE NODES THE WALK DID NOT REACH, named rather than silently absent. "The editor SHALL be
    // able to show which nodes were dropped" is answered by a dump of the AUTHORED module that
    // lists them, and by the optimised module's not having them.
    u32 unreachable = 0;
    for (NodeId id = 0; id < module.size(); ++id) {
        bool reached = false;
        for (const NodeId visited : order) {
            reached = reached || visited == id;
        }
        unreachable += reached ? 0U : 1U;
    }
    put(out, "  unreachable ");
    put_u32(out, unreachable);
    put(out, " of ");
    put_u32(out, module.size());
    put(out, "\n");
    return out.push_back('\0') ? ok()
                               : fail(ErrorCode::OutOfMemory, "the module dump could not grow");
}

namespace {

/// Why the fifth stage is not here, said where the fifth stage would be.
///
/// A NAMED CONSTANT, for the same reason `preview.cpp`'s refusal is one: it makes the assignment a
/// single statement that `just roadmap-falsify` can DELETE and still get a tree that compiles — an
/// inspection presenting an unexplained blank as its fifth stage, which is precisely the panel that
/// shows four stages and calls them five. A mutation that only breaks the build would prove nothing
/// about this check.
constexpr const char* kBackendStageIsOwed =
    "the compiled backend output is `shader-system`'s: this target does not link the shader "
    "toolchain, and `attach_backend_stage` in cy::rendering-material-slang fills it in";

/// Fill the four stages the material compiler owns, from an already-lowered authored module.
[[nodiscard]] Status collect(LoweringInspection& inspection, const Module& authored,
                             const CompileOptions& options, ProgramKind kind, QualityTier tier,
                             Allocator& allocator) noexcept {
    StageDump& authored_stage = inspection.stage(LoweringStage::AuthoredIr);
    if (Status dumped = dump_module(authored, authored_stage.text); !dumped) {
        return dumped;
    }
    authored_stage.available = true;
    authored_stage.digest = digest_of(authored_stage.text);

    auto compiled = compile_material(authored, options, allocator);
    if (!compiled) {
        return make_unexpected(compiled.error());
    }
    inspection.cook_key = compiled.value().cook_key();

    const CompiledProgram* program = compiled.value().find(kind, tier);
    if (program == nullptr) {
        return fail(ErrorCode::NotFound,
                    "this material has no program of that kind and tier — an opaque material's "
                    "shadow program is absent by derivation, which is a structural fact rather "
                    "than an empty stage");
    }

    StageDump& optimised = inspection.stage(LoweringStage::OptimisedIr);
    if (Status dumped = dump_module(program->module, optimised.text); !dumped) {
        return dumped;
    }
    optimised.available = true;
    optimised.digest = digest_of(optimised.text);

    StageDump& slang = inspection.stage(LoweringStage::GeneratedSlang);
    for (const char character : program->source.view()) {
        if (!slang.text.push_back(character)) {
            return fail(ErrorCode::OutOfMemory, "the generated source could not be copied");
        }
    }
    slang.available = true;
    // THE EMITTER'S OWN DIGEST, not a re-hash of the copy. `GeneratedSource::digest` is what the
    // cook key is built from, so a stage list carrying a second digest of the same text would be
    // a second identity for one artefact.
    slang.digest = program->source.digest;

    StageDump& backend = inspection.stage(LoweringStage::CompiledProgram);
    backend.available = false;
    backend.reason = kBackendStageIsOwed;
    return ok();
}

}  // namespace

Expected<LoweringInspection, Error> inspect_lowering(const MaterialGraph& graph,
                                                     const CompileOptions& options,
                                                     ProgramKind kind, QualityTier tier,
                                                     Allocator& allocator) noexcept {
    LoweringInspection inspection(allocator);
    inspection.kind = kind;
    inspection.tier = tier;

    StageDump& stage = inspection.stage(LoweringStage::Graph);
    if (Status dumped = dump_graph(graph, stage.text); !dumped) {
        return make_unexpected(dumped.error());
    }
    stage.available = true;
    stage.digest = digest_of(stage.text);

    auto authored = lower_graph(graph, allocator, options.passes);
    if (!authored) {
        return make_unexpected(authored.error());
    }
    if (Status collected = collect(inspection, authored.value(), options, kind, tier, allocator);
        !collected) {
        return make_unexpected(collected.error());
    }
    return inspection;
}

Expected<LoweringInspection, Error> inspect_lowering(std::string_view source,
                                                     const Module& authored,
                                                     const CompileOptions& options,
                                                     ProgramKind kind, QualityTier tier,
                                                     Allocator& allocator) noexcept {
    LoweringInspection inspection(allocator);
    inspection.kind = kind;
    inspection.tier = tier;

    // THE TEXT IS THE AUTHORED FORM, so it is what stage one shows. A text-authored material has no
    // graph and saying "graph: unavailable" about one would be reporting an absence that is not a
    // gap — the specification's two front-ends are alternatives, not layers.
    StageDump& stage = inspection.stage(LoweringStage::Graph);
    put(stage.text, "text ");
    put(stage.text, authored.name().text());
    put(stage.text, "\n");
    put(stage.text, source);
    if (!stage.text.push_back('\0')) {
        return make_unexpected(
            Error{ErrorCode::OutOfMemory, "the authored text could not be copied", 0});
    }
    stage.available = true;
    stage.digest = digest_of(stage.text);

    if (Status collected = collect(inspection, authored, options, kind, tier, allocator);
        !collected) {
        return make_unexpected(collected.error());
    }
    return inspection;
}

}  // namespace cy::rendering::material
