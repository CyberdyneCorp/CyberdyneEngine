// The nine things `foliage`'s diagnostics requirement enumerates, each populated from a world that
// exercises it. M10 task 2.4.

#include <cy/test/test.h>

#include <cy/foliage/diagnostics.h>

#include "fixtures.h"

namespace test = cy::foliage::test;
using cy::foliage::ClusterBuilder;
using cy::foliage::ClusterCoord;
using cy::foliage::ClusterId;
using cy::foliage::ClusterStore;
using cy::foliage::CullView;
using cy::foliage::DiagnosticSources;
using cy::foliage::ExceptionKind;
using cy::foliage::ExceptionStore;
using cy::foliage::FoliageBudget;
using cy::foliage::FoliageCluster;
using cy::foliage::FoliageException;
using cy::foliage::FoliageSettings;
using cy::foliage::gather_diagnostics;
using cy::foliage::GrassBudget;
using cy::foliage::GrassField;
using cy::foliage::GrassPatch;
using cy::foliage::InstanceFlags;
using cy::foliage::InstanceId;
using cy::foliage::InteractionBounds;
using cy::foliage::InteractionEffect;
using cy::foliage::InteractionField;
using cy::foliage::InteractionPrimitive;
using cy::foliage::InteractionShape;
using cy::foliage::PromotionCause;
using cy::foliage::PromotionRegistry;
using cy::foliage::species_id;
using cy::foliage::SpeciesLibrary;
using cy::foliage::VisibleCluster;

