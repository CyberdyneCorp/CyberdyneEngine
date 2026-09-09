// Source emission from the shared expression core. Task 2.2. See emit.h for the split between what
// is shared here and what is the domain's.

#include <cy/graph/emit.h>

#include <cstdio>
#include <utility>

namespace cy::graph {

void TextWriter::text(std::string_view value) noexcept {
    if (!status_) {
        return;
    }
    for (const char character : value) {
        if (Status pushed = out_->push_back(character); !pushed) {
            status_ = pushed;
            return;
        }
    }
}

void TextWriter::number(u32 value) noexcept {
    char buffer[16] = {};
    (void)std::snprintf(buffer, sizeof(buffer), "%u", value);
    text(buffer);
}

void TextWriter::hex(u64 value) noexcept {
    char buffer[24] = {};
    (void)std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    text(buffer);
}

void TextWriter::literal(f32 value) noexcept {
    char buffer[32] = {};
    (void)std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    text(buffer);
    bool fractional = false;
    for (const char character : buffer) {
        fractional = fractional || character == '.' || character == 'e' || character == 'n';
    }
    if (!fractional) {
        text(".0");
    }
}

void EmitView::operand(NodeId id) const noexcept {
    if (module_->domain().op_desc(module_->node(id).op).inline_leaf) {
        target_->leaf(*writer_, *module_, id);
        return;
    }
    writer_->text("v");
    writer_->number(statement_index_[id]);
}

void EmitView::type_name(TypeId type) const noexcept {
    writer_->text(target_->type_spelling(*module_, type));
}

namespace {

[[nodiscard]] bool is_statement(const Module& module, NodeId id) noexcept {
    return !module.domain().op_desc(module.node(id).op).inline_leaf;
}

void count_node(const Module& module, NodeId id, GeneratedSource& source) noexcept {
    switch (module.domain().op_desc(module.node(id).op).category) {
        case OpCategory::Sample:
            ++source.samples;
            return;
        case OpCategory::Branch:
            ++source.branches;
            return;
        case OpCategory::Aggregate:
            ++source.aggregates;
            return;
        case OpCategory::Arithmetic:
        case OpCategory::Leaf:
            break;
    }
    ++source.arithmetic;
}

/// Sort one segment of the emitted order so that the hoistable values come first. A STABLE
/// partition, so the relative order within each half is the canonical order unchanged.
[[nodiscard]] Status partition_uniform(const Module& module, const EmitOptions& options,
                                       Array<NodeId>& order, u32& hoisted) noexcept {
    if (!options.hoist_uniform) {
        return ok();
    }
    Array<NodeId> partitioned(module.allocator());
    if (Status reserved = partitioned.reserve(order.size()); !reserved) {
        return reserved;
    }
    for (u32 pass = 0; pass < 2U; ++pass) {
        for (const NodeId id : order) {
            const bool uniform = (module.flags(id) & kFlagUniform) != 0;
            if ((pass == 0) == uniform) {
                if (Status pushed = partitioned.push_back(id); !pushed) {
                    return pushed;
                }
                hoisted += pass == 0 && is_statement(module, id) ? 1U : 0U;
            }
        }
    }
    order = std::move(partitioned);
    return ok();
}

/// The emitted order, in ONE SEGMENT PER ROOT, in slot order.
///
/// The segmentation is what makes the preview relationship exact. A single walk over every root
/// would let a value only a later root reaches be hoisted ahead of an earlier root's value, which
/// shifts every SSA number after it — and a preview of the first root would then no longer be the
/// program's body with the later assignments removed. Segmenting costs one walk per root and buys a
/// property a test can state byte for byte.
[[nodiscard]] Status build_order(const Module& module, const EmitOptions& options,
                                 Span<const NodeId> roots, Array<NodeId>& order,
                                 u32& hoisted) noexcept {
    hoisted = 0;
    order.clear();
    Array<NodeId> segment(module.allocator());
    Array<u8> emitted(module.allocator());
    if (Status sized = emitted.resize(module.size()); !sized) {
        return sized;
    }
    for (u8& mark : emitted) {
        mark = 0;
    }
    for (const NodeId root : roots) {
        if (root == kInvalidNode) {
            continue;
        }
        if (Status walked = canonical_order(module, Span<const NodeId>(&root, 1), segment);
            !walked) {
            return walked;
        }
        // Values an earlier segment already emitted keep their number and are not repeated.
        usize kept = 0;
        for (usize index = 0; index < segment.size(); ++index) {
            if (emitted[segment[index]] == 0) {
                emitted[segment[index]] = 1;
                segment[kept++] = segment[index];
            }
        }
        while (segment.size() > kept) {
            segment.pop_back();
        }
        if (Status partitioned = partition_uniform(module, options, segment, hoisted);
            !partitioned) {
            return partitioned;
        }
        if (Status appended = order.append(segment.span()); !appended) {
            return appended;
        }
    }
    return ok();
}

/// Number by node id rather than by emitted position. The `canonical_emission_order` switch off,
/// and the reason it changes the program.
void order_by_node_id(Array<NodeId>& order) noexcept {
    for (usize outer = 1; outer < order.size(); ++outer) {
        for (usize inner = outer; inner > 0 && order[inner - 1] > order[inner]; --inner) {
            const NodeId swap = order[inner - 1];
            order[inner - 1] = order[inner];
            order[inner] = swap;
        }
    }
}

/// The block comments that mark where the hoisted values end and the per-invocation ones begin.
class BlockMarks {
public:
    BlockMarks(const SourceTarget& target, bool enabled) noexcept
        : target_(&target), enabled_(enabled) {}

