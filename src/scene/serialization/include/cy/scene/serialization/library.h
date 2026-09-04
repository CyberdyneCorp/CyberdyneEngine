#pragma once
// The asset library and the dependency graph over it. Tasks 3.2.5 and 3.2.8.
//
// `serialization-and-prefabs` — "Dependency cycles are rejected": a prefab may not contain,
// directly or transitively, an instance of itself; a scene may not either; a variant chain may not
// contain a cycle. "Cycles SHALL be detected through the asset dependency graph and rejected at
// authoring time, with the cycle reported as a chain."
//
// Two verbs, and the difference between them is the whole requirement. `validate()` walks the graph
// that exists and reports any cycle in it — that is the cook-time check, and it is the safety net.
// `check_placement()` answers "would putting this inside that create a cycle", which is the check
// the *editor* makes before the placement happens. The specification's scenario is explicit that
// the second is the one that matters: the operation "SHALL be rejected with the cycle shown, rather
// than saved and failing at cook".
//
// THE LIBRARY DOES NOT OWN ITS DOCUMENTS. It holds pointers to documents the caller owns, because a
// document is large, move-only, and in a real editor is owned by the asset system with its own
// residency rules. Owning them here would mean either copying one on registration or making this
// class the asset system, and it is neither.
//
// The pointers are non-const, and that is deliberate rather than lax. "An override whose target no
// longer exists SHALL become an explicit override conflict … Conflicts SHALL be retained in the
// authoring data" — so resolution has to be able to write the conflict back onto the override that
// carries it. `find()` hands out a const view for everything that only reads.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/scene/serialization/document.h>

namespace cy::scene::serialization {

/// A dependency chain, reported when a cycle is found: `[a, b, c, a]`.
using AssetChain = Array<AssetId>;

/// The documents a resolve or a cook can see, and the graph over them.
class Library {
public:
    explicit Library(Allocator& allocator) noexcept : documents_(allocator) {}

    /// Register a document. Borrowed: it must outlive the library.
    [[nodiscard]] Status add(Document& document) noexcept;

    [[nodiscard]] const Document* find(AssetId id) const noexcept;
    /// The same document, writable. For resolution, which records override conflicts back onto the
    /// authoring data, and for the editor. Everything that only reads uses `find()`.
    [[nodiscard]] Document* find_mutable(AssetId id) const noexcept;
    [[nodiscard]] usize size() const noexcept { return documents_.size(); }

    /// The recommended maximum variant chain depth. Exceeding it warns rather than fails, because
    /// the specification calls it a recommendation and the editor is what surfaces it.
    [[nodiscard]] u32 recommended_variant_depth() const noexcept { return recommended_depth_; }
    void set_recommended_variant_depth(u32 depth) noexcept { recommended_depth_ = depth; }

    /// Every cycle-free document reachable from `root`, deepest dependency first.
    ///
    /// A cook resolves in this order so that a document is never resolved before something it
    /// depends on. Fails with the cycle in `chain` if there is one.
    [[nodiscard]] Status dependency_order(AssetId root, Array<AssetId>& out,
                                          AssetChain& chain) const noexcept;

    /// Walk the whole graph and report the first cycle found, as a chain.
    [[nodiscard]] Status validate(AssetChain& chain) const noexcept;

    /// Would placing `candidate` inside `container` create a cycle? The editor's check, made before
    /// the placement rather than after it.
    ///
    /// Returns `AlreadyExists` with the chain filled in when it would, so the caller can show the
    /// cycle it prevented rather than a refusal with no explanation.
    [[nodiscard]] Status check_placement(AssetId container, AssetId candidate,
                                         AssetChain& chain) const noexcept;

    /// How many variants deep a prefab is: zero for a prefab with no base.
    [[nodiscard]] Expected<u32, Error> variant_depth(AssetId id, AssetChain& chain) const noexcept;

    /// True when the chain is deeper than the recommendation. What the editor and the build warn
    /// on, naming the chain.
    [[nodiscard]] Expected<bool, Error> variant_depth_exceeds_recommendation(
        AssetId id, AssetChain& chain) const noexcept;

    [[nodiscard]] Allocator& allocator() const noexcept { return documents_.allocator(); }

private:
    /// Depth-first walk with an explicit colour per node, so a cycle is distinguishable from a
    /// diamond: a document reached twice is fine, a document reached while still on the stack is a
    /// cycle. Iterative rather than recursive, because a deeply nested authoring graph is exactly
    /// the input a malicious or merely disorganised project supplies.
    [[nodiscard]] Status walk(AssetId root, Array<AssetId>* order,
                              AssetChain& chain) const noexcept;

    Array<Document*> documents_;
    u32 recommended_depth_ = kRecommendedVariantDepth;
};

/// The documents `document` depends on directly: its variant base, then each instance's source, in
/// the order they appear.
[[nodiscard]] Status direct_dependencies(const Document& document, Array<AssetId>& out) noexcept;

}  // namespace cy::scene::serialization
