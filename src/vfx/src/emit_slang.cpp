// The Slang back end: one program per kernel, and the prelude that makes the unit compilable on its
// own. M8.c task 2.1.
//
// ================================================================================================
// THE STORAGE SPELLING IS THE LAYOUT'S DECISION, NOT THIS FILE'S
// ================================================================================================
//
// An attribute the compiler put at `Float16` is loaded through `f16tof32` and stored through
// `f32tof16`; one at `Unorm8` is four components to a word. That is what makes task 2.2's
// "compiler-derived attribute layout" reach the generated code rather than stop at a report: the
// same declaration that shrank the per-particle byte size also changed what the kernel emits.
//
// Every attribute is one `RWStructuredBuffer<uint>` addressed in 32-bit words, because a structured
// buffer of `half` is not portable and a byte-address buffer needs a different spelling on every
// backend. The words-per-particle count is derived from the precision and the component count, and
// it is written into the generated comment beside the buffer so a reader can check the arithmetic.

#include "emit_slang.h"

#include "access.h"

#include <cy/graph/emit.h>

#include <utility>

namespace cy::vfx {
namespace {

using graph::EmitOptions;
using graph::EmitView;
using graph::Node;
using graph::SourceTarget;
using graph::TextWriter;

/// How many components of an attribute at this precision share one 32-bit word.
[[nodiscard]] u32 components_per_word(Precision precision) noexcept {
    switch (precision) {
        case Precision::Float32:
            return 1;
        case Precision::Float16:
        case Precision::Snorm16:
            return 2;
        case Precision::Unorm8:
            return 4;
        case Precision::Auto:
            break;
    }
    return 1;
}

[[nodiscard]] u32 words_per_particle(const AttributeSlot& slot) noexcept {
    const u32 per_word = components_per_word(slot.precision);
    return (slot.components + per_word - 1U) / per_word;
}

/// Slang's spelling of a VFX type.
[[nodiscard]] const char* slang_type(TypeId type) noexcept {
    switch (type) {
        case Float:
            return "float";
        case Float2:
            return "float2";
        case Float3:
            return "float3";
        case Float4:
            return "float4";
        case Int:
            return "int";
        case Bool:
            return "bool";
        default:
            break;
    }
    return "float";
}

/// A name safe to paste into an identifier: `wind_field.velocity` becomes `wind_field_velocity`.
void write_identifier(TextWriter& writer, std::string_view text) noexcept {
    char buffer[192];
    usize length = 0;
    for (const char character : text) {
        if (length + 1 >= sizeof(buffer)) {
            break;
        }
        const bool safe = (character >= 'a' && character <= 'z') ||
                          (character >= 'A' && character <= 'Z') ||
                          (character >= '0' && character <= '9') || character == '_';
        buffer[length++] = safe ? character : '_';
    }
    writer.text({buffer, length});
}

/// The expression that reads component `component` of `slot` for particle `p`.
void write_component_load(TextWriter& writer, const AttributeSlot& slot, u32 component) noexcept {
    const u32 per_word = components_per_word(slot.precision);
    const u32 word = component / per_word;
    const u32 lane = component % per_word;
    const char* unpack = "cyVfxUnpackF32";
    switch (slot.precision) {
        case Precision::Float16:
            unpack = "cyVfxUnpackF16";
            break;
        case Precision::Unorm8:
            unpack = "cyVfxUnpackUnorm8";
            break;
        case Precision::Snorm16:
            unpack = "cyVfxUnpackSnorm16";
            break;
        default:
            break;
    }
    writer.text(unpack);
    writer.text("(cyVfxAttr_");
    write_identifier(writer, slot.name.text());
    writer.text("[base + ");
    writer.number(word);
    writer.text("u]");
    if (slot.precision != Precision::Float32) {
        writer.text(", ");
        writer.number(lane);
        writer.text("u");
    }
    writer.text(")");
}

void write_attribute_accessors(TextWriter& writer, const AttributeSlot& slot) noexcept {
    const u32 words = words_per_particle(slot);
    writer.text("// `");
    writer.text(slot.name.text());
    writer.text("` at ");
    writer.text(precision_name(slot.precision));
    writer.text(": ");
    writer.number(slot.components);
    writer.text(" component(s), ");
    writer.number(slot.stride);
    writer.text(" bytes a particle, ");
    writer.number(words);
    writer.text(" word(s).\n");
    writer.text("RWStructuredBuffer<uint> cyVfxAttr_");
    write_identifier(writer, slot.name.text());
    writer.text(";\n");

    writer.text(slang_type(slot.type));
    writer.text(" cyVfxLoad_");
    write_identifier(writer, slot.name.text());
    writer.text("(uint particle) {\n    uint base = particle * ");
    writer.number(words);
    writer.text("u;\n    return ");
    if (slot.components > 1) {
        writer.text(slang_type(slot.type));
        writer.text("(");
    }
    for (u32 component = 0; component < slot.components; ++component) {
        if (component != 0) {
            writer.text(", ");
        }
        write_component_load(writer, slot, component);
    }
    if (slot.components > 1) {
        writer.text(")");
    }
    writer.text(";\n}\n");

    writer.text("void cyVfxStore_");
    write_identifier(writer, slot.name.text());
    writer.text("(uint particle, ");
    writer.text(slang_type(slot.type));
    writer.text(" value) {\n    uint base = particle * ");
    writer.number(words);
    writer.text("u;\n");
    const u32 per_word = components_per_word(slot.precision);
    for (u32 word = 0; word < words; ++word) {
        writer.text("    uint w");
        writer.number(word);
        writer.text(" = ");
        // A partly-filled word is read back first: two components of one word are two stores, and
        // the second must not erase the first.
        const bool partial = slot.precision != Precision::Float32 &&
                             (slot.components - (word * per_word)) < per_word;
        if (partial) {
            writer.text("cyVfxAttr_");
            write_identifier(writer, slot.name.text());
            writer.text("[base + ");
            writer.number(word);
            writer.text("u]");
        } else {
            writer.text("0u");
        }
        writer.text(";\n");
    }
    for (u32 component = 0; component < slot.components; ++component) {
        const u32 word = component / per_word;
        const u32 lane = component % per_word;
        writer.text("    cyVfxPackLane(w");
        writer.number(word);
        writer.text(", ");
        writer.number(lane);
        writer.text("u, ");
        switch (slot.precision) {
            case Precision::Float16:
                writer.text("cyVfxPackF16(");
                break;
            case Precision::Unorm8:
                writer.text("cyVfxPackUnorm8(");
                break;
            case Precision::Snorm16:
                writer.text("cyVfxPackSnorm16(");
                break;
            default:
                writer.text("asuint(");
                break;
        }
        if (slot.components == 1) {
            writer.text("value");
        } else {
            writer.text("value[");
            writer.number(component);
            writer.text("]");
        }
        writer.text("), ");
        writer.number(32U / per_word);
        writer.text("u);\n");
    }
    for (u32 word = 0; word < words; ++word) {
        writer.text("    cyVfxAttr_");
        write_identifier(writer, slot.name.text());
        writer.text("[base + ");
        writer.number(word);
        writer.text("u] = w");
        writer.number(word);
        writer.text(";\n");
    }
    writer.text("}\n");
}

/// The helpers every generated unit needs, and nothing an effect does not use. Fixed text, so it is
/// one string rather than a generator.
constexpr const char* kHelpers =
    R"(// --- Storage helpers. One spelling of each conversion, so a load and a store agree. ---
float cyVfxUnpackF32(uint word) { return asfloat(word); }
float cyVfxUnpackF16(uint word, uint lane) { return f16tof32((word >> (lane * 16u)) & 0xFFFFu); }
float cyVfxUnpackUnorm8(uint word, uint lane) {
    return float((word >> (lane * 8u)) & 0xFFu) * (1.0 / 255.0);
}
float cyVfxUnpackSnorm16(uint word, uint lane) {
    int raw = int((word >> (lane * 16u)) & 0xFFFFu);
    raw = (raw & 0x8000) != 0 ? raw - 65536 : raw;
    return max(float(raw) * (1.0 / 32767.0), -1.0);
}
uint cyVfxPackF16(float value) { return f32tof16(value); }
uint cyVfxPackUnorm8(float value) { return uint(saturate(value) * 255.0 + 0.5); }
uint cyVfxPackSnorm16(float value) { return uint(int(clamp(value, -1.0, 1.0) * 32767.0)) & 0xFFFFu; }
void cyVfxPackLane(inout uint word, uint lane, uint bits, uint width) {
    if (width >= 32u) { word = bits; return; }
    uint shift = lane * width;
    uint mask = ((1u << width) - 1u) << shift;
    word = (word & ~mask) | ((bits << shift) & mask);
}

// --- Per-dispatch inputs the host supplies. ---
struct CyVfxInput {
    float dt;
    float emitter_age;
    float particle_index;
    float spawn_index;
    float normalised_age;
};
ConstantBuffer<CyVfxInput> cyVfxInput;

// A per-particle random draw. The stream index is the authoring node's own key, so two `random`
// nodes are two streams and one node is one value per particle per evaluation.
float cyVfxRandom(uint particle, uint stream) {
    uint state = particle * 747796405u + stream * 2891336453u + 1u;
    state ^= state >> 16u;
    state *= 2246822519u;
    state ^= state >> 13u;
    state *= 3266489917u;
    state ^= state >> 16u;
    return float(state >> 8u) * (1.0 / 16777216.0);
}

float cyVfxNoise(float x) {
    float i = floor(x);
    float f = frac(x);
    float a = frac(sin(i * 12.9898) * 43758.5453);
    float b = frac(sin((i + 1.0) * 12.9898) * 43758.5453);
    return lerp(a, b, f * f * (3.0 - 2.0 * f));
}

// A curve is a cook-time resource; until one is bound this is the identity over [0, 1], and the
// generated call names the curve so a binding can replace it without the kernel changing.
float cyVfxCurve(float t) { return saturate(t); }

// An event channel's buffer and its append. The channel's declared maximum is enforced on the CPU
// side by `EventRouter`; this is the GPU-side append, and the bound is the buffer's own size.
struct CyVfxEvent {
    uint source;
    uint depth;
    float rank;
    float payload;
};

// The kill and spawn sinks the roots assign into.
RWStructuredBuffer<uint> cyVfxAlive;
RWStructuredBuffer<uint> cyVfxSpawnCount;
void cyVfxKill(uint particle) { cyVfxAlive[particle] = 0u; }
)";

/// The Slang target. See `cy::graph::emit.h` for the split: the ORDER, the numbering and what is a
/// statement are the shared core's; everything below is this domain's.
class SlangTarget final : public SourceTarget {
public:
    SlangTarget(const CompiledEmitter& emitter, const VfxKernel& kernel) noexcept
        : emitter_(&emitter), kernel_(&kernel) {}

