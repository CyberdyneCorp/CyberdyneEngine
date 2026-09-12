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
// Every attribute is a RANGE OF ONE shared `RWStructuredBuffer<uint>` addressed in 32-bit words,
// because a structured buffer of `half` is not portable and a byte-address buffer needs a different
// spelling on every backend. The words-per-particle count is derived from the precision and the
// component count, and both it and the array's base word are written into the generated comment
// beside the accessors so a reader can check the arithmetic.
//
// ================================================================================================
// THE BINDING SET IS FIXED, AND THAT IS WHAT MAKES A DISPATCH POSSIBLE. M10 task 5.1.
// ================================================================================================
//
// One shared particle buffer rather than one buffer an attribute; the counters in words of one
// `cyVfxCounts` rather than a buffer a counter; one event ring divided into regions rather than a
// pair of buffers a channel; the parameters unpacked from words rather than laid out by the
// target's uniform rules. Every one of those is the same decision: the descriptor set a generated
// kernel binds must NOT be a function of the effect, because a set layout that changed with the
// effect could only be built by reflecting the generated module, and a reflected layout that
// drifted from this generator would bind the wrong buffer to a kernel that still compiled.
//
// `<cy/vfx/gpu_layout.h>` is the contract, and `src/vfx/gpu/` is the other party to it.

#include "emit_slang.h"

#include "access.h"

#include <cy/graph/emit.h>
#include <cy/vfx/gpu_layout.h>

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
    writer.text("(cyVfxParticles[cyVfxBase_");
    write_identifier(writer, slot.name.text());
    writer.text(" + base + ");
    writer.number(word);
    writer.text("u]");
    if (slot.precision != Precision::Float32) {
        writer.text(", ");
        writer.number(lane);
        writer.text("u");
    }
    writer.text(")");
}

