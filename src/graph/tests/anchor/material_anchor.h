#pragma once
// The material anchor domain. M8.b task 2.3.
//
// ================================================================================================
// WHY THIS EXISTS, AND WHY IT IS A TEST FIXTURE AND NOT A MODULE
// ================================================================================================
//
// design.md §1.7: "Do not modify `src/rendering/material/` in this milestone: that module is M7's
// closed work, E2 invalidates its cook keys, and re-testing it mid-milestone buys nothing the
// criterion below does not buy more cheaply. Instead, its criterion is P13's anchor."
//
// So this is the material compiler's op table, type lattice, typing rules, constant folding,
// closure algebra and Slang emitter, re-expressed as a `cy::graph::Domain` and a
// `cy::graph::SourceTarget` over the generalised core — and nothing else. It compiles the tree's
// own reference material and must reproduce three digests the spike measured against the real
// compiler:
//
//   IR             f48f3faf395e52fd   (26 authored nodes -> 26 IR nodes)
//   post-pipeline  178a3630921e0506   (16 nodes; 3 dropped, 10 merged, 1 folded)
//   program        7f74500626ea001a   (11 statements)
//
// A core that hits all three has lost nothing the material compiler depends on, and porting the
// material compiler onto it becomes a later mechanical change rather than a risk inside this
// milestone.
//
// ================================================================================================
// THE ONE PLACE THIS DOMAIN CONTRADICTS TASK 2.2, STATED OUT LOUD
// ================================================================================================
//
// Task 2.2 requires operation and type identity to move to TEXT. Task 2.3 requires this domain to
// reproduce a digest computed from the material IR's ENUMERATOR VALUES. Those cannot both hold of
// one domain, because the identity is an input to every content hash. The core resolves it by
// letting a domain PIN its identities (`pinned_identity`), and this is the only domain in the tree
// that does: `kPinnedIdentity` below is the material enumerator's own value, op for op and type for
// type. Text identity is the default everywhere else, including in every consumer lowering.
//
// That pin is the anchor's whole cost and it is a real one: it says this domain is compatible with
// a body of cooked data that exists. Dropping it — which is what porting the material compiler for
// real would do — invalidates every material cook key once, exactly as E2 said it would.

#include <cy/core/memory/allocator.h>
#include <cy/graph/emit.h>
#include <cy/graph/expr.h>
#include <cy/graph/passes.h>

namespace cy::graph::anchor {

/// The type lattice, in the material IR's own order. The enumerator value IS the pinned identity.
enum MaterialType : TypeId { Float = 0, Vec2, Vec3, Vec4, Int, Bool, Closure, TypeCount };

/// The operation table, in the material IR's own order, for the same reason.
enum MaterialOp : OpId {
    OpConstant = 0,
    OpParameter,
    OpAttribute,
    OpField,
    OpTextureSample,
    OpAdd,
    OpSub,
    OpMul,
    OpDiv,
    OpMin,
    OpMax,
    OpDot,
    OpPow,
    OpSaturate,
    OpOneMinus,
    OpNormalize,
    OpLerp,
    OpSelect,
    OpSwizzle,
    OpCombine,
    OpCustom,
    OpDiffuse,
    OpSpecular,
    OpCoat,
    OpTransmission,
    OpSubsurface,
    OpSheen,
    OpEmission,
    OpClosureScale,
    OpClosureAdd,
    OpClosureLayer,
    OpCount,
};

/// The declaration kinds this domain uses. `Decl::kind`.
enum DeclKind : u16 { ParameterDecl = 0, TextureDecl = 1 };
/// `Decl::flags`. A parameter's author-requested static-ness, a texture's shadow criticality.
inline constexpr u32 kDeclRequestedStatic = 1U << 0U;
inline constexpr u32 kDeclShadowCritical = 1U << 0U;

/// The root slots this domain declares (E3). Two, and neither of the other six consumers wants
/// either — which is exactly why the list is the domain's rather than the core's.
inline constexpr u32 kSurfaceRoot = 0;
inline constexpr u32 kOpacityRoot = 1;

/// The domain's own node-flag bits, above the core's `kFlagUniform`.
inline constexpr u32 kFlagBaseReflectance = kFirstDomainFlag << 0U;
inline constexpr u32 kFlagOpacityCritical = kFirstDomainFlag << 1U;
inline constexpr u32 kFlagMicrodetail = kFirstDomainFlag << 2U;

[[nodiscard]] const Domain& material_domain() noexcept;
[[nodiscard]] const SourceTarget& material_target() noexcept;

/// The swizzle mask the material IR packs: up to four component indices, one per nibble, and the
/// count in the top nibble.
[[nodiscard]] u32 swizzle_mask(Span<const u8> components) noexcept;

// --- The authoring front end -------------------------------------------------------------------
//
// Shaped like an editor rather than like the IR, for the reason `src/rendering/material/graph.h`
// gives: a front end written to look like the IR would meet the criterion by construction and prove
// nothing. Every wart below is one an editor produces — a weight port on every closure, pairwise
// sums, one texture dragged in twice, a muted node, a disconnected node.

enum class GraphOp : u16 {
    Constant = 0,
    Parameter,
    Attribute,
    Field,
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
    Diffuse,
    Specular,
    Coat,
    Sheen,
    Emission,
    Transmission,
    Subsurface,
    AddClosures,
    LayerClosures,
    Count,
};

struct GraphNode {
    u32 id = 0;
    GraphOp op = GraphOp::Constant;
    Name symbol;
    Immediate value;
    TypeId type = Float;
    bool muted = false;
    u32 flags = 0;
};

/// An authored material graph. Deliberately the same shape as the M7 front end it stands in for.
class MaterialGraph {
public:
    static constexpr u8 kMaxPorts = 4;

