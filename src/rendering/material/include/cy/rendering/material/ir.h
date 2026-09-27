#pragma once
// The material IR: a typed SSA expression DAG whose identity is a content hash. M7 task 6.1.
//
// `material-compiler` — "Material intermediate representation": "a typed material IR: a static
// single-assignment expression graph whose values carry types and semantic roles, whose leaves are
// inputs, parameters, constants, and texture samples, and whose root is a surface closure set", and
// "the IR SHALL be serialisable and versioned, so it can be cached, diffed, and inspected".
//
// It is also where the milestone's own exit criterion lives — "a graph and a hand-written material
// produce identical programs" — and design.md §1.7 is explicit that this layer is built first:
//
//   "The IR, its content hash, the builder, the serialiser and the round-trip test — before any
//    front-end. The criterion is a property of that layer and of nothing above it, and it is cheap
//    to hold from the start and expensive to retrofit."
//
// ================================================================================================
// THE SEVEN DECISIONS THAT MAKE TWO FRONT-ENDS ONE MATERIAL (design.md §1.2)
// ================================================================================================
//
// 1. IDENTITY IS A CONTENT HASH COMPUTED BOTTOM-UP OVER THE DAG, and nothing that is not the
//    material's meaning is allowed into it. `Node::hash` closes over the op, the result type, the
//    symbol's TEXT, the immediates and the operands' hashes. It closes over nothing else — not the
//    node id, not the order a front-end called the builder, not which front-end it was.
//
// 2. THE SYMBOL'S TEXT, NEVER THE SYMBOL TABLE'S INDEX. This engine makes that trap concrete:
//    `cy::Name` is an index into a process-wide intern table and its own header says the ordering
//    "is interning order: it is a total order ... and it is NOT lexicographic and NOT stable across
//    runs". Two front-ends intern in different orders, so a hash over `Name::index()` — or an
//    operand list SORTED by it — makes two identical materials differ. Everything here hashes and
//    orders by `Name::text()`.
//
// 3. COMMUTATIVE OPERAND LISTS ARE CANONICALLY ORDERED BY THAT SAME CONTENT HASH. Never by node
//    id, which is construction order wearing a disguise. A graph wires `scale * texture` and a text
//    definition writes `texture * scale`; only this makes them one value.
//
// 4. PROVENANCE IS A SIDE TABLE, NEVER A NODE FIELD. Two authoring nodes routinely produce one
//    value once interning has run, so attribution is a SET keyed by node id. An origin inside the
//    identity would fork the value and defeat the criterion. `Module::origins()` is that set, and
//    the cost report's per-node attribution reads it.
//
// 5. THE BUILDER IS THE ONLY WAY TO MAKE A NODE. Typing, canonicalisation and interning all happen
//    in `Builder::make`, so a front-end cannot construct something the other front-end could not
//    reproduce. `Module` has no mutating interface at all.
//
// 6. EMISSION NUMBERS SSA VALUES BY CANONICAL VISIT ORDER — see emit.h. It is stated here because
//    it is why `NodeId` may not appear in generated source.
//
// 7. ONE SPELLING PER OPERATION. `1 - x` as an `OneMinus` node in one front-end and as
//    `Sub(Constant(1), x)` in the other would never unify. The builder offers `one_minus` and the
//    `Sub` op is not reachable with a constant 1 on the left after folding — see passes.cpp.
//
// ================================================================================================
// WHAT IS NOT IN THE IDENTITY, AND WHY EACH IS DELIBERATE
// ================================================================================================
//
// `NodeFlags` (author annotations: "this node contributes to base reflectance", "this node is
// microdetail") and provenance are side tables. Flags steer DERIVATION — which nodes survive into
// the shadow or far-field program — so they change a DERIVED module's content hash by changing what
// that module contains, and they leave the primary module's identity alone. That is the correct
// split: annotating a node must not recompile the material it annotates.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/core/values/name.h>

#include <string_view>

