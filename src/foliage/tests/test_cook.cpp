// The renderer-facing half: the surface class mapping, publication as the GPU scene's own instance
// record, and the arbiter adapter. M10 task 2.4.

#include <cy/test/test.h>

#include <cy/foliage/cook.h>

#include "fixtures.h"

namespace test = cy::foliage::test;
using cy::foliage::AssetBinding;
using cy::foliage::AssetTable;
using cy::foliage::build_instances;
using cy::foliage::ClusterBuilder;
using cy::foliage::ClusterCoord;
using cy::foliage::ClusterId;
using cy::foliage::ClusterStore;
using cy::foliage::declare_to_arbiter;
using cy::foliage::DetailTier;
using cy::foliage::FoliageBudget;
using cy::foliage::FoliageCluster;
using cy::foliage::FoliageSettings;
using cy::foliage::FoliageSurface;
using cy::foliage::GameplayImportance;
using cy::foliage::InstanceFlags;
using cy::foliage::position_for_allocation;
using cy::foliage::species_id;
using cy::foliage::SpeciesLibrary;
using cy::foliage::to_geometry_importance;
using cy::foliage::to_surface_class;
using cy::foliage::VisibleCluster;

namespace {

[[nodiscard]] cy::Expected<FoliageCluster, cy::Error> stand(ClusterId id) noexcept {
    cy::foliage::ClusterBounds bounds;
    bounds.min_x = 0.0;
    bounds.min_z = 0.0;
    bounds.max_x = 64.0;
    bounds.max_z = 64.0;
    bounds.max_y = 24.0F;
    ClusterBuilder builder(test::allocator(), test::policy(), id, ClusterCoord{0, 0, 0}, bounds);
    for (cy::u32 index = 0; index < 60; ++index) {
        const auto species = index % 3 == 0 ? species_id(test::kFern) : species_id(test::kPine);
        if (cy::Status added =
                builder.add(species,
                            cy::world::WorldVec3d{static_cast<cy::f64>(index % 10) * 6.0, 5.0,
                                                  static_cast<cy::f64>(index) / 10.0 * 6.0},
                            0.5F, 0.5F, 1, 190, 0.0F, 0.0F, InstanceFlags{});
            !added) {
            return cy::make_unexpected(added.error());
        }
    }
    return builder.finish();
}

}  // namespace

CY_TEST_CASE("the surface class mapping is total and never produces solid geometry") {
    // `foliage` — "WHEN foliage is simplified THEN the geometry system SHALL APPLY FOLIAGE POLICY
    // rather than treating it as solid geometry." `FoliageSurface` carries only the three classes a
    // plant may be, so the guarantee is the absent enumerator; this is the mapping's half of it.
    const FoliageSurface every[] = {FoliageSurface::Foliage, FoliageSurface::Aggregate,
                                    FoliageSurface::Thin};
    for (FoliageSurface surface : every) {
        CY_CHECK_NE(to_surface_class(surface), cy::rendering::vg::SurfaceClass::Solid);
        CY_CHECK_NE(to_surface_class(surface), cy::rendering::vg::SurfaceClass::Count);
    }
    CY_CHECK_EQ(to_surface_class(FoliageSurface::Foliage),
                cy::rendering::vg::SurfaceClass::Foliage);
    CY_CHECK_EQ(to_surface_class(FoliageSurface::Aggregate),
                cy::rendering::vg::SurfaceClass::Aggregate);
    CY_CHECK_EQ(to_surface_class(FoliageSurface::Thin), cy::rendering::vg::SurfaceClass::Thin);
    // Even a value cast in from outside the enumeration cannot make a plant solid.
    CY_CHECK_NE(to_surface_class(static_cast<FoliageSurface>(200)),
                cy::rendering::vg::SurfaceClass::Solid);
}

CY_TEST_CASE("tactically important cover keeps its geometry detail too") {
    CY_CHECK_EQ(to_geometry_importance(GameplayImportance::Cover),
                cy::rendering::vg::Importance::Gameplay);
    CY_CHECK_EQ(to_geometry_importance(GameplayImportance::Decorative),
                cy::rendering::vg::Importance::Background);
    // The two axes really are different: importance scales a geometric threshold, and the budget's
    // protection is a separate decision in budget.cpp.
    CY_CHECK_NE(to_geometry_importance(GameplayImportance::Cover),
                to_geometry_importance(GameplayImportance::Decorative));
}

