#ifndef CY_BUILD_KEY_H
#define CY_BUILD_KEY_H
// The derivation key, built in ONE function. M6 task 7.1, design.md §1.2, §1.3, §1.5 and §1.6.
//
// `build-and-packaging` — "Derivation keys": "Every node SHALL have a derivation key hashing: the
// content of its declared inputs, the versions of the tools it invokes, its parameters, the target
// platform, the cook and renderer profiles, and the relevant configuration … Keys SHALL NOT
// incorporate timestamps, absolute paths, hostnames, or environment state that varies between
// machines, since any of these defeats sharing."
//
// --- ONE FUNCTION, AND THAT IS THE POINT ---------------------------------------------------------
//
// `DerivationKeyBuilder` hashes contributions IN ORDER, so a producer that adds its fields wherever
// each input happens to be discovered misses its own cache the first time two runs discover them in
// a different order. The spike measured that as E10. The remedy is not a convention: it is that
// there is exactly one function in this tree that turns a node into a key, no producer may write
// its own, and everything that could vary — the option order, the source order, the upstream order
// — is sorted here rather than trusted to arrive sorted.
//
// --- THE TOOLCHAIN IS CONTRIBUTED BY CONSTRUCTION
// -------------------------------------------------
//
// `KeyInputs` holds a `ToolchainFingerprint` by reference and `derivation_key` refuses a null one.
// A producer therefore cannot compute a key that omits the compiler, which is the correctness bug
// the spike found in `import_derivation_key` — two builds of one importer at -O2 and -O0 producing
// the identical key, and one being served the other's artefact with a reported cache hit.
//
// --- WHAT A DOWNSTREAM NODE CONTRIBUTES ABOUT AN UPSTREAM ----------------------------------------
//
// Its OUTPUT DIGEST, never its key. design.md §1.5 is the decision that is expensive to reverse: an
// edit that reaches a node without changing what it produces rebuilds three nodes under deep input
// keys and one under output digests. The cost is that keys cannot all be computed up front and the
// service must evaluate in dependency order — which it does anyway.

#include <cy/build/graph.h>
#include <cy/build/toolchain.h>
#include <cy/core/assets/derivation.h>
#include <cy/core/assets/hash.h>
#include <cy/core/base/expected.h>

#include <string>
#include <utility>
#include <vector>

namespace cy::build {

/// A named digest: an input's logical name and its content hash, or an upstream node's name and the
/// digest of what it produced.
struct KeyedDigest {
    std::string name;
    assets::ContentHash hash;
};

/// Everything one key is computed from. Assembled by the build service; nothing else builds one.
struct KeyInputs {
    /// The node's declaration: producer, version, kind, platform, profile, options, output names.
    const NodeDesc* node = nullptr;
    /// What produced it. Required — see the note above.
    const ToolchainFingerprint* toolchain = nullptr;
    /// Declared sources, with the content each had. Sorted by name inside `derivation_key`, so a
    /// cosmetic reordering of a build description rebuilds nothing.
    std::vector<KeyedDigest> sources;
    /// Upstream nodes, with the digest of what each PRODUCED.
    std::vector<KeyedDigest> upstreams;
};

/// The key. Fails with `InvalidArgument` on a missing node, a missing toolchain, or a toolchain
/// whose generated library list is empty.
[[nodiscard]] Expected<assets::DerivationKey, Error> derivation_key(const KeyInputs& inputs);

/// The cache's own label for a node kind, so one derived data cache can report what made an entry.
[[nodiscard]] assets::DerivedKind derived_kind_of(NodeKind kind) noexcept;

/// The digest a downstream node contributes for this node: a framed listing of the node's outputs,
/// each by logical name and content digest, sorted by name.
///
/// It is derived from CONTENT alone. Two nodes that produce the same outputs under the same names
/// have the same result digest even when their keys differ, which is exactly what early cutoff
/// needs and exactly what a key cannot give.
[[nodiscard]] assets::ContentHash result_digest(
    const std::vector<std::pair<std::string, assets::ContentHash>>& outputs) noexcept;

}  // namespace cy::build

#endif  // CY_BUILD_KEY_H
