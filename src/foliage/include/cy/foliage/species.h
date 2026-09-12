#pragma once
// What a species IS: its identity, the detail tiers it renders through, the surface class the
// geometry system applies to it, the wind response it supports, and the gameplay importance that
// protects it from budget pressure. M10 task 2.4.
//
// `foliage` — "Foliage geometry classes": "Foliage assets SHALL declare their surface class (see
// `virtual-geometry`) so that the geometry system applies appropriate simplification, culling, and
// rasterisation policy: foliage simplifies poorly, occludes poorly, and is dominated by
// alpha-tested thin surfaces."
//
// ================================================================================================
// "SHALL NOT TREAT IT AS SOLID GEOMETRY" IS AN ABSENT ENUMERATOR, NOT A REFUSAL
// ================================================================================================
//
// `rendering::SurfaceClass` has five values and two of them — `Solid` and `Hair` — are not things a
// plant may be. A `FoliageSurface` that carried all five and refused two at registration would put
// the guarantee in a function a caller can forget to call; this enumeration carries the three that
// are legal and there is no expression that declares a species solid. The one mapping onto the
// renderer's enumeration lives in `cy::foliage-render` (cook.h), which is the only half of this
// module that names a renderer type at all, and `test_cook.cpp` holds that the mapping is total and
// that no input produces `Solid`.
//
// ================================================================================================
// THE TIER LADDER, AND WHY AN IMPOSTOR CANNOT BE THE LAST RUNG
// ================================================================================================
//
// "Detail SHALL progress from detailed geometry near the camera, through simplified virtual
// geometry, to AGGREGATE REPRESENTATIONS in which many plants become one object, to a canopy or
// forest macro representation at long range. Billboard impostors MAY be used as a fallback tier but
// SHALL NOT BE THE ONLY DISTANT REPRESENTATION."
//
// A species declares which tiers it has. `validate_species()` refuses one whose only tier beyond
// `Simplified` is `Impostor`, because that is the configuration the sentence forbids — and it
// refuses it at declaration time rather than reporting a cardboard forest from a screenshot.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash.h>
#include <cy/core/memory/hash_map.h>

namespace cy::foliage {

/// A species' identity: a hash of its stable name, resolved at compile time where the name is a
/// literal.
///
/// An identity and not an index, for the reason `environment::FieldId` is one: a cooked cluster, a
/// stored exception and a placement rule all key on it, and a number that depended on registration
/// order would rename every stored instance the day a species was added.
struct SpeciesId {
    u64 value = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(SpeciesId, SpeciesId) noexcept = default;
    /// Ordered so a diagnostic, a cluster's species table and a cook manifest sort one way.
    friend constexpr bool operator<(SpeciesId a, SpeciesId b) noexcept { return a.value < b.value; }
};

namespace detail {

/// FNV-1a over a name. Unseeded and `constexpr`, for the reason `environment::detail::hash_name()`
/// is: `cy::hash_bytes()` is randomised per process in development builds, and a species identity
/// that changed between two runs of the cooker would not be an identity.
[[nodiscard]] constexpr u64 hash_name(const char* text) noexcept {
    u64 value = 0xcbf2'9ce4'8422'2325ULL;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        value ^= static_cast<u64>(static_cast<u8>(*cursor));
        value *= 0x0000'0100'0000'01b3ULL;
    }
    return value;
}

}  // namespace detail

[[nodiscard]] constexpr SpeciesId species_id(const char* name) noexcept {
    return SpeciesId{detail::hash_name(name)};
}

/// The surface classes a plant may declare. Three of `rendering::SurfaceClass`'s five; see the
/// header note for why the other two are absent rather than refused.
enum class FoliageSurface : u8 {
    /// Alpha-tested thin surfaces — leaves, fronds, needles. The ordinary case.
    Foliage = 0,
    /// Many small elements the cooker may merge — grass clumps, bramble, a canopy shell.
    Aggregate,
    /// Single-sided thin geometry with no volume — reeds, wheat, a cattail.
    Thin,
};

[[nodiscard]] const char* foliage_surface_name(FoliageSurface surface) noexcept;

/// What a species is, for placement, for budget and for promotion. Not a rendering category: the
/// renderer sees `FoliageSurface`, and these are the classes `foliage`'s budget requirement means
/// by "instance counts BY SPECIES CLASS".
enum class SpeciesClass : u8 {
    /// Trees: the expensive instances, the ones that promote, the last to be cut.
    Canopy = 0,
    /// Shrubs and bushes: mid-sized, numerous, rarely promoted.
    Understory,
    /// Grass, flowers, moss — the ground cover expanded on the GPU from patches (grass.h).
    GroundCover,
    /// Rocks, deadfall, debris placed by the same rules. Not a plant, placed like one.
    Scatter,
    kCount,
};

inline constexpr u32 kSpeciesClassCount = static_cast<u32>(SpeciesClass::kCount);

