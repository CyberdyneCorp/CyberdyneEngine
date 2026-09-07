#ifndef CY_BUILD_PACKAGE_H
#define CY_BUILD_PACKAGE_H
// Packages, install bundles, and the content manifest a patch is computed from. M6 task 7.5.
//
// `build-and-packaging` — "Packages and install bundles": "Cooked content SHALL be packaged into
// containers, and logical asset identity SHALL remain independent of physical package placement. A
// shipping product SHALL NOT be distributed as one file per asset. Content SHALL be assignable to
// **install bundles** … Bundle assignment SHALL be a declared policy, and the build SHALL report
// each bundle's size and contents."
//
// --- WHAT A PACKAGE IS HERE, AND WHY IT IS SHAPED THIS WAY ---------------------------------------
//
// A bundle is a MANIFEST over content-addressed chunks, not a container with the bytes inside it.
// That is not a shortcut; it is what makes the next requirement — "Patching SHALL operate on
// content-addressed chunks, not whole packages" — follow rather than need a second mechanism. The
// specification says as much: "The engine's page-oriented formats … are already content-addressed,
// and patch granularity SHALL follow from that rather than from a separate delta mechanism."
//
// So: logical name → content digest → size, and the bytes live once in the artefact store however
// many bundles name them. Logical identity is independent of physical placement by construction,
// and two bundles that contain the same asset contain it once on disk.
//
// --- DETERMINISM ---------------------------------------------------------------------------------
//
// Bundles are sorted by name and entries by logical name, and the build id is a digest over the
// sorted manifest. Two runs of one graph therefore produce the same manifest bytes and the same
// build id, which is what makes "a cold build and a cache-warm build produce byte-identical
// artefacts" checkable at the level of the whole product rather than one artefact at a time.

#include <cy/build/artefact_store.h>
#include <cy/build/graph.h>
#include <cy/build/service.h>
#include <cy/core/assets/hash.h>
#include <cy/core/base/expected.h>

#include <string>
#include <vector>

namespace cy::build {

/// The bundle a node's outputs belong to when the node names none. `build-and-packaging`: "the base
/// bundle SHALL be playable without optional bundles present".
inline constexpr const char* kBaseBundle = "base";

/// One piece of content in a bundle.
struct PackageEntry {
    /// The logical name — the asset's identity, independent of where the bytes sit.
    std::string name;
    assets::ContentHash digest{};
    u64 size = 0;
    /// The node that produced it, for the content audit's "why is this in the build?".
    std::string node;
};

/// One install bundle.
struct Bundle {
    std::string name;
    std::vector<PackageEntry> entries;

    [[nodiscard]] u64 size() const noexcept;
};

/// What a build recorded about itself. `build-and-packaging` — "Build provenance and symbols":
/// "Every produced build SHALL record provenance: a build identity, the engine and project source
/// revisions, the plugin lockfile hash, the build and cook configuration, toolchain versions, and
/// the content manifest hash."
struct Provenance {
    std::string project;
    std::string revision;
    std::string platform;
    std::string profile;
    /// The toolchain fingerprint's digest — the same fingerprint every derivation key contributed,
    /// so a shipped build can be traced to the compiler that produced it.
    std::string toolchain;
    /// The engine content version the manifest was produced against, for
    /// `build-and-packaging`'s "Content compatibility".
    u32 content_version = 1;
};

/// A whole build's content, by bundle.
struct PackageSet {
    /// The build's identity: a digest over the sorted manifest. Content-derived, so two identical
    /// builds have one identity and a patch between them is empty.
    std::string build_id;
    std::vector<Bundle> bundles;
    Provenance provenance;

    [[nodiscard]] const Bundle* bundle(std::string_view name) const noexcept;
    /// Every entry across every bundle, sorted by logical name. What the patcher diffs.
    [[nodiscard]] std::vector<PackageEntry> entries() const;
    [[nodiscard]] u64 size() const noexcept;
};

/// Assemble a build's outputs into bundles.
///
/// Fails when the report contains a failed node: `build-and-packaging` requires validation to run
/// "**before** packaging, so a problem is not discovered after producing tens of gigabytes".
[[nodiscard]] Expected<PackageSet, Error> assemble(const BuildGraph& graph,
                                                   const BuildReport& report,
                                                   Provenance provenance);

/// The `cypackage 1` manifest text. Deterministic: sorted, and carrying no path, timestamp or host.
[[nodiscard]] std::string write_package(const PackageSet& packages);
[[nodiscard]] Expected<PackageSet, Error> read_package(std::string_view document);

/// Each bundle's size and its largest contributors — the report the specification asks for.
[[nodiscard]] std::string bundle_report(const PackageSet& packages, u32 top = 5);

/// The two content-audit questions, answered from the graph rather than from a separate index.
struct AuditAnswer {
    /// The chain from a delivery root down to the node — "why is this in the build?".
    std::vector<std::string> chain;
    /// What depends on it — "what references this?".
    std::vector<std::string> dependents;
};

[[nodiscard]] Expected<AuditAnswer, Error> audit(const BuildGraph& graph, std::string_view node);

}  // namespace cy::build

#endif  // CY_BUILD_PACKAGE_H
