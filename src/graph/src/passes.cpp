// The optimisation pipeline: a rebuild, iterated to a fixed point. Task 2.2.
//
// See passes.h for why every pass is a rebuild and why dead-node elimination is not a pass. Every
// rule that is not structural — what folds, what an aggregate simplifies to — belongs to the
// domain, and this file only decides when to ask.

#include <cy/graph/passes.h>

#include <utility>

#include "rebuild.h"

namespace cy::graph {
namespace {

struct PassState {
    const PassSwitches* switches = nullptr;
    OptimiseReport* report = nullptr;
    const Array<u8>* uniform = nullptr;
};

/// True when this value is constant across a dispatch: it depends on declarations and literals
/// only. The rule is structural — the domain's table says which ops read per-invocation data.
void mark_uniform(const Module& source, Array<u8>& uniform) noexcept {
    const Domain& domain = source.domain();
    for (NodeId id = 0; id < source.size(); ++id) {
        const Node& node = source.node(id);
        const OpDesc& desc = domain.op_desc(node.op);
        bool value = desc.uniform_leaf;
        if (!desc.varying && node.operand_count > 0) {
            value = true;
            for (const NodeId operand : source.operands(id)) {
                value = value && uniform[operand] != 0;
            }
        }
        uniform[id] = value ? 1U : 0U;
    }
}

[[nodiscard]] Expected<NodeId, Error> apply_fold(const Module& source, NodeId id,
                                                 Span<const NodeId> operands, Builder& out,
                                                 PassState& state) noexcept {
    const Node& node = source.node(id);
    const Domain& domain = out.domain();
    const FoldOutcome outcome = domain.fold(out, node.op, node.type, node.value, operands);
    switch (outcome.kind) {
        case FoldOutcome::Kind::Constant: {
            const OpId constant = domain.constant_op();
            if (constant == kInvalidOp) {
                break;
            }
            ++state.report->folded_constants;
            return out.make(constant, outcome.type, Name{}, outcome.value, {});
        }
        case FoldOutcome::Kind::Replace:
            if (outcome.node == kInvalidNode) {
                break;
            }
            ++state.report->folded_constants;
            return outcome.node;
        case FoldOutcome::Kind::None:
            break;
    }
    return kInvalidNode;
}

[[nodiscard]] Expected<NodeId, Error> rewrite(const Module& source, NodeId id,
                                              Span<const NodeId> operands, Builder& out,
                                              PassState& state) noexcept {
    const Node& node = source.node(id);
    const PassSwitches& switches = *state.switches;
    const Domain& domain = out.domain();

    if (domain.op_desc(node.op).aggregate) {
        if (!switches.aggregate_simplification) {
            return detail::rebuild_as_is(source, id, operands, out);
        }
        return domain.simplify_aggregate(node, operands, out, state.report->simplified_aggregates);
    }
    for (const NodeId operand : operands) {
        if (operand == kInvalidNode) {
            return kInvalidNode;
        }
    }

    if (switches.constant_folding && node.op != domain.constant_op()) {
        auto folded = apply_fold(source, id, operands, out, state);
        if (!folded) {
            return folded;
        }
        if (folded.value() != kInvalidNode) {
            return folded;
        }
    }

    auto made = out.make(node.op, node.type, node.symbol, node.value, operands);
    if (made && switches.uniform_varying && (*state.uniform)[id] != 0) {
        if (Status annotated = out.annotate(made.value(), kFlagUniform); !annotated) {
            return make_unexpected(annotated.error());
        }
    }
    return made;
}

/// Carry across the nodes a rebuild from the roots could not reach. Without it the
/// `dead_node_elimination` switch is a placebo.
[[nodiscard]] Status carry_orphans(const Module& source, Builder& builder, PassState& state,
                                   Array<NodeId>& mapping) noexcept {
    for (NodeId id = 0; id < source.size(); ++id) {
        if (mapping[id] != kInvalidNode) {
            continue;
        }
        NodeId operands[kMaxOperands] = {};
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
    return ok();
}

/// One pass of the pipeline: a rebuild from the declared roots, plus — when dead-node elimination
/// is off — an explicit second walk that carries the orphans across.
[[nodiscard]] Expected<Module, Error> rebuild_once(const Module& source, PassState& state,
                                                   Array<NodeId>& mapping) noexcept {
    const PassSwitches& switches = *state.switches;
    Builder builder(source.allocator(), source.domain(), source.name());
    builder.set_policy(builder_policy(switches));
    for (const Decl& decl : source.decls()) {
        if (Status declared = builder.declare(decl); !declared) {
            return make_unexpected(declared.error());
        }
    }

    const auto visit = [&state](const Module& module, NodeId id, Span<const NodeId> operands,
                                Builder& out) noexcept {
        return rewrite(module, id, operands, out, state);
    };
    if (Status rebuilt = detail::rebuild_module(source, source.roots(), builder, mapping, visit);
        !rebuilt) {
        return make_unexpected(rebuilt.error());
    }

    if (!switches.dead_node_elimination) {
        if (Status carried = carry_orphans(source, builder, state, mapping); !carried) {
            return make_unexpected(carried.error());
        }
    }

    const Span<const NodeId> roots = source.roots();
    for (usize slot = 0; slot < roots.size(); ++slot) {
        if (roots[slot] == kInvalidNode || mapping[roots[slot]] == kInvalidNode) {
            continue;
        }
        if (Status set = builder.set_root(static_cast<u32>(slot), mapping[roots[slot]]); !set) {
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
    report.duplicate_samples_removed += source.merged_samples();
    Array<u8> seen(source.allocator());
    if (!seen.resize(source.size() + 1)) {
        return;
    }
    for (u8& mark : seen) {
        mark = 0;
    }
    const Domain& domain = source.domain();
    for (NodeId id = 0; id < source.size(); ++id) {
        const NodeId mapped = mapping[id];
        if (mapped == kInvalidNode) {
            continue;
        }
        const bool duplicate = mapped < seen.size() && seen[mapped] != 0;
        if (duplicate) {
            ++report.merged_values;
            if (domain.op_desc(source.node(id).op).merge == MergeClass::Sample) {
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
    policy.intern_samples = switches.sample_dedup;
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
    if (!sample_dedup) {
        return "sample_dedup";
    }
    if (!aggregate_simplification) {
        return "aggregate_simplification";
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

    Module current(allocator, source.domain());
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
                                     "the optimisation pipeline did not reach a fixed point, which "
                                     "means two passes are undoing each other",
                                     0});
    }

    report.nodes_after = current.size();
    report.uniform_values = 0;
    for (NodeId id = 0; id < current.size(); ++id) {
        if ((current.flags(id) & kFlagUniform) != 0 && current.node(id).operand_count > 0) {
            ++report.uniform_values;
        }
    }
    return current;
}

}  // namespace cy::graph
