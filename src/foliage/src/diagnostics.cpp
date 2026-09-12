// The nine things `foliage` requires the engine to report, read out of the structures that already
// maintained them. See diagnostics.h: nothing here computes.

#include <cy/foliage/diagnostics.h>

namespace cy::foliage {

u32 FoliageDiagnostics::populated_sections() const noexcept {
    u32 sections = 0;
    sections += by_species.empty() && by_cluster.empty() ? 0U : 1U;  // 1. counts
    sections += total_bytes() > 0 ? 1U : 0U;                         // 2. memory
    sections += clusters_resident > 0 ? 1U : 0U;                     // 3. residency
    sections += cull.clusters_tested > 0 ? 1U : 0U;                  // 4. tier distribution
    sections += promotion.promoted > 0 || promotion.by_cause[0] > 0 ? 1U : 0U;  // 5. promotion
    sections += exceptions > 0 ? 1U : 0U;                                       // 6. exceptions
    sections += grass.patches_considered > 0 ? 1U : 0U;                         // 7. ground cover
    sections += interaction.registered > 0 ? 1U : 0U;                           // 8. interaction
    sections += reduction_steps > 0 || budget_position > 0 ? 1U : 0U;           // 9. budget
    return sections;
}

namespace {

[[nodiscard]] Status count_species(const DiagnosticSources& sources,
                                   FoliageDiagnostics& out) noexcept {
    if (sources.clusters == nullptr) {
        return ok();
    }
    for (const FoliageCluster& cluster : sources.clusters->clusters()) {
        ClusterCount entry;
        entry.cluster = cluster.id();
        entry.coord = cluster.coord();
        entry.cell = cluster.cell();
        entry.instances = static_cast<u32>(cluster.size());
        entry.bytes = cluster.bytes();
        if (Status pushed = out.by_cluster.push_back(entry); !pushed) {
            return pushed;
        }
        for (const SpeciesBlock& block : cluster.blocks()) {
            SpeciesCount* found = nullptr;
            for (SpeciesCount& candidate : out.by_species) {
                if (candidate.species == block.species) {
                    found = &candidate;
                    break;
                }
            }
            if (found == nullptr) {
                SpeciesCount fresh;
                fresh.species = block.species;
                if (sources.library != nullptr) {
                    if (const SpeciesDeclaration* declaration =
                            sources.library->find(block.species);
                        declaration != nullptr) {
                        fresh.name = declaration->name;
                        fresh.klass = declaration->klass;
                    }
                }
                if (Status pushed = out.by_species.push_back(fresh); !pushed) {
                    return pushed;
                }
                found = &out.by_species.back();
            }
            found->instances += block.count;
            found->drawable += block.drawable;
            ++found->clusters;
        }
    }
    return ok();
}

}  // namespace

Expected<FoliageDiagnostics, Error> gather_diagnostics(Allocator& allocator,
                                                       const DiagnosticSources& sources) noexcept {
    FoliageDiagnostics out(allocator);

    if (Status counted = count_species(sources, out); !counted) {
        return make_unexpected(counted.error());
    }
    if (sources.clusters != nullptr) {
        out.instances = sources.clusters->instance_count();
        out.cluster_bytes = sources.clusters->bytes();
        out.clusters_resident = static_cast<u32>(sources.clusters->size());
    }
    out.clusters_bound_to_cells = sources.clusters_bound_to_cells;
    if (sources.grass != nullptr) {
        out.grass_bytes = sources.grass->bytes();
    }
    if (sources.exceptions != nullptr) {
        out.exception_bytes = sources.exceptions->bytes();
        u32 total = 0;
        Array<ClusterId> clusters(allocator);
        if (Status listed = sources.exceptions->clusters(clusters); !listed) {
            return make_unexpected(listed.error());
        }
        for (ClusterId cluster : clusters) {
            total += static_cast<u32>(sources.exceptions->of_cluster(cluster).size());
        }
        out.exceptions = total;
    }
    out.orphaned_exceptions = sources.orphaned_exceptions;
    if (sources.interaction != nullptr) {
        out.interaction_bytes = sources.interaction->bytes();
    }
    if (sources.promotion != nullptr) {
        out.promotion = sources.promotion->diagnostics();
    }
    if (sources.last_cull != nullptr) {
        out.cull = *sources.last_cull;
    }
    for (const VisibleCluster& visible : sources.visible) {
        const u32 tier = static_cast<u32>(visible.tier);
        if (tier < kDetailTierCount) {
            ++out.tier_distribution[tier];
        }
    }
    if (sources.last_grass != nullptr) {
        out.grass = *sources.last_grass;
    }
    if (sources.last_interaction != nullptr) {
        out.interaction = *sources.last_interaction;
    }
    if (sources.budget != nullptr) {
        out.budget_position = sources.budget->position();
        out.reduction_steps = static_cast<u32>(sources.budget->steps().size());
        out.grass_budget_max_blades = sources.budget->settings().grass.max_blades;
    }
    return out;
}

}  // namespace cy::foliage