CY_TEST_CASE("the diagnostics report every one of the nine things the requirement enumerates") {
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());

    // 1, 2, 3 — clusters, their instances and their bytes.
    ClusterStore clusters(test::allocator());
    for (cy::i32 index = 0; index < 3; ++index) {
        cy::foliage::ClusterBounds bounds;
        bounds.min_x = static_cast<cy::f64>(index) * 64.0;
        bounds.min_z = 0.0;
        bounds.max_x = bounds.min_x + 64.0;
        bounds.max_z = 64.0;
        bounds.max_y = 24.0F;
        ClusterBuilder builder(test::allocator(), test::policy(),
                               ClusterId{static_cast<cy::u64>(index + 1)},
                               ClusterCoord{index, 0, 0}, bounds);
        for (cy::u32 instance = 0; instance < 20; ++instance) {
            const auto species =
                instance % 2 == 0 ? species_id(test::kPine) : species_id(test::kFern);
            CY_REQUIRE(
                builder
                    .add(species,
                         cy::world::WorldVec3d{
                             bounds.min_x + (static_cast<cy::f64>(instance) * 3.0), 4.0, 4.0},
                         0.0F, 0.5F, 0, 120, 0.0F, 0.0F, InstanceFlags{})
                    .has_value());
        }
        auto built = builder.finish();
        CY_REQUIRE(built.has_value());
        CY_REQUIRE(clusters.insert(static_cast<FoliageCluster&&>(built.value())).has_value());
    }

    // 4 — the tier distribution, from a real cull.
    CullView view;
    view.eye = cy::world::WorldVec3d{0.0, 10.0, 32.0};
    view.max_distance_metres = 4096.0F;
    view.pixels_per_metre = 900.0F;
    cy::Array<VisibleCluster> visible(test::allocator());
    auto cull = clusters.cull(library, view, visible);
    CY_REQUIRE(cull.has_value());

    // 5 — promotion and its causes.
    PromotionRegistry promotion(test::allocator(), library, 5);
    promotion.begin_frame();
    FoliageCluster* first = clusters.find(ClusterId{1});
    CY_REQUIRE(first != nullptr);
    cy::u32 pine_slot = FoliageCluster::kNoSlot;
    for (cy::u32 slot = 0; slot < static_cast<cy::u32>(first->size()); ++slot) {
        if (first->species_at(first->at(slot)->species_slot) == species_id(test::kPine)) {
            pine_slot = slot;
            break;
        }
    }
    CY_REQUIRE_NE(pine_slot, FoliageCluster::kNoSlot);
    CY_REQUIRE(
        promotion.promote(*first, first->identity_at(5, pine_slot), PromotionCause::Attachment, 1)
            .has_value());

    // 6 — exceptions, including an orphan the caller holds the count of.
    ExceptionStore exceptions(test::allocator());
    FoliageException removed;
    removed.kind = ExceptionKind::Removed;
    removed.identity = InstanceId{123};
    removed.cluster = ClusterId{1};
    removed.species = species_id(test::kPine);
    CY_REQUIRE(exceptions.record(removed).has_value());

    // 7 — ground cover against its budget.
    GrassField grass(test::allocator());
    GrassPatch patch;
    patch.seed = 9;
    patch.density = 120;
    CY_REQUIRE(grass.add(ClusterId{1}, clusters.find(ClusterId{1})->bounds(), patch).has_value());
    GrassBudget grass_budget;
    cy::Array<cy::foliage::GrassBlade> blades(test::allocator());
    auto grass_report = grass.expand(grass_budget, view, nullptr, blades);
    CY_REQUIRE(grass_report.has_value());

    // 8 — the interaction field's contributors.
    InteractionBounds limits;
    InteractionField interaction(test::allocator(), limits);
    CY_REQUIRE(interaction.recentre(cy::world::WorldVec3d{0.0, 0.0, 0.0}).has_value());
    InteractionPrimitive walker;
    walker.source = 1;
    walker.shape = InteractionShape::Sphere;
    walker.effect = InteractionEffect::Bend;
    walker.radius_metres = 1.0F;
    walker.strength = 1.0F;
    CY_REQUIRE(interaction.register_primitive(walker).has_value());
    auto interaction_report = interaction.resolve(0.016F);
    CY_REQUIRE(interaction_report.has_value());

    // 9 — the budget position the frame ran at.
    FoliageSettings settings;
    FoliageBudget budget(settings);
    budget.set_position(2);

    DiagnosticSources sources;
    sources.library = &library;
    sources.clusters = &clusters;
    sources.grass = &grass;
    sources.exceptions = &exceptions;
    sources.promotion = &promotion;
    sources.interaction = &interaction;
    sources.budget = &budget;
    sources.last_cull = &cull.value();
    sources.visible = visible.span();
    sources.last_grass = &grass_report.value();
    sources.last_interaction = &interaction_report.value();
    sources.orphaned_exceptions = 1;
    sources.clusters_bound_to_cells = 3;

    auto report = gather_diagnostics(test::allocator(), sources);
    CY_REQUIRE(report.has_value());
    const cy::foliage::FoliageDiagnostics& diagnostics = report.value();

    // All nine, populated.
    CY_CHECK_EQ(diagnostics.populated_sections(), 9u);

    // And each one says something true rather than merely being non-zero.
    CY_CHECK_EQ(diagnostics.instances, 60u);
    CY_CHECK_EQ(diagnostics.by_cluster.size(), 3u);
    CY_CHECK_EQ(diagnostics.by_species.size(), 2u);
    for (const cy::foliage::SpeciesCount& count : diagnostics.by_species) {
        CY_CHECK_EQ(count.instances, 30u);
        CY_CHECK_EQ(count.clusters, 3u);
        CY_CHECK(count.name[0] != '\0');
    }
    CY_CHECK_EQ(diagnostics.clusters_resident, 3u);
    CY_CHECK_EQ(diagnostics.clusters_bound_to_cells, 3u);
    CY_CHECK_EQ(diagnostics.cull.clusters_tested, 3u);
    CY_CHECK_EQ(diagnostics.promotion.promoted, 1u);
    CY_CHECK_EQ(diagnostics.promotion.by_cause[static_cast<cy::u32>(PromotionCause::Attachment)],
                1u);
    CY_CHECK_EQ(diagnostics.exceptions, 1u);
    CY_CHECK_EQ(diagnostics.orphaned_exceptions, 1u);
    CY_CHECK_GT(diagnostics.grass.blades_expanded, 0u);
    CY_CHECK_GT(diagnostics.grass_budget_max_blades, 0u);
    CY_CHECK_EQ(diagnostics.interaction.admitted, 1u);
    CY_CHECK_EQ(diagnostics.budget_position, 2u);
    CY_CHECK_GT(diagnostics.reduction_steps, 0u);
    CY_CHECK_GT(diagnostics.total_bytes(), 0u);

    // The promoted instance is drawable nowhere and is counted once.
    cy::u32 drawable = 0;
    for (const cy::foliage::SpeciesCount& count : diagnostics.by_species) {
        drawable += static_cast<cy::u32>(count.drawable);
    }
    CY_CHECK_EQ(drawable, 59u);

    // A tier distribution that summed to something other than the visible clusters would mean the
    // report and the cull disagree about what was drawn.
    cy::u32 tiers = 0;
    for (const cy::u32 tier : diagnostics.tier_distribution) {
        tiers += tier;
    }
    CY_CHECK_EQ(tiers, static_cast<cy::u32>(visible.size()));
}

CY_TEST_CASE("a report gathered from an empty world populates nothing and does not fault") {
    // A dedicated server has no interaction field and a cook has no promotion registry; a report
    // that required them would be a report neither could produce.
    DiagnosticSources sources;
    auto report = gather_diagnostics(test::allocator(), sources);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report.value().populated_sections(), 0u);
    CY_CHECK_EQ(report.value().instances, 0u);
    CY_CHECK_EQ(report.value().total_bytes(), 0u);
}
