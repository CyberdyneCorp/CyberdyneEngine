#pragma once
// The shared pure-expression SSA core. M8.b task 2.2.
//
// This is `src/rendering/material/`'s IR generalised by the four extensions design.md §1.4 named,
// and by nothing else. The material IR is a bottom-up hash-consed pure-expression DAG whose
// identity is a content hash; §1.3 measured that six of its properties follow from that one
// sentence, and every one of them is kept here, because they are what makes the structure good at
// its own job:
//
//   * an operand must already exist, so there is NO BACK EDGE — no Phi, no Loop, no Block, no Call;
//   * identity is content, so two reads of one mutable cell ARE one value — there is no Store;
//   * a commutative operand list is sorted by content hash, so an AUTHORED ORDER DOES NOT SURVIVE;
//   * optimisation is a rebuild from the roots, so A VALUE NOTHING READS IS DELETED;
//   * a conditional is a value, so BOTH ARMS ARE EVALUATED.
//
// A consumer that needs any of those to be false does not lower through this core. `animation` and
// `ai-system` each need a lazy branch and each keep their own IR — see lower/animation.h and
// lower/behaviour.h. Reaching for one IR again is refused by `visual-scripting`'s "No universal
// representation" requirement, and §1.3's probes P3-P7 and P10 are what it would cost.
//
// ================================================================================================
// THE FOUR EXTENSIONS, AND THE ONE TRAP THEY CARRY
// ================================================================================================
//
// E1 — AN OPEN TYPE LATTICE. `TypeDesc` is a domain-supplied row, not an enumerator.
// E2 — AN OPEN OPERATION TABLE. `OpDesc` likewise: arity, commutativity, category, merge class.
// E3 — DECLARED ROOTS. A domain declares a typed list of roots; the material domain declares two.
// E4 — A DECLARED PHASE BOUNDARY. An op may be marked as one, and `Module::phase_of` then splits a
//      single DAG into the dispatches an externally-batched result separates.
//
// THE TRAP, which `ir.h` states about itself: "the enumerator VALUES are part of every content hash
// and therefore of every cook key". A table two domains extend cannot have positional identity, so
// **operation and type identity are TEXT here** — `TypeDesc::identity` and `OpDesc::identity`
// default to a hash of the descriptor's name, exactly as `Name` is hashed by `text()` rather than
// by `index()` and for the same reason.
//
// The one deliberate exception is `pinned_identity()`, and it exists because task 2.3's anchor and
// task 2.2's text identity are in direct conflict: reproducing the tree's reference material at IR
// digest f48f3faf395e52fd requires the numeric op and type identity the material IR hashes today.
// A domain that pins its identities is declaring compatibility with an existing body of cooked
// data. The material anchor domain is the only one in the tree that does, and porting the material
// compiler for real is the moment that pin is dropped and its cook keys are invalidated once.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/core/values/name.h>

#include <string_view>

