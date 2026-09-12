// Species declaration and its refusals. See species.h for the two design notes.

#include <cy/foliage/species.h>

namespace cy::foliage {

const char* foliage_surface_name(FoliageSurface surface) noexcept {
    switch (surface) {
        case FoliageSurface::Foliage:
            return "foliage";
        case FoliageSurface::Aggregate:
            return "aggregate";
        case FoliageSurface::Thin:
            return "thin";
    }
    return "unknown";
}

const char* species_class_name(SpeciesClass value) noexcept {
    switch (value) {
        case SpeciesClass::Canopy:
            return "canopy";
        case SpeciesClass::Understory:
            return "understory";
        case SpeciesClass::GroundCover:
            return "ground-cover";
        case SpeciesClass::Scatter:
            return "scatter";
        case SpeciesClass::kCount:
            break;
    }
    return "unknown";
}

const char* detail_tier_name(DetailTier tier) noexcept {
    switch (tier) {
        case DetailTier::Detailed:
            return "detailed";
        case DetailTier::Simplified:
            return "simplified";
        case DetailTier::Aggregate:
            return "aggregate";
        case DetailTier::Macro:
            return "macro";
        case DetailTier::Impostor:
            return "impostor";
        case DetailTier::kCount:
            break;
    }
    return "unknown";
}

const char* wind_detail_name(WindDetail detail) noexcept {
    switch (detail) {
        case WindDetail::None:
            return "none";
        case WindDetail::Sway:
            return "sway";
        case WindDetail::Branch:
            return "branch";
        case WindDetail::Leaf:
            return "leaf";
    }
    return "unknown";
}

const char* gameplay_importance_name(GameplayImportance value) noexcept {
    switch (value) {
        case GameplayImportance::Cover:
            return "cover";
        case GameplayImportance::Relevant:
            return "relevant";
        case GameplayImportance::Decorative:
            return "decorative";
        case GameplayImportance::kCount:
            break;
    }
    return "unknown";
}

const char* species_problem_name(SpeciesProblem problem) noexcept {
    switch (problem) {
        case SpeciesProblem::None:
            return "none";
        case SpeciesProblem::NoName:
            return "no-name";
        case SpeciesProblem::NoDetailedTier:
            return "no-detailed-tier";
        case SpeciesProblem::ImpostorOnlyDistantTier:
            return "impostor-only-distant-tier";
        case SpeciesProblem::TiersNotOrdered:
            return "tiers-not-ordered";
        case SpeciesProblem::EmptyScaleRange:
            return "empty-scale-range";
        case SpeciesProblem::NoVariations:
            return "no-variations";
        case SpeciesProblem::GroundCoverClassMismatch:
            return "ground-cover-class-mismatch";
        case SpeciesProblem::PromotableGroundCover:
            return "promotable-ground-cover";
        case SpeciesProblem::Redeclared:
            return "redeclared";
    }
    return "unknown";
}

namespace {

/// The tiers that count as a DISTANT representation: everything coarser than `Simplified` except
/// the impostor itself. The specification's "SHALL NOT be the only distant representation" is
/// exactly "this set is not empty when `Impostor` is declared".
[[nodiscard]] bool has_non_impostor_distant_tier(TierMask tiers) noexcept {
    return tiers.has(DetailTier::Aggregate) || tiers.has(DetailTier::Macro);
}

/// The declared tiers' thresholds must descend across the tiers that EXIST. A species with no
/// `Macro` tier may leave `tier_pixels[Macro]` at any value, so the walk skips undeclared rungs
/// rather than comparing numbers nothing reads.
[[nodiscard]] bool thresholds_descend(const SpeciesDeclaration& declaration) noexcept {
    f32 previous = 0.0F;
    bool first = true;
    for (u32 index = 0; index < kDetailTierCount; ++index) {
        const auto tier = static_cast<DetailTier>(index);
        if (!declaration.tiers.has(tier)) {
            continue;
        }
        const f32 value = declaration.tier_pixels[index];
        if (!first && value > previous) {
            return false;
        }
        previous = value;
        first = false;
    }
    return true;
}

[[nodiscard]] bool same_declaration(const SpeciesDeclaration& a,
                                    const SpeciesDeclaration& b) noexcept {
    if (a.klass != b.klass || a.surface != b.surface || a.importance != b.importance ||
        !(a.tiers == b.tiers) || a.wind != b.wind || a.variations != b.variations ||
        a.promotable != b.promotable || a.ground_cover != b.ground_cover) {
        return false;
    }
    if (a.scale_min != b.scale_min || a.scale_max != b.scale_max ||
        a.footprint_metres != b.footprint_metres || a.wind_amplitude != b.wind_amplitude ||
        a.stiffness != b.stiffness) {
        return false;
    }
    for (u32 index = 0; index < kDetailTierCount; ++index) {
        if (a.tier_pixels[index] != b.tier_pixels[index]) {
            return false;
        }
    }
    return true;
}

}  // namespace

SpeciesProblem validate_species(const SpeciesDeclaration& declaration) noexcept {
    if (declaration.name == nullptr || declaration.name[0] == '\0') {
        return SpeciesProblem::NoName;
    }
    if (!declaration.tiers.has(DetailTier::Detailed)) {
        return SpeciesProblem::NoDetailedTier;
    }
    // The specification's own sentence: an impostor MAY be a fallback tier and SHALL NOT be the
    // only distant representation. A species that declares one without an aggregate or a macro rung
    // is a forest that becomes cardboard, and it is refused here rather than reported from a
    // screenshot.
    if (declaration.tiers.has(DetailTier::Impostor) &&
        !has_non_impostor_distant_tier(declaration.tiers)) {
        return SpeciesProblem::ImpostorOnlyDistantTier;
    }
    if (!thresholds_descend(declaration)) {
        return SpeciesProblem::TiersNotOrdered;
    }
    if (!(declaration.scale_max >= declaration.scale_min) || declaration.scale_max <= 0.0F) {
        return SpeciesProblem::EmptyScaleRange;
    }
    if (declaration.variations == 0) {
        return SpeciesProblem::NoVariations;
    }
    if (declaration.ground_cover && declaration.klass != SpeciesClass::GroundCover) {
        return SpeciesProblem::GroundCoverClassMismatch;
    }
    // A patch-expanded blade has no instance and therefore no identity to promote. Refusing the
    // pairing is what stops a caller from asking for an entity that could never be demoted back.
    if (declaration.ground_cover && declaration.promotable) {
        return SpeciesProblem::PromotableGroundCover;
    }
    return SpeciesProblem::None;
}

DetailTier resolve_tier(const SpeciesDeclaration& declaration, DetailTier wanted) noexcept {
    for (u32 index = static_cast<u32>(wanted); index < kDetailTierCount; ++index) {
        const auto tier = static_cast<DetailTier>(index);
        if (declaration.tiers.has(tier)) {
            return tier;
        }
    }
    // Nothing at or beyond `wanted`: fall back to the finest declared rung, which every valid
    // species has because `Detailed` is required.
    for (u32 index = 0; index < kDetailTierCount; ++index) {
        const auto tier = static_cast<DetailTier>(index);
        if (declaration.tiers.has(tier)) {
            return tier;
        }
    }
    return DetailTier::Detailed;
}

DetailTier tier_for_pixels(const SpeciesDeclaration& declaration, f32 pixels) noexcept {
    DetailTier chosen = DetailTier::Detailed;
    for (u32 index = 0; index < kDetailTierCount; ++index) {
        const auto tier = static_cast<DetailTier>(index);
        if (!declaration.tiers.has(tier)) {
            continue;
        }
        if (pixels >= declaration.tier_pixels[index]) {
            return tier;
        }
        chosen = tier;
    }
    return chosen;
}

SpeciesLibrary::SpeciesLibrary(Allocator& allocator) noexcept
    : allocator_(&allocator), species_(allocator), index_(allocator) {}

Status SpeciesLibrary::declare(const SpeciesDeclaration& declaration) noexcept {
    problem_ = validate_species(declaration);
    if (problem_ != SpeciesProblem::None) {
        return fail(ErrorCode::InvalidArgument, species_problem_name(problem_));
    }
    const SpeciesId id = declaration.id();
    if (const usize* existing = index_.find(id.value); existing != nullptr) {
        if (same_declaration(species_[*existing], declaration)) {
            return ok();
        }
        problem_ = SpeciesProblem::Redeclared;
        return fail(ErrorCode::AlreadyExists,
                    "a species of this name is already declared with different parameters");
    }
    if (Status pushed = species_.push_back(declaration); !pushed) {
        return pushed;
    }
    if (Expected<usize*, Error> placed = index_.insert(id.value, species_.size() - 1); !placed) {
        species_.pop_back();
        return fail(placed.error().code, placed.error().message);
    }
    return ok();
}

const SpeciesDeclaration* SpeciesLibrary::find(SpeciesId id) const noexcept {
    const usize* slot = index_.find(id.value);
    return slot == nullptr ? nullptr : &species_[*slot];
}

}  // namespace cy::foliage
