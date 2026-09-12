#pragma once
// The authoring graph: nodes, their declared inputs, their declared reach, their execution domains
// and their iteration policy. M10 task 4.1.
//
// `procedural-content-generation` — "Graphs compile to programs": "Generators SHALL be authored as
// graphs and compiled through a typed intermediate representation into an executable program", and
// "Execution SHALL run the compiled program. Interpreted graphs of virtual node objects SHALL NOT
// be executed in hot paths."
//
// NOTHING IN THIS FILE EXECUTES, AND NOTHING BELOW THE COMPILER INCLUDES IT. A `Graph` is authoring
// data: a flat array of nodes, an edge list, an attribute table and the strings a subgraph
// instantiation owns. `program.h` reads it once through `compile()`, which takes it by forward
// declaration, and the evaluator in `execute.h` cannot name the type — which is how "the authoring
// graph SHALL not be traversed" is a property of the include graph rather than of a reviewer's
// attention. The vocabulary the two layers share is in `ops.h`.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/pcg/dataset.h>
#include <cy/pcg/identity.h>
#include <cy/pcg/ops.h>

namespace cy::pcg {

/// One authored node.
struct GraphNode {
    /// The authored name. It is what identity derives from and what every diagnostic prints, so it
    /// must be unique within the graph and stable across edits — `program.h` refuses a duplicate.
    /// A literal the caller owns for the graph's lifetime, like `environment::FieldDeclaration`'s.
    const char* name = "";
    NodeKind kind = NodeKind::Constant;

    NodeId inputs[kMaxNodeInputs];
    u8 input_count = 0;

    /// The attribute this node writes: a raster channel for a raster kind, a point column for
    /// `Compute` and `Filter` over points. Assigned by the caller through the graph's own
    /// `AttributeTable`.
    AttributeId output;
    /// The attribute a point-shaped node reads. Zero where the node reads a raster instead.
    AttributeId reads;

    NeighbourAccess neighbour_access = NeighbourAccess::None;
    /// Regions, not metres. `procedural-content-generation` — "Dependencies and spatial
    /// invalidation": "Generators SHALL declare their inputs, outputs, and sampling radius, so that
    /// the effect of a change is computable rather than guessed."
    u8 reach_regions = 0;

    IterationPolicy iteration = IterationPolicy::None;
    /// Sweeps. A refusal point under `Convergence`, a budget under `Budget`. Zero with an iterative
    /// policy is refused: "Every iterative node SHALL declare a bound, so that generation cannot
    /// fail to terminate."
    u32 iteration_bound = 0;

    /// Only `Scatter` mints identity, and only `Derived` is accepted.
    IdentitySource identity_source = IdentitySource::Derived;

    /// Whether this node is eligible for GPU execution, as the AUTHOR believes. The compiler
    /// classifies independently and records a disagreement; see `program.h`'s `CompileReport`.
    bool gpu_hint = false;

    NodeParams params;
};

/// Typed parameters a subgraph exposes. `procedural-content-generation`: "Graphs SHALL support
/// subgraphs with typed exposed parameters, so that large generators remain composable."
struct SubgraphParam {
    const char* name = "";
    AttributeType type = AttributeType::F32;
    /// The node inside the subgraph whose `params.value` this parameter drives.
    NodeId target;
    f32 value = 0.0F;
};

/// An authored generator.
class Graph {
public:
    explicit Graph(Allocator& allocator) noexcept
        : nodes_(allocator), params_(allocator), attributes_(allocator) {}

    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;
    Graph(Graph&&) noexcept = default;
    Graph& operator=(Graph&&) noexcept = default;

    /// The generator's own name and version. The version participates in every derivation key and
    /// in the network version check, so bumping it is what makes a rule change regenerate rather
    /// than serve a stale cache.
    void declare(const char* name, u32 version) noexcept {
        name_ = name;
        version_ = version;
    }
    [[nodiscard]] const char* name() const noexcept { return name_; }
    [[nodiscard]] u32 version() const noexcept { return version_; }

    void declare_domains(DomainMask domains) noexcept { domains_ = domains; }
    [[nodiscard]] DomainMask domains() const noexcept { return domains_; }

    void declare_budget(const GenerationBudget& budget) noexcept { budget_ = budget; }
    [[nodiscard]] const GenerationBudget& budget() const noexcept { return budget_; }

    /// The generator's determinism level. `Gameplay` is the safe default: a generator that forgot
    /// to declare one must not be the one that silently stops being reproducible.
    void declare_determinism(DeterminismLevel level) noexcept { determinism_ = level; }
    [[nodiscard]] DeterminismLevel determinism() const noexcept { return determinism_; }

    /// The region edge in metres. `procedural-content-generation` — "Generation regions": "Region
    /// size SHALL be a per-generator property. It SHALL NOT be required to match world partition
    /// cell size, since a road network's domain and a grass patch's are not the same scale."
    void declare_region_size(f64 metres, u8 level) noexcept {
        region_metres_ = metres;
        region_level_ = level;
    }
    [[nodiscard]] f64 region_metres() const noexcept { return region_metres_; }
    [[nodiscard]] u8 region_level() const noexcept { return region_level_; }

    [[nodiscard]] AttributeTable& attributes() noexcept { return attributes_; }
    [[nodiscard]] const AttributeTable& attributes() const noexcept { return attributes_; }

    [[nodiscard]] Expected<NodeId, Error> add(const GraphNode& node) noexcept;

    /// Inline a subgraph, binding its exposed parameters. The nodes are copied in with their names
    /// PREFIXED by `instance`, so that two instantiations of one subgraph are two sets of nodes
    /// with distinct identities — instances generated by each must not collide, and identity
    /// derives from the name.
    ///
    /// The prefixed names are owned by this graph, which is the one place in this module that owns
    /// a string: an inlined name cannot be a literal in anyone's translation unit.
    [[nodiscard]] Status instantiate(const Graph& subgraph, const char* instance,
                                     Span<const SubgraphParam> bindings) noexcept;

    [[nodiscard]] Span<const GraphNode> nodes() const noexcept { return nodes_.span(); }
    [[nodiscard]] const GraphNode* find(NodeId id) const noexcept;
    [[nodiscard]] const GraphNode* find_by_name(const char* name) const noexcept;
    [[nodiscard]] usize size() const noexcept { return nodes_.size(); }

private:
    [[nodiscard]] Expected<const char*, Error> own_name(const char* instance,
                                                        const char* name) noexcept;

    Array<GraphNode> nodes_;
    /// Owned, prefixed names produced by `instantiate()`. A stable arena: `Array<char>` would
    /// reallocate and invalidate every `const char*` handed out, so each name is its own
    /// allocation.
    Array<Array<char>> params_;
    AttributeTable attributes_;

    const char* name_ = "";
    u32 version_ = 0;
    DomainMask domains_;
    GenerationBudget budget_;
    DeterminismLevel determinism_ = DeterminismLevel::Gameplay;
    f64 region_metres_ = 64.0;
    u8 region_level_ = 0;
};

}  // namespace cy::pcg