namespace cy::graph {

/// A value in a module. An index into `Module::nodes()`; never emitted, never hashed.
using NodeId = u32;
inline constexpr NodeId kInvalidNode = 0xFFFFFFFFU;

/// A row in the domain's type lattice (E1) and operation table (E2).
using TypeId = u16;
using OpId = u16;
inline constexpr TypeId kInvalidType = 0xFFFFU;
inline constexpr OpId kInvalidOp = 0xFFFFU;

/// `OpDesc::arity` for an operation that takes between `min_operands` and `max_operands`.
inline constexpr u32 kVariadic = 0xFFFFFFFFU;
/// The widest operand list a node may have. The operand pool is flat and every rebuild copies an
/// operand list onto the stack, so this is a real bound rather than a hint.
inline constexpr u32 kMaxOperands = 4;

/// The core's own version, part of every module digest. A change to the representation invalidates
/// derived data without touching an authored graph.
inline constexpr u32 kIrVersion = 1;

// --- Hashing --------------------------------------------------------------------------------
//
// The 64-bit FNV-1a the whole core hashes with, exposed because a domain's identity table, the
// emitter and a cook key all use it, and two hash functions spelled differently is how a cook cache
// serves the wrong artefact.

[[nodiscard]] u64 hash_bytes(u64 seed, const void* data, usize size) noexcept;
[[nodiscard]] u64 hash_u64(u64 seed, u64 value) noexcept;
[[nodiscard]] u64 hash_text(u64 seed, std::string_view text) noexcept;
inline constexpr u64 kHashSeed = 14695981039346656037ULL;

/// The identity of an operation or a type, derived from its NAME. This is E2's answer to the trap:
/// a table two domains extend has no stable positional identity, and text has one.
[[nodiscard]] u64 text_identity(std::string_view name) noexcept;

/// A pinned identity: compatibility with a body of cooked data that already exists. See the note at
/// the top of this file; `domains/material_anchor` is the only user in the tree.
[[nodiscard]] constexpr u64 pinned_identity(u64 value) noexcept {
    return value;
}

/// A node's literal payload. Hashed as bits, so it must be canonical: the builder normalises a
/// negative zero to zero, because -0.0 and 0.0 are the same value and different bytes.
struct Immediate {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 z = 0.0F;
    f32 w = 0.0F;
    /// A packed mask, an integer literal, or a boolean — whatever the domain's op says it is.
    u32 mask = 0;

    friend bool operator==(const Immediate& a, const Immediate& b) noexcept;
    friend bool operator!=(const Immediate& a, const Immediate& b) noexcept { return !(a == b); }

