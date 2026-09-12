#pragma once
// What CyberFoliage reports. M10 task 2.4.
//
// `foliage` — "Foliage diagnostics": "The engine SHALL report: instance counts by species and
// cluster, memory in use, cluster residency, detail tier distribution, promoted instance count and
// their causes, exception counts and orphaned exceptions, ground cover expansion counts against
// budget, and interaction field contributors."
//
// Nine things, and every one of them is a member below. The file exists rather than the report
// being assembled at the call site because a requirement that enumerates its outputs is a
// requirement a test should be able to walk — `test_diagnostics.cpp` asserts that each of the nine
// is populated from a world that exercises it, which is a different claim from "the struct has a
// field for it".
//
// NOTHING HERE COMPUTES. Every number is read out of the structure that already maintained it: the
// cluster store's own counts, the promotion registry's own causes, the last `GrassReport`, the last
// `InteractionReport`. A diagnostic that recomputed would be a second answer that can disagree with
// the first, and the first is what the frame actually used.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/foliage/budget.h>
#include <cy/foliage/exceptions.h>
#include <cy/foliage/grass.h>
#include <cy/foliage/instance.h>
#include <cy/foliage/interaction.h>
#include <cy/foliage/promotion.h>

namespace cy::foliage {

/// Per-species counts. `foliage` — "instance counts BY SPECIES and cluster".
struct SpeciesCount {
    SpeciesId species;
    const char* name = "";
    SpeciesClass klass = SpeciesClass::Canopy;
    u64 instances = 0;
    u64 drawable = 0;
    u32 clusters = 0;
};

/// Per-cluster counts, for the residency half.
struct ClusterCount {
    ClusterId cluster;
    ClusterCoord coord;
    world::CellId cell;
    u32 instances = 0;
    u64 bytes = 0;
};

/// The whole report.
struct FoliageDiagnostics {
    // 1. Instance counts by species and cluster.
    Array<SpeciesCount> by_species;
    Array<ClusterCount> by_cluster;
    u64 instances = 0;

    // 2. Memory in use.
    u64 cluster_bytes = 0;
    u64 grass_bytes = 0;
    u64 exception_bytes = 0;
    u64 interaction_bytes = 0;
    [[nodiscard]] u64 total_bytes() const noexcept {
        return cluster_bytes + grass_bytes + exception_bytes + interaction_bytes;
    }

    // 3. Cluster residency.
    u32 clusters_resident = 0;
    u32 clusters_bound_to_cells = 0;

    // 4. Detail tier distribution — clusters at each tier, from the last cull.
    u32 tier_distribution[kDetailTierCount] = {};
    CullResult cull;

    // 5. Promoted instance count and their causes.
    PromotionDiagnostics promotion;

    // 6. Exception counts and orphaned exceptions.
    u32 exceptions = 0;
    u32 orphaned_exceptions = 0;

    // 7. Ground cover expansion counts against budget.
    GrassReport grass;
    u32 grass_budget_max_blades = 0;

    // 8. Interaction field contributors.
    InteractionReport interaction;

    // 9. The budget position the frame ran at, and what it gave up to get there.
    u8 budget_position = 0;
    u32 reduction_steps = 0;

    explicit FoliageDiagnostics(Allocator& allocator) noexcept
        : by_species(allocator), by_cluster(allocator) {}

    FoliageDiagnostics(const FoliageDiagnostics&) = delete;
    FoliageDiagnostics& operator=(const FoliageDiagnostics&) = delete;
    FoliageDiagnostics(FoliageDiagnostics&&) noexcept = default;
    FoliageDiagnostics& operator=(FoliageDiagnostics&&) noexcept = default;
    ~FoliageDiagnostics() = default;

    /// Whether every one of the nine is populated. The predicate `test_diagnostics.cpp` uses, so
    /// "the engine SHALL report nine things" is checked rather than eyeballed against the struct.
    [[nodiscard]] u32 populated_sections() const noexcept;
};

/// Everything the report is gathered from. Pointers, all optional: a dedicated server has no
/// interaction field and a cook has no promotion registry, and a report that required them would be
/// a report neither could produce.
struct DiagnosticSources {
    const SpeciesLibrary* library = nullptr;
    const ClusterStore* clusters = nullptr;
    const GrassField* grass = nullptr;
    const ExceptionStore* exceptions = nullptr;
    const PromotionRegistry* promotion = nullptr;
    const InteractionField* interaction = nullptr;
    const FoliageBudget* budget = nullptr;
    /// The last cull, the last expansion and the last interaction resolve. Passed in rather than
    /// re-run: see the header note.
    const CullResult* last_cull = nullptr;
    Span<const VisibleCluster> visible;
    const GrassReport* last_grass = nullptr;
    const InteractionReport* last_interaction = nullptr;
    /// The orphan count from the last re-resolution, which only the caller holds.
    u32 orphaned_exceptions = 0;
    u32 clusters_bound_to_cells = 0;
};

[[nodiscard]] Expected<FoliageDiagnostics, Error> gather_diagnostics(
    Allocator& allocator, const DiagnosticSources& sources) noexcept;

}  // namespace cy::foliage