    void header(TextWriter& writer, const Module& module,
                const EmitOptions& /*options*/) const override {
        writer.text("// Generated by cy::vfx. Emitter `");
        writer.text(emitter_->name().text());
        writer.text("`, stages");
        for (u32 which = 0; which < static_cast<u32>(Stage::Count); ++which) {
            if (kernel_->covers(static_cast<Stage>(which))) {
                writer.text(" ");
                writer.text(stage_name(static_cast<Stage>(which)));
            }
        }
        writer.text(", IR digest 0x");
        writer.hex(module.digest());
        writer.text(".\n");
    }

    void signature(TextWriter& writer, const Module& /*module*/, const EmitOptions& /*options*/,
                   bool /*preview*/) const override {
        writer.text("void ");
        write_entry_name(writer);
        writer.text("(uint particle) {\n");
    }

    [[nodiscard]] const char* type_spelling(const Module& /*module*/, TypeId type) const override {
        return slang_type(type);
    }

    void leaf(TextWriter& writer, const Module& module, NodeId id) const override {
        const Node& node = module.node(id);
        switch (node.op) {
            case Constant:
                write_constant(writer, node);
                return;
            case Parameter:
                writer.text("cyVfxParams.");
                write_identifier(writer, node.symbol.text());
                return;
            case Attribute:
                writer.text("cyVfxLoad_");
                write_identifier(writer, node.symbol.text());
                writer.text("(particle)");
                return;
            case EmitterInput:
                writer.text("cyVfxInput.");
                write_identifier(writer, node.symbol.text());
                return;
            case Random:
                writer.text("cyVfxRandom(particle, ");
                writer.number(node.value.mask);
                writer.text("u)");
                return;
            default:
                break;
        }
        writer.text("0.0");
    }

