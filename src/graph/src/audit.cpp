// CyberGraph's capability and determinism audit. Task 2.1.
//
// See audit.h for why "I could not examine this node" is a third answer and not a pass.

#include <cy/graph/audit.h>

namespace cy::graph {
namespace {

constexpr Capability kAllCapabilities[] = {
    Capability::ReadWorld,     Capability::WriteWorld, Capability::SpawnEntity,
    Capability::DestroyEntity, Capability::Physics,    Capability::Audio,
    Capability::Network,       Capability::FileSystem, Capability::Randomness,
    Capability::WallClock,     Capability::NativeCall,
};

void report_missing(const GraphNode& node, Capability missing, DiagnosticSink& sink) noexcept {
    for (const Capability capability : kAllCapabilities) {
        if (!has_capability(missing, capability)) {
            continue;
        }
        Diagnostic diagnostic;
        diagnostic.severity = Severity::Error;
        diagnostic.node = node.key;
        diagnostic.detail = Name::intern(capability_name(capability));
        diagnostic.message =
            "this node needs a capability the graph was not granted; grant it deliberately or "
            "remove the node";
        sink.report(diagnostic);
    }
}

void examine_determinism(const GraphNode& node, const NodeType& type, AuditReport& report,
                         bool claims, DiagnosticSink& sink) noexcept {
    switch (type.determinism()) {
        case Determinism::Deterministic:
            return;
        case Determinism::SeedDependent:
            ++report.seed_dependent_nodes;
            // Deterministic exactly when the stream it draws from is the simulation's. The graph
            // says which by granting `Randomness`; a node that draws without the grant has already
            // been reported by the capability half of the audit.
            return;
        case Determinism::NonDeterministic:
            break;
    }
    ++report.nondeterministic_nodes;
    report.deterministic = false;
    if (!claims) {
        return;
    }
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.node = node.key;
    diagnostic.detail = type.name();
    diagnostic.message =
        "this graph claims to be deterministic and this node is not; the claim is what a "
        "simulation's replay depends on, so it is checked rather than trusted";
    sink.report(diagnostic);
}

}  // namespace

Status audit(const Graph& graph, const NodeRegistry& registry, AuditReport& report,
             DiagnosticSink& sink) noexcept {
    for (const GraphNode& node : graph.nodes()) {
        if (node.muted) {
            continue;
        }
        if (node.subgraph != Name{}) {
            // A subgraph instance is audited through the graph it names, which the caller audits
            // in its own right; the instance itself requires nothing.
            continue;
        }
        const NodeType* type = registry.find(node.type);
        if (type == nullptr) {
            ++report.nodes_opaque;
            report.complete = false;
            Diagnostic diagnostic;
            diagnostic.severity = Severity::Warning;
            diagnostic.node = node.key;
            diagnostic.detail = node.type;
            diagnostic.message =
                "the audit could not examine this node because its type is not registered; the "
                "result is INCOMPLETE rather than clean";
            sink.report(diagnostic);
            continue;
        }
        ++report.nodes_examined;
        report.required = report.required | type->required();
        const auto missing = static_cast<Capability>(static_cast<u32>(type->required()) &
                                                     ~static_cast<u32>(graph.granted()));
        if (missing != Capability::None) {
            report.missing = report.missing | missing;
            report_missing(node, missing, sink);
        }
        examine_determinism(node, *type, report, graph.claims_deterministic(), sink);
    }
    return ok();
}

}  // namespace cy::graph
