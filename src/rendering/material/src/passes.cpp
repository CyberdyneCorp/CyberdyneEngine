// The optimisation pipeline: a rebuild, iterated to a fixed point. M7 task 6.2.
//
// See passes.h for why every pass is a rebuild and why dead-node elimination is not a pass.

#include <cy/rendering/material/passes.h>

#include <cmath>
#include <utility>

#include "closure_algebra.h"
#include "rebuild.h"

namespace cy::rendering::material {
namespace {

/// A constant, unpacked into components so folding is written once rather than per type.
struct Scalars {
    f32 component[4] = {};
    u32 count = 1;

    [[nodiscard]] f32 at(u32 index) const noexcept {
        return count == 1 ? component[0] : component[index];
    }
};

[[nodiscard]] Scalars scalars_of(const Node& node) noexcept {
    Scalars value;
    value.count = value_type_components(node.type);
    value.component[0] = node.value.x;
    value.component[1] = node.value.y;
    value.component[2] = node.value.z;
    value.component[3] = node.value.w;
    if (node.type == ValueType::Int || node.type == ValueType::Bool) {
        value.component[0] = static_cast<f32>(node.value.mask);
    }
    return value;
}

[[nodiscard]] Immediate immediate_of(const Scalars& value) noexcept {
    return Immediate{value.component[0], value.component[1], value.component[2], value.component[3],
                     0};
}

/// Whether every operand is already a constant in the module being built.
[[nodiscard]] bool all_constant(const Builder& out, Span<const NodeId> operands) noexcept {
    for (const NodeId operand : operands) {
        if (out.node(operand).op != Op::Constant) {
            return false;
        }
    }
    return !operands.empty();
}

/// The component-wise arithmetic. Returns false for the cases folding must decline: a division by
/// zero, a `pow` of a negative base, a normalise of a zero vector. Declining is not a failure — the
/// node stays in the program and the GPU evaluates it, which is what would have happened anyway.
[[nodiscard]] bool fold_components(Op op, const Scalars* in, u32 arity, u32 components,
                                   Scalars& out) noexcept {
    out.count = components;
    for (u32 index = 0; index < components; ++index) {
        const f32 a = in[0].at(index);
        const f32 b = arity > 1 ? in[1].at(index) : 0.0F;
        switch (op) {
            case Op::Add:
                out.component[index] = a + b;
                break;
            case Op::Sub:
                out.component[index] = a - b;
                break;
            case Op::Mul:
                out.component[index] = a * b;
                break;
            case Op::Div:
                if (b == 0.0F) {
                    return false;
                }
                out.component[index] = a / b;
                break;
            case Op::Min:
                out.component[index] = a < b ? a : b;
                break;
            case Op::Max:
                out.component[index] = a > b ? a : b;
                break;
            case Op::Pow:
                if (a < 0.0F) {
                    return false;
                }
                out.component[index] = std::pow(a, b);
                break;
            case Op::Saturate:
                out.component[index] = a > 1.0F ? 1.0F : a;
                out.component[index] = out.component[index] < 0.0F ? 0.0F : out.component[index];
                break;
            case Op::OneMinus:
                out.component[index] = 1.0F - a;
                break;
            case Op::Lerp:
                out.component[index] = a + ((b - a) * in[2].at(index));
                break;
            default:
                return false;
        }
    }
    return true;
}

/// Folding for the operations whose result is not component-wise.
[[nodiscard]] bool fold_special(Op op, const Node& node, const Scalars* in, u32 arity,
                                Scalars& out) noexcept {
    switch (op) {
        case Op::Dot: {
            f32 sum = 0.0F;
            for (u32 index = 0; index < in[0].count; ++index) {
                sum += in[0].at(index) * in[1].at(index);
            }
            out.count = 1;
            out.component[0] = sum;
            return true;
        }
        case Op::Normalize: {
            f32 sum = 0.0F;
            for (u32 index = 0; index < in[0].count; ++index) {
                sum += in[0].at(index) * in[0].at(index);
            }
            if (sum <= 0.0F) {
                return false;
            }
            const f32 length = std::sqrt(sum);
            out.count = in[0].count;
            for (u32 index = 0; index < out.count; ++index) {
                out.component[index] = in[0].at(index) / length;
            }
            return true;
        }
        case Op::Swizzle: {
            out.count = (node.value.mask >> 28U) & 0xFU;
            for (u32 index = 0; index < out.count; ++index) {
                out.component[index] = in[0].at((node.value.mask >> (index * 4U)) & 0x3U);
            }
            return true;
        }
        case Op::Combine:
            out.count = arity;
            for (u32 index = 0; index < arity; ++index) {
                out.component[index] = in[index].at(0);
            }
            return true;
        default:
            return false;
    }
}

/// Fold a node whose operands are all constants. `false` means "leave it alone".
[[nodiscard]] bool fold(const Builder& out, const Node& node, Span<const NodeId> operands,
                        ValueType result, Immediate& folded) noexcept {
    Scalars in[4];
    const auto arity = static_cast<u32>(operands.size());
    for (u32 index = 0; index < arity; ++index) {
        in[index] = scalars_of(out.node(operands[index]));
    }
    Scalars value;
    const bool done = fold_special(node.op, node, in, arity, value) ||
                      fold_components(node.op, in, arity, value_type_components(result), value);
    if (!done) {
        return false;
    }
    folded = immediate_of(value);
    return true;
}

/// The identities constant folding exposes when only ONE operand is constant.
///
/// They matter for the exit criterion rather than for speed: an editor emits a weight port and a
/// scale node on every closure and a text definition does not, so `x * 1` has to disappear or the
/// two front-ends produce different programs. Only applied where the surviving operand's type is
/// already the result type — `float * float3(1,1,1)` is a widening and dropping it would change the
/// value's type.
[[nodiscard]] NodeId apply_identity(const Builder& out, Op op, Span<const NodeId> operands,
                                    ValueType result) noexcept {
    if (operands.size() != 2) {
        return kInvalidNode;
    }
    const auto is_splat = [&out, result](NodeId id, f32 wanted) noexcept {
        const Node& node = out.node(id);
        if (node.op != Op::Constant) {
            return false;
        }
        const Scalars value = scalars_of(node);
        for (u32 index = 0; index < value_type_components(result); ++index) {
            if (value.at(index) != wanted) {
                return false;
            }
        }
        return true;
    };
    const auto surviving = [&out, result](NodeId id) noexcept {
        return out.node(id).type == result ? id : kInvalidNode;
    };
    switch (op) {
        case Op::Mul:
            if (is_splat(operands[0], 1.0F)) {
                return surviving(operands[1]);
            }
            if (is_splat(operands[1], 1.0F)) {
                return surviving(operands[0]);
            }
            return kInvalidNode;
        case Op::Add:
            if (is_splat(operands[0], 0.0F)) {
                return surviving(operands[1]);
            }
            if (is_splat(operands[1], 0.0F)) {
                return surviving(operands[0]);
            }
            return kInvalidNode;
        case Op::Sub:
        case Op::Div:
            return is_splat(operands[1], op == Op::Sub ? 0.0F : 1.0F) ? surviving(operands[0])
                                                                      : kInvalidNode;
        default:
            return kInvalidNode;
    }
}

/// True when this value is constant across a draw: it depends on parameters and literals only.
void mark_uniform(const Module& source, Array<u8>& uniform) noexcept {
    for (NodeId id = 0; id < source.size(); ++id) {
        const Node& node = source.node(id);
        bool value = node.op == Op::Constant || node.op == Op::Parameter;
        if (node.op != Op::Attribute && node.op != Op::Field && node.op != Op::TextureSample &&
            node.op != Op::Custom && node.operand_count > 0) {
            value = true;
            for (const NodeId operand : source.operands(id)) {
                value = value && uniform[operand] != 0;
            }
        }
        uniform[id] = value ? 1U : 0U;
    }
}

struct PassState {
    const PassSwitches* switches = nullptr;
    OptimiseReport* report = nullptr;
    const Array<u8>* uniform = nullptr;
};

[[nodiscard]] Expected<NodeId, Error> rewrite(const Module& source, NodeId id,
                                              Span<const NodeId> operands, Builder& out,
                                              PassState& state) noexcept {
    const Node& node = source.node(id);
    const PassSwitches& switches = *state.switches;

    if (op_is_closure(node.op)) {
        if (!switches.closure_simplification) {
            return detail::rebuild_as_is(source, id, operands, out);
        }
        return detail::simplify_closure(node, operands, out, state.report->simplified_closures);
    }
    for (const NodeId operand : operands) {
        if (operand == kInvalidNode) {
            return kInvalidNode;
        }
    }

    if (switches.constant_folding && node.op != Op::Constant) {
        Immediate folded;
        if (all_constant(out, operands) && fold(out, node, operands, node.type, folded)) {
            ++state.report->folded_constants;
            return out.make(Op::Constant, node.type, Name{}, folded, {});
        }
        const NodeId identity = apply_identity(out, node.op, operands, node.type);
        if (identity != kInvalidNode) {
            ++state.report->folded_constants;
            return identity;
        }
    }

    auto made = out.make(node.op, node.type, node.symbol, node.value, operands);
    if (made && switches.uniform_varying && (*state.uniform)[id] != 0) {
        if (Status annotated = out.annotate(made.value(), NodeFlags::Uniform); !annotated) {
            return make_unexpected(annotated.error());
        }
    }
    return made;
}

/// One pass of the pipeline: a rebuild from the roots, plus — when dead-node elimination is off —
/// an explicit second walk that carries the orphans across.
[[nodiscard]] Expected<Module, Error> rebuild_once(const Module& source, PassState& state,
                                                   Array<NodeId>& mapping) noexcept {
    const PassSwitches& switches = *state.switches;
    Builder builder(source.allocator(), source.name());
    builder.set_policy(builder_policy(switches));
    for (const ParameterDecl& decl : source.parameters()) {
        if (Status declared = builder.declare_parameter(decl); !declared) {
            return make_unexpected(declared.error());
        }
    }
    for (const TextureDecl& decl : source.textures()) {
        if (Status declared = builder.declare_texture(decl); !declared) {
            return make_unexpected(declared.error());
        }
    }

    const NodeId roots[] = {source.surface(), source.opacity()};
    const auto visit = [&state](const Module& module, NodeId id, Span<const NodeId> operands,
                                Builder& out) noexcept {
        return rewrite(module, id, operands, out, state);
    };
    if (Status rebuilt =
            detail::rebuild_module(source, Span<const NodeId>(roots, 2), builder, mapping, visit);
        !rebuilt) {
        return make_unexpected(rebuilt.error());
    }

    if (!switches.dead_node_elimination) {
        // The orphan carry. Without it the switch is a placebo, because a rebuild from the roots
        // has already removed every unreachable node — design.md §1.3, and the spike's own first
        // draft made exactly this mistake.
        for (NodeId id = 0; id < source.size(); ++id) {
            if (mapping[id] != kInvalidNode) {
                continue;
            }
            NodeId operands[4] = {};
            const Span<const NodeId> source_operands = source.operands(id);
            bool reachable = true;
            for (usize index = 0; index < source_operands.size(); ++index) {
                operands[index] = mapping[source_operands[index]];
                reachable = reachable && operands[index] != kInvalidNode;
            }
            if (!reachable) {
                continue;
            }
            auto carried = rewrite(source, id, Span<const NodeId>(operands, source_operands.size()),
                                   builder, state);
            if (!carried) {
                return make_unexpected(carried.error());
            }
            mapping[id] = carried.value();
        }
    }

    if (source.surface() != kInvalidNode && mapping[source.surface()] != kInvalidNode) {
        if (Status set = builder.set_surface(mapping[source.surface()]); !set) {
            return make_unexpected(set.error());
        }
    }
    if (source.opacity() != kInvalidNode && mapping[source.opacity()] != kInvalidNode) {
        if (Status set = builder.set_opacity(mapping[source.opacity()]); !set) {
            return make_unexpected(set.error());
        }
    }
    return builder.finish();
}

/// What the first rebuild removed, by authoring node, so an editor can grey the nodes out.
[[nodiscard]] Status record_drops(const Module& source, const Array<NodeId>& mapping,
                                  OptimiseReport& report) noexcept {
    for (NodeId id = 0; id < source.size(); ++id) {
        if (mapping[id] != kInvalidNode) {
            continue;
        }
        ++report.dropped_nodes;
        for (const u32 origin : source.origins(id)) {
            bool already = false;
            for (const u32 existing : report.dropped_origins) {
                already = already || existing == origin;
            }
            if (!already) {
                if (Status pushed = report.dropped_origins.push_back(origin); !pushed) {
                    return pushed;
                }
            }
        }
    }
    return ok();
}

/// What was merged: what the FRONT-END's builder merged when it produced `source`, plus what this
/// rebuild merged. Both halves are needed — a graph that dragged one texture in twice is deduped by
/// the front-end's builder, and a pass that exposes two identical expressions is deduped here.
void count_merges(const Module& source, const Array<NodeId>& mapping,
                  OptimiseReport& report) noexcept {
    report.merged_values += source.merged_values();
    report.duplicate_samples_removed += source.merged_texture_samples();
    Array<u8> seen(source.allocator());
    if (!seen.resize(source.size() + 1)) {
        return;
    }
    for (u8& mark : seen) {
        mark = 0;
    }
    for (NodeId id = 0; id < source.size(); ++id) {
        const NodeId mapped = mapping[id];
        if (mapped == kInvalidNode) {
            continue;
        }
        const bool duplicate = mapped < seen.size() && seen[mapped] != 0;
        if (duplicate) {
            ++report.merged_values;
            if (source.node(id).op == Op::TextureSample) {
                ++report.duplicate_samples_removed;
            }
        } else if (mapped < seen.size()) {
            seen[mapped] = 1;
        }
    }
}

}  // namespace

BuilderPolicy builder_policy(const PassSwitches& switches) noexcept {
    BuilderPolicy policy;
    policy.intern = switches.interning;
    policy.canonical_commutative = switches.canonical_commutative;
    policy.intern_expressions = switches.common_subexpression;
    policy.intern_texture_samples = switches.texture_sample_dedup;
    return policy;
}

bool PassSwitches::all_enabled() const noexcept {
    return first_disabled() == nullptr;
}

const char* PassSwitches::first_disabled() const noexcept {
    if (!interning) {
        return "interning";
    }
    if (!canonical_commutative) {
        return "canonical_commutative";
    }
    if (!constant_folding) {
        return "constant_folding";
    }
    if (!common_subexpression) {
        return "common_subexpression";
    }
    if (!texture_sample_dedup) {
        return "texture_sample_dedup";
    }
    if (!closure_simplification) {
        return "closure_simplification";
    }
    if (!dead_node_elimination) {
        return "dead_node_elimination";
    }
    if (!uniform_varying) {
        return "uniform_varying";
    }
    if (!canonical_emission_order) {
        return "canonical_emission_order";
    }
    return nullptr;
}

Expected<Module, Error> optimise(const Module& source, const PassSwitches& switches,
                                 OptimiseReport& report) noexcept {
    report.bisection_build = report.bisection_build || !switches.all_enabled();
    report.nodes_before = source.size();

    Allocator& allocator = source.allocator();
    Array<u8> uniform(allocator);
    Array<NodeId> mapping(allocator);
    PassState state;
    state.switches = &switches;
    state.report = &report;
    state.uniform = &uniform;

    Module current(allocator);
    u64 previous_digest = 0;
    u32 previous_size = 0;
    for (u32 iteration = 0; iteration < kMaxOptimiseIterations; ++iteration) {
        const Module& input = iteration == 0 ? source : current;
        if (Status sized = uniform.resize(input.size()); !sized) {
            return make_unexpected(sized.error());
        }
        mark_uniform(input, uniform);

        auto rebuilt = rebuild_once(input, state, mapping);
        if (!rebuilt) {
            return rebuilt;
        }
        if (iteration == 0) {
            if (Status recorded = record_drops(source, mapping, report); !recorded) {
                return make_unexpected(recorded.error());
            }
            count_merges(source, mapping, report);
        }
        ++report.iterations;
        Module produced = std::move(rebuilt.value());
        const bool settled = report.iterations > 1 && produced.digest() == previous_digest &&
                             produced.size() == previous_size;
        previous_digest = produced.digest();
        previous_size = produced.size();
        current = std::move(produced);
        if (settled) {
            break;
        }
    }
    if (report.iterations >= kMaxOptimiseIterations && current.digest() != previous_digest) {
        return make_unexpected(Error{ErrorCode::Internal,
                                     "the material optimisation pipeline did not reach a fixed "
                                     "point, which means two passes are undoing each other",
                                     0});
    }

    report.nodes_after = current.size();
    report.uniform_values = 0;
    for (NodeId id = 0; id < current.size(); ++id) {
        if (has_flag(current.flags(id), NodeFlags::Uniform) && current.node(id).operand_count > 0) {
            ++report.uniform_values;
        }
    }
    return current;
}

}  // namespace cy::rendering::material