    void expression(const EmitView& view, NodeId id) const override {
        const Node& node = view.module().node(id);
        const Span<const NodeId> operands = view.module().operands(id);
        TextWriter& writer = view.out();
        switch (node.op) {
            case Add:
                infix(view, operands, " + ");
                return;
            case Sub:
                infix(view, operands, " - ");
                return;
            case Mul:
                infix(view, operands, " * ");
                return;
            case Div:
                infix(view, operands, " / ");
                return;
            case Less:
                infix(view, operands, " < ");
                return;
            case Greater:
                infix(view, operands, " > ");
                return;
            case LogicalAnd:
                infix(view, operands, " && ");
                return;
            case LogicalOr:
                infix(view, operands, " || ");
                return;
            case LogicalNot:
                writer.text("!");
                view.operand(operands[0]);
                return;
            case Select:
                writer.text("(");
                view.operand(operands[0]);
                writer.text(" ? ");
                view.operand(operands[1]);
                writer.text(" : ");
                view.operand(operands[2]);
                writer.text(")");
                return;
            case MakeVec3:
                call(view, operands, "float3");
                return;
            case MakeVec4:
                call(view, operands, "float4");
                return;
            case Sample:
                writer.text("cyVfxSample_");
                write_identifier(writer, node.symbol.text());
                writer.text("(");
                view.operand(operands[0]);
                writer.text(")");
                return;
            case Curve:
                writer.text("cyVfxCurve(");
                view.operand(operands[0]);
                writer.text(")");
                return;
            case Noise:
                call(view, operands, "cyVfxNoise");
                return;
            default:
                break;
        }
        call(view, operands, vfx_op_name(node.op));
    }