void write_attribute_accessors(TextWriter& writer, const AttributeSlot& slot, u32 base_words,
                               u32 capacity) noexcept {
    const u32 words = words_per_particle(slot);
    writer.text("// `");
    writer.text(slot.name.text());
    writer.text("` at ");
    writer.text(precision_name(slot.precision));
    writer.text(": ");
    writer.number(slot.components);
    writer.text(" component(s), ");
    writer.number(slot.stride);
    writer.text(" byte(s) a particle on the CPU, ");
    writer.number(words);
    writer.text(" word(s) here.\n");
    // ONE SHARED BUFFER, NOT ONE A PIECE. `vfx-system` asks for "a single VFX simulation world
    // backed by SHARED PARTICLE MEMORY"; a buffer per attribute would also have made the binding
    // set a function of the effect, and a set layout that changed with the effect cannot be built
    // without reflecting the generated module. The base is `gpu_array_base_words` and the two are
    // required to agree — see <cy/vfx/gpu_layout.h>, which computes it for the host.
    writer.text("// array base = ");
    writer.number(base_words);
    writer.text(" word(s), capacity ");
    writer.number(capacity);
    writer.text(".\nstatic const uint cyVfxBase_");
    write_identifier(writer, slot.name.text());
    writer.text(" = ");
    writer.number(base_words);
    writer.text("u;\n");

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
            writer.text("cyVfxParticles[cyVfxBase_");
            write_identifier(writer, slot.name.text());
            writer.text(" + base + ");
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
        writer.text("    cyVfxParticles[cyVfxBase_");
        write_identifier(writer, slot.name.text());
        writer.text(" + base + ");
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

// --- The binding set. FIXED for every effect, which is what lets one descriptor set layout serve
//     every generated kernel and the fixed support dispatches at the same time. The numbers are
//     `cy::vfx::GpuBinding` in <cy/vfx/gpu_layout.h> and are spelled there once. ---
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> cyVfxParticles;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> cyVfxAlive;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> cyVfxIndices;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> cyVfxCounts;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> cyVfxFree;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> cyVfxParamWords;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> cyVfxKeys;

// The counter words, `cy::vfx::GpuCountWord`. Every one of them is written by a dispatch and read
// by a dispatch: this is what "live particle counts MAINTAINED ON THE GPU" means as addresses.
static const uint CY_VFX_COUNT_LIVE = 0u;
static const uint CY_VFX_COUNT_FREE = 1u;
static const uint CY_VFX_COUNT_SPAWN_REQUEST = 2u;
static const uint CY_VFX_COUNT_SPAWN_GRANTED = 3u;
static const uint CY_VFX_COUNT_SPAWNED = 4u;
static const uint CY_VFX_COUNT_KILLED = 5u;
static const uint CY_VFX_COUNT_REPORTED_LIVE = 6u;
static const uint CY_VFX_COUNT_SORT_PASSES = 7u;
static const uint CY_VFX_COUNT_EVENTS_BASE = 8u;
static const uint CY_VFX_EVENT_STRIDE = 256u;

// --- Per-dispatch inputs. PUSHED rather than bound, because `pass` changes between two dispatches
//     in one command buffer and a word in device memory cannot. Matches
//     `cy::vfx::GpuPushConstants` field for field. ---
struct CyVfxInput {
    float dt;
    float emitter_age;
    // Per-thread, and therefore overwritten by the entry point after it reads the block. They are
    // members so that the pushed struct and the struct a kernel body reads are one struct.
    float particle_index;
    float spawn_index;
    float normalised_age;
    uint pass;
    uint capacity;
    uint spawn_scale_fixed;
};
[[vk::push_constant]] ConstantBuffer<CyVfxInput> cyVfxPush;

// What a kernel body reads. A `static` global is per-thread in this model, which is exactly what
// `particle_index` and `spawn_index` need to be — the CPU executor sets them per particle too, and
// a uniform block could not have expressed that at all.
static CyVfxInput cyVfxInput;

static const uint CY_VFX_PASS_SPAWN = 0u;
static const uint CY_VFX_PASS_INITIALISE = 1u;
static const uint CY_VFX_PASS_UPDATE = 2u;
static const uint CY_VFX_PASS_KEYS = 3u;

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

// An event channel's record. The channel's declared maximum is enforced on the CPU side by
// `EventRouter`; the GPU-side append below is bounded by BOTH that declaration and the ring's own
// region, because a channel declared larger than the ring must not write past it.
struct CyVfxEvent {
    uint source;
    uint depth;
    float rank;
    float payload;
};
[[vk::binding(7, 0)]] RWStructuredBuffer<CyVfxEvent> cyVfxEvents;

// The kill sink the kill root assigns into. THE TWO COUNTERS ARE PART OF THE KILL, not bookkeeping
// beside it.
//
// `CY_VFX_COUNT_KILLED` rising is what makes a falling population visible at all — a kill that did
// not count would leave `StepReport::killed` reading zero while particles disappeared.
//
// `CY_VFX_COUNT_REPORTED_LIVE` falling is what makes the population the number the CPU path means
// by one. The compaction sets it to `live + granted` BEFORE this pass runs, so without this
// decrement it would report the population the sub-step STARTED with plus the spawn, and a run long
// enough for particles to expire would over-count by exactly this sub-step's kills. That is not a
// hypothetical: it is what the ninety-six-step comparison in `test_vfx_gpu_pass.cpp` caught.
//
// The subtraction is spelled as an addition of `0xFFFFFFFF` because `InterlockedAdd` is unsigned.
// It cannot underflow: only a particle the compaction counted into `live`, or one the grant counted
// into `granted`, can reach the guard above.
void cyVfxKill(uint particle) {
    if (cyVfxAlive[particle] != 0u) {
        uint ignored = 0u;
        InterlockedAdd(cyVfxCounts[CY_VFX_COUNT_KILLED], 1u, ignored);
        InterlockedAdd(cyVfxCounts[CY_VFX_COUNT_REPORTED_LIVE], 0xFFFFFFFFu, ignored);
    }
    cyVfxAlive[particle] = 0u;
}
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
            writer.text("    cyVfxCounts[CY_VFX_COUNT_SPAWN_REQUEST] = uint(max(");
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
void write_channel(TextWriter& writer, const EventChannelDecl& channel, u32 index) noexcept {
    // THE COUNTER IS A WORD OF `cyVfxCounts` AND THE RECORDS ARE A REGION OF ONE RING, not a pair
    // of buffers a piece. A binding per channel would make the binding set a function of how many
    // channels the author declared, and one set layout has to serve every effect — see the note on
    // `write_attribute_accessors`.
    writer.text("static const uint cyVfxChannel_");
    write_identifier(writer, channel.name.text());
    writer.text(" = ");
    writer.number(index);
    writer.text("u;\nvoid cyVfxRaise_");
    write_identifier(writer, channel.name.text());
    writer.text("(uint particle, float rank) {\n    uint slot = 0;\n");
    writer.text("    InterlockedAdd(cyVfxCounts[CY_VFX_COUNT_EVENTS_BASE + ");
    writer.number(index);
    writer.text("u], 1u, slot);\n");
    // BOTH BOUNDS, and the declared one first because it is the one the author reasons about.
    // "Every event channel SHALL declare a maximum events per frame [...] Exceeding either SHALL
    // drop events by a deterministic rank and report the overflow, rather than compounding" — the
    // counter keeps rising past the bound, which is what makes the overflow a number the host reads
    // rather than a silence.
    writer.text("    if (slot >= ");
    writer.number(channel.max_events_per_frame);
    writer.text("u || slot >= CY_VFX_EVENT_STRIDE) { return; }\n");
    writer.text("    CyVfxEvent raised;\n    raised.source = particle;\n");
    writer.text("    raised.depth = 0u;\n    raised.rank = rank;\n    raised.payload = rank;\n");
    writer.text("    cyVfxEvents[");
    writer.number(index);
    writer.text("u * CY_VFX_EVENT_STRIDE + slot] = raised;\n}\n");
}

/// The effect's parameter block, and the function that fills it from the bound words.
///
/// A `ConstantBuffer<CyVfxParams>` would put the members wherever the target's uniform layout rules
/// put them — 16-byte alignment for a `float3` on one target, tight packing on another — while the
/// host writes FOUR FLOATS PER PARAMETER in declaration order, which is what
/// `SimulationWorld::write_parameters` does for the CPU path. One packing for both paths means the
/// load is explicit and the two paths read the same words.
void write_parameter_block(TextWriter& writer, Span<const ParameterDecl> parameters) noexcept {
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
    // UNPACKED FROM WORDS, NOT LAID OUT BY THE COMPILER. A `ConstantBuffer<CyVfxParams>` would put
    // the members wherever the target's uniform layout rules put them — 16-byte alignment for a
    // `float3` on one target, tight packing on another — while the host writes four floats per
    // parameter in declaration order, which is what `SimulationWorld::write_parameters` does for
    // the CPU path. One packing for both paths means the load is explicit and the two paths read
    // the same words.
    writer.text("};\nstatic CyVfxParams cyVfxParams;\nvoid cyVfxLoadParams() {\n");
    if (parameters.empty()) {
        writer.text("    cyVfxParams.cyVfxUnused = 0.0;\n");
    }
    for (usize index = 0; index < parameters.size(); ++index) {
        const TypeId type = vfx_type_from_name(parameters[index].type);
        const u32 components =
            vfx_type_components(type == kInvalidType ? static_cast<TypeId>(Float) : type);
        writer.text("    cyVfxParams.");
        write_identifier(writer, parameters[index].name.text());
        writer.text(" = ");
        if (components > 1) {
            writer.text(slang_type(type));
            writer.text("(");
        }
        for (u32 component = 0; component < components; ++component) {
            if (component != 0) {
                writer.text(", ");
            }
            writer.text("asfloat(cyVfxParamWords[");
            writer.number((static_cast<u32>(index) * 4U) + component);
            writer.text("u])");
        }
        if (components > 1) {
            writer.text(")");
        }
        writer.text(";\n");
    }
    writer.text("}\n\n");
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
    write_parameter_block(writer, parameters);

    writer.text("// --- The derived attribute layout. ");
    writer.number(emitter.layout().allocated_attributes());
    writer.text(" attribute(s) allocated, ");
    writer.number(emitter.layout().elided_attributes());
    writer.text(" elided,\n// ");
    writer.number(emitter.layout().bytes_per_particle());
    writer.text(" bytes a particle. ---\n");
    const u32 capacity = emitter.capacity();
    for (const AttributeSlot& slot : emitter.layout().slots()) {
        if (slot.elided) {
            writer.text("// `");
            writer.text(slot.name.text());
            writer.text("` was written and never read: not allocated.\n");
            continue;
        }
        write_attribute_accessors(writer, slot,
                                  gpu_array_base_words(emitter.layout(), slot, capacity), capacity);
    }

    bool any_channel = false;
    // THE CHANNEL'S INDEX IS ITS POSITION IN THE SYSTEM'S DECLARATION LIST, not its position among
    // the channels this emitter happens to raise on. Two emitters of one system share the event
    // ring, and an index that counted only one emitter's channels would have them appending into
    // each other's region.
    for (usize index = 0; index < channels.size(); ++index) {
        if (!raises_on(emitter, channels[index].name)) {
            continue;
        }
        if (index >= kGpuMaxEventChannels) {
            return fail(ErrorCode::OutOfRange,
                        "vfx: an emitter raises on a channel past `kGpuMaxEventChannels`, which is "
                        "the number of regions the GPU event ring is divided into");
        }
        if (!any_channel) {
            writer.text(
                "\n// --- The event channels this emitter raises on, with their declared "
                "per-frame bounds. ---\n");
            any_channel = true;
        }
        write_channel(writer, channels[index], static_cast<u32>(index));
    }

    Array<Name> samples(out.allocator());
    Array<TypeId> sample_types(out.allocator());
    if (Status collected = collect_samples(emitter, samples, sample_types); !collected) {
        return collected;
    }
    if (!samples.empty()) {
        // DEFINED, NOT DECLARED. A forward declaration with no definition is not a translation unit
        // a shader front end can turn into a module, so the "self-contained" claim this file's
        // header makes was true only for effects that sampled nothing. The body is the same answer
        // src/vfx/README.md records for the CPU executor — "a sample of an unbound data interface
        // reads zero [...] on both paths, at the same place in each" — and a bound interface
        // replaces the definition rather than adding one.
        writer.text(
            "\n// --- One sampler per data-interface field this effect reads. Unbound here, and\n"
            "//     an unbound interface reads zero on both paths — see src/vfx/README.md. ---\n");
    }
    for (usize index = 0; index < samples.size(); ++index) {
        const TypeId type = sample_types[index];
        writer.text(slang_type(type));
        writer.text(" cyVfxSample_");
        write_identifier(writer, samples[index].text());
        writer.text("(float x) { return ");
        writer.text(slang_type(type));
        writer.text("(0);\n}\n");
    }
    writer.text("\n");
    return writer.status();
}

namespace {

/// The body every entry point starts with: the per-thread inputs and the parameter block. Shared by
/// the probe unit and the dispatch unit so the two cannot drift about what a kernel may read.
void write_entry_prologue(TextWriter& writer) noexcept {
    writer.text(
        "    cyVfxInput = cyVfxPush;\n"
        "    cyVfxLoadParams();\n");
}

/// The kernel of `emitter` that covers `stage`, written as a call. Null when the effect has no
/// graph for that stage, which is legitimate: an emitter with no Update never moves.
void write_kernel_call(TextWriter& writer, const CompiledEmitter& emitter, const VfxKernel& kernel,
                       const char* argument) noexcept {
    SlangTarget target(emitter, kernel);
    target.write_entry_name(writer);
    writer.text("(");
    writer.text(argument);
    writer.text(");\n");
}

}  // namespace

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

    // The PROBE entry point. It calls every kernel over the raw thread index with no liveness
    // check and no indirect argument, because what it proves is that the generated program COMPILES
    // and reflects — the same claim `src/rendering/material/`'s probe entry point makes for a
    // material. It is NOT the program a frame dispatches; that one is `assemble_dispatch_unit`, and
    // the two are separate so that a cook's compile check cannot be mistaken for a simulation.
    writer.text("[shader(\"compute\")]\n[numthreads(64, 1, 1)]\nvoid ");
    writer.text(kVfxKernelEntryPoint);
    writer.text("(uint3 thread : SV_DispatchThreadID) {\n");
    write_entry_prologue(writer);
    for (const VfxKernel& kernel : emitter.kernels()) {
        writer.text("    ");
        write_kernel_call(writer, emitter, kernel, "thread.x");
    }
    writer.text("}\n");
    return writer.status();
}