[[nodiscard]] const char* species_class_name(SpeciesClass value) noexcept;

/// The detail ladder, in the specification's own order, coarsening left to right.
enum class DetailTier : u8 {
    /// Full authored geometry. Near the camera.
    Detailed = 0,
    /// Simplified virtual geometry clusters — the geometry system's own ladder.
    Simplified,
    /// Many plants become one object. "A forest becomes one object" is this tier.
    Aggregate,
    /// A canopy or forest macro representation: one object for a whole cluster region.
    Macro,
    /// A camera-facing billboard. A FALLBACK tier and never the only distant one.
    Impostor,
    kCount,
};

inline constexpr u32 kDetailTierCount = static_cast<u32>(DetailTier::kCount);

[[nodiscard]] const char* detail_tier_name(DetailTier tier) noexcept;

/// Which tiers a species has, as a bitmask. Small, copied, and comparable — a species table in a
/// cooked cluster carries one of these per species rather than an array of five booleans.
struct TierMask {
    u8 bits = 0;

    [[nodiscard]] static constexpr TierMask of(DetailTier tier) noexcept {
        return TierMask{static_cast<u8>(1U << static_cast<u32>(tier))};
    }
    [[nodiscard]] constexpr bool has(DetailTier tier) const noexcept {
        return (bits & of(tier).bits) != 0;
    }
    constexpr void set(DetailTier tier) noexcept { bits |= of(tier).bits; }
    [[nodiscard]] constexpr TierMask operator|(TierMask other) const noexcept {
        return TierMask{static_cast<u8>(bits | other.bits)};
    }
    [[nodiscard]] constexpr u32 count() const noexcept {
        u32 total = 0;
        for (u32 bit = 0; bit < kDetailTierCount; ++bit) {
            total += ((bits >> bit) & 1U);
        }
        return total;
    }

    friend constexpr bool operator==(TierMask, TierMask) noexcept = default;
};

/// How much wind detail a species can express at its finest. `foliage` — "Near foliage SHALL
/// support HIERARCHICAL RESPONSE — trunk, branch, twig, and leaf motion at different amplitudes and
/// frequencies — and distant foliage SHALL receive a simple sway."
///
/// The DECLARED level is the ceiling; the level actually evaluated is the lesser of it and what
/// distance and budget allow (wind.h). A blade of grass declares `Sway` because it has no branches,
/// and no budget setting can make it grow some.
enum class WindDetail : u8 {
    /// No response at all: a rock placed by foliage rules.
    None = 0,
    /// One whole-plant bend. Grass, and every plant far enough away.
    Sway,
    /// Trunk plus branch.
    Branch,
    /// Trunk, branch, twig and leaf — the full hierarchy.
    Leaf,
};

[[nodiscard]] const char* wind_detail_name(WindDetail detail) noexcept;

/// `foliage` — "species MAY declare a gameplay importance that protects them — cover that matters
/// tactically SHALL NOT vanish because the frame is busy."
///
/// Ordered most important first, so the budget's reduction walk is an ascending one and the
/// comparison reads the way the sentence does.
enum class GameplayImportance : u8 {
    /// Removing it changes what the player can do. Never reduced by budget pressure.
    Cover = 0,
    /// Gameplay reads it — a harvestable, a trail marker — but losing it is survivable.
    Relevant,
    /// Scenery.
    Decorative,
    kCount,
};

[[nodiscard]] const char* gameplay_importance_name(GameplayImportance value) noexcept;

/// Everything a species declares.
///
/// `name` is a literal the caller owns for the lifetime of the library: it is what a diagnostic
/// prints and what `species_id()` hashed, and a library that copied it would allocate at startup
/// for a string nothing but a report ever reads. `environment::FieldDeclaration` makes the same
/// choice for the same reason.
struct SpeciesDeclaration {
    const char* name = "";
    SpeciesClass klass = SpeciesClass::Canopy;
    FoliageSurface surface = FoliageSurface::Foliage;
    GameplayImportance importance = GameplayImportance::Decorative;

    /// The tiers this species has. Must contain `Detailed`; see `validate_species()`.
    TierMask tiers = TierMask::of(DetailTier::Detailed);

    /// Screen-space size in pixels below which each tier gives way to the next. Indexed by the
    /// tier being LEFT, so `tier_pixels[Detailed]` is where detailed geometry stops.
    f32 tier_pixels[kDetailTierCount] = {256.0F, 48.0F, 12.0F, 3.0F, 0.0F};

    WindDetail wind = WindDetail::Sway;
    /// Metres of trunk sway at one metre per second of wind. The amplitude the hierarchy scales.
    f32 wind_amplitude = 0.05F;
    /// How stiff the plant is: 0 is a reed, 1 is a mature oak. Scales the response and the phase
    /// lag between the trunk and the leaves.
    f32 stiffness = 0.5F;