    void assign_root(const EmitView& view, u32 slot, NodeId id, bool /*preview*/) const override {
        TextWriter& writer = view.out();
        if (slot == kKillRoot) {
            writer.text("    if (");
            view.operand(id);
            writer.text(") { cyVfxKill(particle); }\n");
            return;
        }
        if (slot == kSpawnRoot) {
            writer.text("    cyVfxSpawnCount[0] = uint(max(");
            view.operand(id);
            writer.text(", 0.0));\n");
            return;
        }
        for (const KernelEvent& raise : kernel_->events()) {
            if (raise.root_slot != slot) {
                continue;
            }
            writer.text("    if (");
            view.operand(id);
            writer.text(" != 0.0) { cyVfxRaise_");
            write_identifier(writer, raise.channel.text());
            writer.text("(particle, ");
            view.operand(id);
            writer.text("); }\n");
            return;
        }
        for (const KernelWrite& write : kernel_->writes()) {
            if (write.root_slot != slot) {
                continue;
            }
            const AttributeSlot* attribute = emitter_->layout().find(write.attribute);
            if (attribute == nullptr || attribute->elided) {
                // AN ELIDED ATTRIBUTE'S WRITE IS NOT EMITTED. That is the visible half of task
                // 2.2: liveness does not only shrink a report, it removes a store from the kernel.
                writer.text("    // `");
                writer.text(write.attribute.text());
                writer.text("` is written and never read: elided by attribute liveness.\n");
                return;
            }
            writer.text("    cyVfxStore_");
            write_identifier(writer, write.attribute.text());
            writer.text("(particle, ");
            view.operand(id);
            writer.text(");\n");
            return;
        }
    }

    void epilogue(TextWriter& writer) const override { writer.text("}\n"); }

    [[nodiscard]] const char* hoist_comment() const override {
        return "    // Uniform across the dispatch.\n";
    }
    [[nodiscard]] const char* varying_comment() const override { return "    // Per particle.\n"; }

    void write_entry_name(TextWriter& writer) const noexcept {
        writer.text("cyVfxKernel_");
        write_identifier(writer, emitter_->name().text());
        writer.text("_");
        writer.number(kernel_->stages());
    }

private:
    static void infix(const EmitView& view, Span<const NodeId> operands,
                      const char* separator) noexcept {
        view.out().text("(");
        for (usize index = 0; index < operands.size(); ++index) {
            if (index != 0) {
                view.out().text(separator);
            }
            view.operand(operands[index]);
        }
        view.out().text(")");
    }