    [[nodiscard]] static Immediate scalar(f32 value) noexcept {
        return Immediate{value, 0.0F, 0.0F, 0.0F, 0};
    }
};

// AN IMMEDIATE IS HASHED AS RAW BYTES IN THREE PLACES — `Builder::hash_of` here, `hash_literal` in
// cybergraph.cpp and the rig digest in lower_camera.cpp — and that is only sound while the layout
// is flat. Five four-byte members pack to twenty bytes with nothing between them; a member that
// forced eight-byte alignment would introduce padding no initialiser writes, and every content
// hash, every cook key and every merge decision would then close over indeterminate memory. That
// is not hypothetical: `script::Value` in lower_script.h did exactly this, and it took a vertical
// slice compiling one graph three times to find it. Add a member and either keep the layout flat or
// hash the fields, as `script::hash_constant` does.
static_assert(sizeof(Immediate) == (4 * sizeof(f32)) + sizeof(u32),
              "Immediate has grown padding; see the note above this assertion");

/// Whether two structurally identical instances of an operation may become one value.
///
/// The three classes are the three the material IR's `BuilderPolicy` distinguishes, and they are
/// distinguished because `material-compiler` requires each pass to be individually disableable: a
/// bisection that wants leaves shared and interior expressions apart needs the builder to tell them
/// apart. `Never` is here for an op whose two instances are two things — a domain that adds one
/// should say why in its own table.
enum class MergeClass : u8 { Leaf = 0, Expression, Sample, Never };

/// What a node costs, for the emitted program's own report. The emitter counts by this rather than
/// by naming ops, so a domain's vocabulary does not have to be known to count its branches.
enum class OpCategory : u8 { Arithmetic = 0, Sample, Branch, Aggregate, Leaf };

/// One row of the type lattice (E1).
struct TypeDesc {
    const char* name = "?";
    /// How many scalar components the type has. Zero for a type that is not a number — a closure,
    /// a pose, a handle.
    u32 components = 0;
    /// Whether arithmetic may be performed on it at all.
    bool numeric = true;
    /// The identity that enters every content hash. Left alone it is `text_identity(name)`, which
    /// is what every domain but the anchor wants; `pinned` makes it the literal value below, which
    /// is compatibility with a body of cooked data that already exists.
    u64 identity = 0;
    bool pinned = false;
};

/// One row of the operation table (E2).
struct OpDesc {
    const char* name = "?";
    /// The exact operand count, or `kVariadic`.
    u32 arity = 0;
    /// The bounds a variadic op accepts. Ignored when `arity` is exact.
    u32 min_operands = 2;
    u32 max_operands = kMaxOperands;
    /// Whether the operand list may be reordered. A commutative list is sorted by CONTENT HASH.
    bool commutative = false;
    /// The value is spelled at its use site rather than given a statement: a constant, a parameter,
    /// an attribute. This is what makes a folded root produce no statement at all.
    bool inline_leaf = false;
    /// An aggregate value — a closure, a layer stack — which the domain's own algebra may drop.
    /// The asymmetry that matters is the domain's: a LEAF aggregate may vanish, and a combinator
    /// may only collapse onto something still present.
    bool aggregate = false;
    bool leaf_aggregate = false;
    /// The value varies across the dispatch: it reads per-invocation data.
    bool varying = false;
    /// The value is constant across the dispatch by construction: a literal, a parameter.
    bool uniform_leaf = false;
    /// E4. A node of this op ends one dispatch and begins the next, because its operand's value
    /// arrives from a batched external query rather than from this DAG.
    bool phase_boundary = false;
    MergeClass merge = MergeClass::Expression;
    OpCategory category = OpCategory::Arithmetic;
    /// As `TypeDesc::identity`: text by default, a pinned literal only for a compatibility domain.
    u64 identity = 0;
    bool pinned = false;
};

/// One declared root (E3). The material domain declares two — a surface closure and an opacity —
/// and neither of the other consumers wants either, which is why the list is the domain's.
struct RootDecl {
    const char* name = "?";
    /// The type the root must have, or `kInvalidType` for a root that accepts any.
    TypeId type = kInvalidType;
    /// A root that must be set before `finish()`.
    bool required = false;
};

/// A declaration the module carries: a parameter, a texture, a channel — `kind` is the domain's.
///
/// One structure rather than one per kind, because the core neither reads nor validates them: it
/// carries them through every rebuild in declaration order so that a pass cannot silently drop one.
struct Decl {
    Name name;
    TypeId type = kInvalidType;
    Immediate value;
    u16 kind = 0;
    u32 flags = 0;
};

/// One value. Plain data: operands live in the module's pool, so a node is copyable and a module is
/// one contiguous array rather than a graph of allocations.
struct Node {
    OpId op = kInvalidOp;
    TypeId type = kInvalidType;
    /// A parameter's name, a texture's name, an attribute's semantic, a domain's own text. HASHED
    /// BY TEXT, never by `Name::index()`, which is interning order and is not stable across runs.
    Name symbol;
    Immediate value;
    u32 operand_begin = 0;
    u32 operand_count = 0;
    /// The content hash: the value's meaning and nothing else.
    u64 hash = 0;
};

/// Core-owned side-table bits. Bit 0 is the uniform/varying analysis, which the pipeline writes and
/// the emitter reads; a domain's own annotations start at bit 1.
inline constexpr u32 kFlagUniform = 1U << 0U;
inline constexpr u32 kFirstDomainFlag = 1U << 1U;

class Domain;
class Builder;

/// A module: nodes, declared roots, declarations, and the two side tables.
///
/// Immutable. Every module comes out of a `Builder`, including the ones a pass produces, which is
/// what makes "a pass's output is in the same canonical form as its input" true by construction.
class Module {
public:
    Module(Allocator& allocator, const Domain& domain) noexcept;

    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    Module(Module&&) noexcept = default;
    Module& operator=(Module&&) noexcept = default;

    [[nodiscard]] const Domain& domain() const noexcept { return *domain_; }
    [[nodiscard]] Name name() const noexcept { return name_; }
    [[nodiscard]] Span<const Node> nodes() const noexcept { return nodes_.span(); }
    [[nodiscard]] const Node& node(NodeId id) const noexcept { return nodes_[id]; }
    [[nodiscard]] Span<const NodeId> operands(NodeId id) const noexcept;
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(nodes_.size()); }

