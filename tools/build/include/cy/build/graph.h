#ifndef CY_BUILD_GRAPH_H
#define CY_BUILD_GRAPH_H
// The derivation graph: nodes, their declared inputs and outputs, and the questions a build has to
// be able to answer about them. M6 tasks 7.1 and 7.2.
//
// `build-and-packaging` — "The build is a graph of derivations": "Building SHALL be expressed as a
// directed acyclic graph of nodes, each a derivation with declared inputs and outputs … A build
// SHALL NOT be a procedural script whose correctness depends on ordering by hand."
//
// Five milestones were built by a script that is correct because it is re-run from scratch. This is
// the type that replaces it.
//
// --- WHAT A NODE DECLARES, AND WHY EACH PIECE IS SEPARATE ----------------------------------------
//
// A node names its producer, its sources (files, by project-relative logical name), its upstreams
// (other nodes, by name) and its outputs (logical names). The three input kinds are separate
// because they enter the derivation key differently:
//
//   * a SOURCE contributes its content hash — content, so an identical rewrite changes nothing;
//   * an UPSTREAM contributes that node's OUTPUT DIGEST, never its key (design.md §1.5: an edit
//     that reaches a node without changing what it produces rebuilds three nodes under deep input
//     keys and one under output digests);
//   * an OPTION contributes its name and value, and options are held sorted by name so that a
//     producer discovering them in a different order cannot miss its own cache (§1.2, E10).
//
// --- PATHS ---------------------------------------------------------------------------------------
//
// Every name in a node is PROJECT-RELATIVE. `build-and-packaging`: "Keys SHALL NOT incorporate
// timestamps, absolute paths, hostnames, or environment state that varies between machines." The
// graph refuses an absolute name at `add()` rather than at key time, because the cache that a stray
// absolute path breaks is the shared one, and the failure is silent — E5 measured it: every node
// with a file input misses while early cutoff spares the nodes below them, so the shared cache
// becomes dead weight without a single error being reported.
//
// --- ORDER IS DETERMINISTIC --------------------------------------------------------------------
//
// Nodes keep their insertion order and the topological sort is stable within it, so two runs over
// one description produce the same order, the same keys and the same artefacts. A graph that
// iterated a hash table would make the *build* non-deterministic while every individual node stayed
// deterministic, which is the hardest version of this bug to find.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cy::build {

/// A node's index in the graph. Stable for the graph's lifetime; not stable across graphs, which is
/// why every persisted reference is by name.
enum class NodeId : u32 { Invalid = 0xFFFFFFFFU };

[[nodiscard]] constexpr u32 node_index(NodeId id) noexcept {
    return static_cast<u32>(id);
}

/// What kind of work a node is. A label, and a contribution to the key: two producers of different
/// kinds over one source cannot collide.
///
/// Persistent: the numbers appear in reports and manifests, so an enumerator is appended and never
/// renumbered.
enum class NodeKind : u16 {
    Unknown = 0,
    /// Generating code — reflection metadata, bindings.
    Generate = 1,
    /// Importing a source asset into the engine's own form.
    Import = 2,
    /// Cooking for a target: a world cell, a package payload.
    Cook = 3,
    /// Compiling a shader or building a pipeline state.
    Shader = 4,
    /// Assembling a package or an install bundle.
    Package = 5,
    /// Producing a manifest, a report or a patch.
    Manifest = 6,
};

[[nodiscard]] const char* node_kind_name(NodeKind kind) noexcept;
[[nodiscard]] Expected<NodeKind, Error> node_kind_from_name(std::string_view name) noexcept;

/// One declared setting. Held sorted by name inside a node.
struct NodeOption {
    std::string name;
    std::string value;
};

/// What a node declares before it runs. Everything the key needs, and nothing the key must not see.
struct NodeDesc {
    NodeKind kind = NodeKind::Unknown;
    /// The node's stable name, unique in the graph: "import:assets/lamppost.gltf". It is what a
    /// report, an invalidation and a patch manifest refer to, and it never contains a path outside
    /// the project.
    std::string name;
    /// The producer that runs this node, resolved through the producer registry.
    std::string producer;
    /// The producer's version, as the registry declares it. Held on the node so that the key is
    /// computable without the registry — a remote worker keys the node it was handed.
    u32 producer_version = 0;
    /// The target platform, and the cook profile. Two of the six contributions
    /// `build-and-packaging` names by hand.
    std::string platform = "host";
    std::string profile = "client";
    /// Files this node reads, by project-relative logical name.
    std::vector<std::string> sources;
    /// Nodes this node consumes, by name. Their OUTPUT digests enter the key.
    std::vector<std::string> upstreams;
    /// What this node produces, by logical name. A node writing outside them is reported.
    std::vector<std::string> outputs;
    /// Declared settings. Sorted by name when the node is added.
    std::vector<NodeOption> options;
    /// Whether this node may run on a remote worker. Some tools are licensed or platform-bound, so
    /// `build-and-packaging` makes it a per-kind declaration rather than an assumption.
    bool distributable = true;
    /// The install bundle this node's outputs belong to. Empty means the base bundle.
    std::string bundle;

