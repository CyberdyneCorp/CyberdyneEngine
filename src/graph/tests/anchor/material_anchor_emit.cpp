// The material anchor's Slang target. Task 2.3.
//
// The emitted TEXT is what the program digest is a hash of, so this file is byte-for-byte the
// material compiler's emitter with the core's half — order, numbering, hoisting, the debug map —
// removed. Everything that remains is language, which is exactly the split emit.h draws.

#include "material_anchor.h"

namespace cy::graph::anchor {
namespace {

[[nodiscard]] const char* program_kind_name(u32 kind) noexcept {
    switch (kind) {
        case 0:
            return "primary";
        case 1:
            return "secondary";
        case 2:
            return "far_field";
        case 3:
            return "shadow";
        default:
            break;
    }
    return "?";
}

[[nodiscard]] const char* quality_tier_name(u32 tier) noexcept {
    switch (tier) {
        case 0:
            return "high";
        case 1:
            return "medium";
        case 2:
            return "low";
        default:
            break;
    }
    return "?";
}

void write_swizzle(TextWriter& writer, u32 mask) noexcept {
    static constexpr char kComponents[] = "xyzw";
    writer.text(".");
    const u32 count = (mask >> 28U) & 0xFU;
    for (u32 index = 0; index < count; ++index) {
        const char component[2] = {kComponents[(mask >> (index * 4U)) & 0x3U], '\0'};
        writer.text(component);
    }
}

struct CallForm {
    const char* prefix;
    const char* infix;
};

/// The two shapes an expression takes: a call `f(a, b)` or an infix `(a + b)`.
[[nodiscard]] CallForm form_of(OpId op) noexcept {
    switch (op) {
        case OpAdd:
            return {nullptr, " + "};
        case OpSub:
            return {nullptr, " - "};
        case OpMul:
            return {nullptr, " * "};
        case OpDiv:
            return {nullptr, " / "};
        case OpMin:
            return {"min", nullptr};
        case OpMax:
            return {"max", nullptr};
        case OpDot:
            return {"dot", nullptr};
        case OpPow:
            return {"pow", nullptr};
        case OpSaturate:
            return {"saturate", nullptr};
        case OpNormalize:
            return {"normalize", nullptr};
        case OpLerp:
            return {"lerp", nullptr};
        case OpDiffuse:
            return {"cy_closure_diffuse", nullptr};
        case OpSpecular:
            return {"cy_closure_specular", nullptr};
        case OpCoat:
            return {"cy_closure_coat", nullptr};
        case OpTransmission:
            return {"cy_closure_transmission", nullptr};
        case OpSubsurface:
            return {"cy_closure_subsurface", nullptr};
        case OpSheen:
            return {"cy_closure_sheen", nullptr};
        case OpEmission:
            return {"cy_closure_emission", nullptr};
        case OpClosureScale:
            return {"cy_closure_scale", nullptr};
        case OpClosureAdd:
            return {"cy_closure_add", nullptr};
        case OpClosureLayer:
            return {"cy_closure_layer", nullptr};
        default:
            return {nullptr, nullptr};
    }
}

class MaterialTarget final : public SourceTarget {
public:
    void header(TextWriter& writer, const Module& module,
                const EmitOptions& options) const override;
    void signature(TextWriter& writer, const Module& module, const EmitOptions& options,
                   bool preview) const override;
    [[nodiscard]] const char* type_spelling(const Module& module, TypeId type) const override;
    void leaf(TextWriter& writer, const Module& module, NodeId id) const override;
    void expression(const EmitView& view, NodeId id) const override;
    void assign_root(const EmitView& view, u32 slot, NodeId id, bool preview) const override;
    void epilogue(TextWriter& writer) const override { writer.text("}\n"); }

