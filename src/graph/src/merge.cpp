// CyberGraph's semantic diff, three-way merge and migration. Task 2.1.
//
// See merge.h for why the unit of a diff is a change keyed by node and pin rather than a line.

#include <cy/graph/merge.h>

#include <algorithm>
#include <utility>

namespace cy::graph {
namespace {

[[nodiscard]] bool has_link(const Graph& graph, const Link& link) noexcept {
    return std::ranges::any_of(graph.links(),
                               [&link](const Link& existing) noexcept { return existing == link; });
}

[[nodiscard]] const Literal* property_of(const Graph& graph, NodeKey key, Name name) noexcept {
    return graph.property(key, name);
}

[[nodiscard]] Status push(Array<Change>& out, ChangeKind kind, NodeKey node, Name detail,
                          const Link& link = Link{}) noexcept {
    Change change;
    change.kind = kind;
    change.node = node;
    change.detail = detail;
    change.link = link;
    return out.push_back(change);
}

[[nodiscard]] Status diff_node(const Graph& base, const Graph& other, const GraphNode& node,
                               Array<Change>& out) noexcept {
    const GraphNode* before = base.find_node(node.key);
    if (before == nullptr) {
        return push(out, ChangeKind::NodeAdded, node.key, node.type);
    }
    if (before->type != node.type) {
        if (Status pushed = push(out, ChangeKind::NodeRetyped, node.key, node.type); !pushed) {
            return pushed;
        }
    }
    if (before->version != node.version) {
        if (Status pushed = push(out, ChangeKind::NodeVersioned, node.key, node.type); !pushed) {
            return pushed;
        }
    }
    if (before->muted != node.muted) {
        if (Status pushed = push(out, ChangeKind::NodeMuted, node.key, Name{}); !pushed) {
            return pushed;
        }
    }
    if (before->subgraph != node.subgraph) {
        if (Status pushed = push(out, ChangeKind::NodeSubgraphChanged, node.key, node.subgraph);
            !pushed) {
            return pushed;
        }
    }
    for (const Property& property : other.properties(node.key)) {
        const Literal* was = property_of(base, node.key, property.name);
        if (was == nullptr || *was != property.value) {
            if (Status pushed = push(out, ChangeKind::PropertySet, node.key, property.name);
                !pushed) {
                return pushed;
            }
        }
    }
    for (const Property& property : base.properties(node.key)) {
        if (property_of(other, node.key, property.name) == nullptr) {
            if (Status pushed = push(out, ChangeKind::PropertyRemoved, node.key, property.name);
                !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

/// Whether `changes` touches the same thing a candidate change does. This is the whole conflict
/// rule: two changes collide when they name one node and one detail.
[[nodiscard]] bool touches(Span<const Change> changes, const Change& candidate) noexcept {
    for (const Change& change : changes) {
        if (change.node != candidate.node) {
            continue;
        }
        switch (candidate.kind) {
            case ChangeKind::PropertySet:
            case ChangeKind::PropertyRemoved:
                if ((change.kind == ChangeKind::PropertySet ||
                     change.kind == ChangeKind::PropertyRemoved) &&
                    change.detail == candidate.detail) {
                    return true;
                }
                break;
            case ChangeKind::NodeRemoved:
                return true;
            default:
                if (change.kind == candidate.kind || change.kind == ChangeKind::NodeRemoved) {
                    return true;
                }
                break;
        }
    }
    return false;
}

[[nodiscard]] Status conflict(MergeReport& report, const Change& change,
                              const char* message) noexcept {
    Conflict entry;
    entry.kind = change.kind;
    entry.node = change.node;
    entry.detail = change.detail;
    entry.message = message;
    return report.conflicts.push_back(entry);
}

/// Apply one change taken from `theirs` onto `result`.
[[nodiscard]] Status apply(Graph& result, const Graph& theirs, const Change& change) noexcept {
    switch (change.kind) {
        case ChangeKind::NodeAdded: {
            const GraphNode* node = theirs.find_node(change.node);
            if (node == nullptr) {
                return ok();
            }
            if (Status added = result.add_node(node->key, node->type, node->version); !added) {
                return added;
            }
            if (node->muted) {
                if (Status muted = result.mute(node->key, true); !muted) {
                    return muted;
                }
            }
            if (node->subgraph != Name{}) {
                if (Status set = result.set_subgraph(node->key, node->subgraph); !set) {
                    return set;
                }
            }
            for (const Property& property : theirs.properties(node->key)) {
                if (Status set = result.set_property(node->key, property.name, property.value);
                    !set) {
                    return set;
                }
            }
            const std::string_view body = theirs.opaque_body(node->key);
            return body.empty() ? ok() : result.set_opaque_body(node->key, body);
        }
        case ChangeKind::NodeRemoved:
            return result.find_node(change.node) == nullptr ? ok()
                                                            : result.remove_node(change.node);
        case ChangeKind::NodeMuted: {
            const GraphNode* node = theirs.find_node(change.node);
            return node == nullptr ? ok() : result.mute(change.node, node->muted);
        }
        case ChangeKind::NodeSubgraphChanged: {
            const GraphNode* node = theirs.find_node(change.node);
            return node == nullptr ? ok() : result.set_subgraph(change.node, node->subgraph);
        }
        case ChangeKind::NodeRetyped:
        case ChangeKind::NodeVersioned: {
            const GraphNode* node = theirs.find_node(change.node);
            GraphNode* target = result.find_node(change.node);
            if (node == nullptr || target == nullptr) {
                return ok();
            }
            target->type = node->type;
            target->version = node->version;
            return ok();
        }
        case ChangeKind::PropertySet: {
            const Literal* value = theirs.property(change.node, change.detail);
            return value == nullptr ? ok()
                                    : result.set_property(change.node, change.detail, *value);
        }
        case ChangeKind::PropertyRemoved:
            // A removed property is a value returning to its node type's default. There is no
            // erase on the property table by design — a graph with a property nobody declares is
            // the same graph — so the merge records it and leaves the value in place.
            return ok();
        case ChangeKind::LinkAdded:
            return has_link(result, change.link)
                       ? ok()
                       : result.connect(change.link.from, change.link.from_pin, change.link.to,
                                        change.link.to_pin);
        case ChangeKind::LinkRemoved:
            return has_link(result, change.link)
                       ? result.disconnect(change.link.to, change.link.to_pin, change.link.from,
                                           change.link.from_pin)
                       : ok();
        case ChangeKind::LayoutMoved: {
            const NodeLayout* layout = theirs.layout(change.node);
            return layout == nullptr ? ok() : result.set_layout(*layout);
        }
        case ChangeKind::GraphRenamed:
            result.set_name(theirs.name());
            return ok();
        case ChangeKind::CapabilitiesChanged:
            result.grant(theirs.granted());
            return ok();
        case ChangeKind::DeterminismClaimChanged:
            result.set_claims_deterministic(theirs.claims_deterministic());
            return ok();
    }
    return ok();
}

}  // namespace

const char* change_kind_name(ChangeKind kind) noexcept {
    switch (kind) {
        case ChangeKind::NodeAdded:
            return "node_added";
        case ChangeKind::NodeRemoved:
            return "node_removed";
        case ChangeKind::NodeRetyped:
            return "node_retyped";
        case ChangeKind::NodeVersioned:
            return "node_versioned";
        case ChangeKind::NodeMuted:
            return "node_muted";
        case ChangeKind::NodeSubgraphChanged:
            return "node_subgraph_changed";
        case ChangeKind::PropertySet:
            return "property_set";
        case ChangeKind::PropertyRemoved:
            return "property_removed";
        case ChangeKind::LinkAdded:
            return "link_added";
        case ChangeKind::LinkRemoved:
            return "link_removed";
        case ChangeKind::LayoutMoved:
            return "layout_moved";
        case ChangeKind::GraphRenamed:
            return "graph_renamed";
        case ChangeKind::CapabilitiesChanged:
            return "capabilities_changed";
        case ChangeKind::DeterminismClaimChanged:
            return "determinism_claim_changed";
    }
    return "?";
}

Status diff(const Graph& base, const Graph& other, Array<Change>& out) noexcept {
    if (base.name() != other.name()) {
        if (Status pushed = push(out, ChangeKind::GraphRenamed, kInvalidNodeKey, other.name());
            !pushed) {
            return pushed;
        }
    }
    if (base.granted() != other.granted()) {
        if (Status pushed = push(out, ChangeKind::CapabilitiesChanged, kInvalidNodeKey, Name{});
            !pushed) {
            return pushed;
        }
    }
    if (base.claims_deterministic() != other.claims_deterministic()) {
        if (Status pushed = push(out, ChangeKind::DeterminismClaimChanged, kInvalidNodeKey, Name{});
            !pushed) {
            return pushed;
        }
    }
    for (const GraphNode& node : other.nodes()) {
        if (Status diffed = diff_node(base, other, node, out); !diffed) {
            return diffed;
        }
    }
    for (const GraphNode& node : base.nodes()) {
        if (other.find_node(node.key) == nullptr) {
            if (Status pushed = push(out, ChangeKind::NodeRemoved, node.key, node.type); !pushed) {
                return pushed;
            }
        }
    }
    for (const Link& link : other.links()) {
        if (!has_link(base, link)) {
            if (Status pushed = push(out, ChangeKind::LinkAdded, link.to, link.to_pin, link);
                !pushed) {
                return pushed;
            }
        }
    }
    for (const Link& link : base.links()) {
        if (!has_link(other, link)) {
            if (Status pushed = push(out, ChangeKind::LinkRemoved, link.to, link.to_pin, link);
                !pushed) {
                return pushed;
            }
        }
    }
    for (const NodeLayout& layout : other.layouts()) {
        const NodeLayout* before = base.layout(layout.key);
        const bool moved = before == nullptr || before->x != layout.x || before->y != layout.y ||
                           before->tint != layout.tint || before->comment != layout.comment;
        if (moved) {
            if (Status pushed = push(out, ChangeKind::LayoutMoved, layout.key, Name{}); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Expected<Graph, Error> merge3(const Graph& base, const Graph& ours, const Graph& theirs,
                              MergeReport& report) noexcept {
    Allocator& allocator = ours.allocator();
    auto merged = ours.clone(allocator);
    if (!merged) {
        return merged;
    }

    Array<Change> our_changes(allocator);
    Array<Change> their_changes(allocator);
    if (Status diffed = diff(base, ours, our_changes); !diffed) {
        return make_unexpected(diffed.error());
    }
    if (Status diffed = diff(base, theirs, their_changes); !diffed) {
        return make_unexpected(diffed.error());
    }

    for (const Change& change : their_changes) {
        // LAYOUT NEVER CONFLICTS (merge.h). Whoever moved a node last moved it.
        if (change.kind == ChangeKind::LayoutMoved) {
            if (Status applied = apply(merged.value(), theirs, change); !applied) {
                return make_unexpected(applied.error());
            }
            ++report.taken_from_theirs;
            continue;
        }
        // Two authors who made the SAME change agree; that is not a conflict and not a second
        // application either.
        if (change.kind == ChangeKind::LinkAdded && has_link(merged.value(), change.link)) {
            ++report.already_agreed;
            continue;
        }
        if (change.kind == ChangeKind::PropertySet) {
            const Literal* mine = merged.value().property(change.node, change.detail);
            const Literal* yours = theirs.property(change.node, change.detail);
            if (mine != nullptr && yours != nullptr && *mine == *yours) {
                ++report.already_agreed;
                continue;
            }
        }
        if (touches(our_changes.span(), change)) {
            if (Status recorded =
                    conflict(report, change,
                             "both sides changed this, and differently: the merge keeps OURS and "
                             "reports the change it did not take");
                !recorded) {
                return make_unexpected(recorded.error());
            }
            continue;
        }
        if (Status applied = apply(merged.value(), theirs, change); !applied) {
            return make_unexpected(applied.error());
        }
        ++report.taken_from_theirs;
    }
    return merged;
}

// --- Migration ------------------------------------------------------------------------------

Status MigrationTable::add(const MigrationRule& rule) noexcept {
    if (rule.apply == nullptr || rule.to_version <= rule.from_version) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "a migration rule carries a node forward, and needs a "
                                     "function to do it with",
                                     0});
    }
    return rules_.push_back(rule);
}

Expected<u32, Error> MigrationTable::migrate(Graph& graph, const NodeRegistry& registry,
                                             DiagnosticSink& sink) const noexcept {
    // The keys are snapshotted first: a rule may add or remove a node, and iterating the node
    // array while a rule mutates it would be a use-after-grow.
    Array<NodeKey> keys(graph.allocator());
    for (const GraphNode& node : graph.nodes()) {
        if (Status pushed = keys.push_back(node.key); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    u32 migrated = 0;
    for (const NodeKey key : keys) {
        const GraphNode* present = graph.find_node(key);
        if (present == nullptr) {
            continue;
        }
        const GraphNode node = *present;
        const NodeType* type = registry.find(node.type);
        if (type == nullptr) {
            // An unregistered node is not migrated and NOT DROPPED — cybergraph.h, decision 5.
            continue;
        }
        if (node.version > type->version()) {
            Diagnostic diagnostic;
            diagnostic.node = node.key;
            diagnostic.detail = node.type;
            diagnostic.message =
                "this node was authored by a NEWER build than the one reading it; there is no "
                "backward migration and the node is left exactly as it was";
            sink.report(diagnostic);
            continue;
        }
        u32 version = node.version;
        bool moved = false;
        while (version < type->version()) {
            const MigrationRule* step = nullptr;
            for (const MigrationRule& rule : rules_) {
                if (rule.type == node.type && rule.from_version == version) {
                    step = &rule;
                }
            }
            if (step == nullptr) {
                Diagnostic diagnostic;
                diagnostic.node = node.key;
                diagnostic.detail = node.type;
                diagnostic.message =
                    "no migration rule carries this node forward from the version it was authored "
                    "at; it is left alone rather than guessed at";
                sink.report(diagnostic);
                break;
            }
            if (Status applied = step->apply(graph, node.key, version); !applied) {
                return make_unexpected(applied.error());
            }
            version = step->to_version;
            moved = true;
        }
        if (moved) {
            GraphNode* target = graph.find_node(node.key);
            if (target != nullptr) {
                target->version = version;
            }
            ++migrated;
        }
    }
    return migrated;
}

}  // namespace cy::graph
