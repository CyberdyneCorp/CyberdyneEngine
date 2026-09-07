#include <cy/build/graph.h>

#include <algorithm>
#include <ranges>

namespace cy::build {
namespace {

struct KindName {
    NodeKind kind;
    const char* name;
};

constexpr KindName kKindNames[] = {
    {NodeKind::Unknown, "unknown"},   {NodeKind::Generate, "generate"},
    {NodeKind::Import, "import"},     {NodeKind::Cook, "cook"},
    {NodeKind::Shader, "shader"},     {NodeKind::Package, "package"},
    {NodeKind::Manifest, "manifest"},
};

[[nodiscard]] Error invalid(const char* message) noexcept {
    return Error{ErrorCode::InvalidArgument, message, 0};
}

}  // namespace

const char* node_kind_name(NodeKind kind) noexcept {
    for (const KindName& entry : kKindNames) {
        if (entry.kind == kind) {
            return entry.name;
        }
    }
    return "unknown";
}

Expected<NodeKind, Error> node_kind_from_name(std::string_view name) noexcept {
    for (const KindName& entry : kKindNames) {
        if (name == entry.name) {
            return entry.kind;
        }
    }
    return make_unexpected(invalid("not a node kind"));
}

bool is_project_relative(std::string_view name) noexcept {
    if (name.empty() || name.front() == '/' || name.front() == '\\') {
        return false;
    }
    if (name.size() >= 2 && name[1] == ':') {
        return false;  // a drive letter: absolute on the one platform that spells it that way
    }
    if (name.find('\\') != std::string_view::npos) {
        return false;  // one separator everywhere, or two machines resolve one name differently
    }
    // `..` may not appear as a whole segment. A name that escapes the project resolves to different
    // content depending on where the project sits, which is an absolute path wearing a disguise.
    usize start = 0;
    while (start <= name.size()) {
        const usize end = std::min(name.find('/', start), name.size());
        if (name.substr(start, end - start) == "..") {
            return false;
        }
        start = end + 1;
    }
    return true;
}

std::string_view NodeDesc::option(std::string_view key, std::string_view fallback) const noexcept {
    for (const NodeOption& candidate : options) {
        if (candidate.name == key) {
            return candidate.value;
        }
    }
    return fallback;
}

namespace {

/// Every name a node carries must be project-relative. Checked once, at declaration.
[[nodiscard]] Status check_names(const NodeDesc& desc) noexcept {
    const std::vector<std::string>* lists[] = {&desc.sources, &desc.outputs};
    for (const std::vector<std::string>* list : lists) {
        for (const std::string& name : *list) {
            if (!is_project_relative(name)) {
                return make_unexpected(
                    invalid("a node's source and output names must be project-relative"));
            }
        }
    }
    return ok();
}

}  // namespace

Expected<NodeId, Error> BuildGraph::add(NodeDesc desc) {
    if (finalized_) {
        return make_unexpected(invalid("the graph is finalised; add nodes before finalising"));
    }
    if (desc.name.empty() || desc.producer.empty()) {
        return make_unexpected(invalid("a node needs a name and a producer"));
    }
    if (desc.outputs.empty()) {
        return make_unexpected(invalid("a node with no declared output cannot be cached"));
    }
    if (Status named = check_names(desc); !named) {
        return make_unexpected(named.error());
    }
    if (by_name_.contains(desc.name)) {
        return make_unexpected(Error{ErrorCode::AlreadyExists, "duplicate node name", 0});
    }

    // Sorted here, once: §1.2's rule that options enter the key by name and never by insertion.
    std::ranges::sort(desc.options,
                      [](const NodeOption& a, const NodeOption& b) { return a.name < b.name; });

    const u32 index = static_cast<u32>(nodes_.size());
    by_name_.emplace(desc.name, index);
    nodes_.push_back(std::move(desc));
    upstreams_.emplace_back();
    downstreams_.emplace_back();
    return static_cast<NodeId>(index);
}

NodeId BuildGraph::find(std::string_view name) const noexcept {
    const auto found = by_name_.find(std::string(name));
    return found == by_name_.end() ? NodeId::Invalid : static_cast<NodeId>(found->second);
}

Status BuildGraph::resolve_edges() {
    for (u32 index = 0; index < nodes_.size(); ++index) {
        for (const std::string& upstream : nodes_[index].upstreams) {
            const NodeId id = find(upstream);
            if (id == NodeId::Invalid) {
                return make_unexpected(Error{ErrorCode::NotFound, "unknown upstream node", 0});
            }
            if (node_index(id) == index) {
                return make_unexpected(invalid("a node cannot consume itself"));
            }
            upstreams_[index].push_back(id);
            downstreams_[node_index(id)].push_back(static_cast<NodeId>(index));
        }
    }
    return ok();
}

Status BuildGraph::compute_order() {
    // Kahn's algorithm over insertion order: the ready set is scanned lowest index first, so the
    // order is a function of the description and not of a container's iteration.
    std::vector<u32> remaining(nodes_.size(), 0);
    for (u32 index = 0; index < nodes_.size(); ++index) {
        remaining[index] = static_cast<u32>(upstreams_[index].size());
    }

    order_.clear();
    order_.reserve(nodes_.size());
    std::vector<bool> emitted(nodes_.size(), false);
    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (u32 index = 0; index < nodes_.size(); ++index) {
            if (emitted[index] || remaining[index] != 0) {
                continue;
            }
            emitted[index] = true;
            progressed = true;
            order_.push_back(static_cast<NodeId>(index));
            for (const NodeId down : downstreams_[index]) {
                --remaining[node_index(down)];
            }
        }
    }