    /// Look an option up by name. Absent options answer `fallback`, which is how a producer reads a
    /// setting it did not require.
    [[nodiscard]] std::string_view option(std::string_view key,
                                          std::string_view fallback = {}) const noexcept;
};

/// The declarations, plus the resolved edges. Immutable once `finalize()` has run.
///
/// Not thread-safe for mutation; freely readable from many threads once finalised, which is what
/// the build service relies on when it runs independent nodes concurrently.
class BuildGraph {
public:
    BuildGraph() = default;

    /// Add a node. Fails with `AlreadyExists` on a duplicate name, `InvalidArgument` on an empty
    /// name, an empty producer, an absolute path anywhere in it, or a node that declares no output.
    ///
    /// Options are sorted by name here — once, at declaration — so that a producer building its
    /// options in discovery order cannot produce two keys for one configuration.
    [[nodiscard]] Expected<NodeId, Error> add(NodeDesc desc);

    /// Resolve upstream names to ids and check the graph is acyclic. Fails with `NotFound` naming
    /// the missing upstream, or `InvalidArgument` naming a node on a cycle.
    [[nodiscard]] Status finalize();

    [[nodiscard]] bool is_finalized() const noexcept { return finalized_; }
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(nodes_.size()); }

    [[nodiscard]] const NodeDesc& node(NodeId id) const noexcept { return nodes_[node_index(id)]; }
    [[nodiscard]] NodeId find(std::string_view name) const noexcept;

    /// The nodes this node consumes, resolved. Empty before `finalize()`.
    [[nodiscard]] const std::vector<NodeId>& upstreams(NodeId id) const noexcept {
        return upstreams_[node_index(id)];
    }
    /// The nodes that consume this node directly.
    [[nodiscard]] const std::vector<NodeId>& downstreams(NodeId id) const noexcept {
        return downstreams_[node_index(id)];
    }

    /// Evaluation order: every node after its upstreams, stable within insertion order.
    [[nodiscard]] const std::vector<NodeId>& order() const noexcept { return order_; }

    /// Every node reachable downstream of `id`, `id` excluded, in evaluation order.
    ///
    /// This is the set a change to `id` CAN reach. What it actually invalidates is smaller, because
    /// early cutoff stops propagation where an output digest did not change; the build report says
    /// which. Both numbers matter: this one bounds the blast radius, and the report measures it.
    [[nodiscard]] std::vector<NodeId> dependents(NodeId id) const;

    /// Every node that declares `source` as an input, plus everything downstream of them.
    /// "Which nodes does editing this file reach?", which is half of precise invalidation.
    [[nodiscard]] std::vector<NodeId> dependents_of_source(std::string_view source) const;

    /// The chain of nodes from a root to `id` — "why is this in the build?". Empty when `id` is
    /// itself a root. `build-and-packaging` makes this a first-class question rather than a report,
    /// because it is how a team controls build size.
    [[nodiscard]] std::vector<NodeId> reference_chain(NodeId id) const;

    /// The nodes nothing consumes: what a build produces for delivery.
    [[nodiscard]] std::vector<NodeId> roots() const;

private:
    [[nodiscard]] Status resolve_edges();
    [[nodiscard]] Status compute_order();

    std::vector<NodeDesc> nodes_;
    std::unordered_map<std::string, u32> by_name_;
    std::vector<std::vector<NodeId>> upstreams_;
    std::vector<std::vector<NodeId>> downstreams_;
    std::vector<NodeId> order_;
    bool finalized_ = false;
};

/// Whether a logical name is acceptable in a node: non-empty, relative, no `..` segment, no
/// backslash, no drive letter.
///
/// Exposed because the description reader and the producer registry both check it, and two copies
/// of this rule would eventually disagree about what a project-relative path is — which is exactly
/// the "resolved differently on CI than on a developer's machine" failure design.md §1.4 names.
[[nodiscard]] bool is_project_relative(std::string_view name) noexcept;

}  // namespace cy::build

#endif  // CY_BUILD_GRAPH_H
