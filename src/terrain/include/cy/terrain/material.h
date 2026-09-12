#pragma once
// Terrain surface materials: what a texel stores, how rules and painting compose into it, how the
// frequencies are separated, and the runtime virtual-texture producer that evaluates the whole of
// it once per page. M10 task 2.1.
//
// `terrain` — "Terrain material layers", "Material frequency separation" and "Environment-aware
// terrain material inputs".
//
// ================================================================================================
// FOUR DECISIONS, AND THE REQUIREMENT EACH ONE ANSWERS
// ================================================================================================
//
// 1. A TEXEL STORES DOMINANT LAYERS, NOT A WEIGHT PER LAYER. "compact per-texel layer data — a
//    small number of dominant layer indices with weights — rather than one weight map per layer
//    across the world". `MaterialTexel` (tile.h) is eight bytes whatever the world's layer count
//    is, and `LayerCompositor` is what reduces an author's blend to it — REPORTING what it dropped
//    and WHERE, because "cooking SHALL report it and name the location, rather than silently
//    dropping a layer".
//
// 2. FREQUENCIES ARE COMPOSED, NOT LAYERED BY HAND. "macro (biome and geology at kilometre scale),
//    meso (material distribution at hundreds of metres), detail (breakup at metres), and micro
//    (procedural structure at centimetres)". `FrequencyStack` holds the four, `compose()` sums the
//    bands that are coarser than the viewer's footprint, and the periods are DELIBERATELY
//    INCOMMENSURATE — see `FrequencyStack::composes_without_repeating()`, which is the property the
//    "no visible repetition" scenario actually asks for and the one a test can measure.
//
// 3. RULES READ ENVIRONMENT FIELDS AS FIRST-CLASS INPUTS. "world position, slope, altitude,
//    curvature, and any environment field — biome, moisture, wetness, snow depth, water distance,
//    water depth, and flow". A rule naming a field holds an `environment::FieldId` and samples it
//    through `environment::FieldReader`, so the firewall and the one-producer rule apply to terrain
//    exactly as they apply to everyone else. Terrain does not compute wetness; it reads it.
//
// 4. THE GRAPH IS EVALUATED WHEN A PAGE IS PRODUCED, NEVER PER PIXEL. "the terrain material graph
//    is evaluated when a page is produced and sampled cheaply thereafter". `MaterialPageCache` is
//    that producer: `produce()` evaluates, `sample()` fetches, and `invalidate()` drops ONLY the
//    pages a rectangle covers. `productions()` is the counter the "evaluated once per page"
//    scenario is measured with — a claim about cost that is not counted is a claim.

#include <cy/core/base/expected.h>
#include <cy/core/determinism/random.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/terrain/surface.h>
#include <cy/terrain/tile.h>

namespace cy::terrain {

// --- Bounded layers
// -------------------------------------------------------------------------------

/// An author's contribution at one texel, before the bound is applied.
struct LayerBlend {
    u8 layer = 0;
    f32 weight = 0.0F;
};

/// A layer the bound dropped, and where. `terrain` — "the cooker SHALL report where content
/// exceeds the bound and what was dropped", and its scenario asks the report to NAME THE LOCATION.
struct LayerOverflow {
    TileCoord tile;
    u32 texel_x = 0;
    u32 texel_z = 0;
    f64 world_x = 0.0;
    f64 world_z = 0.0;
    u8 dropped_layer = 0;
    f32 dropped_weight = 0.0F;
    /// How many layers the author asked to blend here.
    u32 requested = 0;
};

/// Reduces an author's blend to the bounded texel, and reports what it cost.
///
/// The bound is `layers_per_texel` and it may not exceed `kMaxTexelLayers`, which is the storage
/// bound. Configurable downward, because a project shading two layers should not pay for four.
class LayerCompositor {
public:
    LayerCompositor(Allocator& allocator, const TileLayout& layout, u32 layers_per_texel) noexcept;

    /// Compose one texel. `blends` may be in any order and may name a layer twice; the weights of a
    /// repeated layer are summed before the bound is applied, so an author's two strokes of grass
    /// are one layer rather than two slots.
    [[nodiscard]] Status compose(const TileCoord& tile, u32 texel_x, u32 texel_z,
                                 Span<const LayerBlend> blends, MaterialTexel& out) noexcept;

