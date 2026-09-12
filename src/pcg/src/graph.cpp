// The authoring graph. See include/cy/pcg/graph.h.

#include <cy/pcg/graph.h>

#include <cstring>

namespace cy::pcg {

namespace {

[[nodiscard]] bool same_name(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    return std::strcmp(a, b) == 0;
}

/// The most nodes a program can address. `Stage::inputs` is a `u8` index into the stage array, so
/// 254 is the ceiling and 0xFF is `Program::kNoStage`. A generator larger than this is a generator
/// that should be subgraphs.
constexpr usize kMaxGraphNodes = 254;

}  // namespace

Expected<NodeId, Error> Graph::add(const GraphNode& node) noexcept {
    if (node.name == nullptr || node.name[0] == '\0') {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "pcg: a graph node must have a name; identity derives from it "
                                     "and every diagnostic prints it"});
    }
    if (nodes_.size() >= kMaxGraphNodes) {
        return make_unexpected(
            Error{ErrorCode::OutOfRange, "pcg: too many nodes in one graph; use subgraphs"});
    }
    if (node.input_count > kMaxNodeInputs) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "pcg: a node declares more inputs than it can hold"});
    }
    if (Status pushed = nodes_.push_back(node); !pushed) {
        return make_unexpected(pushed.error());
    }
    return NodeId{static_cast<u32>(nodes_.size())};
}

const GraphNode* Graph::find(NodeId id) const noexcept {
    if (!id.is_valid() || id.value > nodes_.size()) {
        return nullptr;
    }
    return &nodes_[id.value - 1];
}

const GraphNode* Graph::find_by_name(const char* name) const noexcept {
    for (const GraphNode& node : nodes_) {
        if (same_name(node.name, name)) {
            return &node;
        }
    }
    return nullptr;
}

Expected<const char*, Error> Graph::own_name(const char* instance, const char* name) noexcept {
    const usize prefix = std::strlen(instance);
    const usize body = std::strlen(name);
    Expected<Array<char>*, Error> slot = params_.emplace_back(nodes_.allocator());
    if (!slot) {
        return make_unexpected(slot.error());
    }
    Array<char>& owned = **slot;
    if (Status sized = owned.resize(prefix + body + 2); !sized) {
        return make_unexpected(sized.error());
    }
    std::memcpy(owned.data(), instance, prefix);
    owned[prefix] = '.';
    std::memcpy(owned.data() + prefix + 1, name, body);
    owned[prefix + body + 1] = '\0';
    return owned.data();
}

Status Graph::instantiate(const Graph& subgraph, const char* instance,
                          Span<const SubgraphParam> bindings) noexcept {
    if (instance == nullptr || instance[0] == '\0') {
        return Status{make_unexpected(
            Error{ErrorCode::InvalidArgument,
                  "pcg: a subgraph instantiation must be named; two instantiations of one subgraph "
                  "must mint different identities, and the name is what separates them"})};
    }
    // `params_` hands out pointers into its elements, and `Array` relocates on growth — so the
    // owned-name arena is reserved for every node this call will add BEFORE any of them is created.
    // Each name is its own `Array<char>`, whose buffer does not move when the outer array does.
    if (Status reserved = params_.reserve(params_.size() + subgraph.nodes_.size()); !reserved) {
        return reserved;
    }
    const u32 base = static_cast<u32>(nodes_.size());
    for (const GraphNode& source : subgraph.nodes_) {
        GraphNode copy = source;
        Expected<const char*, Error> owned = own_name(instance, source.name);
        if (!owned) {
            return Status{make_unexpected(owned.error())};
        }
        copy.name = *owned;
        // Edges are subgraph-local and are rebased, so an inlined subgraph's nodes read each other
        // and never reach into the host graph by accident.
        for (u8 index = 0; index < copy.input_count; ++index) {
            if (copy.inputs[index].is_valid()) {
                copy.inputs[index] = NodeId{copy.inputs[index].value + base};
            }
        }
        // Attributes are interned into the HOST's table, so two instantiations writing `density`
        // write one column. That is the composition the specification's hierarchy scenario wants —
        // climate produces biomes, biomes produce forests — and a per-instance column would make
        // each stage read a name the previous one did not write.
        if (copy.output.is_valid()) {
            const AttributeDecl* declaration = subgraph.attributes_.find(copy.output);
            if (declaration == nullptr) {
                return Status{make_unexpected(
                    Error{ErrorCode::NotFound,
                          "pcg: a subgraph node writes an attribute its own table does not hold"})};
            }
            Expected<AttributeId, Error> interned =
                attributes_.intern(declaration->name, declaration->type);
            if (!interned) {
                return Status{make_unexpected(interned.error())};
            }
            copy.output = *interned;
        }
        if (copy.reads.is_valid()) {
            const AttributeDecl* declaration = subgraph.attributes_.find(copy.reads);
            if (declaration == nullptr) {
                return Status{make_unexpected(
                    Error{ErrorCode::NotFound,
                          "pcg: a subgraph node reads an attribute its own table does not hold"})};
            }
            Expected<AttributeId, Error> interned =
                attributes_.intern(declaration->name, declaration->type);
            if (!interned) {
                return Status{make_unexpected(interned.error())};
            }
            copy.reads = *interned;
        }
        Expected<NodeId, Error> added = add(copy);
        if (!added) {
            return Status{make_unexpected(added.error())};
        }
    }
    // Exposed parameters, applied after the copy so that a binding names a node by its SUBGRAPH
    // identifier — which is what an author has in front of them — rather than by an index into the
    // host graph they cannot see.
    for (const SubgraphParam& binding : bindings) {
        if (!binding.target.is_valid() || binding.target.value > subgraph.nodes_.size()) {
            return Status{make_unexpected(
                Error{ErrorCode::NotFound,
                      "pcg: a subgraph parameter binds a node the subgraph does not hold"})};
        }
        nodes_[base + binding.target.value - 1].params.value = binding.value;
    }
    return ok();
}

}  // namespace cy::pcg
