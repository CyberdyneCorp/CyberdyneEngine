// Slang emission. M7 tasks 6.1 and 6.4. See emit.h for why there is exactly one emitter.

#include <cy/rendering/material/emit.h>

#include <cstdio>
#include <utility>

namespace cy::rendering::material {
namespace {

/// Appends to an `Array<char>`, carrying one status rather than checking every call site.
class Writer {
public:
    explicit Writer(Array<char>& out) noexcept : out_(&out) {}

    void text(std::string_view value) noexcept {
        if (!status_) {
            return;
        }
        for (const char character : value) {
            if (Status pushed = out_->push_back(character); !pushed) {
                status_ = pushed;
                return;
            }
        }
    }

    void number(u32 value) noexcept {
        char buffer[16] = {};
        (void)std::snprintf(buffer, sizeof(buffer), "%u", value);
        text(buffer);
    }

    void hex(u64 value) noexcept {
        char buffer[24] = {};
        (void)std::snprintf(buffer, sizeof(buffer), "%016llx",
                            static_cast<unsigned long long>(value));
        text(buffer);
    }

    /// A float literal that two compilers spell identically. `%.9g` round-trips a `float` exactly,
    /// and the trailing `.0` keeps an integral value a float in Slang rather than an int.
    void literal(f32 value) noexcept {
        char buffer[32] = {};
        (void)std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
        text(buffer);
        bool fractional = false;
        for (const char character : buffer) {
            fractional = fractional || character == '.' || character == 'e' || character == 'n';
        }
        if (!fractional) {
            text(".0");
        }
    }

