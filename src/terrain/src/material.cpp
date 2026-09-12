// The material half: the bound applied to a texel and the report of what it cost, the frequency
// composition, the rules and the painting, and the page producer. M10 task 2.1.

#include <cy/terrain/material.h>

#include <cmath>
#include <utility>

namespace cy::terrain {
namespace {

[[nodiscard]] f32 clamp_f32(f32 value, f32 low, f32 high) noexcept {
    if (value < low) {
        return low;
    }
    return (value > high) ? high : value;
}

[[nodiscard]] i64 ifloor(f64 value) noexcept {
    return static_cast<i64>(std::floor(value));
}

/// The stream every terrain noise draw is derived from.
///
/// `simulation-and-determinism`'s counter-based streams and nothing else: design.md §3 says M10
/// adds no second mechanism, and the M10 spike's fourth condition says a generated value is derived
/// from stable identifiers — here the band and the lattice point, through `substream` then `draw`,
/// which is the derivation §1.6 names. A spatial hash written here would be a second random model
/// with its own failure modes and nothing checking them.
[[nodiscard]] determinism::RandomStream band_stream(u64 seed, Frequency band) noexcept {
    const determinism::StreamId root = determinism::stream_id("terrain.material.frequency");
    return {seed, determinism::substream(root, static_cast<u64>(band)),
            determinism::StreamPurpose::Authoritative};
}

/// A lattice point's value in [0, 1). `entity` is the packed lattice coordinate, which is the
/// stable identifier of the point rather than a traversal position.
[[nodiscard]] f32 lattice_value(const determinism::RandomStream& stream, i64 lx, i64 lz) noexcept {
    const u64 packed = (static_cast<u64>(static_cast<u32>(static_cast<i32>(lx))) << 32U) |
                       static_cast<u64>(static_cast<u32>(static_cast<i32>(lz)));
    return stream.unit_float(determinism::SimulationPoint{}, packed, 0);
}

/// Smoothstep, so the interpolation between lattice points has a continuous first derivative and a
/// composed band does not show its own lattice as a grid of creases.
[[nodiscard]] f32 smooth(f32 t) noexcept {
    return t * t * (3.0F - (2.0F * t));
}

/// One band's value noise at a position.
[[nodiscard]] f32 band_value(u64 seed, Frequency band, const FrequencyBand& parameters, f64 x,
                             f64 z) noexcept {
    const determinism::RandomStream stream = band_stream(seed, band);
    const f64 period = static_cast<f64>(parameters.metres_per_period);
    const f64 lx = x / period;
    const f64 lz = z / period;
    const i64 x0 = ifloor(lx);
    const i64 z0 = ifloor(lz);
    const f32 fx = smooth(static_cast<f32>(lx - static_cast<f64>(x0)));
    const f32 fz = smooth(static_cast<f32>(lz - static_cast<f64>(z0)));
    const f32 v00 = lattice_value(stream, x0, z0);
    const f32 v10 = lattice_value(stream, x0 + 1, z0);
    const f32 v01 = lattice_value(stream, x0, z0 + 1);
    const f32 v11 = lattice_value(stream, x0 + 1, z0 + 1);
    return (((v00 * (1.0F - fx)) + (v10 * fx)) * (1.0F - fz)) +
           (((v01 * (1.0F - fx)) + (v11 * fx)) * fz);
}

/// The trapezoid a rule contributes: 0 outside [low - feather, high + feather], 1 inside
/// [low, high], and a linear ramp between.
[[nodiscard]] f32 rule_response(const MaterialRule& rule, f32 value) noexcept {
    if (value >= rule.low && value <= rule.high) {
        return 1.0F;
    }
    if (rule.feather <= 0.0F) {
        return 0.0F;
    }
    if (value < rule.low) {
        return clamp_f32((value - (rule.low - rule.feather)) / rule.feather, 0.0F, 1.0F);
    }
    return clamp_f32(((rule.high + rule.feather) - value) / rule.feather, 0.0F, 1.0F);
}

[[nodiscard]] u64 page_key(const MaterialPage& page) noexcept {
    u64 value = hash_integer(static_cast<u64>(static_cast<u32>(page.x)), kTerrainHashSeed);
    value = hash_combine(value, static_cast<u64>(static_cast<u32>(page.z)));
    return hash_combine(value, page.level);
}

}  // namespace

const char* frequency_name(Frequency band) noexcept {
    switch (band) {
        case Frequency::Macro:
            return "macro";
        case Frequency::Meso:
            return "meso";
        case Frequency::Detail:
            return "detail";
        case Frequency::Micro:
            return "micro";
        case Frequency::kCount:
            break;
    }
    return "unknown";
}

const char* rule_input_name(RuleInput input) noexcept {
    switch (input) {
        case RuleInput::Slope:
            return "slope";
        case RuleInput::Altitude:
            return "altitude";
        case RuleInput::Curvature:
            return "curvature";
        case RuleInput::WorldX:
            return "world-x";
        case RuleInput::WorldZ:
            return "world-z";
        case RuleInput::Field:
            return "field";
    }
    return "unknown";
}

// --- The bound
// ------------------------------------------------------------------------------------

LayerCompositor::LayerCompositor(Allocator& allocator, const TileLayout& layout,
                                 u32 layers_per_texel) noexcept
    : layout_(layout),
      bound_((layers_per_texel == 0 || layers_per_texel > kMaxTexelLayers) ? kMaxTexelLayers
                                                                           : layers_per_texel),
      overflows_(allocator) {}

namespace {

/// Merge repeats and drop empties. An author's two strokes of the same layer are one layer, and
/// counting them as two would report an overflow the author did not cause.
[[nodiscard]] u32 merge_blends(Span<const LayerBlend> blends, LayerBlend* merged,
                               u32 capacity) noexcept {
    u32 count = 0;
    for (const LayerBlend& blend : blends) {
        if (blend.weight <= 0.0F) {
            continue;
        }
        bool found = false;
        for (u32 index = 0; index < count; ++index) {
            if (merged[index].layer == blend.layer) {
                merged[index].weight += blend.weight;
                found = true;
                break;
            }
        }
        if (!found && count < capacity) {
            merged[count] = blend;
            ++count;
        }
    }
    return count;
}

/// Descending by weight, with the layer index breaking ties, so two machines keep the same layers
/// and a re-cook of unchanged content produces the same bytes.
void sort_blends(LayerBlend* merged, u32 count) noexcept {
    for (u32 a = 1; a < count; ++a) {
        const LayerBlend key = merged[a];
        u32 b = a;
        while (b > 0 && (merged[b - 1].weight < key.weight ||
                         (merged[b - 1].weight == key.weight && merged[b - 1].layer > key.layer))) {
            merged[b] = merged[b - 1];
            --b;
        }
        merged[b] = key;
    }
}

/// The surviving layers, renormalised so the surface is fully covered by what is left rather than
/// fading toward nothing where the bound bit.
void write_texel(const LayerBlend* merged, u32 keep, f32 kept, MaterialTexel& out) noexcept {
    out = MaterialTexel{};
    if (kept <= 0.0F || keep == 0) {
        out.weight[0] = 255;
        return;
    }
    // The share is rounded with `lround` rather than by adding a half: casting `(x + 0.5)` to an
    // integer rounds the wrong way for an exact half, and a weight that is exactly half a step is
    // the ordinary case for two layers blended evenly.
    u32 assigned = 0;
    for (u32 index = 0; index < keep; ++index) {
        out.layer[index] = merged[index].layer;
        const u32 share = static_cast<u32>(std::lround(merged[index].weight / kept * 255.0F));
        out.weight[index] = static_cast<u8>((share > 255) ? 255 : share);
        assigned += out.weight[index];
    }
    // Rounding leaves the sum a step or two from 255; the dominant layer absorbs it, so the sum is
    // exactly 255 and shading divides by a constant.
    if (assigned != 255) {
        const i32 corrected = static_cast<i32>(out.weight[0]) + (255 - static_cast<i32>(assigned));
        const i32 low = (corrected < 0) ? 0 : corrected;
        out.weight[0] = static_cast<u8>((low > 255) ? 255 : low);
    }
}

}  // namespace

Status LayerCompositor::compose(const TileCoord& tile, u32 texel_x, u32 texel_z,
                                Span<const LayerBlend> blends, MaterialTexel& out) noexcept {
    LayerBlend merged[256];
    const u32 merged_count = merge_blends(blends, merged, 256);
    sort_blends(merged, merged_count);

    const u32 keep = (merged_count < bound_) ? merged_count : bound_;
    f32 kept = 0.0F;
    for (u32 index = 0; index < keep; ++index) {
        kept += merged[index].weight;
    }
    for (u32 index = keep; index < merged_count; ++index) {
        // "cooking SHALL report it and name the location, rather than silently dropping a layer."
        const TerrainPoint at = sample_position(layout_, tile, texel_x, texel_z);
        LayerOverflow overflow;
        overflow.tile = tile;
        overflow.texel_x = texel_x;
        overflow.texel_z = texel_z;
        overflow.world_x = at.x;
        overflow.world_z = at.z;
        overflow.dropped_layer = merged[index].layer;
        overflow.dropped_weight = merged[index].weight;
        overflow.requested = merged_count;
        if (Status pushed = overflows_.push_back(overflow); !pushed) {
            return pushed;
        }
    }
    write_texel(merged, keep, kept, out);
    return ok();
}

// --- Frequency separation
// -------------------------------------------------------------------

f32 FrequencyStack::compose(u64 seed, f64 x, f64 z, f32 footprint_metres) const noexcept {
    f32 total = 0.0F;
    for (u32 index = 0; index < kFrequencyCount; ++index) {
        const FrequencyBand& band = bands[index];
        if (band.amplitude <= 0.0F || band.metres_per_period <= 0.0F) {
            continue;
        }
        // A band whose whole period fits inside the footprint contributes its mean, which for value
        // noise is one half — summing its detail would be summing noise the viewer cannot resolve.
        if (band.metres_per_period < footprint_metres) {
            total += band.amplitude * 0.5F;
            continue;
        }
        total += band.amplitude * band_value(seed, static_cast<Frequency>(index), band, x, z);
    }
    return total;
}

Frequency FrequencyStack::dominant(f32 footprint_metres) const noexcept {
    Frequency chosen = Frequency::Macro;
    f32 best = -1.0F;
    for (u32 index = 0; index < kFrequencyCount; ++index) {
        const FrequencyBand& band = bands[index];
        if (band.amplitude <= 0.0F || band.metres_per_period < footprint_metres) {
            continue;
        }
        if (band.amplitude > best) {
            best = band.amplitude;
            chosen = static_cast<Frequency>(index);
        }
    }
    return chosen;
}

bool FrequencyStack::composes_without_repeating() const noexcept {
    for (u32 a = 0; a < kFrequencyCount; ++a) {
        if (bands[a].amplitude <= 0.0F) {
            continue;
        }
        for (u32 b = a + 1; b < kFrequencyCount; ++b) {
            if (bands[b].amplitude <= 0.0F) {
                continue;
            }
            const f32 coarse = bands[a].metres_per_period;
            const f32 fine = bands[b].metres_per_period;
            if (fine <= 0.0F || coarse <= 0.0F) {
                return false;
            }
            const f32 ratio = coarse / fine;
            const f32 nearest = std::round(ratio);
            // Commensurate periods repeat at their least common multiple. A ratio within a
            // thousandth of an integer is close enough to one that the repeat is visible.
            if (nearest >= 1.0F && std::fabs(ratio - nearest) < 1.0e-3F) {
                return false;
            }
        }
    }
    return true;
}

// --- Rules and painting
// ---------------------------------------------------------------------

RuleContext context_of(const SurfaceSample& sample, f64 x, f64 z) noexcept {
    RuleContext context;
    context.x = x;
    context.z = z;
    context.altitude = sample.height;
    context.slope_degrees = sample.slope_degrees;
    context.curvature = sample.curvature;
    return context;
}

MaterialRuleSet::MaterialRuleSet(Allocator& allocator) noexcept
    : rules_(allocator), strokes_(allocator) {}

Status MaterialRuleSet::add_rule(const MaterialRule& rule) noexcept {
    return rules_.push_back(rule);
}

Status MaterialRuleSet::add_stroke(const PaintStroke& stroke) noexcept {
    return strokes_.push_back(stroke);
}

namespace {

/// The value one rule reads, and whether it could be read at all. A field rule with no store is the
/// only case that cannot: contributing zero and contributing one are both wrong, and only a count
/// distinguishes them — see `skipped_field_rules()`.
[[nodiscard]] bool rule_value(const MaterialRule& rule, const RuleContext& context,
                              const environment::FieldStore* fields, f32& out) noexcept {
    switch (rule.input) {
        case RuleInput::Slope:
            out = context.slope_degrees;
            return true;
        case RuleInput::Altitude:
            out = context.altitude;
            return true;
        case RuleInput::Curvature:
            out = context.curvature;
            return true;
        case RuleInput::WorldX:
            out = static_cast<f32>(context.x);
            return true;
        case RuleInput::WorldZ:
            out = static_cast<f32>(context.z);
            return true;
        case RuleInput::Field:
            break;
    }
    if (fields == nullptr) {
        return false;
    }
    // The deterministic path, whatever the field's class: a terrain material that read the finest
    // RESIDENT level would shade differently on two machines, and `environment::FieldStore` decides
    // which path a field gets rather than letting this caller choose.
    const environment::FieldSample sampled = fields->sample_deterministic(
        rule.field, world::WorldVec3d{context.x, static_cast<f64>(context.altitude), context.z});
    out = sampled.value.x();
    return true;
}

}  // namespace

Status MaterialRuleSet::evaluate(const RuleContext& context, const environment::FieldStore* fields,
                                 Array<LayerBlend>& out) const noexcept {
    out.clear();

    // Painting wins locally, and it is decided BEFORE the rules run rather than by overwriting
    // their answer: an exclusive stroke means the rules did not apply here, which is a different
    // statement from "the rules applied and were then covered".
    bool exclusive = false;
    for (const PaintStroke& stroke : strokes_.span()) {
        if (stroke.exclusive && stroke.bounds.contains(context.x, context.z)) {
            exclusive = true;
            break;
        }
    }

    if (!exclusive) {
        for (const MaterialRule& rule : rules_.span()) {
            f32 value = 0.0F;
            if (!rule_value(rule, context, fields, value)) {
                ++skipped_;
                continue;
            }
            const f32 response = rule_response(rule, value);
            if (response <= 0.0F) {
                continue;
            }
            if (Status pushed = out.push_back(LayerBlend{rule.layer, rule.weight * response});
                !pushed) {
                return pushed;
            }
        }
    }

    for (const PaintStroke& stroke : strokes_.span()) {
        if (!stroke.bounds.contains(context.x, context.z)) {
            continue;
        }
        if (exclusive && !stroke.exclusive) {
            continue;
        }
        if (Status pushed = out.push_back(LayerBlend{stroke.layer, stroke.weight}); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status MaterialRuleSet::fields_read(Array<environment::FieldId>& out) const noexcept {
    for (const MaterialRule& rule : rules_.span()) {
        if (rule.input != RuleInput::Field || !rule.field.is_valid()) {
            continue;
        }
        bool seen = false;
        for (const environment::FieldId& existing : out.span()) {
            if (existing == rule.field) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        if (Status pushed = out.push_back(rule.field); !pushed) {
            return pushed;
        }
    }
    return ok();
}

// --- The page producer
// ----------------------------------------------------------------------

MaterialPageCache::MaterialPageCache(Allocator& allocator, const TileLayout& layout,
                                     f32 page_metres, bool virtual_texturing) noexcept
    : allocator_(&allocator),
      layout_(layout),
      page_metres_(page_metres),
      virtual_texturing_(virtual_texturing),
      pages_(allocator),
      index_(allocator) {}

MaterialPage MaterialPageCache::page_at(f64 x, f64 z, u8 level) const noexcept {
    f64 metres = static_cast<f64>(page_metres_);
    for (u8 step = 0; step < level; ++step) {
        metres *= 2.0;
    }
    MaterialPage page;
    page.x = static_cast<i32>(std::floor((x - layout_.origin_x) / metres));
    page.z = static_cast<i32>(std::floor((z - layout_.origin_z) / metres));
    page.level = level;
    return page;
}

TerrainBounds MaterialPageCache::page_bounds(const MaterialPage& page) const noexcept {
    f64 metres = static_cast<f64>(page_metres_);
    for (u8 step = 0; step < page.level; ++step) {
        metres *= 2.0;
    }
    TerrainBounds bounds;
    bounds.min_x = layout_.origin_x + (static_cast<f64>(page.x) * metres);
    bounds.min_z = layout_.origin_z + (static_cast<f64>(page.z) * metres);
    bounds.max_x = bounds.min_x + metres;
    bounds.max_z = bounds.min_z + metres;
    return bounds;
}

const MaterialPageCache::Produced* MaterialPageCache::find(
    const MaterialPage& page) const noexcept {
    const usize* slot = index_.find(page_key(page));
    if (slot == nullptr || !(pages_[*slot].page == page)) {
        return nullptr;
    }
    return &pages_[*slot];
}

bool MaterialPageCache::is_produced(const MaterialPage& page) const noexcept {
    return find(page) != nullptr;
}

Status MaterialPageCache::produce(const MaterialPage& page, TexelProducer producer,
                                  void* user) noexcept {
    if (producer == nullptr) {
        return fail(ErrorCode::InvalidArgument, "terrain: a page needs a producer to evaluate");
    }
    if (find(page) != nullptr) {
        // Already produced. The whole point: "sampling that page thereafter SHALL cost an ordinary
        // texture fetch", and `productions()` does not move.
        return ok();
    }

    Produced produced(*allocator_);
    produced.page = page;
    if (Status sized = produced.texels.resize(kPageTexelCount); !sized) {
        return sized;
    }
    const TerrainBounds bounds = page_bounds(page);
    const f64 step = (bounds.max_x - bounds.min_x) / static_cast<f64>(kPageTexels);
    for (u32 j = 0; j < kPageTexels; ++j) {
        for (u32 i = 0; i < kPageTexels; ++i) {
            const f64 x = bounds.min_x + ((static_cast<f64>(i) + 0.5) * step);
            const f64 z = bounds.min_z + ((static_cast<f64>(j) + 0.5) * step);
            if (Status made = producer(user, x, z, produced.texels[(j * kPageTexels) + i]); !made) {
                return made;
            }
            ++productions_;
        }
    }

    if (Status pushed = pages_.push_back(std::move(produced)); !pushed) {
        return pushed;
    }
    Expected<usize*, Error> mapped = index_.insert(page_key(page), pages_.size() - 1);
    if (!mapped) {
        pages_.remove_unordered(pages_.size() - 1);
        return make_unexpected(mapped.error());
    }
    return ok();
}

const MaterialTexel* MaterialPageCache::sample(const MaterialPage& page, f64 x,
                                               f64 z) const noexcept {
    const Produced* produced = find(page);
    if (produced == nullptr) {
        return nullptr;
    }
    const TerrainBounds bounds = page_bounds(page);
    const f64 step = (bounds.max_x - bounds.min_x) / static_cast<f64>(kPageTexels);
    const i64 i = ifloor((x - bounds.min_x) / step);
    const i64 j = ifloor((z - bounds.min_z) / step);
    if (i < 0 || j < 0 || std::cmp_greater_equal(i, kPageTexels) ||
        std::cmp_greater_equal(j, kPageTexels)) {
        return nullptr;
    }
    return &produced->texels[(static_cast<usize>(j) * kPageTexels) + static_cast<usize>(i)];
}

u32 MaterialPageCache::invalidate(const TerrainBounds& bounds) noexcept {
    u32 dropped = 0;
    usize index = 0;
    while (index < pages_.size()) {
        if (!page_bounds(pages_[index].page).overlaps(bounds)) {
            ++index;
            continue;
        }
        const MaterialPage victim = pages_[index].page;
        (void)index_.remove(page_key(victim));
        const usize last = pages_.size() - 1;
        if (index != last) {
            const MaterialPage moved = pages_[last].page;
            pages_[index] = std::move(pages_[last]);
            if (usize* slot = index_.find(page_key(moved)); slot != nullptr) {
                *slot = index;
            }
        }
        pages_.remove_unordered(last);
        ++dropped;
    }
    return dropped;
}

MaterialPathReport MaterialPageCache::path() const noexcept {
    MaterialPathReport report;
    report.virtual_texturing = virtual_texturing_;
    if (virtual_texturing_) {
        return report;
    }
    // The conventional path repeats one tiled texture set over the page, so the unique detail it
    // carries is one texel set per page rather than one per texel. Reported rather than hidden:
    // "the limitation reported" is a requirement, not a courtesy.
    report.unique_detail = 1.0F / static_cast<f32>(kPageTexels * kPageTexels);
    report.limitation =
        "terrain: virtual texturing is unavailable; materials are tiled and unique detail is "
        "reduced to one texel set per page";
    return report;
}

}  // namespace cy::terrain