    /// The declared roots, in the domain's slot order. An unset root is `kInvalidNode`, which is a
    /// structural fact rather than an empty function.
    [[nodiscard]] Span<const NodeId> roots() const noexcept { return roots_.span(); }
    [[nodiscard]] NodeId root(u32 slot) const noexcept;

    /// `kIrVersion`, the module's name, and every root's content hash in slot order.
    [[nodiscard]] u64 digest() const noexcept { return digest_; }

    [[nodiscard]] Span<const Decl> decls() const noexcept { return decls_.span(); }
    [[nodiscard]] const Decl* find_decl(u16 kind, Name name) const noexcept;

    /// How many values the builder merged into an existing one, and how many of those were samples.
    /// NOT part of the identity: it is a fact about how the module was built.
    [[nodiscard]] u32 merged_values() const noexcept { return merged_values_; }
    [[nodiscard]] u32 merged_samples() const noexcept { return merged_samples_; }

    [[nodiscard]] u32 flags(NodeId id) const noexcept;
    /// The authoring nodes that produced this value. A SET, because interning merges two of them
    /// into one value; empty when nothing claimed it.
    [[nodiscard]] Span<const u32> origins(NodeId id) const noexcept;

    /// E4. Which dispatch this value belongs to: zero, plus one for every phase boundary between it
    /// and the leaves.
    [[nodiscard]] u32 phase_of(NodeId id) const noexcept;
    [[nodiscard]] u32 phase_count() const noexcept { return phase_count_; }

    [[nodiscard]] Allocator& allocator() const noexcept { return nodes_.allocator(); }

private:
    friend class Builder;

    const Domain* domain_ = nullptr;
    Name name_;
    Array<Node> nodes_;
    Array<NodeId> operand_pool_;
    Array<Decl> decls_;
    Array<u32> flags_;
    Array<u8> phase_;
    /// Provenance, flattened: `origin_begin_[id] .. origin_begin_[id + 1]` into `origin_pool_`.
    Array<u32> origin_begin_;
    Array<u32> origin_pool_;
    Array<NodeId> roots_;
    u64 digest_ = 0;
    u32 merged_values_ = 0;
    u32 merged_samples_ = 0;
    u32 phase_count_ = 1;
};

/// What the builder is allowed to merge and reorder.
///
/// A POLICY RATHER THAN TWO BOOLEANS, because interning happens in the only constructor: a
/// front-end that ignored a disabled pass would merge the values a bisection is trying to keep
/// apart before the pipeline ever ran, and turning the pass off downstream could not unmerge them.
struct BuilderPolicy {
    bool intern = true;
    bool canonical_commutative = true;
    bool intern_expressions = true;
    bool intern_samples = true;
};

/// The only constructor of a node.
///
/// `make` normalises the call through the domain, types it through the domain, canonicalises a
/// commutative operand list by content hash, and interns it.
class Builder {
public:
    Builder(Allocator& allocator, const Domain& domain, Name module_name) noexcept;

    Builder(const Builder&) = delete;
    Builder& operator=(const Builder&) = delete;

    void set_policy(const BuilderPolicy& policy) noexcept { policy_ = policy; }
    [[nodiscard]] const BuilderPolicy& policy() const noexcept { return policy_; }
    [[nodiscard]] const Domain& domain() const noexcept { return *domain_; }

    /// Declare a parameter, a texture, a channel. Refused when `kind` and `name` are already taken,
    /// so a misspelt second declaration is a diagnostic rather than a shadowed first one.
    [[nodiscard]] Status declare(const Decl& decl) noexcept;

    /// Type-check, canonicalise, intern, and return the value.
    ///
    /// `hint` is the result type for the ops whose type cannot be derived — a constant, an
    /// attribute, a domain's own escape hatch. It reaches `Domain::result_type` for every op, and
    /// what a derivable op does with it is the domain's rule. `kInvalidType` means "derive it".
    [[nodiscard]] Expected<NodeId, Error> make(OpId op, TypeId hint, Name symbol,
                                               const Immediate& value,
                                               Span<const NodeId> operands) noexcept;
    [[nodiscard]] Expected<NodeId, Error> make(OpId op, Span<const NodeId> operands) noexcept;