CY_TEST_CASE("foliage publishes the engine's own instance record and nothing else") {
    // `foliage` — "Foliage SHALL publish into the GPU scene AS INSTANCES ... and SHALL be culled,
    // shaded, and shadowed BY THE SAME PASSES as other geometry." The record produced below is
    // `rendering::vg::GeometryInstance` — the same one a static mesh publishes.
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    ClusterStore clusters(test::allocator());
    auto built = stand(ClusterId{5});
    CY_REQUIRE(built.has_value());
    CY_REQUIRE(clusters.insert(static_cast<FoliageCluster&&>(built.value())).has_value());

    AssetTable assets(test::allocator());
    CY_REQUIRE(
        assets.bind(AssetBinding{species_id(test::kPine), DetailTier::Detailed, 1, 0}).has_value());
    CY_REQUIRE(assets.bind(AssetBinding{species_id(test::kPine), DetailTier::Aggregate, 2, 4})
                   .has_value());
    CY_REQUIRE(
        assets.bind(AssetBinding{species_id(test::kFern), DetailTier::Detailed, 3, 0}).has_value());

    cy::Array<VisibleCluster> visible(test::allocator());
    CY_REQUIRE(visible.push_back(VisibleCluster{ClusterId{5}, DetailTier::Detailed, 10.0F, 60})
                   .has_value());

    FoliageSettings settings;
    FoliageBudget budget(settings);
    cy::Array<cy::rendering::vg::GeometryInstance> out(test::allocator());
    auto report = build_instances(library, clusters, assets, visible.span(), budget, out);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report.value().instances, 60u);
    CY_CHECK_EQ(out.size(), 60u);
    CY_CHECK_EQ(report.value().unbound, 0u);
    CY_CHECK_EQ(report.value().suppressed, 0u);

    // Each instance carries the asset its (species, tier) names, and an importance derived from the
    // species' own gameplay importance.
    cy::u32 pines = 0;
    for (const cy::rendering::vg::GeometryInstance& instance : out) {
        const bool bound_asset = instance.asset == 1u || instance.asset == 3u;
        CY_CHECK(bound_asset);
        pines += instance.asset == 1u ? 1U : 0U;
        CY_CHECK_GT(instance.scale, 0.0F);
    }
    CY_CHECK_EQ(pines, 40u);
}

CY_TEST_CASE("a promoted instance is suppressed rather than drawn twice") {
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    ClusterStore clusters(test::allocator());
    auto built = stand(ClusterId{6});
    CY_REQUIRE(built.has_value());
    CY_REQUIRE(clusters.insert(static_cast<FoliageCluster&&>(built.value())).has_value());

    FoliageCluster* resident = clusters.find(ClusterId{6});
    CY_REQUIRE(resident != nullptr);
    InstanceFlags promoted;
    promoted.set(InstanceFlags::kPromoted);
    CY_REQUIRE(resident->set_flags(0, promoted).has_value());
    InstanceFlags removed;
    removed.set(InstanceFlags::kRemoved);
    CY_REQUIRE(resident->set_flags(1, removed).has_value());

    AssetTable assets(test::allocator());
    CY_REQUIRE(
        assets.bind(AssetBinding{species_id(test::kPine), DetailTier::Detailed, 1, 0}).has_value());
    CY_REQUIRE(
        assets.bind(AssetBinding{species_id(test::kFern), DetailTier::Detailed, 3, 0}).has_value());
    cy::Array<VisibleCluster> visible(test::allocator());
    CY_REQUIRE(visible.push_back(VisibleCluster{ClusterId{6}, DetailTier::Detailed, 10.0F, 60})
                   .has_value());

    FoliageSettings settings;
    FoliageBudget budget(settings);
    cy::Array<cy::rendering::vg::GeometryInstance> out(test::allocator());
    auto report = build_instances(library, clusters, assets, visible.span(), budget, out);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report.value().suppressed, 2u);
    CY_CHECK_EQ(report.value().instances, 58u);
    CY_CHECK_EQ(out.size(), 58u);
}

