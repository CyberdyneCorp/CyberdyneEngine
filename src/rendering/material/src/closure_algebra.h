#pragma once
// Closure algebra: the simplifications that make an editor's weight ports cost nothing, and the
// asymmetry that keeps a derivation from deleting a material. M7 tasks 6.1 and 6.2.
//
// Shared by the optimisation pipeline and by program derivation because they need exactly the same
// rule, and design.md §1.5 is explicit about what happens when only one of them has it:
//
//   "Derivation must drop only LEAF closures, never the combinators. Dropping a `CScale`, `CAdd`
//    or `CLayer` deletes everything beneath it. The spike's first far-field program came out with
//    zero closures for this reason."
//
// So a leaf closure may vanish, and a combinator may only collapse onto something still present.

#include <cy/core/base/expected.h>
#include <cy/rendering/material/ir.h>

namespace cy::rendering::material::detail {

/// Flatten a sum of sums. `add(add(a, b), c)` and `add(a, b, c)` are one material: an editor builds
/// the first, because a node has two inputs and an author adds a third closure with a second sum
/// node, and a text definition builds the second.
///
/// Returns the number of operands after flattening, or `live` unchanged when nothing nested.
[[nodiscard]] inline u32 flatten_closure_sum(Builder& out, NodeId* live, u32 count,
                                             bool& flattened) noexcept {
    NodeId flat[4] = {};
    u32 size = 0;
    flattened = false;
    for (u32 index = 0; index < count && size < 4; ++index) {
        const Span<const NodeId> inner = out.operands(live[index]);
        if (out.node(live[index]).op == Op::ClosureAdd && size + inner.size() <= 4) {
            for (const NodeId operand : inner) {
                flat[size++] = operand;
            }
            flattened = true;
            continue;
        }
        flat[size++] = live[index];
    }
    if (!flattened) {
        return count;
    }
    for (u32 index = 0; index < size; ++index) {
        live[index] = flat[index];
    }
    return size;
}

/// Closure algebra. Returns the node to use, or `kInvalidNode` when the closure disappears
/// entirely — which is what a weight of zero means and is why a muted closure in an editor costs
/// nothing.
///
/// NOTE THE ASYMMETRY, and it is design.md §1.5's: a LEAF closure may vanish, and a combinator may
/// only collapse to something that is still there. `ClosureAdd` with one surviving operand becomes
/// that operand; with none it vanishes, which can only happen when every leaf beneath it vanished.
[[nodiscard]] inline Expected<NodeId, Error> simplify_closure(const Node& node,
                                                              Span<const NodeId> operands,
                                                              Builder& out,
                                                              u32& simplifications) noexcept {
    NodeId live[4] = {};
    u32 count = 0;
    for (const NodeId operand : operands) {
        if (operand != kInvalidNode) {
            live[count++] = operand;
        }
    }
    switch (node.op) {
        case Op::ClosureScale: {
            if (count < 2) {
                ++simplifications;
                return kInvalidNode;
            }
            const Node& weight = out.node(live[1]);
            const bool unit = weight.op == Op::Constant && weight.value.x == 1.0F;
            const bool zero = weight.op == Op::Constant && weight.value.x == 0.0F;
            if (unit || zero) {
                ++simplifications;
                return unit ? live[0] : kInvalidNode;
            }
            break;
        }
        case Op::ClosureAdd: {
            bool flattened = false;
            count = flatten_closure_sum(out, live, count, flattened);
            simplifications += flattened ? 1U : 0U;
            if (count < 2) {
                ++simplifications;
                return count == 0 ? kInvalidNode : live[0];
            }
            break;
        }
        case Op::ClosureLayer:
            if (count < 2) {
                ++simplifications;
                return count == 0 ? kInvalidNode : live[0];
            }
            break;
        default:
            // A leaf closure whose colour was dropped cannot happen: derivation drops closures, and
            // a numeric value is never dropped out from under one.
            if (count != operands.size()) {
                return kInvalidNode;
            }
            break;
    }
    return out.make(node.op, node.type, node.symbol, node.value, Span<const NodeId>(live, count));
}

}  // namespace cy::rendering::material::detail
