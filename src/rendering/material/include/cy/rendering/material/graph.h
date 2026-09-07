#pragma once
// The node-graph front-end. M7 task 6.1.
//
// `material-compiler` — "Authoring forms share one representation": "A material SHALL be authorable
// as a node graph or as a text material definition, and both SHALL be front-ends producing the same
// material IR. Neither form SHALL be able to express something the other cannot represent."
//
// ================================================================================================
// THIS IS DELIBERATELY SHAPED LIKE AN EDITOR AND NOT LIKE AN IR
// ================================================================================================
//
// The exit criterion is that a graph and a hand-written material produce identical programs, and a
// graph front-end written to look like the IR would meet it by construction and prove nothing. So
// this one emits what an editor emits (design.md §1.1):
//
//   * a WEIGHT PORT on every closure node, defaulting to one, whether or not the author touched it;
//   * closure sums built pairwise, because a node has two inputs and an author adds a third by
//     adding a second sum node;
//   * one texture sampled from two separate nodes, because two parts of a graph each dragged the
//     texture in;
//   * operand order decided by which port a wire landed on;
//   * a MUTED node the author did not delete, which lowers to a weight of zero rather than to
//     nothing, because the editor still shows it;
//   * a DISCONNECTED node, which is lowered like every other one — the IR contains it and the
//     optimisation pipeline drops it, which is what makes "the editor SHALL be able to show which
//     nodes were dropped" answerable.
//
// Every one of those is removed by the compiler and by nothing else. If any of them survived, the
// text front-end would have to grow a matching wart to keep the criterion, which is the failure
// this shape exists to detect.
//
// PROVENANCE. Every IR value records the graph node it came from, through `Builder::add_origin`.
// After interning, one value can carry several — which is why attribution is a set and why it is a
// side table (design.md §1.2, decision 4).

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/material/ir.h>
#include <cy/rendering/material/passes.h>

namespace cy::rendering::material {

/// What a graph node is. These are the editor's palette, not the IR's opcodes: `OneMinus` is a node
/// an author drags in, and it lowers to the one spelling the builder offers.
enum class GraphOp : u16 {
    Constant = 0,
    Parameter,
    Attribute,
    Field,
    /// Samples a declared texture. Its coordinate is input 0.
    TextureSample,
    Multiply,
    Add,
    Subtract,
    Divide,
    OneMinus,
    Saturate,
    Lerp,
    Swizzle,
    Combine,
    Custom,
    /// Closure nodes. Input 0 is the colour (or roughness, for a coat), input 1 the roughness where
    /// the closure has one, and THE LAST INPUT IS ALWAYS THE WEIGHT.
    Diffuse,
    Specular,
    Coat,
    Sheen,
    Emission,
    Transmission,
    Subsurface,
    /// Two closures in, one out. An author adds a third closure with a second one of these.
    AddClosures,
    LayerClosures,
    Count,
};

[[nodiscard]] const char* graph_op_name(GraphOp op) noexcept;

/// One node in the authored graph.
struct GraphNode {
    u32 id = 0;
    GraphOp op = GraphOp::Constant;
    /// A parameter's, texture's, attribute's or field's name; Slang text for `Custom`.
    Name symbol;
    /// A literal, or a swizzle mask.
    Immediate value;
    /// The result type, for the nodes whose type the IR cannot derive.
    ValueType type = ValueType::Float;
    /// The author muted it. It stays in the graph and contributes nothing.
    bool muted = false;
    NodeFlags flags = NodeFlags::None;
};

/// An authored material graph.
class MaterialGraph {
public:
    MaterialGraph(Allocator& allocator, Name material_name) noexcept;

    MaterialGraph(const MaterialGraph&) = delete;
    MaterialGraph& operator=(const MaterialGraph&) = delete;

    [[nodiscard]] Status declare_parameter(const ParameterDecl& decl) noexcept;
    [[nodiscard]] Status declare_texture(const TextureDecl& decl) noexcept;

    /// Add a node and return its id. Ids are assigned in insertion order, which is exactly the
    /// construction order that must not reach the IR's identity.
    [[nodiscard]] Expected<u32, Error> add(GraphOp op, Name symbol = Name{},
                                           ValueType type = ValueType::Float,
                                           const Immediate& value = Immediate{}) noexcept;
    /// Wire `from`'s output into `to`'s input port. A port wired twice keeps the last wire, which
    /// is what an editor does.
    [[nodiscard]] Status connect(u32 from, u32 to, u8 port) noexcept;
    [[nodiscard]] Status mute(u32 node, bool muted) noexcept;
    [[nodiscard]] Status annotate(u32 node, NodeFlags flags) noexcept;

    [[nodiscard]] Status set_surface_output(u32 node) noexcept;
    [[nodiscard]] Status set_opacity_output(u32 node) noexcept;

    [[nodiscard]] Name name() const noexcept { return name_; }
    [[nodiscard]] Span<const GraphNode> nodes() const noexcept { return nodes_.span(); }
    [[nodiscard]] Span<const ParameterDecl> parameters() const noexcept {
        return parameters_.span();
    }
    [[nodiscard]] Span<const TextureDecl> textures() const noexcept { return textures_.span(); }
    /// The node wired into `port` of `node`, or `kInvalidNode`.
    [[nodiscard]] u32 input(u32 node, u8 port) const noexcept;
    [[nodiscard]] u32 surface_output() const noexcept { return surface_; }
    [[nodiscard]] u32 opacity_output() const noexcept { return opacity_; }
    [[nodiscard]] Allocator& allocator() const noexcept { return nodes_.allocator(); }

    /// The most inputs any node has: two operands plus a weight.
    static constexpr u8 kMaxPorts = 4;

private:
    Name name_;
    Array<GraphNode> nodes_;
    Array<u32> links_;  // kMaxPorts entries per node
    Array<ParameterDecl> parameters_;
    Array<TextureDecl> textures_;
    u32 surface_ = kInvalidNode;
    u32 opacity_ = kInvalidNode;
};

/// Lower an authored graph to the IR. Every node is lowered, including the disconnected ones: the
/// optimisation pipeline is what removes them, and it can only report what it removed if it saw it.
///
/// `switches` reaches the builder, because interning happens there: a front-end that ignored a
/// disabled pass would merge the values the bisection is trying to keep apart.
[[nodiscard]] Expected<Module, Error> lower_graph(const MaterialGraph& graph, Allocator& allocator,
                                                  const PassSwitches& switches = {}) noexcept;

}  // namespace cy::rendering::material