    /// Merge flags into a node's entry. Merge rather than assign, because a rebuild maps several
    /// old nodes onto one new node and every annotation on either has to survive.
    [[nodiscard]] Status annotate(NodeId id, u32 flags) noexcept;
    /// Record an authoring node as an origin of this value. The set is sorted and deduplicated in
    /// `finish()`, so attribution stays linear in the graph rather than quadratic.
    [[nodiscard]] Status add_origin(NodeId id, u32 authoring_node) noexcept;

    /// Set a declared root. Refused when the value's type is not the one the domain declared.
    [[nodiscard]] Status set_root(u32 slot, NodeId id) noexcept;

    /// Finish. The builder is empty afterwards and must not be reused.
    ///
    /// Every node the builder made is carried into the module, orphans included: dead-node
    /// elimination is a property of a REBUILD FROM THE ROOTS, and a builder that dropped orphans by
    /// itself would make the `dead_node_elimination` switch a placebo.
    [[nodiscard]] Expected<Module, Error> finish() noexcept;

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(module_.nodes_.size()); }
    [[nodiscard]] const Node& node(NodeId id) const noexcept { return module_.nodes_[id]; }
    [[nodiscard]] Span<const NodeId> operands(NodeId id) const noexcept {
        return module_.operands(id);
    }
    [[nodiscard]] const Decl* find_decl(u16 kind, Name name) const noexcept {
        return module_.find_decl(kind, name);
    }

private:
    [[nodiscard]] Expected<NodeId, Error> place(Node node, Span<const NodeId> operands) noexcept;
    void canonicalise(OpId op, Span<const NodeId> operands, NodeId* ordered) const noexcept;
    [[nodiscard]] bool mergeable(OpId op) const noexcept;
    [[nodiscard]] static Status check_arity(const OpDesc& desc, usize count) noexcept;
    [[nodiscard]] u64 hash_of(const Node& node, Span<const NodeId> operands) const noexcept;
    [[nodiscard]] bool same(const Node& node, Span<const NodeId> operands,
                            NodeId candidate) const noexcept;
    [[nodiscard]] Status finish_origins() noexcept;
    [[nodiscard]] Status finish_phases() noexcept;

    const Domain* domain_ = nullptr;
    Module module_;
    /// content hash -> the first node with it. A collision is resolved by comparing the nodes
    /// themselves, so a 64-bit collision costs a duplicate value and never a wrong merge.
    HashMap<u64, NodeId> interned_;
    BuilderPolicy policy_;
    bool finished_ = false;
};

/// What the domain is asked when a node is typed.
struct TypeQuery {
    OpId op = kInvalidOp;
    TypeId hint = kInvalidType;
    Immediate value;
    Span<const TypeId> operands;
};

/// What constant folding decided about a node.
struct FoldOutcome {
    enum class Kind : u8 {
        /// Leave the node alone.
        None = 0,
        /// Replace it with a constant carrying `value` at `type`.
        Constant,
        /// Replace it with an existing value: the algebraic identities, `x * 1` and its family.
        Replace,
    };

    Kind kind = Kind::None;
    Immediate value;
    TypeId type = kInvalidType;
    NodeId node = kInvalidNode;
};

/// A domain: the type lattice, the operation table, the roots, and the rules that read them.
///
/// Everything domain-specific in the material IR lives behind one of these methods, and everything
/// that is not lives in this file. That split is the whole of E1 and E2.
class Domain {
public:
    Domain() = default;
    virtual ~Domain() = default;
    Domain(const Domain&) = delete;
    Domain& operator=(const Domain&) = delete;
    Domain(Domain&&) = delete;
    Domain& operator=(Domain&&) = delete;