    [[nodiscard]] Span<const LayerOverflow> overflows() const noexcept { return overflows_.span(); }
    [[nodiscard]] u32 bound() const noexcept { return bound_; }
    void clear() noexcept { overflows_.clear(); }

private:
    TileLayout layout_;
    u32 bound_;
    Array<LayerOverflow> overflows_;
};

// --- Frequency separation
// -------------------------------------------------------------------

/// `terrain`'s four bands, in its order, coarsest first.
enum class Frequency : u8 { Macro = 0, Meso, Detail, Micro, kCount };

inline constexpr u32 kFrequencyCount = static_cast<u32>(Frequency::kCount);

[[nodiscard]] const char* frequency_name(Frequency band) noexcept;

/// One band: how far apart its features are, and how much it contributes.
struct FrequencyBand {
    f32 metres_per_period = 1.0F;
    f32 amplitude = 0.0F;
};

/// The four bands a terrain material composes. Kilometres to centimetres.
struct FrequencyStack {
    FrequencyBand bands[kFrequencyCount] = {
        FrequencyBand{2048.0F, 0.5F},  // macro: biome and geology
        FrequencyBand{193.0F, 0.3F},   // meso: material distribution
        FrequencyBand{7.0F, 0.15F},    // detail: breakup at metres
        FrequencyBand{0.37F, 0.05F},   // micro: procedural structure
    };

    /// The composed value at a position, for a viewer whose footprint is `footprint_metres`.
    ///
    /// Bands finer than the footprint are NOT summed: they average to their mean under the
    /// footprint, and summing them anyway is what produces a distant surface that shimmers. This is
    /// the whole of "different frequencies dominate at each scale" and it is why the function takes
    /// a footprint at all.
    [[nodiscard]] f32 compose(u64 seed, f64 x, f64 z, f32 footprint_metres) const noexcept;

    /// The band that contributes most at this footprint. What a diagnostic reports and what the
    /// "viewed from a mountain top and from ground level" scenario is measured on.
    [[nodiscard]] Frequency dominant(f32 footprint_metres) const noexcept;

    /// True when no two enabled bands share a period, and no period is an integer multiple of a
    /// coarser one.
    ///
    /// THIS IS THE REPETITION REQUIREMENT, STATED AS SOMETHING CHECKABLE. A composition of bands
    /// whose periods are commensurate repeats exactly at their least common multiple, and that
    /// repeat is the "obvious texture repetition" the scenario forbids. A composition of
    /// incommensurate bands has no such period. The cooker checks this rather than asking an artist
    /// to look at it from two distances.
    [[nodiscard]] bool composes_without_repeating() const noexcept;
};

// --- Rules and painting
// ---------------------------------------------------------------------

/// What a rule reads. `terrain`'s list of first-class inputs, in its order.
enum class RuleInput : u8 {
    Slope = 0,
    Altitude,
    Curvature,
    WorldX,
    WorldZ,
    /// Any environment field, sampled through `environment::FieldReader`. Terrain does not own
    /// moisture, wetness, snow depth, water distance, water depth or flow — it reads them.
    Field,
};

[[nodiscard]] const char* rule_input_name(RuleInput input) noexcept;

/// "steep slopes becoming rock, low flat ground becoming grass, ground near water becoming wet mud,
/// and high altitude accumulating snow, without painting each by hand."
struct MaterialRule {
    const char* name = "";
    RuleInput input = RuleInput::Slope;
    /// The field, when `input` is `Field`. Ignored otherwise.
    environment::FieldId field;
    /// The band in which the rule contributes fully, in the input's own unit — degrees for slope,
    /// metres for altitude, the field's declared unit for a field.
    f32 low = 0.0F;
    f32 high = 1.0F;
    /// The width over which the contribution fades in at each end, in the same unit. Zero is a hard
    /// edge, which is what a biome boundary wants and what a snow line does not.
    f32 feather = 0.0F;
    u8 layer = 0;
    f32 weight = 1.0F;
};

/// Everything a rule can be evaluated against that is not a field.
struct RuleContext {
    f64 x = 0.0;
    f64 z = 0.0;
    f32 altitude = 0.0F;
    f32 slope_degrees = 0.0F;
    f32 curvature = 0.0F;
};

[[nodiscard]] RuleContext context_of(const SurfaceSample& sample, f64 x, f64 z) noexcept;

/// An author's local exception. `terrain` — "Painted data and rule-driven data SHALL compose, with
/// painting able to override rules LOCALLY."
struct PaintStroke {
    TerrainBounds bounds;
    u8 layer = 0;
    f32 weight = 1.0F;
    /// When set, the rules contribute nothing inside `bounds` and this stroke is the answer. That
    /// is the "painting wins locally" half; a stroke without it composes with the rules, which is
    /// the "compose" half.
    bool exclusive = false;
};

/// The rules, the strokes, and the composition of the two.
class MaterialRuleSet {
public:
    explicit MaterialRuleSet(Allocator& allocator) noexcept;

    [[nodiscard]] Status add_rule(const MaterialRule& rule) noexcept;
    [[nodiscard]] Status add_stroke(const PaintStroke& stroke) noexcept;

    [[nodiscard]] Span<const MaterialRule> rules() const noexcept { return rules_.span(); }
    [[nodiscard]] Span<const PaintStroke> strokes() const noexcept { return strokes_.span(); }