    /// The scale range placement draws within, and the quantisation `FoliageInstance::scale`
    /// stores against. A species whose range is a single value stores a constant and costs the
    /// same two bytes; it is not worth a second encoding.
    f32 scale_min = 0.9F;
    f32 scale_max = 1.1F;

    /// How many authored variations exist — different meshes, different tints, different ages.
    /// `FoliageInstance::variation` indexes them.
    u8 variations = 1;

    /// Metres of radius the plant occupies on the ground. Placement uses it for the minimum
    /// spacing between two instances and the interaction field uses it for the bend footprint.
    f32 footprint_metres = 1.0F;

    /// Whether gameplay may promote an instance of this species to an entity (promotion.h). A
    /// grass blade declares false, and a promotion request for it is refused rather than creating
    /// an entity for a thing with no gameplay surface.
    bool promotable = false;

    /// Whether this species is expanded on the GPU from patch descriptions rather than stored as
    /// instances (grass.h). Only `GroundCover` may declare it; `validate_species()` refuses the
    /// other pairing, because a tree expanded from a patch description is a tree nobody can fell.
    bool ground_cover = false;

    [[nodiscard]] SpeciesId id() const noexcept { return species_id(name); }
};

/// Why a species declaration was refused. A code rather than a string, so a test asserts on the
/// reason rather than on the wording — `environment::DeclarationProblem`'s shape, deliberately.
enum class SpeciesProblem : u8 {
    None = 0,
    NoName,
    /// No `Detailed` tier: there is nothing to simplify FROM.
    NoDetailedTier,
    /// The only representation beyond `Simplified` is `Impostor`. The specification's own
    /// "SHALL NOT be the only distant representation", refused.
    ImpostorOnlyDistantTier,
    /// Tier thresholds that do not descend. A ladder whose rungs are out of order selects a
    /// coarser tier near the camera than far from it.
    TiersNotOrdered,
    EmptyScaleRange,
    NoVariations,
    /// `ground_cover` on something that is not `GroundCover`.
    GroundCoverClassMismatch,
    /// A `promotable` species that is expanded on the GPU: there is no instance to promote.
    PromotableGroundCover,
    /// A re-declaration that differs from the one already registered.
    Redeclared,
};

[[nodiscard]] const char* species_problem_name(SpeciesProblem problem) noexcept;

/// Check a declaration on its own. Separate from the library so a cooker and an editor can validate
/// before a library exists, and so this file's refusals are testable without one.
[[nodiscard]] SpeciesProblem validate_species(const SpeciesDeclaration& declaration) noexcept;

/// The coarsest tier a species has at or beyond `wanted`, falling back to the finest it has.
/// `DetailTier` is a ladder with holes in it — a grass species has `Detailed` and nothing else —
/// and every consumer that selects a tier needs the same fallback rule, so it is written once.
[[nodiscard]] DetailTier resolve_tier(const SpeciesDeclaration& declaration,
                                      DetailTier wanted) noexcept;

/// The tier a species renders at for a given projected size in pixels, before any budget bias.
[[nodiscard]] DetailTier tier_for_pixels(const SpeciesDeclaration& declaration,
                                         f32 pixels) noexcept;

/// The declared species of a world. Holds declarations; holds no instances and no geometry.
///
/// The split from `ClusterStore` is `environment`'s registry/store split seen once more: a library
/// that could hold an instance would be a library every system had a reason to reach into.
class SpeciesLibrary {
public:
    explicit SpeciesLibrary(Allocator& allocator) noexcept;

    SpeciesLibrary(const SpeciesLibrary&) = delete;
    SpeciesLibrary& operator=(const SpeciesLibrary&) = delete;

    /// Declare a species. Re-declaring one identically is accepted — two modules that both need
    /// `pine` to exist should not have to agree on which declares it — and re-declaring it
    /// differently is refused, because the difference would silently change what every stored
    /// instance of it means.
    [[nodiscard]] Status declare(const SpeciesDeclaration& declaration) noexcept;

    [[nodiscard]] const SpeciesDeclaration* find(SpeciesId id) const noexcept;
    [[nodiscard]] usize size() const noexcept { return species_.size(); }
    [[nodiscard]] Span<const SpeciesDeclaration> species() const noexcept {
        return species_.span();
    }

    /// The last refusal's reason. Valid until the next refused `declare()`.
    [[nodiscard]] SpeciesProblem last_problem() const noexcept { return problem_; }

private:
    Allocator* allocator_;
    Array<SpeciesDeclaration> species_;
    HashMap<u64, usize> index_;
    SpeciesProblem problem_ = SpeciesProblem::None;
};

}  // namespace cy::foliage

namespace cy {

template <>
struct Hash<foliage::SpeciesId> {
    [[nodiscard]] u64 operator()(foliage::SpeciesId id) const noexcept {
        return hash_integer(id.value, hash_seed());
    }
};

}  // namespace cy
