#pragma once
// Apply and extract: the two editor operations that write BACK into the prefab data model.
// M8.b task 11.1, inherited from M8.a's closing gate.
//
// ================================================================================================
// WHY THESE TWO ARE ONE FILE, AND WHY THEY ARE NOT IN resolve.h
// ================================================================================================
//
// `serialization-and-prefabs` — "Apply and extract":
//
//   > The editor SHALL support applying an instance's overrides back to its prefab source, and
//   > extracting a subtree of a scene into a new prefab asset, replacing it in place with an
//   > instance.
//
// Everything else in this module reads authoring documents and produces something else — a resolved
// graph, a cooked block, a world file. These two are the only operations that take an authoring
// document and hand back a DIFFERENT authoring document, and both of them move authored data across
// an asset boundary. Keeping them together is what makes "which operations rewrite a designer's
// files" a question with a one-file answer.
//
// They are not in resolve.h because resolution is a pure function of the library and these are not:
// `apply_instance_overrides` mutates the prefab every other instance of it reads, and
// `extract_prefab` creates an asset. A reader of resolve.h should be able to assume nothing it
// declares can change a file on disk.
//
// ================================================================================================
// THE INVARIANT BOTH OF THEM ARE WRITTEN AGAINST, AND IT IS TESTABLE RATHER THAN ASSERTED
// ================================================================================================
//
// **Neither operation may change what the container resolves to.** Applying an instance's overrides
// moves a value from the instance to the prefab; the instance's own content is the same value
// either way. Extracting a subtree moves entities from a scene into a prefab the scene then places;
// the scene's content is the same entities either way.
//
// So the criterion for both is the same three lines, and `test_prefab_edit.cpp` runs them:
//
//     resolve(container) -> before;  the operation;  resolve(container) -> after;
//     diff(before, after) is EMPTY.
//
// That is a stronger statement than any field-by-field assertion, because it is made over the whole
// graph — every entity, every component, every field, every parent link — by the same `diff` a
// prefab review shows a person. An operation that dropped a component, renumbered an entity or lost
// a parameter argument fails it without anybody having written a case for that particular loss.
//
// ================================================================================================
// APPLY: WHAT MOVES, WHAT STAYS, AND WHAT IS NEVER DISCARDED
// ================================================================================================
//
// An override that applies is written onto the prefab and REMOVED from the instance — which is what
// "the instance's override list cleared, with other instances picking up the change" means, and the
// removal is the half that makes the change visible: an instance that kept the override would show
// the same value for a different reason and would stop tracking a later edit to the prefab.
//
// An override that CANNOT apply — its entity, component or field is gone from the prefab — is
// marked with its `ConflictKind` and KEPT. overrides.h's rule is absolute here: "Nothing in this
// module ever erases an override on its own", and an apply is not an exception. A caller sees
// `ApplyReport::conflicted` and the marks on the overrides themselves.
//
// The one operation that needs more than a copy is `AddEntity`. Its target names an id the
// CONTAINER invented, which is not in the prefab's id space at all — so applying it allocates a
// fresh prefab-local id, records the container's id against it in the instance's mapping (so the
// entity keeps the local id every reference in the container already uses), and RETARGETS the
// overrides later in the same list that address it. Without the retarget, an `AddEntity` followed
// by the `AddComponent` that fills it would apply the first and conflict on the second.
//
// ================================================================================================
// EXTRACT: WHY THE EXTRACTED ENTITIES KEEP THEIR IDS
// ================================================================================================
//
// "with external references to the subtree rewritten to point at the instance" is the requirement,
// and the cheapest correct rewrite is no rewrite at all.
//
// document.h already says that "an entity a prefab instance contributes gets a local id in the
// containing document at placement time, so a reference from one instance into another is an
// ordinary local id and not a path". An instance's mapping is what assigns those ids. So extraction
// gives the new prefab's entities THE IDS THEY ALREADY HAD in the container, and writes the
// identity mapping onto the placement it creates. Every reference in the container — a parent link,
// a field holding an entity, another instance's override target — then names exactly the entity it
// named before, and now reaches it through the instance.
//
// The new prefab's id counter starts above the container's, so a later edit to either document
// cannot issue an id the other already used.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/scene/serialization/library.h>

#include <string_view>

namespace cy::scene::serialization {

/// What an apply did. Counters rather than a bool, because "the prefab was updated" and "four of
/// the six overrides could not be" are both things a designer has to be told.
struct ApplyReport {
    /// Overrides written onto the source and removed from the instance.
    u32 applied = 0;
    /// Overrides whose target is gone. Marked and KEPT — see the header comment.
    u32 conflicted = 0;
    u32 fields_set = 0;
    u32 components_added = 0;
    u32 components_removed = 0;
    u32 entities_added = 0;
    u32 entities_removed = 0;
    u32 entities_reparented = 0;
};

/// Apply one instance's overrides back onto the document it places.
///
/// `container` is the document holding `instance`; `instance` must be one of its own placements.
/// The source is looked up in `library` and is mutated, so it must be registered there and writable
/// — `Library::find_mutable` is the same access resolution already needs to record a conflict.
///
/// Refuses, changing nothing, when the source is not in the library or when the instance is not the
/// container's. Everything else is reported rather than refused: an override that cannot apply is a
/// conflict, not a failure of the operation.
[[nodiscard]] Status apply_instance_overrides(const Library& library, Document& container,
                                              Instance& instance, ApplyReport& out) noexcept;

/// The same, for a variant's own base overrides: a variant is "a prefab whose base is another
/// prefab, storing only its own overrides", and applying them is the same act one level down.
///
/// resolve.h shares one code path between an instance and a variant for the reason it gives —
/// "sharing the code is why a variant of a variant behaves the same as an instance of an instance,
/// rather than nearly the same" — and this does too.
[[nodiscard]] Status apply_variant_overrides(const Library& library, Document& variant,
                                             ApplyReport& out) noexcept;

/// What an extract produced.
struct ExtractReport {
    /// The placement the container now holds where the subtree was.
    LocalId instance;
    u32 entities = 0;
    /// Nested placements that were inside the subtree and moved with it.
    u32 instances = 0;
};

/// Lift the subtree rooted at `root` out of `container` and into `prefab`, replacing it in place
/// with an instance of `prefab`.
///
/// `prefab` must be empty; it is filled with the subtree, given `prefab_id` and
/// `AssetKind::Prefab`, and is the caller's to register in the library and write to disk. `name`
/// names the placement the container gains.
///
/// The subtree is the closure: `root`, every descendant of it, and every nested placement parented
/// inside it. Refuses when `root` is not an entity of `container`, when `prefab` is not empty, or
/// when `prefab_id` is nil or already the container's own id — each of which would produce an asset
/// graph with a cycle or two documents claiming one identity.
[[nodiscard]] Status extract_prefab(Document& container, LocalId root, AssetId prefab_id,
                                    std::string_view name, Document& prefab,
                                    ExtractReport& out) noexcept;

}  // namespace cy::scene::serialization
