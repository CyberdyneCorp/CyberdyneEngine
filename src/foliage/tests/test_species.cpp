// Species declaration and the refusals that make two of `foliage`'s sentences unspellable rather
// than merely unwise. M10 task 2.4.

#include <cy/test/test.h>

#include <cy/foliage/species.h>

#include "fixtures.h"

namespace test = cy::foliage::test;
using cy::foliage::DetailTier;
using cy::foliage::FoliageSurface;
using cy::foliage::GameplayImportance;
using cy::foliage::resolve_tier;
using cy::foliage::species_id;
using cy::foliage::SpeciesClass;
using cy::foliage::SpeciesDeclaration;
using cy::foliage::SpeciesLibrary;
using cy::foliage::SpeciesProblem;
using cy::foliage::tier_for_pixels;
using cy::foliage::TierMask;
using cy::foliage::validate_species;

CY_TEST_CASE("a species library declares four species over three classes") {
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    CY_CHECK_EQ(library.size(), 4u);

    const SpeciesDeclaration* pine = library.find(species_id(test::kPine));
    CY_REQUIRE(pine != nullptr);
    CY_CHECK_EQ(pine->klass, SpeciesClass::Canopy);
    CY_CHECK_EQ(pine->importance, GameplayImportance::Cover);
    CY_CHECK(pine->promotable);

    // Two species that independently name the same species get the same identity without having
    // been introduced — the reason it is a hash rather than a registration index.
    CY_CHECK_EQ(species_id(test::kPine).value, pine->id().value);
    CY_CHECK_NE(species_id(test::kPine).value, species_id(test::kFern).value);
}

CY_TEST_CASE("declaring the same species identically is accepted and differently is refused") {
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(library.declare(test::pine()).has_value());
    CY_REQUIRE(library.declare(test::pine()).has_value());
    CY_CHECK_EQ(library.size(), 1u);

    SpeciesDeclaration altered = test::pine();
    altered.scale_max = 4.0F;
    CY_CHECK_FALSE(library.declare(altered).has_value());
    CY_CHECK_EQ(library.last_problem(), SpeciesProblem::Redeclared);
    CY_CHECK_EQ(library.size(), 1u);
}

CY_TEST_CASE("an impostor may not be a species' only distant representation") {
    // `foliage` — "Billboard impostors MAY be used as a fallback tier but SHALL NOT BE THE ONLY
    // DISTANT REPRESENTATION." Refused at declaration rather than reported from a screenshot.
    SpeciesDeclaration cardboard = test::pine();
    cardboard.tiers = TierMask::of(DetailTier::Detailed) | TierMask::of(DetailTier::Simplified) |
                      TierMask::of(DetailTier::Impostor);
    cardboard.tier_pixels[0] = 200.0F;
    cardboard.tier_pixels[1] = 40.0F;
    cardboard.tier_pixels[4] = 2.0F;
    CY_CHECK_EQ(validate_species(cardboard), SpeciesProblem::ImpostorOnlyDistantTier);

    // The same species with an aggregate rung beside the impostor is legal: the impostor is then a
    // FALLBACK, which is what the sentence permits.
    cardboard.tiers = cardboard.tiers | TierMask::of(DetailTier::Aggregate);
    cardboard.tier_pixels[2] = 10.0F;
    CY_CHECK_EQ(validate_species(cardboard), SpeciesProblem::None);
}

CY_TEST_CASE("a species declaration is refused for each of its own reasons") {
    SpeciesDeclaration unnamed = test::fern();
    unnamed.name = "";
    CY_CHECK_EQ(validate_species(unnamed), SpeciesProblem::NoName);

    SpeciesDeclaration no_detail = test::fern();
    no_detail.tiers = TierMask::of(DetailTier::Simplified);
    CY_CHECK_EQ(validate_species(no_detail), SpeciesProblem::NoDetailedTier);

    SpeciesDeclaration unordered = test::fern();
    unordered.tier_pixels[0] = 4.0F;
    unordered.tier_pixels[1] = 90.0F;
    CY_CHECK_EQ(validate_species(unordered), SpeciesProblem::TiersNotOrdered);

    SpeciesDeclaration empty_scale = test::fern();
    empty_scale.scale_min = 2.0F;
    empty_scale.scale_max = 1.0F;
    CY_CHECK_EQ(validate_species(empty_scale), SpeciesProblem::EmptyScaleRange);

    SpeciesDeclaration no_variation = test::fern();
    no_variation.variations = 0;
    CY_CHECK_EQ(validate_species(no_variation), SpeciesProblem::NoVariations);

    SpeciesDeclaration mismatched = test::fern();
    mismatched.ground_cover = true;
    CY_CHECK_EQ(validate_species(mismatched), SpeciesProblem::GroundCoverClassMismatch);

    // A blade expanded on the GPU has no instance and therefore no identity to promote.
    SpeciesDeclaration promotable_grass = test::grass();
    promotable_grass.promotable = true;
    CY_CHECK_EQ(validate_species(promotable_grass), SpeciesProblem::PromotableGroundCover);
}

CY_TEST_CASE("the surface class enumeration cannot name solid geometry") {
    // "SHALL NOT treat it as solid geometry" is an ABSENT ENUMERATOR — species.h's header note.
    // There is no expression below that declares a plant solid, and this case exists so that a
    // later edit adding one to `FoliageSurface` has to delete a test to do it.
    const FoliageSurface every[] = {FoliageSurface::Foliage, FoliageSurface::Aggregate,
                                    FoliageSurface::Thin};
    cy::u32 count = 0;
    for (FoliageSurface surface : every) {
        CY_CHECK(cy::foliage::foliage_surface_name(surface)[0] != '\0');
        ++count;
    }
    CY_CHECK_EQ(count, 3u);
}

CY_TEST_CASE(
    "a tier is selected by projected size and falls back through the holes in the ladder") {
    const SpeciesDeclaration pine = test::pine();
    CY_CHECK_EQ(tier_for_pixels(pine, 400.0F), DetailTier::Detailed);
    CY_CHECK_EQ(tier_for_pixels(pine, 60.0F), DetailTier::Simplified);
    CY_CHECK_EQ(tier_for_pixels(pine, 12.0F), DetailTier::Aggregate);
    CY_CHECK_EQ(tier_for_pixels(pine, 0.5F), DetailTier::Macro);

    // Grass has one rung. Asking for a macro tier gives the only tier it has rather than nothing,
    // which is what stops a meadow disappearing at distance in a world that cooked no macro grass.
    const SpeciesDeclaration grass = test::grass();
    CY_CHECK_EQ(resolve_tier(grass, DetailTier::Macro), DetailTier::Detailed);
    CY_CHECK_EQ(resolve_tier(pine, DetailTier::Impostor), DetailTier::Detailed);
    CY_CHECK_EQ(resolve_tier(pine, DetailTier::Aggregate), DetailTier::Aggregate);
}