namespace cy::rendering::material {

/// A value in the module. An index into `Module::nodes()`; never emitted, never hashed.
using NodeId = u32;
inline constexpr NodeId kInvalidNode = 0xFFFFFFFFU;

/// The IR's own version. Part of every module digest, so a change to the representation invalidates
/// derived data without touching an authored material — `material-compiler`'s "a compiler change
/// invalidates derived shader data without invalidating the material asset itself".
inline constexpr u32 kIrVersion = 1;

/// What a value is. Semantic role is carried by the op and the symbol, not by a second enumeration:
/// a `Vec3` that is a colour is a `Vec3` reaching a closure's colour operand, and that is exactly
/// the reachability question §1.5 of design.md asks the derivation report to answer.
enum class ValueType : u8 { Float = 0, Vec2, Vec3, Vec4, Int, Bool, Closure, Count };

[[nodiscard]] const char* value_type_name(ValueType type) noexcept;
/// How many scalar components a type has. `Closure` answers 0 — it is not a number.
[[nodiscard]] u32 value_type_components(ValueType type) noexcept;

/// Every operation the IR can express. One spelling each (decision 7).
///
/// The enumerator VALUES are part of every content hash and therefore of every cook key: inserting
/// an op in the middle renumbers the ones after it and invalidates every cached program. Append.
enum class Op : u16 {
    // --- Leaves ---------------------------------------------------------------------------------
    /// A literal. `Immediate` carries the components; the type says how many are read.
    Constant = 0,
    /// A material parameter, by name. Static or runtime is DERIVED, not declared — see compiler.h.
    Parameter,
    /// A geometry attribute requested semantically: "position", "normal", "tangent", "uv0", "uv1",
    /// "color0", or a custom name. `material-compiler`: materials "SHALL NOT reference vertex
    /// buffer slots, offsets, or formats".
    Attribute,
    /// A declared environment field: "wetness", "snow_depth", "biome". Same shape as an attribute
    /// and a different namespace, because a missing field is a cook-time diagnostic and a missing
    /// attribute is a geometry-source question.
    Field,

    // --- Texture --------------------------------------------------------------------------------
    /// symbol = the texture parameter's name; operand 0 = the coordinate. Always `Vec4`.
    TextureSample,

    // --- Arithmetic -----------------------------------------------------------------------------
    Add,  // commutative
    Sub,
    Mul,  // commutative
    Div,
    Min,  // commutative
    Max,  // commutative
    Dot,
    Pow,
    Saturate,
    OneMinus,
    Normalize,
    Lerp,    // (a, b, t)
    Select,  // (bool, a, b) — the only branch the IR has, and what `branch count` counts
    /// Component selection. `Immediate::mask` holds up to four component indices, one per nibble,
    /// and the result type says how many are read.
    Swizzle,
    /// Assemble a vector from scalars. Variadic, 2 to 4 operands.
    Combine,
    /// An escape hatch: `symbol` is Slang text, operands are its arguments spelled `$0`, `$1`.
    /// `material-compiler`'s "custom expression node embedding Slang within a graph" — it takes
    /// part in interning, elimination and cost like any other node.
    Custom,

    // --- Leaf closures --------------------------------------------------------------------------
    Diffuse,       // (colour: Vec3)
    Specular,      // (colour: Vec3, roughness: Float)
    Coat,          // (roughness: Float)
    Transmission,  // (colour: Vec3)
    Subsurface,    // (colour: Vec3)
    Sheen,         // (colour: Vec3)
    Emission,      // (colour: Vec3)

    // --- Closure combinators --------------------------------------------------------------------
    /// (closure, weight: Float).
    ClosureScale,
    /// Variadic, commutative: an unordered sum of closures.
    ClosureAdd,
    /// (top, base): the top attenuates the base. Not commutative — that is the whole content.
    ClosureLayer,