    /// The blends at one position. `fields` may be null for a terrain with no field-driven rules;
    /// a rule naming a field is then skipped and counted in `skipped_field_rules()`, because
    /// silently contributing zero and silently contributing one are both wrong and only the count
    /// distinguishes them.
    [[nodiscard]] Status evaluate(const RuleContext& context, const environment::FieldStore* fields,
                                  Array<LayerBlend>& out) const noexcept;

    [[nodiscard]] u64 skipped_field_rules() const noexcept { return skipped_; }

    /// Every field any rule reads, so a caller can declare its consumption to
    /// `environment::FieldRegistry` once rather than per sample — which is what makes the firewall
    /// a configuration-time check for terrain as well.
    [[nodiscard]] Status fields_read(Array<environment::FieldId>& out) const noexcept;

private:
    Array<MaterialRule> rules_;
    Array<PaintStroke> strokes_;
    mutable u64 skipped_ = 0;
};

// --- The runtime virtual-texture producer
// -----------------------------------------------

/// One page of the terrain material virtual texture. `level` 0 is the finest.
struct MaterialPage {
    i32 x = 0;
    i32 z = 0;
    u8 level = 0;

    friend constexpr bool operator==(const MaterialPage&, const MaterialPage&) noexcept = default;
};

/// Texels along a page edge. A page is a square of composed texels, produced once and thereafter
/// sampled as an ordinary texture fetch.
inline constexpr u32 kPageTexels = 32;
inline constexpr u32 kPageTexelCount = kPageTexels * kPageTexels;

/// What produces one texel of a page. The terrain material graph, as a callback, so the cache does
/// not know what a rule is and the rules do not know what a page is.
using TexelProducer = Status (*)(void* user, f64 x, f64 z, MaterialTexel& out);

/// Whether the device carries the paths terrain's rendering prefers, and what it cost when it does
/// not. `terrain` — "Where virtual texturing is unavailable ... terrain materials SHALL degrade to
/// conventional tiled textures with reduced unique detail AND THE LIMITATION REPORTED", and the
/// same sentence for virtual geometry.
struct MaterialPathReport {
    bool virtual_texturing = true;
    /// Unique detail relative to the virtual-texture path, where 1 is no loss. The conventional
    /// path repeats one tiled texture set over a region, so its unique detail is the fraction of
    /// the region one tile covers.
    f32 unique_detail = 1.0F;
    const char* limitation = "";
};

/// The runtime virtual texture producer, its cache, and the invalidation that keeps an edit local.
class MaterialPageCache {
public:
    /// `page_metres` is the world extent of one page. It and `kPageTexels` together fix the
    /// texel density, which is what an invalidation's granularity is measured in.
    MaterialPageCache(Allocator& allocator, const TileLayout& layout, f32 page_metres,
                      bool virtual_texturing) noexcept;

    /// Produce a page, evaluating the material graph once per texel. A page already produced and
    /// not invalidated is NOT re-evaluated, and `productions()` does not move.
    [[nodiscard]] Status produce(const MaterialPage& page, TexelProducer producer,
                                 void* user) noexcept;

    /// The composed texel at a position, from the produced page. Null when the page covering the
    /// position has not been produced — a caller shows the coarser page or the default, and does
    /// not evaluate the graph on the sampling path.
    [[nodiscard]] const MaterialTexel* sample(const MaterialPage& page, f64 x,
                                              f64 z) const noexcept;

    /// Which page covers a position at a level.
    [[nodiscard]] MaterialPage page_at(f64 x, f64 z, u8 level) const noexcept;
    [[nodiscard]] TerrainBounds page_bounds(const MaterialPage& page) const noexcept;

    /// Drop every produced page the rectangle touches. Returns how many. `terrain` — "Produced
    /// terrain pages SHALL be cached and invalidated ONLY WHERE THEIR INPUTS CHANGE ... so an edit
    /// re-produces a region rather than the world."
    [[nodiscard]] u32 invalidate(const TerrainBounds& bounds) noexcept;

    [[nodiscard]] bool is_produced(const MaterialPage& page) const noexcept;
    [[nodiscard]] usize produced_pages() const noexcept { return pages_.size(); }
    /// Texels evaluated since construction. The number the "evaluated once per page" scenario is
    /// measured with.
    [[nodiscard]] u64 productions() const noexcept { return productions_; }
    [[nodiscard]] MaterialPathReport path() const noexcept;

private:
    struct Produced {
        MaterialPage page;
        Array<MaterialTexel> texels;

        explicit Produced(Allocator& allocator) noexcept : texels(allocator) {}
    };

    [[nodiscard]] const Produced* find(const MaterialPage& page) const noexcept;

    Allocator* allocator_;
    TileLayout layout_;
    f32 page_metres_;
    bool virtual_texturing_;
    Array<Produced> pages_;
    HashMap<u64, usize> index_;
    u64 productions_ = 0;
};

}  // namespace cy::terrain
