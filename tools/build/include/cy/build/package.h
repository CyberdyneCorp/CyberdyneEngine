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
    /// The PROJECT's source revision. The requirement says "the engine **and** project source
    /// revisions" and until M11.d there was one field, so a build could not say which tree a
    /// difference came from — the commonest question a bug report about a shipped build asks.
    std::string revision;
    std::string platform;
    std::string profile;
    /// The toolchain fingerprint's digest — the same fingerprint every derivation key contributed,
    /// so a shipped build can be traced to the compiler that produced it.
    std::string toolchain;
    /// The engine content version the manifest was produced against, for
    /// `build-and-packaging`'s "Content compatibility".
    u32 content_version = 1;

    // --- What M11.d task 7.5 added, field by field against the requirement's own list -----------
    //
    // "Every produced build SHALL record provenance: a build identity, the engine and project
    //  source revisions, the plugin lockfile hash, the build and cook configuration, toolchain
    //  versions, and the content manifest hash."
    //
    // Before this rung the manifest carried four of the seven. THE BUILD IDENTITY AND THE CONTENT
    // MANIFEST HASH ARE ONE FIELD AND THAT IS NOT AN OMISSION: `build_id` is a digest over the
    // sorted manifest, so it IS the content manifest hash, and writing it twice under two names
    // would create two things that can disagree. The rest are here.

    /// The ENGINE's source revision, which is not the project's. An engine built from a tag and a
    /// project built from a branch is the ordinary case and the one a single field cannot report.
    std::string engine_revision;
    /// The plugin lockfile's hash. `project-and-plugins` makes the lockfile what fixes a build's
    /// plugin set; a build whose provenance omits it cannot be reproduced even with both revisions.
    std::string lockfile;
    /// The COOK configuration, which is not the build configuration. `profile` above is the build's;
    /// two builds of one revision at one profile with different cook settings produce different
    /// content, and nothing in the manifest said so.
    std::string cook_configuration;
    /// Toolchain VERSIONS, readable. `toolchain` above is a digest: it proves two builds used the
    /// same toolchain and tells a human nothing about which. Both are needed and neither replaces
    /// the other — the digest is what a cache key compares, this is what a bug report quotes.
    std::string toolchain_versions;
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

// --- The content audit's cost half — M11.d task 7.4 ------------------------------------------------
//
// `build-and-packaging` asks the content audit two questions about SHAPE — "why is this in the
// build?" and "what references this?" — which `audit()` above answers from the graph. It asks two
// more about COST, and nothing answered them:
//
//     The build SHALL report size by category, asset, plugin, world region and install bundle.
//     The build SHALL report cook and compile time by stage, with cache hit rates.
//
// Every number both sentences need was already on the report: `NodeResult` carries the stage
// (`NodeKind`), the duration, the bytes and the outcome, and `BuildReport` carries them per node in
// evaluation order. What did not exist was the aggregation, so `cy_build build` printed a bundle
// table and a build id and a developer asking "what is slow?" had nothing to read.

/// One stage's cost across a whole build.
struct StageCost {
    NodeKind kind = NodeKind::Unknown;
    u64 nodes = 0;
    /// Served from the cache without running. The numerator of the hit rate.
    u64 cached = 0;
    u64 rebuilt = 0;
    u64 failed = 0;
    /// Summed node durations. NOT wall time: the service runs nodes in parallel, so this is the
    /// work done rather than the time taken, and reporting it as wall time would make a build on
    /// more cores look slower.
    u64 work_ns = 0;
    u64 bytes_produced = 0;

    /// Hits over nodes, as a percentage. Zero nodes is zero rather than a division.
    [[nodiscard]] u32 hit_rate_percent() const noexcept {
        return nodes == 0 ? 0U : static_cast<u32>((cached * 100U) / nodes);
    }
};

/// Every stage that ran, in `NodeKind` order. A stage with no nodes is omitted — a table of zeroes
/// is harder to read than a shorter table, and `stage_report` says how many stages it found.
[[nodiscard]] std::vector<StageCost> stage_costs(const BuildReport& report);

/// "Cook and compile time by stage, with cache hit rates", as text.
[[nodiscard]] std::string stage_report(const BuildReport& report);

/// One category's share of a package set. The category is the KIND OF NODE that produced the bytes,
/// which is the only categorisation the graph can answer without a second declaration — an import's
/// output is an imported asset, a cook's is cooked content, a shader node's is a compiled shader.
struct CategoryShare {
    NodeKind kind = NodeKind::Unknown;
    u64 entries = 0;
    u64 bytes = 0;
    /// The largest single entry in this category, for the "by asset" half of the same requirement.
    std::string largest;
    u64 largest_bytes = 0;
};

/// Size by category, over every bundle. Needs the graph because a `PackageEntry` names the node that
/// produced it and the node's KIND lives in the graph — deliberately not copied onto the entry,
/// which would put a second copy of the graph's own answer into the manifest where it could go
/// stale.
[[nodiscard]] std::vector<CategoryShare> category_shares(const BuildGraph& graph,
                                                         const PackageSet& packages);

/// "Size by category, asset and install bundle", as text.
///
/// PLUGIN AND WORLD REGION ARE NOT HERE, and the report says so rather than omitting them silently.
/// Both need a declaration this graph does not carry: a node does not record which plugin declared
/// it, and `cybuild 1` has no world-region concept at all. Reporting them would mean inventing an
/// attribution, and an invented attribution in a size report is worse than an absent one — it is
/// the shape of claim `delivery-roadmap` calls evidence about something nobody examined.
[[nodiscard]] std::string content_report(const BuildGraph& graph, const PackageSet& packages,
                                         u32 top = 5);

}  // namespace cy::build

#endif  // CY_BUILD_PACKAGE_H
