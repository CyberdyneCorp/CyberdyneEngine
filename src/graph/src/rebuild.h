#pragma once
// The rebuild: how every pass and every derivation over the expression core is expressed.
//
// One template rather than two loops, because the pipeline and a domain's own derivation do the
// same walk and differ only in what they decide per node — and a second copy of "carry the flags
// and the origins across" is exactly where an attribution quietly stops arriving.
//
// A rewrite MAY return `kInvalidNode`, which means the node is dropped. Its consumers see
// `kInvalidNode` in their mapped operand list and decide for themselves: dropping a leaf aggregate
// out of a sum is correct, and dropping the sum itself deletes everything beneath it.

#include <cy/core/base/expected.h>
#include <cy/graph/expr.h>

namespace cy::graph::detail {

/// Visit `source` in canonical order from `roots`, rewriting each node into `out`.
///
/// `mapping` is sized to `source.size()` and holds the new id of every visited node, or
/// `kInvalidNode` for a node that was dropped or never reached. `rewrite` is called as
/// `rewrite(source, id, Span<const NodeId> mapped_operands, Builder& out)`.
template <typename Rewrite>
[[nodiscard]] Status rebuild_module(const Module& source, Span<const NodeId> roots, Builder& out,
                                    Array<NodeId>& mapping, Rewrite&& rewrite) noexcept {
    mapping.clear();
    if (Status sized = mapping.resize(source.size()); !sized) {
        return sized;
    }
    for (NodeId& mapped : mapping) {
        mapped = kInvalidNode;
    }

    Array<NodeId> order(source.allocator());
    if (Status walked = canonical_order(source, roots, order); !walked) {
        return walked;
    }

    NodeId operands[kMaxOperands] = {};
    for (const NodeId id : order) {
        const Span<const NodeId> source_operands = source.operands(id);
        for (usize index = 0; index < source_operands.size(); ++index) {
            operands[index] = mapping[source_operands[index]];
        }
        auto rewritten =
            rewrite(source, id, Span<const NodeId>(operands, source_operands.size()), out);
        if (!rewritten) {
            return make_unexpected(rewritten.error());
        }
        mapping[id] = rewritten.value();
        if (rewritten.value() == kInvalidNode) {
            continue;
        }
        // The side tables travel with the value, and they MERGE: interning maps several source
        // nodes onto one rebuilt node, and an annotation on any of them has to survive.
        if (Status annotated = out.annotate(rewritten.value(), source.flags(id)); !annotated) {
            return annotated;
        }
        for (const u32 origin : source.origins(id)) {
            if (Status added = out.add_origin(rewritten.value(), origin); !added) {
                return added;
            }
        }
    }
    return ok();
}

/// The default rewrite: reconstruct the node as it stands. Used by the passes that only want the
/// canonicalisation a rebuild performs, and by the orphan carry that keeps
/// `dead_node_elimination = false` from being a placebo.
[[nodiscard]] inline Expected<NodeId, Error> rebuild_as_is(const Module& source, NodeId id,
                                                           Span<const NodeId> operands,
                                                           Builder& out) noexcept {
    for (const NodeId operand : operands) {
        if (operand == kInvalidNode) {
            return kInvalidNode;
        }
    }
    const Node& node = source.node(id);
    return out.make(node.op, node.type, node.symbol, node.value, operands);
}

}  // namespace cy::graph::detail