    MaterialGraph(Allocator& allocator, Name material_name) noexcept;

    MaterialGraph(const MaterialGraph&) = delete;
    MaterialGraph& operator=(const MaterialGraph&) = delete;

    [[nodiscard]] Status declare(const Decl& decl) noexcept;
    [[nodiscard]] Expected<u32, Error> add(GraphOp op, Name symbol = Name{}, TypeId type = Float,
                                           const Immediate& value = Immediate{}) noexcept;
    /// A port wired twice keeps the last wire, which is what an editor does.
    [[nodiscard]] Status connect(u32 from, u32 to, u8 port) noexcept;
    [[nodiscard]] Status mute(u32 node, bool muted) noexcept;
    [[nodiscard]] Status annotate(u32 node, u32 flags) noexcept;
    [[nodiscard]] Status set_surface_output(u32 node) noexcept;
    [[nodiscard]] Status set_opacity_output(u32 node) noexcept;

    [[nodiscard]] Name name() const noexcept { return name_; }
    [[nodiscard]] Span<const GraphNode> nodes() const noexcept { return nodes_.span(); }
    [[nodiscard]] Span<const Decl> decls() const noexcept { return decls_.span(); }
    [[nodiscard]] u32 input(u32 node, u8 port) const noexcept;
    [[nodiscard]] u32 surface_output() const noexcept { return surface_; }
    [[nodiscard]] u32 opacity_output() const noexcept { return opacity_; }
    [[nodiscard]] Allocator& allocator() const noexcept { return nodes_.allocator(); }

private:
    Name name_;
    Array<GraphNode> nodes_;
    Array<u32> links_;
    Array<Decl> decls_;
    u32 surface_ = kInvalidNode;
    u32 opacity_ = kInvalidNode;
};

/// Lower an authored graph to the core's IR. Every node is lowered, including the disconnected
/// ones: the optimisation pipeline is what removes them, and it can only report what it removed if
/// it saw it.
[[nodiscard]] Expected<Module, Error> lower_graph(const MaterialGraph& graph, Allocator& allocator,
                                                  const PassSwitches& switches = {}) noexcept;

/// The authoring node ids the reference fixture uses, so a case can name one.
struct ReferenceIds {
    u32 uv = 0;
    u32 albedo_sample = 0;
    u32 grime_sample = 0;
    u32 tint_constant = 0;
    u32 emission = 0;
    u32 orphan = 0;
    u32 orphan_constant = 0;
};

/// The tree's own reference material, authored exactly as `src/rendering/material/tests/fixtures.h`
/// authors it. 26 nodes; the digests at the top of this file are what it must produce.
[[nodiscard]] bool build_reference_graph(MaterialGraph& graph, ReferenceIds& ids) noexcept;

}  // namespace cy::graph::anchor