CY_TEST_CASE("budget pressure cuts decorative species and leaves cover standing") {
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    ClusterStore clusters(test::allocator());
    auto built = stand(ClusterId{7});
    CY_REQUIRE(built.has_value());
    CY_REQUIRE(clusters.insert(static_cast<FoliageCluster&&>(built.value())).has_value());

    AssetTable assets(test::allocator());
    CY_REQUIRE(
        assets.bind(AssetBinding{species_id(test::kPine), DetailTier::Detailed, 1, 0}).has_value());
    CY_REQUIRE(
        assets.bind(AssetBinding{species_id(test::kFern), DetailTier::Detailed, 3, 0}).has_value());
    cy::Array<VisibleCluster> visible(test::allocator());
    CY_REQUIRE(visible.push_back(VisibleCluster{ClusterId{7}, DetailTier::Detailed, 10.0F, 60})
                   .has_value());

    FoliageSettings settings;
    FoliageBudget budget(settings);
    budget.set_position(4);
    cy::Array<cy::rendering::vg::GeometryInstance> out(test::allocator());
    auto report = build_instances(library, clusters, assets, visible.span(), budget, out);
    CY_REQUIRE(report.has_value());
    CY_CHECK_GT(report.value().budget_dropped, 0u);

    // Every pine survived — `GameplayImportance::Cover` is protected — and ferns were cut.
    cy::u32 pines = 0;
    cy::u32 ferns = 0;
    for (const cy::rendering::vg::GeometryInstance& instance : out) {
        pines += instance.asset == 1u ? 1U : 0U;
        ferns += instance.asset == 3u ? 1U : 0U;
    }
    CY_CHECK_EQ(pines, 40u);
    CY_CHECK_LT(ferns, 20u);
}

CY_TEST_CASE("a species with no asset at the wanted tier falls back rather than vanishing") {
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    AssetTable assets(test::allocator());
    CY_REQUIRE(
        assets.bind(AssetBinding{species_id(test::kPine), DetailTier::Detailed, 1, 0}).has_value());
    CY_REQUIRE(assets.bind(AssetBinding{species_id(test::kPine), DetailTier::Aggregate, 2, 0})
                   .has_value());
    // A world that cooked no macro pines draws the aggregate rather than nothing.
    const AssetBinding* macro = assets.find(species_id(test::kPine), DetailTier::Macro);
    CY_REQUIRE(macro != nullptr);
    CY_CHECK_EQ(macro->asset, 2u);
    CY_CHECK_EQ(assets.find(species_id(test::kPine), DetailTier::Detailed)->asset, 1u);
    // And a species with no binding at all is a COOKING GAP, reported rather than silent.
    CY_CHECK(assets.find(species_id(test::kGrass), DetailTier::Detailed) == nullptr);

    ClusterStore clusters(test::allocator());
    auto built = stand(ClusterId{8});
    CY_REQUIRE(built.has_value());
    CY_REQUIRE(clusters.insert(static_cast<FoliageCluster&&>(built.value())).has_value());
    cy::Array<VisibleCluster> visible(test::allocator());
    CY_REQUIRE(visible.push_back(VisibleCluster{ClusterId{8}, DetailTier::Detailed, 10.0F, 60})
                   .has_value());
    FoliageSettings settings;
    FoliageBudget budget(settings);
    cy::Array<cy::rendering::vg::GeometryInstance> out(test::allocator());
    auto report = build_instances(library, clusters, assets, visible.span(), budget, out);
    CY_REQUIRE(report.has_value());
    // The ferns have no binding at all.
    CY_CHECK_EQ(report.value().unbound, 20u);
    CY_CHECK_EQ(report.value().instances, 40u);
}

CY_TEST_CASE("foliage declares a priced ladder to the arbiter under the geometry allocation") {
    // budget.h's header note at length: adding an eighth `BudgetSubsystem` would be a change to
    // `rendering-architecture`, made from inside a foliage module. Foliage is geometry, and it
    // declares a composite position under it — the arrangement `virtual-shadows` already uses.
    FoliageSettings settings;
    FoliageBudget budget(settings);
    const cy::rendering::SubsystemDeclaration declaration = declare_to_arbiter(budget.ladder(), 3);
    CY_CHECK_EQ(declaration.subsystem, cy::rendering::BudgetSubsystem::Geometry);
    CY_CHECK_EQ(declaration.reduction_order, 3u);
    CY_CHECK_GT(declaration.ladder.positions, 1u);
    CY_CHECK_LE(declaration.ladder.positions, cy::rendering::kMaxLadderPositions);
    CY_CHECK_EQ(declaration.ladder.cost_at(0), 1.0F);
    CY_CHECK_LT(declaration.ladder.cost_at(declaration.ladder.last_position()), 1.0F);

    // And an allocation maps back to the coarsest position that fits inside it.
    const cy::foliage::FoliageLadder ladder = budget.ladder();
    CY_CHECK_EQ(position_for_allocation(ladder, 1.0F), 0u);
    CY_CHECK_GT(position_for_allocation(ladder, 0.5F), 0u);
    CY_CHECK_EQ(position_for_allocation(ladder, 0.0F), ladder.last_position());
}