    Count,
};

[[nodiscard]] const char* op_name(Op op) noexcept;
/// The exact operand count, or `kVariadic` when the op takes 2 to 4.
[[nodiscard]] u32 op_arity(Op op) noexcept;
inline constexpr u32 kVariadic = 0xFFFFFFFFU;
/// Whether the operand list may be reordered. Decision 3 applies to exactly these.
[[nodiscard]] bool op_is_commutative(Op op) noexcept;
/// Whether the op produces a closure — a leaf closure or a combinator.
[[nodiscard]] bool op_is_closure(Op op) noexcept;
/// Whether the op is a LEAF closure. Derivation drops leaves and never combinators: dropping a
/// `ClosureScale`, `ClosureAdd` or `ClosureLayer` deletes everything beneath it, which is how the
/// spike's first far-field program came out with zero closures (design.md §1.5).
[[nodiscard]] bool op_is_leaf_closure(Op op) noexcept;

/// A node's literal payload. Hashed as bits, so it must be canonical: `Builder` normalises a
/// negative zero to zero, because -0.0 and 0.0 are the same material and different bytes.
struct Immediate {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 z = 0.0F;
    f32 w = 0.0F;
    /// Swizzle components (one per nibble), an integer literal, or a boolean.
    u32 mask = 0;

    friend bool operator==(const Immediate& a, const Immediate& b) noexcept;
    friend bool operator!=(const Immediate& a, const Immediate& b) noexcept { return !(a == b); }