    void before(TextWriter& writer, bool uniform) noexcept {
        if (!enabled_) {
            return;
        }
        if (uniform && !hoisted_) {
            write(writer, target_->hoist_comment());
            hoisted_ = true;
        }
        if (!uniform && hoisted_ && !varying_) {
            write(writer, target_->varying_comment());
            varying_ = true;
        }
    }

private:
    static void write(TextWriter& writer, const char* comment) noexcept {
        if (comment != nullptr) {
            writer.text(comment);
        }
    }

    const SourceTarget* target_;
    bool enabled_;
    bool hoisted_ = false;
    bool varying_ = false;
};

/// Write the function, from its signature to whatever closes it.
///
/// A FUNCTION OF ITS OWN so that the `TextWriter` — which holds a pointer into `source.text` —
/// cannot outlive any early return out of `emit_program`.
[[nodiscard]] Status write_program(const Module& module, const SourceTarget& target,
                                   const EmitOptions& options, Span<const NodeId> order,
                                   Array<u32>& statement_index, GeneratedSource& source) noexcept {
    TextWriter writer(source.text);
    const bool preview = options.preview_root != kInvalidNode;
    target.header(writer, module, options);
    target.signature(writer, module, options, preview);
    source.body_begin = writer.size();

    const EmitView view(module, target, statement_index.span(), writer);
    BlockMarks marks(target, options.hoist_uniform);
    u32 next = 0;
    for (const NodeId id : order) {
        if (!is_statement(module, id)) {
            continue;
        }
        marks.before(writer, (module.flags(id) & kFlagUniform) != 0);
        statement_index[id] = next;
        if (Status pushed = source.value_nodes.push_back(id); !pushed) {
            return pushed;
        }
        writer.text("    ");
        view.type_name(module.node(id).type);
        writer.text(" v");
        writer.number(next);
        writer.text(" = ");
        target.expression(view, id);
        writer.text(";\n");
        count_node(module, id, source);
        ++next;
        ++source.statements;
    }
    source.peak_live_values = next;

    if (preview) {
        target.assign_root(view, 0, options.preview_root, true);
    } else {
        const Span<const NodeId> roots = module.roots();
        for (usize slot = 0; slot < roots.size(); ++slot) {
            if (roots[slot] != kInvalidNode) {
                target.assign_root(view, static_cast<u32>(slot), roots[slot], false);
            }
        }
    }
    source.body_end = writer.size();
    target.epilogue(writer);
    // A FRESH ERROR RATHER THAN THE WRITER'S. `TextWriter` holds a pointer into `source.text`, and
    // propagating a `Status` out of it makes the static analyser believe an address of this
    // function's frame travels with the error and reaches the caller.
    if (!writer.status()) {
        return fail(ErrorCode::OutOfMemory, "the generated source could not be grown");
    }
    return ok();
}

}  // namespace

Expected<GeneratedSource, Error> emit_program(const Module& module, const SourceTarget& target,
                                              const EmitOptions& options) noexcept {
    Allocator& allocator = module.allocator();
    const bool preview = options.preview_root != kInvalidNode;

    GeneratedSource source(allocator);
    Array<NodeId> order(allocator);
    Array<NodeId> preview_roots(allocator);
    if (preview) {
        if (Status pushed = preview_roots.push_back(options.preview_root); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    const Span<const NodeId> roots = preview ? preview_roots.span() : module.roots();
    if (Status built = build_order(module, options, roots, order, source.hoisted_statements);
        !built) {
        return make_unexpected(built.error());
    }
    if (!options.canonical_order) {
        order_by_node_id(order);
    }

    Array<u32> statement_index(allocator);
    if (Status sized = statement_index.resize(module.size()); !sized) {
        return make_unexpected(sized.error());
    }
    for (u32& number : statement_index) {
        number = kInvalidNode;
    }

    if (Status written =
            write_program(module, target, options, order.span(), statement_index, source);
        !written) {
        return make_unexpected(written.error());
    }
    source.digest = hash_bytes(kHashSeed, source.text.data(), source.text.size());
    return source;
}

}  // namespace cy::graph