    static void call(const EmitView& view, Span<const NodeId> operands,
                     const char* function) noexcept {
        view.out().text(function);
        view.out().text("(");
        for (usize index = 0; index < operands.size(); ++index) {
            if (index != 0) {
                view.out().text(", ");
            }
            view.operand(operands[index]);
        }
        view.out().text(")");
    }

    static void write_constant(TextWriter& writer, const Node& node) noexcept {
        const u32 components = vfx_type_components(node.type);
        if (node.type == Bool) {
            writer.text(node.value.x != 0.0F ? "true" : "false");
            return;
        }
        if (components > 1) {
            writer.text(slang_type(node.type));
            writer.text("(");
        }
        for (u32 component = 0; component < (components == 0 ? 1U : components); ++component) {
            if (component != 0) {
                writer.text(", ");
            }
            writer.literal(immediate_component(node.value, component));
        }
        if (components > 1) {
            writer.text(")");
        }
    }

    const CompiledEmitter* emitter_;
    const VfxKernel* kernel_;
};

/// Every distinct `<interface>.<field>` any kernel of this emitter samples, so the prelude declares
/// one sampler per pair and no more.
[[nodiscard]] Status collect_samples(const CompiledEmitter& emitter, Array<Name>& out,
                                     Array<TypeId>& types) noexcept {
    out.clear();
    types.clear();
    for (const VfxKernel& kernel : emitter.kernels()) {
        for (const KernelStep& step : kernel.program()) {
            if (step.op != Sample) {
                continue;
            }
            bool seen = false;
            for (const Name existing : out) {
                seen = seen || existing == step.symbol;
            }
            if (seen) {
                continue;
            }
            if (Status pushed = out.push_back(step.symbol); !pushed) {
                return pushed;
            }
            if (Status pushed = types.push_back(step.type); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

/// One append per event channel this emitter raises on. The channel's declared per-frame maximum is
/// the buffer's bound and is generated into the code, so a kernel cannot write past it — which is
/// the GPU-side half of "exceeding either SHALL drop events ... rather than compounding".
void write_channel(TextWriter& writer, const EventChannelDecl& channel) noexcept {
    writer.text("RWStructuredBuffer<uint> cyVfxEventCount_");
    write_identifier(writer, channel.name.text());
    writer.text(";\nRWStructuredBuffer<CyVfxEvent> cyVfxEvents_");
    write_identifier(writer, channel.name.text());
    writer.text(";\nvoid cyVfxRaise_");
    write_identifier(writer, channel.name.text());
    writer.text("(uint particle, float rank) {\n    uint slot = 0;\n    InterlockedAdd(");
    writer.text("cyVfxEventCount_");
    write_identifier(writer, channel.name.text());
    writer.text("[0], 1u, slot);\n    if (slot >= ");
    writer.number(channel.max_events_per_frame);
    writer.text("u) { return; }\n    CyVfxEvent raised;\n    raised.source = particle;\n");
    writer.text("    raised.depth = 0u;\n    raised.rank = rank;\n    raised.payload = rank;\n");
    writer.text("    cyVfxEvents_");
    write_identifier(writer, channel.name.text());
    writer.text("[slot] = raised;\n}\n");
}

/// Does any kernel of this emitter raise on `channel`? A channel nothing raises on gets no buffer,
/// for the same reason an attribute nothing touches gets no array.
[[nodiscard]] bool raises_on(const CompiledEmitter& emitter, Name channel) noexcept {
    for (const VfxKernel& kernel : emitter.kernels()) {
        for (const KernelEvent& raise : kernel.events()) {
            if (raise.channel == channel) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

Status emit_prelude(const CompiledEmitter& emitter, Span<const ParameterDecl> parameters,
                    Span<const EventChannelDecl> channels, Array<char>& out) noexcept {
    TextWriter writer(out);
    writer.text("// Generated prelude for emitter `");
    writer.text(emitter.name().text());
    writer.text(
        "`. Everything below is derived from this effect's own declarations, which is the\n");
    writer.text("// only place it can come from: a parameter block and an attribute layout are\n");
    writer.text("// different for every effect and cannot live in a shared standard library.\n");
    writer.text("// This unit imports nothing, so it compiles on its own.\n\n");
    writer.text(kHelpers);
    writer.text("\n// --- The effect's parameter block. ---\nstruct CyVfxParams {\n");
    if (parameters.empty()) {
        // A struct with no members is not valid in every Slang target; one padding word costs
        // nothing and keeps the declaration uniform.
        writer.text("    float cyVfxUnused;\n");
    }
    for (const ParameterDecl& parameter : parameters) {
        writer.text("    ");
        const TypeId type = vfx_type_from_name(parameter.type);
        writer.text(slang_type(type == kInvalidType ? static_cast<TypeId>(Float) : type));
        writer.text(" ");
        write_identifier(writer, parameter.name.text());
        writer.text(";\n");
    }
    writer.text("};\nConstantBuffer<CyVfxParams> cyVfxParams;\n\n");

    writer.text("// --- The derived attribute layout. ");
    writer.number(emitter.layout().allocated_attributes());
    writer.text(" attribute(s) allocated, ");
    writer.number(emitter.layout().elided_attributes());
    writer.text(" elided,\n// ");
    writer.number(emitter.layout().bytes_per_particle());
    writer.text(" bytes a particle. ---\n");
    for (const AttributeSlot& slot : emitter.layout().slots()) {
        if (slot.elided) {
            writer.text("// `");
            writer.text(slot.name.text());
            writer.text("` was written and never read: not allocated.\n");
            continue;
        }
        write_attribute_accessors(writer, slot);
    }

    bool any_channel = false;
    for (const EventChannelDecl& channel : channels) {
        if (!raises_on(emitter, channel.name)) {
            continue;
        }
        if (!any_channel) {
            writer.text(
                "\n// --- The event channels this emitter raises on, with their declared "
                "per-frame bounds. ---\n");
            any_channel = true;
        }
        write_channel(writer, channel);
    }

    Array<Name> samples(out.allocator());
    Array<TypeId> sample_types(out.allocator());
    if (Status collected = collect_samples(emitter, samples, sample_types); !collected) {
        return collected;
    }
    if (!samples.empty()) {
        writer.text("\n// --- One sampler per data-interface field this effect reads. ---\n");
    }
    for (usize index = 0; index < samples.size(); ++index) {
        writer.text(slang_type(sample_types[index]));
        writer.text(" cyVfxSample_");
        write_identifier(writer, samples[index].text());
        writer.text("(float x);\n");
    }
    writer.text("\n");
    return writer.status();
}

Status emit_emitter_sources(CompiledEmitter& emitter,
                            Span<const ParameterDecl> /*parameters*/) noexcept {
    Array<GeneratedSource>& sources = CompiledAccess::sources(emitter);
    sources.clear();
    for (const VfxKernel& kernel : emitter.kernels()) {
        SlangTarget target(emitter, kernel);
        EmitOptions options;
        options.variant = kernel.stages();
        auto emitted = graph::emit_program(kernel.expressions(), target, options);
        if (!emitted) {
            return make_unexpected(emitted.error());
        }
        if (Status pushed = sources.push_back(std::move(emitted.value())); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status assemble_translation_unit(const CompiledEmitter& emitter,
                                 Span<const ParameterDecl> parameters,
                                 Span<const EventChannelDecl> channels, Array<char>& out) noexcept {
    out.clear();
    if (Status prelude = emit_prelude(emitter, parameters, channels, out); !prelude) {
        return prelude;
    }
    TextWriter writer(out);
    for (const GeneratedSource& source : emitter.sources()) {
        writer.text(source.view());
        writer.text("\n");
    }

    // The entry point. A compute shader, because a simulation kernel is one — and because what this
    // proves is that the generated program COMPILES and reflects, which is the same claim
    // `src/rendering/material/`'s probe entry point makes for a material.
    writer.text("[shader(\"compute\")]\n[numthreads(64, 1, 1)]\nvoid ");
    writer.text(kVfxKernelEntryPoint);
    writer.text("(uint3 thread : SV_DispatchThreadID) {\n");
    for (const VfxKernel& kernel : emitter.kernels()) {
        SlangTarget target(emitter, kernel);
        writer.text("    ");
        target.write_entry_name(writer);
        writer.text("(thread.x);\n");
    }
    writer.text("}\n");
    return writer.status();
}

}  // namespace cy::vfx