    [[nodiscard]] Status status() const noexcept { return status_; }
    [[nodiscard]] usize size() const noexcept { return out_->size(); }

private:
    Array<char>* out_;
    Status status_ = ok();
};

[[nodiscard]] bool is_statement(Op op) noexcept {
    return op != Op::Constant && op != Op::Parameter && op != Op::Attribute && op != Op::Field;
}

/// The Slang spelling of a value's type.
[[nodiscard]] const char* slang_type(ValueType type) noexcept {
    return type == ValueType::Closure ? "CyClosure" : value_type_name(type);
}

struct Names {
    /// `statement_index[id]` is the SSA number of a node that gets a statement, or `kInvalidNode`.
    Array<u32> statement_index;
};

void write_swizzle(Writer& writer, u32 mask) noexcept {
    static constexpr char kComponents[] = "xyzw";
    writer.text(".");
    const u32 count = (mask >> 28U) & 0xFU;
    for (u32 index = 0; index < count; ++index) {
        const char component[2] = {kComponents[(mask >> (index * 4U)) & 0x3U], '\0'};
        writer.text(component);
    }
}

/// The inline spelling of a leaf.
void write_leaf(Writer& writer, const Node& node) noexcept {
    switch (node.op) {
        case Op::Constant: {
            const u32 components = value_type_components(node.type);
            if (node.type == ValueType::Bool) {
                writer.text(node.value.mask != 0 ? "true" : "false");
                return;
            }
            if (node.type == ValueType::Int) {
                writer.number(node.value.mask);
                return;
            }
            if (components > 1) {
                writer.text(slang_type(node.type));
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
        case Op::Parameter:
            writer.text("ctx.params.");
            writer.text(node.symbol.text());
            return;
        case Op::Attribute:
            writer.text("ctx.attributes.");
            writer.text(node.symbol.text());
            return;
        case Op::Field:
            writer.text("cy_field_sample(ctx, CY_FIELD_");
            writer.text(node.symbol.text());
            writer.text(")");
            return;
        default:
            writer.text("<leaf?>");
            return;
    }
}

void write_operand(Writer& writer, const Module& module, const Names& names, NodeId id) noexcept {
    const Node& node = module.node(id);
    if (!is_statement(node.op)) {
        write_leaf(writer, node);
        return;
    }
    writer.text("v");
    writer.number(names.statement_index[id]);
}

/// A custom node's Slang text, with `$0`, `$1` … replaced by its operands.
void write_custom(Writer& writer, const Module& module, const Names& names, NodeId id) noexcept {
    const std::string_view source = module.node(id).symbol.text();
    const Span<const NodeId> operands = module.operands(id);
    for (usize index = 0; index < source.size(); ++index) {
        if (source[index] == '$' && index + 1 < source.size() && source[index + 1] >= '0' &&
            source[index + 1] <= '9') {
            const auto slot = static_cast<usize>(source[index + 1] - '0');
            if (slot < operands.size()) {
                write_operand(writer, module, names, operands[slot]);
                ++index;
                continue;
            }
        }
        writer.text(source.substr(index, 1));
    }
}

struct CallForm {
    const char* prefix;
    const char* infix;
};

/// The two shapes an expression takes: a call `f(a, b)` or an infix `(a + b)`.
[[nodiscard]] CallForm form_of(Op op) noexcept {
    switch (op) {
        case Op::Add:
            return {nullptr, " + "};
        case Op::Sub:
            return {nullptr, " - "};
        case Op::Mul:
            return {nullptr, " * "};
        case Op::Div:
            return {nullptr, " / "};
        case Op::Min:
            return {"min", nullptr};
        case Op::Max:
            return {"max", nullptr};
        case Op::Dot:
            return {"dot", nullptr};
        case Op::Pow:
            return {"pow", nullptr};
        case Op::Saturate:
            return {"saturate", nullptr};
        case Op::Normalize:
            return {"normalize", nullptr};
        case Op::Lerp:
            return {"lerp", nullptr};
        case Op::Diffuse:
            return {"cy_closure_diffuse", nullptr};
        case Op::Specular:
            return {"cy_closure_specular", nullptr};
        case Op::Coat:
            return {"cy_closure_coat", nullptr};
        case Op::Transmission:
            return {"cy_closure_transmission", nullptr};
        case Op::Subsurface:
            return {"cy_closure_subsurface", nullptr};
        case Op::Sheen:
            return {"cy_closure_sheen", nullptr};
        case Op::Emission:
            return {"cy_closure_emission", nullptr};
        case Op::ClosureScale:
            return {"cy_closure_scale", nullptr};
        case Op::ClosureAdd:
            return {"cy_closure_add", nullptr};
        case Op::ClosureLayer:
            return {"cy_closure_layer", nullptr};
        default:
            return {nullptr, nullptr};
    }
}

void write_expression(Writer& writer, const Module& module, const Names& names,
                      NodeId id) noexcept {
    const Node& node = module.node(id);
    const Span<const NodeId> operands = module.operands(id);
    const auto operand = [&](usize index) noexcept {
        write_operand(writer, module, names, operands[index]);
    };
    switch (node.op) {
        case Op::TextureSample:
            writer.text("cy_material_sample(ctx, CY_TEXTURE_");
            writer.text(node.symbol.text());
            writer.text(", ");
            operand(0);
            writer.text(")");
            return;
        case Op::OneMinus:
            writer.text("(1.0 - ");
            operand(0);
            writer.text(")");
            return;
        case Op::Select:
            writer.text("(");
            operand(0);
            writer.text(" ? ");
            operand(1);
            writer.text(" : ");
            operand(2);
            writer.text(")");
            return;
        case Op::Swizzle:
            operand(0);
            write_swizzle(writer, node.value.mask);
            return;
        case Op::Combine:
            writer.text(slang_type(node.type));
            writer.text("(");
            for (usize index = 0; index < operands.size(); ++index) {
                if (index > 0) {
                    writer.text(", ");
                }
                operand(index);
            }
            writer.text(")");
            return;
        case Op::Custom:
            writer.text("(");
            write_custom(writer, module, names, id);
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
    writer.text(form.prefix != nullptr ? form.prefix : op_name(node.op));
    writer.text("(");
    for (usize index = 0; index < operands.size(); ++index) {
        if (index > 0) {
            writer.text(", ");
        }
        operand(index);
    }
    writer.text(")");
}

void count_node(const Node& node, GeneratedSource& source) noexcept {
    if (node.op == Op::TextureSample) {
        ++source.texture_samples;
    } else if (op_is_closure(node.op)) {
        ++source.closures;
    } else if (node.op == Op::Select) {
        ++source.branches;
    } else {
        ++source.arithmetic;
    }
}

/// Sort one segment of the emitted order so that the hoistable values come first. A STABLE
/// partition, so the relative order within each half is the canonical order unchanged.
[[nodiscard]] Status partition_uniform(const Module& module, const EmitOptions& options,
                                       Array<NodeId>& order, u32& hoisted) noexcept {
    if (!options.hoist_uniform) {
        return ok();
    }
    Array<NodeId> partitioned(module.allocator());
    if (Status reserved = partitioned.reserve(order.size()); !reserved) {
        return reserved;
    }
    for (u32 pass = 0; pass < 2U; ++pass) {
        for (const NodeId id : order) {
            const bool uniform = has_flag(module.flags(id), NodeFlags::Uniform);
            if ((pass == 0) == uniform) {
                if (Status pushed = partitioned.push_back(id); !pushed) {
                    return pushed;
                }
                hoisted += pass == 0 && is_statement(module.node(id).op) ? 1U : 0U;
            }
        }
    }
    order = std::move(partitioned);
    return ok();
}

/// The emitted order, in TWO SEGMENTS: everything the surface root reaches, then whatever only the
/// opacity root reaches.
///
/// The segmentation is what makes task 6.4's first claim exact. A single walk over both roots
/// would let an opacity-only value be hoisted ahead of a surface value, which shifts every SSA
/// number after it — and the preview of the surface root would then no longer be the primary
/// program's body with the opacity assignment removed. Segmenting costs one extra walk and buys a
/// property a test can state byte for byte.
[[nodiscard]] Status build_order(const Module& module, const EmitOptions& options,
                                 Span<const NodeId> roots, Array<NodeId>& order,
                                 u32& hoisted) noexcept {
    hoisted = 0;
    order.clear();
    Array<NodeId> segment(module.allocator());
    Array<u8> emitted(module.allocator());
    if (Status sized = emitted.resize(module.size()); !sized) {
        return sized;
    }
    for (u8& mark : emitted) {
        mark = 0;
    }
    for (const NodeId root : roots) {
        if (root == kInvalidNode) {
            continue;
        }
        if (Status walked = canonical_order(module, Span<const NodeId>(&root, 1), segment);
            !walked) {
            return walked;
        }
        // Values an earlier segment already emitted keep their number and are not repeated.
        usize kept = 0;
        for (usize index = 0; index < segment.size(); ++index) {
            if (emitted[segment[index]] == 0) {
                emitted[segment[index]] = 1;
                segment[kept++] = segment[index];
            }
        }
        while (segment.size() > kept) {
            segment.pop_back();
        }
        if (Status partitioned = partition_uniform(module, options, segment, hoisted);
            !partitioned) {
            return partitioned;
        }
        if (Status appended = order.append(segment.span()); !appended) {
            return appended;
        }
    }
    return ok();
}

/// Number by node id rather than by emitted position. The `canonical_emission_order` switch off,
/// and the reason it changes the program.
void order_by_node_id(Array<NodeId>& order) noexcept {
    for (usize outer = 1; outer < order.size(); ++outer) {
        for (usize inner = outer; inner > 0 && order[inner - 1] > order[inner]; --inner) {
            const NodeId swap = order[inner - 1];
            order[inner - 1] = order[inner];
            order[inner] = swap;
        }
    }
}

void write_header(Writer& writer, const Module& module, const EmitOptions& options) noexcept {
    writer.text("// CyberMaterial generated source. Do not edit: the material is the source.\n");
    writer.text("// material: ");
    writer.text(module.name().text());
    writer.text("\n// program: ");
    writer.text(program_kind_name(options.kind));
    writer.text("  tier: ");
    writer.text(quality_tier_name(options.tier));
    writer.text("  model: ");
    writer.text(options.shading_model);
    writer.text("\n// ir: 0x");
    writer.hex(module.digest());
    writer.text("\nimport cy.material;\n\n");
}

}  // namespace

const char* program_kind_name(ProgramKind kind) noexcept {
    switch (kind) {
        case ProgramKind::Primary:
            return "primary";
        case ProgramKind::Secondary:
            return "secondary";
        case ProgramKind::FarField:
            return "far_field";
        case ProgramKind::Shadow:
            return "shadow";
        case ProgramKind::Count:
            break;
    }
    return "?";
}

const char* quality_tier_name(QualityTier tier) noexcept {
    switch (tier) {
        case QualityTier::High:
            return "high";
        case QualityTier::Medium:
            return "medium";
        case QualityTier::Low:
            return "low";
        case QualityTier::Count:
            break;
    }
    return "?";
}

Status entry_point_name(Name material, ProgramKind kind, QualityTier tier,
                        Array<char>& out) noexcept {
    Writer writer(out);
    writer.text("cy_material_");
    writer.text(material.text());
    writer.text("_");
    writer.text(program_kind_name(kind));
    writer.text("_");
    writer.text(quality_tier_name(tier));
    if (!writer.status()) {
        return fail(ErrorCode::OutOfMemory, "the entry point's name could not be grown");
    }
    return ok();
}

namespace {

/// Write the function, from its signature to its closing brace.
///
/// A FUNCTION OF ITS OWN so that the `Writer` — which holds a pointer into `source.text` — cannot
/// outlive any early return out of `emit_program`. The static analyser is right to object to a
/// pointer into a local that is about to be returned by value, and splitting the two also keeps
/// each half short enough to read.
[[nodiscard]] Status write_program(const Module& module, const EmitOptions& options,
                                   Span<const NodeId> order, Names& names,
                                   GeneratedSource& source) noexcept {
    Writer writer(source.text);
    const bool preview = options.preview_root != kInvalidNode;
    write_header(writer, module, options);
    writer.text("void ");
    Array<char> entry(module.allocator());
    if (Status named = entry_point_name(module.name(), options.kind, options.tier, entry); !named) {
        return named;
    }
    writer.text(std::string_view(entry.data(), entry.size()));
    writer.text(preview ? "_preview" : "");
    writer.text("(in CyMaterialContext ctx, inout CySurface surface) {\n");
    source.body_begin = writer.size();

    u32 next = 0;
    bool hoist_marked = false;
    bool varying_marked = false;
    for (const NodeId id : order) {
        const Node& node = module.node(id);
        if (!is_statement(node.op)) {
            continue;
        }
        if (options.hoist_uniform) {
            const bool uniform = has_flag(module.flags(id), NodeFlags::Uniform);
            if (uniform && !hoist_marked) {
                writer.text(
                    "    // hoisted: constant across the draw, evaluated into parameter data\n");
                hoist_marked = true;
            }
            if (!uniform && hoist_marked && !varying_marked) {
                writer.text("    // per pixel\n");
                varying_marked = true;
            }
        }
        names.statement_index[id] = next;
        if (Status pushed = source.value_nodes.push_back(id); !pushed) {
            return pushed;
        }
        writer.text("    ");
        writer.text(slang_type(node.type));
        writer.text(" v");
        writer.number(next);
        writer.text(" = ");
        write_expression(writer, module, names, id);
        writer.text(";\n");
        count_node(node, source);
        ++next;
        ++source.statements;
    }
    source.peak_live_values = next;

    const NodeId surface_root = preview ? options.preview_root : module.surface();
    if (surface_root != kInvalidNode) {
        const bool closure = module.node(surface_root).type == ValueType::Closure;
        writer.text(closure ? "    surface.closures = " : "    surface.preview = ");
        write_operand(writer, module, names, surface_root);
        writer.text(";\n");
    }
    if (!preview && module.opacity() != kInvalidNode) {
        writer.text("    surface.opacity = ");
        write_operand(writer, module, names, module.opacity());
        writer.text(";\n");
    }
    source.body_end = writer.size();
    writer.text("}\n");
    // A FRESH ERROR RATHER THAN THE WRITER'S. `Writer` holds a pointer into `source.text`, and
    // propagating a `Status` out of it makes the static analyser believe an address of this
    // function's frame travels with the error and reaches the caller. It does not — every message
    // in this file is a string literal — but the analyser cannot see that, and re-raising the one
    // failure a writer has is both shorter to read and free of the question.
    if (!writer.status()) {
        return fail(ErrorCode::OutOfMemory, "the generated material source could not be grown");
    }
    return ok();
}

}  // namespace

Expected<GeneratedSource, Error> emit_program(const Module& module,
                                              const EmitOptions& options) noexcept {
    Allocator& allocator = module.allocator();
    const bool preview = options.preview_root != kInvalidNode;
    const NodeId roots[] = {preview ? options.preview_root : module.surface(),
                            preview ? kInvalidNode : module.opacity()};

    GeneratedSource source(allocator);
    Array<NodeId> order(allocator);
    if (Status built = build_order(module, options, Span<const NodeId>(roots, 2), order,
                                   source.hoisted_statements);
        !built) {
        return make_unexpected(built.error());
    }
    if (!options.canonical_order) {
        order_by_node_id(order);
    }

    Names names;
    if (Status sized = names.statement_index.resize(module.size()); !sized) {
        return make_unexpected(sized.error());
    }
    for (u32& number : names.statement_index) {
        number = kInvalidNode;
    }

    if (Status written = write_program(module, options, order.span(), names, source); !written) {
        return make_unexpected(written.error());
    }
    source.digest = hash_bytes(kHashSeed, source.text.data(), source.text.size());
    return source;
}

}  // namespace cy::rendering::material