    [[nodiscard]] virtual std::string_view domain_name() const noexcept = 0;
    [[nodiscard]] virtual Span<const TypeDesc> types() const noexcept = 0;
    [[nodiscard]] virtual Span<const OpDesc> ops() const noexcept = 0;
    [[nodiscard]] virtual Span<const RootDecl> roots() const noexcept = 0;

    /// The result type of `op` over these operand types, or a diagnostic naming the rule.
    [[nodiscard]] virtual Expected<TypeId, Error> result_type(
        const TypeQuery& query) const noexcept = 0;

    /// ONE SPELLING PER OPERATION. An editor emits a "one minus" node and a text definition writes
    /// `1 - x`; unless one becomes the other in the only constructor, the two front-ends produce
    /// different values for the same source. Return true when `op` or the operands were rewritten;
    /// `out` has room for `kMaxOperands`.
    [[nodiscard]] virtual bool normalise(const Builder& builder, OpId& op,
                                         Span<const NodeId> operands, NodeId* out,
                                         u32& out_count) const noexcept;

    /// The op a folded constant is built with, or `kInvalidOp` in a domain that does not fold.
    [[nodiscard]] virtual OpId constant_op() const noexcept;

    /// Constant folding and the algebraic identities it exposes.
    [[nodiscard]] virtual FoldOutcome fold(const Builder& out, OpId op, TypeId type,
                                           const Immediate& value,
                                           Span<const NodeId> operands) const noexcept;

    /// The domain's algebra over aggregate values — the closure algebra, in the material domain.
    /// Called instead of a plain rebuild for every op whose descriptor says `aggregate`. An operand
    /// may arrive as `kInvalidNode`, meaning it was dropped; returning `kInvalidNode` drops this
    /// value in turn.
    [[nodiscard]] virtual Expected<NodeId, Error> simplify_aggregate(
        const Node& node, Span<const NodeId> operands, Builder& out,
        u32& simplifications) const noexcept;

    /// Normalise an immediate before it is hashed. The default removes a negative zero.
    [[nodiscard]] virtual Immediate canonical_immediate(const Immediate& value) const noexcept;

    // --- Table lookups, resolved once at construction ------------------------------------------

    [[nodiscard]] const TypeDesc& type_desc(TypeId type) const noexcept;
    [[nodiscard]] const OpDesc& op_desc(OpId op) const noexcept;
    [[nodiscard]] u64 type_identity(TypeId type) const noexcept;
    [[nodiscard]] u64 op_identity(OpId op) const noexcept;
    [[nodiscard]] TypeId find_type(std::string_view name) const noexcept;
    [[nodiscard]] OpId find_op(std::string_view name) const noexcept;
    [[nodiscard]] bool valid_type(TypeId type) const noexcept;
    [[nodiscard]] bool valid_op(OpId op) const noexcept;
    [[nodiscard]] u32 components(TypeId type) const noexcept;
};

// --- Traversal ----------------------------------------------------------------------------------

/// The canonical visit order: every node after its operands, reachable from `roots` only, and a
/// function of content alone.
///
/// Emission numbers SSA values by position in this array, so two modules with the same content
/// produce the same source even when their node ids differ.
[[nodiscard]] Status canonical_order(const Module& module, Span<const NodeId> roots,
                                     Array<NodeId>& out) noexcept;

// --- Serialisation --------------------------------------------------------------------------

/// The wire format's version. Bumping it invalidates cooked data and no authored asset.
inline constexpr u32 kModuleFormatVersion = 1;

/// Encode a module. Deterministic: two encodes of one module produce identical bytes, which is what
/// lets a cook key be a digest over these bytes.
[[nodiscard]] Status encode_module(const Module& module, Array<u8>& out) noexcept;

/// Decode a module against the domain that produced it. A truncated, mis-versioned or
/// wrong-domain buffer is refused rather than half-read.
///
/// The round trip is checked by identity: `decode(encode(m)).digest() == m.digest()`.
[[nodiscard]] Expected<Module, Error> decode_module(Span<const u8> bytes, const Domain& domain,
                                                    Allocator& allocator) noexcept;

}  // namespace cy::graph