    [[nodiscard]] const char* hoist_comment() const override {
        return "    // hoisted: constant across the draw, evaluated into parameter data\n";
    }
    [[nodiscard]] const char* varying_comment() const override { return "    // per pixel\n"; }

private:
    /// A custom node's Slang text, with `$0`, `$1` ... replaced by its operands.
    static void write_custom(const EmitView& view, NodeId id) noexcept;
};

void MaterialTarget::header(TextWriter& writer, const Module& module,
                            const EmitOptions& options) const {
    writer.text("// CyberMaterial generated source. Do not edit: the material is the source.\n");
    writer.text("// material: ");
    writer.text(module.name().text());
    writer.text("\n// program: ");
    writer.text(program_kind_name(options.variant));
    writer.text("  tier: ");
    writer.text(quality_tier_name(options.tier));
    writer.text("  model: ");
    writer.text(options.annotation != nullptr ? options.annotation : "Lit");
    writer.text("\n// ir: 0x");
    writer.hex(module.digest());
    writer.text("\nimport cy.material;\n\n");
}

void MaterialTarget::signature(TextWriter& writer, const Module& module, const EmitOptions& options,
                               bool preview) const {
    writer.text("void cy_material_");
    writer.text(module.name().text());
    writer.text("_");
    writer.text(program_kind_name(options.variant));
    writer.text("_");
    writer.text(quality_tier_name(options.tier));
    writer.text(preview ? "_preview" : "");
    writer.text("(in CyMaterialContext ctx, inout CySurface surface) {\n");
}

const char* MaterialTarget::type_spelling(const Module& module, TypeId type) const {
    return type == Closure ? "CyClosure" : module.domain().type_desc(type).name;
}

void MaterialTarget::leaf(TextWriter& writer, const Module& module, NodeId id) const {
    const Node& node = module.node(id);
    switch (node.op) {
        case OpConstant: {
            const u32 components = module.domain().components(node.type);
            if (node.type == Bool) {
                writer.text(node.value.mask != 0 ? "true" : "false");
                return;
            }
            if (node.type == Int) {
                writer.number(node.value.mask);
                return;
            }
            if (components > 1) {
                writer.text(type_spelling(module, node.type));
                writer.text("(");
            }
            const f32 component[4] = {node.value.x, node.value.y, node.value.z, node.value.w};
            for (u32 index = 0; index < components; ++index) {
                if (index > 0) {
                    writer.text(", ");
                }
                writer.literal(component[index]);
            }
            if (components > 1) {
                writer.text(")");
            }
            return;
        }
        case OpParameter:
            writer.text("ctx.params.");
            writer.text(node.symbol.text());
            return;
        case OpAttribute:
            writer.text("ctx.attributes.");
            writer.text(node.symbol.text());
            return;
        case OpField:
            writer.text("cy_field_sample(ctx, CY_FIELD_");
            writer.text(node.symbol.text());
            writer.text(")");
            return;
        default:
            writer.text("<leaf?>");
            return;
    }
}

void MaterialTarget::write_custom(const EmitView& view, NodeId id) noexcept {
    const Module& module = view.module();
    const std::string_view source = module.node(id).symbol.text();
    const Span<const NodeId> operands = module.operands(id);
    for (usize index = 0; index < source.size(); ++index) {
        if (source[index] == '$' && index + 1 < source.size() && source[index + 1] >= '0' &&
            source[index + 1] <= '9') {
            const auto slot = static_cast<usize>(source[index + 1] - '0');
            if (slot < operands.size()) {
                view.operand(operands[slot]);
                ++index;
                continue;
            }
        }
        view.out().text(source.substr(index, 1));
    }
}

void MaterialTarget::expression(const EmitView& view, NodeId id) const {
    const Module& module = view.module();
    TextWriter& writer = view.out();
    const Node& node = module.node(id);
    const Span<const NodeId> operands = module.operands(id);
    const auto operand = [&view, operands](usize index) noexcept { view.operand(operands[index]); };
    switch (node.op) {
        case OpTextureSample:
            writer.text("cy_material_sample(ctx, CY_TEXTURE_");
            writer.text(node.symbol.text());
            writer.text(", ");
            operand(0);
            writer.text(")");
            return;
        case OpOneMinus:
            writer.text("(1.0 - ");
            operand(0);
            writer.text(")");
            return;
        case OpSelect:
            writer.text("(");
            operand(0);
            writer.text(" ? ");
            operand(1);
            writer.text(" : ");
            operand(2);
            writer.text(")");
            return;
        case OpSwizzle:
            operand(0);
            write_swizzle(writer, node.value.mask);
            return;
        case OpCombine:
            writer.text(type_spelling(module, node.type));
            writer.text("(");
            for (usize index = 0; index < operands.size(); ++index) {
                if (index > 0) {
                    writer.text(", ");
                }
                operand(index);
            }
            writer.text(")");
            return;
        case OpCustom:
            writer.text("(");
            write_custom(view, id);
            writer.text(")");
            return;
        default:
            break;
    }
    const CallForm form = form_of(node.op);
    if (form.infix != nullptr) {
        writer.text("(");
        operand(0);
        writer.text(form.infix);
        operand(1);
        writer.text(")");
        return;
    }
    writer.text(form.prefix != nullptr ? form.prefix : module.domain().op_desc(node.op).name);
    writer.text("(");
    for (usize index = 0; index < operands.size(); ++index) {
        if (index > 0) {
            writer.text(", ");
        }
        operand(index);
    }
    writer.text(")");
}

void MaterialTarget::assign_root(const EmitView& view, u32 slot, NodeId id, bool preview) const {
    TextWriter& writer = view.out();
    if (slot == kOpacityRoot && !preview) {
        writer.text("    surface.opacity = ");
        view.operand(id);
        writer.text(";\n");
        return;
    }
    const bool closure = view.module().node(id).type == Closure;
    writer.text(closure ? "    surface.closures = " : "    surface.preview = ");
    view.operand(id);
    writer.text(";\n");
}

}  // namespace

const SourceTarget& material_target() noexcept {
    static const MaterialTarget kTarget;
    return kTarget;
}

}  // namespace cy::graph::anchor
