#pragma once
// CyberGraph's capability sets and determinism audit. M8.b task 2.1.
//
// ================================================================================================
// AN AUDIT THAT CANNOT SEE A NODE MUST SAY SO
// ================================================================================================
//
// `visual-scripting` requires capability sets and determinism auditing of the shared
// infrastructure. Both are the same shape of question — "can this graph do X?" — and both have the
// same trap: a graph containing a node whose plugin is missing is a graph the audit cannot answer
// about. The honest result is then NEITHER pass NOR fail but INCOMPLETE, and
// `AuditReport::complete` is that third answer. A gate that treats "I could not look" as "nothing
// found" is a gate that goes green on the day a plugin fails to load.
//
// CAPABILITIES. A node type declares what it needs; a graph declares what it is granted; the audit
// reports, node by node, everything required and not granted. This is the mechanism behind
// `gameplay-abilities-and-effects`' and `ai-system`'s shared requirement that authored logic cannot
// reach outside what its owner allowed, and it is checkable at cook time rather than at play time.
//
// DETERMINISM. A node type declares `Deterministic`, `SeedDependent` or `NonDeterministic`, and a
// graph CLAIMS to be deterministic or not. The audit checks the claim against the nodes; a graph
// that claims determinism and reaches a wall clock is an error, and it is the error M9's
// simulation-determinism work would otherwise discover as a desync months later.

#include <cy/core/base/expected.h>
#include <cy/graph/cybergraph.h>

namespace cy::graph {

struct AuditReport {
    /// The union of what every live node requires.
    Capability required = Capability::None;
    /// What is required and not granted. Empty is the only passing answer.
    Capability missing = Capability::None;
    u32 nodes_examined = 0;
    /// Nodes the audit could not examine, because their type is not registered.
    u32 nodes_opaque = 0;
    u32 seed_dependent_nodes = 0;
    u32 nondeterministic_nodes = 0;
    /// The audited answer, as distinct from the graph's own claim.
    bool deterministic = true;
    /// False when a node could not be examined. See the note at the top of this file: this is the
    /// third answer, and it is not "pass".
    bool complete = true;

    [[nodiscard]] bool passed() const noexcept {
        return complete && missing == Capability::None &&
               (deterministic || nondeterministic_nodes == 0);
    }
};

/// Audit a graph against a registry, reporting node-precise diagnostics.
///
/// A MUTED node is skipped: it contributes nothing, and reporting it would make an author's
/// "switch this off and see" produce a diagnostic about a node that does not run.
[[nodiscard]] Status audit(const Graph& graph, const NodeRegistry& registry, AuditReport& report,
                           DiagnosticSink& sink) noexcept;

}  // namespace cy::graph
