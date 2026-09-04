#include <cy/scene/serialization/library.h>

#include <algorithm>

namespace cy::scene::serialization {
namespace {

/// Where the walk has got to with one document.
enum class Colour : u8 {
    Unseen = 0,
    /// On the stack: reaching it again is a cycle.
    Open = 1,
    /// Finished: reaching it again is a diamond, which is legal and common.
    Closed = 2,
};

/// One frame of the explicit stack. `next` is how far through the node's dependencies the walk is,
/// which is what makes the traversal resumable without recursion.
struct Frame {
    AssetId id;
    usize next = 0;
};

[[nodiscard]] bool contains(Span<const AssetId> ids, AssetId id) noexcept {
    return std::ranges::any_of(ids, [id](AssetId candidate) noexcept { return candidate == id; });
}

}  // namespace

Status direct_dependencies(const Document& document, Array<AssetId>& out) noexcept {
    out.clear();
    if (document.is_variant()) {
        if (Status added = out.push_back(document.base()); !added) {
            return added;
        }
    }
    for (const Instance& instance : document.instances()) {
        if (contains(out.span(), instance.source)) {
            continue;  // Two instances of one prefab are one edge in the dependency graph.
        }
        if (Status added = out.push_back(instance.source); !added) {
            return added;
        }
    }
    return ok();
}

Status Library::add(Document& document) noexcept {
    if (document.id.is_nil()) {
        return fail(ErrorCode::InvalidArgument, "a library document is addressed by its AssetId");
    }
    if (find(document.id) != nullptr) {
        return fail(ErrorCode::AlreadyExists,
                    "a document with that asset id is already registered");
    }
    return documents_.push_back(&document);
}

const Document* Library::find(AssetId id) const noexcept {
    return find_mutable(id);
}

Document* Library::find_mutable(AssetId id) const noexcept {
    for (Document* const document : documents_) {
        if (document->id == id) {
            return document;
        }
    }
    return nullptr;
}

Status Library::walk(AssetId root, Array<AssetId>* order, AssetChain& chain) const noexcept {
    chain.clear();
    if (order != nullptr) {
        order->clear();
    }
    if (find(root) == nullptr) {
        return fail(ErrorCode::NotFound, "the document is not registered in this library");
    }

    // Colours are held beside the library's document list, indexed the same way, so the walk needs
    // no map and no allocation per node beyond the stack itself.
    Array<Colour> colours(documents_.allocator());
    if (Status sized = colours.resize(documents_.size()); !sized) {
        return sized;
    }

    Array<Frame> stack(documents_.allocator());
    Array<AssetId> dependencies(documents_.allocator());

    const auto index_of = [this](AssetId id) noexcept -> usize {
        for (usize index = 0; index < documents_.size(); ++index) {
            if (documents_[index]->id == id) {
                return index;
            }
        }
        return documents_.size();
    };

    if (Status pushed = stack.push_back(Frame{root, 0}); !pushed) {
        return pushed;
    }
    colours[index_of(root)] = Colour::Open;
    if (Status recorded = chain.push_back(root); !recorded) {
        return recorded;
    }

    while (!stack.empty()) {
        Frame& frame = stack.back();
        const Document* document = find(frame.id);
        if (document == nullptr) {
            // A dangling reference is not a cycle, and it is not this function's error to report:
            // resolution fails on it with a message naming the missing asset. Treat it as a leaf.
            colours[index_of(frame.id)] = Colour::Closed;
            stack.pop_back();
            if (!chain.empty()) {
                chain.pop_back();
            }
            continue;
        }
        if (Status listed = direct_dependencies(*document, dependencies); !listed) {
            return listed;
        }
        if (frame.next >= dependencies.size()) {
            colours[index_of(frame.id)] = Colour::Closed;
            if (order != nullptr) {
                if (Status added = order->push_back(frame.id); !added) {
                    return added;
                }
            }
            stack.pop_back();
            if (!chain.empty()) {
                chain.pop_back();
            }
            continue;
        }

        const AssetId next = dependencies[frame.next];
        ++frame.next;

        const usize next_index = index_of(next);
        if (next_index == documents_.size()) {
            continue;  // Unregistered: reported by resolution, not here.
        }
        if (colours[next_index] == Colour::Open) {
            // The chain currently on the stack, plus the edge that closes it. Reported as a chain
            // rather than as a name, because "A is in a cycle" is not enough to fix it.
            if (Status closed = chain.push_back(next); !closed) {
                return closed;
            }
            return fail(ErrorCode::InvalidArgument, "the asset dependency graph contains a cycle");
        }
        if (colours[next_index] == Colour::Closed) {
            continue;
        }
        colours[next_index] = Colour::Open;
        if (Status pushed = stack.push_back(Frame{next, 0}); !pushed) {
            return pushed;
        }
        if (Status recorded = chain.push_back(next); !recorded) {
            return recorded;
        }
    }

    chain.clear();
    return ok();
}

Status Library::dependency_order(AssetId root, Array<AssetId>& out,
                                 AssetChain& chain) const noexcept {
    return walk(root, &out, chain);
}

Status Library::validate(AssetChain& chain) const noexcept {
    for (const Document* const document : documents_) {
        if (Status walked = walk(document->id, nullptr, chain); !walked) {
            return walked;
        }
    }
    chain.clear();
    return ok();
}

Status Library::check_placement(AssetId container, AssetId candidate,
                                AssetChain& chain) const noexcept {
    chain.clear();
    if (container == candidate) {
        if (Status added = chain.push_back(container); !added) {
            return added;
        }
        if (Status added = chain.push_back(candidate); !added) {
            return added;
        }
        return fail(ErrorCode::AlreadyExists, "a document cannot contain an instance of itself");
    }
    if (find(candidate) == nullptr) {
        return fail(ErrorCode::NotFound, "the document being placed is not registered");
    }

    // The placement closes a cycle exactly when `container` is already reachable from `candidate`.
    Array<AssetId> reachable(documents_.allocator());
    if (Status walked = walk(candidate, &reachable, chain); !walked) {
        return walked;  // The candidate is already in a cycle of its own; report that one.
    }
    if (contains(reachable.span(), container)) {
        if (Status added = chain.push_back(container); !added) {
            return added;
        }
        if (Status added = chain.push_back(candidate); !added) {
            return added;
        }
        if (Status added = chain.push_back(container); !added) {
            return added;
        }
        return fail(ErrorCode::AlreadyExists,
                    "placing this document here would close a dependency cycle");
    }
    return ok();
}

Expected<u32, Error> Library::variant_depth(AssetId id, AssetChain& chain) const noexcept {
    chain.clear();
    u32 depth = 0;
    AssetId current = id;
    while (true) {
        const Document* document = find(current);
        if (document == nullptr) {
            return fail(ErrorCode::NotFound, "a document in the variant chain is not registered");
        }
        if (Status added = chain.push_back(current); !added) {
            return make_unexpected(added.error());
        }
        if (!document->is_variant()) {
            return depth;
        }
        if (contains(chain.span(), document->base())) {
            if (Status added = chain.push_back(document->base()); !added) {
                return make_unexpected(added.error());
            }
            return fail(ErrorCode::InvalidArgument, "the variant chain contains a cycle");
        }
        current = document->base();
        ++depth;
    }
}

Expected<bool, Error> Library::variant_depth_exceeds_recommendation(
    AssetId id, AssetChain& chain) const noexcept {
    const Expected<u32, Error> depth = variant_depth(id, chain);
    if (!depth) {
        return make_unexpected(depth.error());
    }
    return depth.value() > recommended_depth_;
}

}  // namespace cy::scene::serialization