    [[nodiscard]] static Immediate scalar(f32 value) noexcept {
        return Immediate{value, 0, 0, 0, 0};
    }
};

/// What an author said about a node, and what the compiler worked out about it.
///
/// A SIDE TABLE (decision 4). None of these bits reaches a content hash.
enum class NodeFlags : u8 {
    None = 0,
    /// "An author marks a node as contributing to base reflectance" — retained in the secondary and
    /// far-field programs regardless of the automatic heuristic.
    BaseReflectance = 1U << 0U,
    /// The same override for the shadow program's opacity.
    OpacityCritical = 1U << 1U,
    /// Detail that automatic derivation may drop: a microdetail normal, a grime layer. The
    /// heuristic's input, not its conclusion.
    Microdetail = 1U << 2U,
    /// Set by uniform/varying analysis: the value is constant across a draw, so it can be hoisted
    /// into parameter data rather than recomputed per pixel.
    Uniform = 1U << 3U,
};

[[nodiscard]] constexpr NodeFlags operator|(NodeFlags a, NodeFlags b) noexcept {
    return static_cast<NodeFlags>(static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr bool has_flag(NodeFlags set, NodeFlags flag) noexcept {
    return (static_cast<u8>(set) & static_cast<u8>(flag)) != 0;
}

/// One value. Plain data: operands live in the module's operand pool, so a node is copyable and a
/// module is one contiguous array rather than a graph of allocations.
struct Node {
    Op op = Op::Constant;
    ValueType type = ValueType::Float;
    /// A parameter's name, a texture's name, an attribute's semantic, or Slang text for `Custom`.
    /// HASHED BY TEXT (decision 2).
    Name symbol;
    Immediate value;
    u32 operand_begin = 0;
    u32 operand_count = 0;
    /// The content hash. The material's meaning and nothing else.
    u64 hash = 0;
};

/// A parameter the module declares. Order is declaration order, which both front-ends must produce
/// from the material rather than from their own traversal — see the front-ends' own notes.
struct ParameterDecl {
    Name name;
    ValueType type = ValueType::Float;
    Immediate default_value;
    /// What the AUTHOR asked for. `material-compiler`: "an author marks a purely numeric parameter
    /// as static THEN the compiler SHALL reject the annotation with a diagnostic". Kept so the
    /// diagnostic can name it; the compiler derives the real answer from use.
    bool requested_static = false;
};

/// A texture the module declares, with the average value its far-field program substitutes for a
/// sample. "WHEN a material is used kilometres from the camera THEN its far-field program SHALL
/// supply averaged constants rather than sampling textures."
struct TextureDecl {
    Name name;
    Immediate average{1.0F, 1.0F, 1.0F, 1.0F, 0};
    /// Sampled by the shadow program, so `residency` must keep a coarse level of it resident.
    bool shadow_critical = false;
};

/// A material as the compiler sees it: nodes, roots, declarations, and the two side tables.
///
/// Immutable. Every module comes out of a `Builder`, including the ones a pass produces — that is
/// what makes "a pass's output is in the same canonical form as its input" true by construction
/// rather than by review (design.md §1.3).
class Module {
public:
    explicit Module(Allocator& allocator) noexcept;

    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    Module(Module&&) noexcept = default;
    Module& operator=(Module&&) noexcept = default;

    [[nodiscard]] Name name() const noexcept { return name_; }
    [[nodiscard]] Span<const Node> nodes() const noexcept { return nodes_.span(); }
    [[nodiscard]] const Node& node(NodeId id) const noexcept { return nodes_[id]; }
    [[nodiscard]] Span<const NodeId> operands(NodeId id) const noexcept;
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(nodes_.size()); }

    /// The surface closure set. `kInvalidNode` in a module that has none — a shadow program derived
    /// from an opaque material is exactly that, and it is a structural fact rather than an empty
    /// function (design.md §1.5).
    [[nodiscard]] NodeId surface() const noexcept { return surface_; }
    /// Opacity. A SECOND ROOT, deliberately: it must survive dead-node elimination and enter the
    /// identity, or the shadow program is built from something the digest did not cover.
    [[nodiscard]] NodeId opacity() const noexcept { return opacity_; }

    /// The module's identity: `kIrVersion`, the material's name, and the roots' content hashes.
    [[nodiscard]] u64 digest() const noexcept { return digest_; }

    [[nodiscard]] Span<const ParameterDecl> parameters() const noexcept {
        return parameters_.span();
    }
    [[nodiscard]] Span<const TextureDecl> textures() const noexcept { return textures_.span(); }
    [[nodiscard]] const TextureDecl* find_texture(Name texture) const noexcept;

    /// How many values this module's builder merged into an existing one, and how many of those
    /// were texture samples. NOT part of the identity: it is a fact about how the module was built,
    /// which is what a cook report means by "duplicate samples removed". It is recorded here
    /// because interning happens in the builder — including the front-end's — so a count taken
    /// later would be a count of what was left over.
    [[nodiscard]] u32 merged_values() const noexcept { return merged_values_; }
    [[nodiscard]] u32 merged_texture_samples() const noexcept { return merged_samples_; }

    [[nodiscard]] NodeFlags flags(NodeId id) const noexcept;
    /// The authoring nodes that produced this value. A SET, because interning merges two of them
    /// into one value; empty when nothing claimed it.
    [[nodiscard]] Span<const u32> origins(NodeId id) const noexcept;

    [[nodiscard]] Allocator& allocator() const noexcept { return nodes_.allocator(); }

private:
    friend class Builder;

    Name name_;
    Array<Node> nodes_;
    Array<NodeId> operand_pool_;
    Array<ParameterDecl> parameters_;
    Array<TextureDecl> textures_;
    Array<u8> flags_;
    /// Provenance, flattened: `origin_begin_[id] .. origin_begin_[id + 1]` into `origin_pool_`.
    Array<u32> origin_begin_;
    Array<u32> origin_pool_;
    NodeId surface_ = kInvalidNode;
    NodeId opacity_ = kInvalidNode;
    u64 digest_ = 0;
    u32 merged_values_ = 0;
    u32 merged_samples_ = 0;
};

/// What the builder is allowed to merge and reorder.
///
/// A POLICY RATHER THAN TWO BOOLEANS ON THE BUILDER, because `material-compiler` requires every
/// pass to be individually disableable and interning happens HERE — in the only constructor —
/// rather than in a pass. A switch the front-end did not obey would be a switch that cannot bisect
/// anything: the front-end's builder would already have merged the values the switch is about, and
/// turning it off downstream could not unmerge them. `builder_policy()` in passes.h is the one
/// translation from the nine switches into this.
struct BuilderPolicy {
    /// Hash-consing at all. Off makes every `make` a new node.
    bool intern = true;
    /// Canonical ordering of a commutative operand list, by content hash.
    bool canonical_commutative = true;
    /// Merge interior expressions. Off with `intern` on keeps leaves shared and expressions apart.
    bool intern_expressions = true;
    /// Merge identical texture samples.
    bool intern_texture_samples = true;
};

/// The only constructor of a node (decision 5).
///
/// `make` types the node, canonicalises a commutative operand list, and interns it. Interning is
/// switchable because `material-compiler` requires every pass to be individually disableable in a
/// development build so a miscompilation can be bisected — and because turning it off is how
/// passes.cpp demonstrates that hash-consing is load-bearing rather than an optimisation.
class Builder {
public:
    Builder(Allocator& allocator, Name material_name) noexcept;

    Builder(const Builder&) = delete;
    Builder& operator=(const Builder&) = delete;

    /// What this builder may merge and reorder. Set before the first node.
    void set_policy(const BuilderPolicy& policy) noexcept { policy_ = policy; }
    [[nodiscard]] const BuilderPolicy& policy() const noexcept { return policy_; }

    // --- Declarations ---------------------------------------------------------------------------

    [[nodiscard]] Status declare_parameter(const ParameterDecl& decl) noexcept;
    [[nodiscard]] Status declare_texture(const TextureDecl& decl) noexcept;

    // --- Leaves ---------------------------------------------------------------------------------

    [[nodiscard]] Expected<NodeId, Error> constant(ValueType type, const Immediate& value) noexcept;
    [[nodiscard]] Expected<NodeId, Error> constant_float(f32 value) noexcept;
    [[nodiscard]] Expected<NodeId, Error> constant_vec3(f32 x, f32 y, f32 z) noexcept;
    /// The parameter must be declared first: a parameter that reaches the IR without a declaration
    /// is a typo that would otherwise become a silent zero.
    [[nodiscard]] Expected<NodeId, Error> parameter(Name name) noexcept;
    [[nodiscard]] Expected<NodeId, Error> attribute(Name semantic, ValueType type) noexcept;
    [[nodiscard]] Expected<NodeId, Error> field(Name field_name, ValueType type) noexcept;
    [[nodiscard]] Expected<NodeId, Error> texture_sample(Name texture, NodeId uv) noexcept;

    // --- The general constructor ------------------------------------------------------------

    /// Type-checks `operands` against `op`, canonicalises, interns, and returns the value.
    ///
    /// `hint` is the result type for the four ops whose type cannot be derived — `Constant`,
    /// `Attribute`, `Field` and `Custom` — and is CHECKED against the derived type for every other
    /// op, so a front-end that believes a multiply produces a `Vec4` is told rather than obeyed.
    /// `ValueType::Count` means "derive it", which is what the arithmetic helpers pass.
    [[nodiscard]] Expected<NodeId, Error> make(Op op, ValueType hint, Name symbol,
                                               const Immediate& value,
                                               Span<const NodeId> operands) noexcept;
    [[nodiscard]] Expected<NodeId, Error> make(Op op, Span<const NodeId> operands) noexcept;

    /// The swizzle mask: up to four component indices and the count, packed so that the mask is
    /// part of the content hash and the result type is derivable from it alone.
    [[nodiscard]] static u32 swizzle_mask(Span<const u8> components) noexcept;

    // --- Side tables ----------------------------------------------------------------------------

    /// Merge flags into a node's entry. Merge rather than assign, because a rebuild maps several
    /// old nodes onto one new node and every annotation on either has to survive.
    [[nodiscard]] Status annotate(NodeId id, NodeFlags flags) noexcept;
    /// Record an authoring node as an origin of this value. Idempotent; the set stays sorted.
    [[nodiscard]] Status add_origin(NodeId id, u32 authoring_node) noexcept;

    // --- Roots ----------------------------------------------------------------------------------

    [[nodiscard]] Status set_surface(NodeId id) noexcept;
    [[nodiscard]] Status set_opacity(NodeId id) noexcept;

    /// Finish. The builder is empty afterwards and must not be reused.
    ///
    /// Every node the builder made is carried into the module, orphans included. That is not
    /// sloppiness: dead-node elimination is a property of a REBUILD FROM THE ROOTS (passes.cpp),
    /// and a builder that dropped orphans by itself would make the `dead_node_elimination` switch a
    /// placebo — which is precisely the defect the spike found in its own first draft (§1.3).
    [[nodiscard]] Expected<Module, Error> finish() noexcept;

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(module_.nodes_.size()); }
    [[nodiscard]] const Node& node(NodeId id) const noexcept { return module_.nodes_[id]; }
    [[nodiscard]] Span<const NodeId> operands(NodeId id) const noexcept {
        return module_.operands(id);
    }

private:
    [[nodiscard]] Expected<NodeId, Error> place(Node node, Span<const NodeId> operands) noexcept;
    /// Copy the operands into `ordered`, sorting a commutative list by content hash.
    void canonicalise(Op op, Span<const NodeId> operands, NodeId* ordered) const noexcept;
    /// Whether a node of this op may join an existing value under the current policy.
    [[nodiscard]] bool mergeable(Op op) const noexcept;
    [[nodiscard]] Expected<ValueType, Error> result_type(
        Op op, ValueType hint, const Immediate& value, Span<const NodeId> operands) const noexcept;
    [[nodiscard]] u64 hash_of(const Node& node, Span<const NodeId> operands) const noexcept;
    [[nodiscard]] bool same(const Node& node, Span<const NodeId> operands,
                            NodeId candidate) const noexcept;

    Module module_;
    /// content hash → the first node with it. A collision is resolved by comparing the nodes
    /// themselves, so a 64-bit collision costs a duplicate value and never a wrong merge.
    HashMap<u64, NodeId> interned_;
    BuilderPolicy policy_;
    bool finished_ = false;
};

// --- Serialisation --------------------------------------------------------------------------

/// The wire format's magic and version. Bumping `kModuleFormatVersion` invalidates cooked material
/// data and no authored asset.
inline constexpr u32 kModuleFormatVersion = 1;

/// Encode a module. Deterministic: two encodes of one module produce identical bytes, which is what
/// lets the cook key be a digest over these bytes.
[[nodiscard]] Status encode_module(const Module& module, Array<u8>& out) noexcept;

/// Decode a module. A truncated or mis-versioned buffer is refused rather than half-read.
///
/// The round trip is checked by identity, not by field comparison: `decode(encode(m)).digest() ==
/// m.digest()` and the two emit byte-identical source. That is the strongest available statement,
/// because the digest is exactly what a cook key and a cache hit are made of.
[[nodiscard]] Expected<Module, Error> decode_module(Span<const u8> bytes,
                                                    Allocator& allocator) noexcept;

// --- Traversal --------------------------------------------------------------------------------

/// The canonical visit order: every node after its operands, reachable from `roots` only, and a
/// function of content alone.
///
/// This is decision 6's mechanism. Emission numbers SSA values by position in this array, so two
/// modules with the same content produce the same source even when their node ids differ — which
/// the spike proved with an id-shifted module producing a different program when numbering was by
/// node id (program `a8bfeafa4edcea00` against `39caab8f7a60ab78`).
[[nodiscard]] Status canonical_order(const Module& module, Span<const NodeId> roots,
                                     Array<NodeId>& out) noexcept;

/// The 64-bit FNV-1a this file hashes with. Exposed because emit.h and the cook key use the same
/// one, and two hash functions spelled differently is how a cook cache serves the wrong artefact.
[[nodiscard]] u64 hash_bytes(u64 seed, const void* data, usize size) noexcept;
[[nodiscard]] u64 hash_u64(u64 seed, u64 value) noexcept;
[[nodiscard]] u64 hash_text(u64 seed, std::string_view text) noexcept;
inline constexpr u64 kHashSeed = 14695981039346656037ULL;

}  // namespace cy::rendering::material
