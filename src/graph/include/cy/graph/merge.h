#pragma once
// CyberGraph's semantic diff, three-way merge, and node migration. M8.b task 2.1.
//
// ================================================================================================
// A SEMANTIC DIFF, NOT A TEXT DIFF
// ================================================================================================
//
// `visual-scripting` requires the shared infrastructure to own "diffing and merging" and
// "versioning and migration". A line diff over the text form would already be better than a binary
// blob, but it is still the wrong unit: inserting a node changes the file wherever the canonical
// order puts it, and a reviewer reading `-node 7 ... +node 7 ...` cannot tell a retype from a
// reposition.
//
// So a diff here is a list of CHANGES KEYED BY NODE AND PIN — the same identity the diagnostics and
// the debug map use — and the merge resolves them per change rather than per line. Two authors who
// touch different nodes always merge; two who touch the same property of one node always conflict.
// That is the whole content of "semantic".
//
// LAYOUT NEVER CONFLICTS. Moving a node is not a change to a graph's meaning (cybergraph.h,
// decision 4), so a layout difference is applied rather than reported. An author whose merge failed
// because a colleague dragged a node two centimetres would stop trusting the merge.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/graph/cybergraph.h>

namespace cy::graph {

enum class ChangeKind : u8 {
    NodeAdded = 0,
    NodeRemoved,
    NodeRetyped,
    NodeVersioned,
    NodeMuted,
    NodeSubgraphChanged,
    PropertySet,
    PropertyRemoved,
    LinkAdded,
    LinkRemoved,
    LayoutMoved,
    GraphRenamed,
    CapabilitiesChanged,
    DeterminismClaimChanged,
};

[[nodiscard]] const char* change_kind_name(ChangeKind kind) noexcept;

struct Change {
    ChangeKind kind = ChangeKind::NodeAdded;
    NodeKey node = kInvalidNodeKey;
    /// The property's name, or the node type for a retype.
    Name detail;
    /// The wire, for `LinkAdded` and `LinkRemoved`.
    Link link;
};

/// What `base` would have to become to be `other`. Deterministic: node changes in key order, then
/// link changes in canonical wire order.
[[nodiscard]] Status diff(const Graph& base, const Graph& other, Array<Change>& out) noexcept;

/// One change the merge could not decide.
struct Conflict {
    ChangeKind kind = ChangeKind::PropertySet;
    NodeKey node = kInvalidNodeKey;
    Name detail;
    const char* message = "";
};

struct MergeReport {
    explicit MergeReport(Allocator& allocator) noexcept : conflicts(allocator) {}

    Array<Conflict> conflicts;
    /// Changes taken from `theirs` because `ours` did not touch the same thing.
    u32 taken_from_theirs = 0;
    /// Changes `ours` already made that `theirs` made identically. Not a conflict: two authors who
    /// wire the same wire agree.
    u32 already_agreed = 0;
};

/// Merge `theirs` into `ours` against their common `base`.
///
/// The result is a new graph; neither input is touched. A conflict is REPORTED AND `ours` IS KEPT,
/// so the result is always a graph an author can open — a merge that produces nothing when it
/// cannot decide is a merge that loses a day's work.
[[nodiscard]] Expected<Graph, Error> merge3(const Graph& base, const Graph& ours,
                                            const Graph& theirs, MergeReport& report) noexcept;

// --- Versioning and migration --------------------------------------------------------------

/// What one migration step does to one node. It may add, remove and rename properties and rewire
/// nothing: a rule that could rewire the graph could not be reviewed.
using MigrateFn = Status (*)(Graph& graph, NodeKey node, u32 from_version) noexcept;

struct MigrationRule {
    Name type;
    /// The version this rule reads.
    u32 from_version = 1;
    /// The version it produces. Rules chain: 1->2 then 2->3 carries a version-1 node to 3.
    u32 to_version = 2;
    MigrateFn apply = nullptr;
};

/// The migration rules one build knows.
class MigrationTable {
public:
    explicit MigrationTable(Allocator& allocator) noexcept : rules_(allocator) {}

    MigrationTable(const MigrationTable&) = delete;
    MigrationTable& operator=(const MigrationTable&) = delete;

    [[nodiscard]] Status add(const MigrationRule& rule) noexcept;

    /// Carry every node forward to the version its registered type is at.
    ///
    /// A node with no rule for its version is REPORTED AND LEFT ALONE, and an unregistered node is
    /// skipped entirely: an unmigratable node must not become a dropped node (cybergraph.h,
    /// decision 5). Returns the number of nodes migrated.
    [[nodiscard]] Expected<u32, Error> migrate(Graph& graph, const NodeRegistry& registry,
                                               DiagnosticSink& sink) const noexcept;

private:
    Array<MigrationRule> rules_;
};

}  // namespace cy::graph