    if (order_.size() != nodes_.size()) {
        return make_unexpected(invalid("the graph has a cycle"));
    }
    return ok();
}

Status BuildGraph::finalize() {
    if (finalized_) {
        return ok();
    }
    if (Status resolved = resolve_edges(); !resolved) {
        return resolved;
    }
    if (Status ordered = compute_order(); !ordered) {
        return ordered;
    }
    finalized_ = true;
    return ok();
}

std::vector<NodeId> BuildGraph::dependents(NodeId id) const {
    std::vector<bool> reached(nodes_.size(), false);
    std::vector<NodeId> frontier{id};
    while (!frontier.empty()) {
        const NodeId current = frontier.back();
        frontier.pop_back();
        for (const NodeId down : downstreams_[node_index(current)]) {
            if (!reached[node_index(down)]) {
                reached[node_index(down)] = true;
                frontier.push_back(down);
            }
        }
    }

    std::vector<NodeId> result;
    for (const NodeId candidate : order_) {
        if (reached[node_index(candidate)]) {
            result.push_back(candidate);
        }
    }
    return result;
}

std::vector<NodeId> BuildGraph::dependents_of_source(std::string_view source) const {
    std::vector<bool> included(nodes_.size(), false);
    for (u32 index = 0; index < nodes_.size(); ++index) {
        const NodeDesc& desc = nodes_[index];
        if (std::ranges::find(desc.sources, source) == desc.sources.end()) {
            continue;
        }
        included[index] = true;
        for (const NodeId down : dependents(static_cast<NodeId>(index))) {
            included[node_index(down)] = true;
        }
    }

    std::vector<NodeId> result;
    for (const NodeId candidate : order_) {
        if (included[node_index(candidate)]) {
            result.push_back(candidate);
        }
    }
    return result;
}

std::vector<NodeId> BuildGraph::reference_chain(NodeId id) const {
    // "Why is this in the build?" is answered from a DELIVERY ROOT downward: the package that was
    // asked for needs a cook, which needs this import. So the walk goes along `downstreams_` until
    // it reaches a node nothing consumes, and the chain is then read back from that node to `id`.
    //
    // Breadth-first, so the answer is the SHORTEST chain: an asset pulled in both directly and
    // through six intermediaries should report the direct route rather than whichever route the
    // traversal happened to take first.
    std::vector<NodeId> came_from(nodes_.size(), NodeId::Invalid);
    std::vector<bool> seen(nodes_.size(), false);
    std::vector<NodeId> queue{id};
    seen[node_index(id)] = true;
    NodeId root = NodeId::Invalid;

    for (usize head = 0; head < queue.size() && root == NodeId::Invalid; ++head) {
        const NodeId current = queue[head];
        if (downstreams_[node_index(current)].empty()) {
            root = current;
            break;
        }
        for (const NodeId down : downstreams_[node_index(current)]) {
            if (!seen[node_index(down)]) {
                seen[node_index(down)] = true;
                came_from[node_index(down)] = current;
                queue.push_back(down);
            }
        }
    }

    // `came_from` points towards `id`, so walking it from the root yields root … id in order.
    std::vector<NodeId> chain;
    for (NodeId step = root; step != NodeId::Invalid; step = came_from[node_index(step)]) {
        chain.push_back(step);
        if (step == id) {
            break;
        }
    }
    return chain;
}

std::vector<NodeId> BuildGraph::roots() const {
    std::vector<NodeId> result;
    for (const NodeId candidate : order_) {
        if (downstreams_[node_index(candidate)].empty()) {
            result.push_back(candidate);
        }
    }
    return result;
}

}  // namespace cy::build
