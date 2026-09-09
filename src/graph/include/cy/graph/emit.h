#pragma once
// Source emission from the shared expression core: SSA values numbered by canonical visit order.
// M8.b task 2.2.
//
// ================================================================================================
// WHAT IS SHARED AND WHAT IS THE DOMAIN'S
// ================================================================================================
//
// Shared, and in this file: the emitted ORDER, the SSA numbering, the decision of what is a
// statement and what is spelled inline, the uniform partition, the counts, the debug map from a
// program location back to a node, and the digest. Every one of those is a property of a
// hash-consed expression DAG rather than of a language.
//
// The domain's, and behind `SourceTarget`: the header, the signature, how a type is spelled, how a
// leaf is spelled, how an expression is spelled, and how a root is assigned. A domain whose
// programs are not source at all — a camera rig program, an animation program — implements no
// target and emits its own compact form instead. `Domain::source_target()` is how a caller asks.
//
// SSA VALUES ARE NUMBERED BY POSITION IN THE EMITTED ORDER, never by `NodeId`: numbering by node id
// leaks construction order into the generated source, which the material spike proved with an
// id-shifted module producing a different program.
//
// WHAT IS A STATEMENT AND WHAT IS NOT. An op whose descriptor says `inline_leaf` is spelled at its
// use site; every computed value gets a statement. That is not cosmetic — it is what makes a folded
// root produce no statement at all, so "this program does no work" is visible in the source rather
// than asserted about it.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/graph/expr.h>

#include <string_view>

namespace cy::graph {

/// Appends to an `Array<char>`, carrying one status rather than checking every call site.
class TextWriter {
public:
    explicit TextWriter(Array<char>& out) noexcept : out_(&out) {}

    void text(std::string_view value) noexcept;
    void number(u32 value) noexcept;
    void hex(u64 value) noexcept;
    /// A float literal two compilers spell identically. `%.9g` round-trips a `float` exactly, and
    /// the trailing `.0` keeps an integral value a float rather than an int.
    void literal(f32 value) noexcept;

    [[nodiscard]] Status status() const noexcept { return status_; }
    [[nodiscard]] usize size() const noexcept { return out_->size(); }

private:
    Array<char>* out_;
    Status status_ = ok();
};

struct EmitOptions {
    /// Emit the sub-DAG rooted here instead of the module's declared roots. This is a node preview,
    /// and it is the same emitter rather than a second one: an editor showing an approximation of
    /// the program it compiles is showing a second answer.
    NodeId preview_root = kInvalidNode;
    /// Off numbers SSA values by node id, which is what makes the switch observable.
    bool canonical_order = true;
    /// Group the values that are constant across the dispatch into their own block, so they can be
    /// hoisted into parameter data rather than recomputed per invocation.
    bool hoist_uniform = true;
    /// The domain's own program variant and quality tier. Both reach the emitted header and
    /// therefore the digest, which is what makes a variant a cook-key difference rather than a
    /// note.
    u32 variant = 0;
    u32 tier = 0;
    /// A domain-defined annotation the header carries, for the same reason.
    const char* annotation = nullptr;
};

/// Generated source, and the identity a cook key is built from.
struct GeneratedSource {
    explicit GeneratedSource(Allocator& allocator) noexcept
        : text(allocator), value_nodes(allocator) {}

    GeneratedSource(const GeneratedSource&) = delete;
    GeneratedSource& operator=(const GeneratedSource&) = delete;
    GeneratedSource(GeneratedSource&&) noexcept = default;
    GeneratedSource& operator=(GeneratedSource&&) noexcept = default;

    Array<char> text;
    /// THE DEBUG MAP: the IR node behind each SSA value, indexed by the number in its `vN` name.
    /// It is what an editor's "which node is this line?" reads, and what lets a preview and a final
    /// program be compared statement by statement rather than by eye.
    Array<NodeId> value_nodes;
    /// A content hash over `text`. Two compilations that produce this digest produce byte-identical
    /// source.
    u64 digest = 0;
    /// Where the function body begins and ends within `text`, so a preview can be compared against
    /// a final program without the differing header getting in the way.
    usize body_begin = 0;
    usize body_end = 0;
    u32 statements = 0;
    u32 hoisted_statements = 0;
    u32 samples = 0;
    u32 arithmetic = 0;
    u32 branches = 0;
    u32 aggregates = 0;
    /// The most SSA values live at once, which is what stands in for register pressure.
    u32 peak_live_values = 0;

    [[nodiscard]] std::string_view view() const noexcept { return {text.data(), text.size()}; }
    [[nodiscard]] std::string_view body() const noexcept {
        return view().substr(body_begin, body_end - body_begin);
    }
};

class SourceTarget;

/// What a `SourceTarget` is given while it writes one value: the module, the writer, and the one
/// operation it must not reimplement — spelling an operand, which is either an SSA name or an
/// inline leaf and never the target's choice.
class EmitView {
public:
    EmitView(const Module& module, const SourceTarget& target, Span<const u32> statement_index,
             TextWriter& writer) noexcept
        : module_(&module), target_(&target), statement_index_(statement_index), writer_(&writer) {}

    [[nodiscard]] const Module& module() const noexcept { return *module_; }
    [[nodiscard]] TextWriter& out() const noexcept { return *writer_; }
    /// Write the spelling of `id` where it is used.
    void operand(NodeId id) const noexcept;
    /// Write the spelling of `id`'s type.
    void type_name(TypeId type) const noexcept;

private:
    const Module* module_;
    const SourceTarget* target_;
    Span<const u32> statement_index_;
    TextWriter* writer_;
};

/// The domain's half of emission. See the note at the top of this file for the split.
class SourceTarget {
public:
    SourceTarget() = default;
    virtual ~SourceTarget() = default;
    SourceTarget(const SourceTarget&) = delete;
    SourceTarget& operator=(const SourceTarget&) = delete;
    SourceTarget(SourceTarget&&) = delete;
    SourceTarget& operator=(SourceTarget&&) = delete;

    virtual void header(TextWriter& writer, const Module& module,
                        const EmitOptions& options) const = 0;
    /// The signature, up to and including the opening brace and its newline. The body begins after
    /// whatever this writes.
    virtual void signature(TextWriter& writer, const Module& module, const EmitOptions& options,
                           bool preview) const = 0;
    /// How a type is spelled in this language.
    [[nodiscard]] virtual const char* type_spelling(const Module& module, TypeId type) const = 0;
    /// How an `inline_leaf` value is spelled at its use site.
    virtual void leaf(TextWriter& writer, const Module& module, NodeId id) const = 0;
    /// The right-hand side of a statement.
    virtual void expression(const EmitView& view, NodeId id) const = 0;
    /// The assignment of one declared root. `slot` is the domain's own root slot; in a preview it
    /// is always slot zero and `id` is the previewed node.
    virtual void assign_root(const EmitView& view, u32 slot, NodeId id, bool preview) const = 0;
    /// Whatever closes the function. The body ends before it.
    virtual void epilogue(TextWriter& writer) const = 0;

    /// The comment that opens the hoisted block, and the one that opens the per-invocation block.
    /// A target that wants neither returns `nullptr`, and no comment is written.
    [[nodiscard]] virtual const char* hoist_comment() const { return nullptr; }
    [[nodiscard]] virtual const char* varying_comment() const { return nullptr; }
};

/// Emit one program. Refused when the domain has no source target.
[[nodiscard]] Expected<GeneratedSource, Error> emit_program(const Module& module,
                                                            const SourceTarget& target,
                                                            const EmitOptions& options) noexcept;

}  // namespace cy::graph