Status assemble_dispatch_unit(const CompiledEmitter& emitter, Span<const ParameterDecl> parameters,
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

    // WHICH KERNEL RUNS IN WHICH PASS, and why it is decided here rather than by the host.
    //
    // The compiler may have FUSED Initialise with Update into one kernel, and when it has, the
    // fused kernel is the one the spawn pass runs and the update pass has none of its own — which
    // is exactly the CPU path's arrangement, where `run_initialise` marks a fused particle `2` so
    // `run_update` does not advance it a second time in the same sub-step. A host that picked the
    // kernels would have to know whether fusion happened; `kernel_for` already knows.
    // `kernel_for` already prefers an exact-stage match over a fused one, so `update` is the plain
    // Update kernel even when a second kernel covers Update as the fused half of Initialise. The
    // only thing left to decide is whether the initialise the spawn pass runs has ALREADY advanced
    // the particle, which is what the alive marker records.
    const VfxKernel* spawn = emitter.kernel_for(Stage::Spawn);
    const VfxKernel* initialise = emitter.kernel_for(Stage::Initialise);
    const VfxKernel* update = emitter.kernel_for(Stage::Update);
    const bool fused = initialise != nullptr && initialise->covers(Stage::Update);

    writer.text(
        "\n// --- The dispatch entry point. One program, four passes, selected by the pushed\n"
        "//     `pass` word; the two that run over the population are dispatched INDIRECTLY from\n"
        "//     counts this program and the support dispatches maintain. ---\n");
    writer.text("[shader(\"compute\")]\n[numthreads(64, 1, 1)]\nvoid ");
    writer.text(kVfxKernelEntryPoint);
    writer.text("(uint3 thread : SV_DispatchThreadID) {\n");
    write_entry_prologue(writer);
    writer.text("    uint tid = thread.x;\n");

    // Pass Spawn: one thread, and the answer is a count rather than a particle.
    writer.text("    if (cyVfxInput.pass == CY_VFX_PASS_SPAWN) {\n");
    if (spawn != nullptr) {
        writer.text(
            "        if (tid != 0u) { return; }\n        cyVfxInput.particle_index = 0.0;\n");
        writer.text("        ");
        write_kernel_call(writer, emitter, *spawn, "0u");
    } else {
        // An emitter with no Spawn graph asks for nothing, and says so in the same word the
        // granting pass reads — rather than leaving the previous step's request standing.
        writer.text("        if (tid == 0u) { cyVfxCounts[CY_VFX_COUNT_SPAWN_REQUEST] = 0u; }\n");
    }
    writer.text("        return;\n    }\n");

    // Pass Initialise: indirect over what the grant pass allowed, and the slot comes off the free
    // list the compaction built in ascending order.
    writer.text("    if (cyVfxInput.pass == CY_VFX_PASS_INITIALISE) {\n");
    writer.text("        if (tid >= cyVfxCounts[CY_VFX_COUNT_SPAWN_GRANTED]) { return; }\n");
    writer.text("        uint particle = cyVfxFree[tid];\n");
    writer.text("        cyVfxInput.particle_index = float(particle);\n");
    writer.text("        cyVfxInput.spawn_index = float(tid);\n");
    writer.text("        cyVfxInput.normalised_age = 0.0;\n");
    if (initialise != nullptr) {
        writer.text("        ");
        write_kernel_call(writer, emitter, *initialise, "particle");
    }
    // ALWAYS 1, and the CPU path's `2` marker has no counterpart here on purpose.
    //
    // `run_update` walks one array doing both jobs, so a particle a FUSED initialise has just
    // advanced must be marked `2` there or the same pass would advance it twice in the sub-step it
    // was born in. The compaction that builds this path's live list runs BEFORE this pass, so a
    // particle born now is not in this sub-step's list and cannot be advanced by it: the exclusion
    // is structural rather than a flag, and a flag would be a second mechanism to keep in step.
    // `vfx_compact` still promotes a `2` it finds, so a kernel cooked before this rule is safe.
    writer.text("        cyVfxAlive[particle] = 1u;\n");
    (void)fused;
    writer.text("        uint ignored = 0u;\n");
    writer.text("        InterlockedAdd(cyVfxCounts[CY_VFX_COUNT_SPAWNED], 1u, ignored);\n");
    writer.text("        return;\n    }\n");

    // Pass Keys: the sort key, read out of the attribute layout because the layout is the effect's
    // and a fixed support dispatch cannot know it.
    writer.text("    if (cyVfxInput.pass == CY_VFX_PASS_KEYS) {\n");
    writer.text("        if (tid >= cyVfxCounts[CY_VFX_COUNT_LIVE]) { return; }\n");
    writer.text("        uint particle = cyVfxIndices[tid];\n");
    const AttributeSlot* position = emitter.layout().find(Name::intern("position"));
    if (position != nullptr && !position->elided && position->components >= 3) {
        // Distance to the camera, which `cyVfxInput.emitter_age`'s neighbours cannot carry — the
        // camera-relative offset arrives in the parameter words' reserved tail. Squared distance,
        // because a sort needs the ORDER and a square root would only cost a transcendental per
        // particle to produce the same one.
        writer.text("        float3 p = cyVfxLoad_position(particle);\n");
        writer.text("        float d = dot(p, p);\n");
        // Key ordering: far to near, so a back-to-front draw walks the array forwards. A float's
        // bit pattern is monotonic for non-negative values, so the complement of the bits of a
        // non-negative distance sorts descending as an unsigned integer.
        writer.text("        cyVfxKeys[tid] = ~asuint(d);\n");
    } else {
        writer.text(
            "        // This emitter has no `position` attribute; every key is equal and the sort\n"
            "        // is order-preserving over the compaction's ascending list.\n"
            "        cyVfxKeys[tid] = 0u;\n");
    }
    writer.text("        return;\n    }\n");

    // Pass Update: indirect over the live count, through the compacted list.
    writer.text("    if (tid >= cyVfxCounts[CY_VFX_COUNT_LIVE]) { return; }\n");
    writer.text("    uint particle = cyVfxIndices[tid];\n");
    writer.text("    cyVfxInput.particle_index = float(particle);\n");
    if (update != nullptr) {
        writer.text("    ");
        write_kernel_call(writer, emitter, *update, "particle");
    }
    writer.text("}\n");
    return writer.status();
}

}  // namespace cy::vfx
