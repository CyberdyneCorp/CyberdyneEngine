// SPDX-License-Identifier: MIT
#pragma once
// Emissive surfaces as illumination: classification by radiant power and area, and promotion.
//
// `rendering-global-illumination` — "Emissive surfaces as illumination".
//
// ================================================================================================
// AN EMITTER IS EITHER A CARD OR A LIGHT, AND WHICH ONE IS A MEASUREMENT
// ================================================================================================
//
// An emissive card already lights its surroundings through the surface cache: `SurfaceCache::
// outgoing` adds its emission, so a gather ray that lands on it brings that radiance back. That is
// right for a large, dim emitter — a neon sign, a lit panel — which a gather samples densely. It is
// the primary source of fireflies for a small, bright one: a gather ray hits it rarely and returns
// an enormous value when it does, and a card smaller than the cache's lookup cell is worse than
// noisy, because every gather hit in that cell resolves to it and its energy is multiplied by the
// cell's area over its own.
//
// So every emitter is CLASSIFIED, per (instance, material), by the two numbers the requirement
// names:
//
//   radiant power  pi * Y(L) * A — the Lambertian emitter's total output, in the engine's units
//   area           the sum of its cards' areas, in square metres
//
// An emitter at least `promotion_power` strong and at most `promotion_max_area_m2` large is
// PROMOTED: its emission is taken out of the cache, and each of its cards becomes a `GiLight` with
// `emitter_area` set, which every consumer of `shaded_direct` — the surface cache, the device card
// shading, the path tracer's next-event term, the irradiance volume capture — integrates in closed
// form (`emitter_form_factor`) with a shadow ray. Deterministic, so a promoted emitter has no
// variance at all. Everything else stays a card: CACHE ONLY, and never importance sampled.
//
// ================================================================================================
// ENERGY IS REDISTRIBUTED, NEVER CLAMPED, AND THE REPORT SAYS SO IN NUMBERS
// ================================================================================================
//
// `EmissiveReport` accounts for every watt: `total_power == cached_power + promoted_power +
// demoted_power`. A promoted emitter that would exceed `max_promoted_lights` is DEMOTED back to the
// cache (lowest power first), and its power is reported rather than lost. `clamped_power` is a
// field and not an omission: nothing here clamps, and the case that checks the invariant checks it
// is zero, so a future clamp is a red test rather than a silent dimming.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/lighting.h>
#include <cy/rendering/gi/scene.h>

namespace cy::rendering::gi {

struct EmissiveSettings {
    /// Radiant power, pi * luminance * area, at or above which a small enough emitter is promoted.
    f32 promotion_power = 1.0F;
    /// The largest emitter, in square metres, that may be promoted. Above it an emitter is large
    /// enough for the gather to sample densely, and stays in the cache.
    f32 promotion_max_area_m2 = 4.0F;
    /// The most lights one cell's promotion may add — one per card of a promoted emitter. Per cell
    /// rather than global so a cell's classification does not depend on what streamed in first.
    u32 max_promoted_lights = 64;
    /// The irradiance below which a promoted light no longer invalidates the caches around it when
    /// it appears or leaves. A scheduling bound only: the light itself is unbounded (`range` 0), so
    /// none of its energy is cut off.
    f32 invalidation_irradiance = 1.0e-3F;
};

enum class EmitterClass : u8 {
    /// Through the surface cache only, and excluded from importance sampling.
    CacheOnly = 0,
    /// Each card a light, integrated deterministically by the direct lighting path.
    PromotedLight,
    /// Promoted by power and area, but over the cell's light budget: back in the cache, reported.
    Demoted,
    /// Switched off by an `EmitterRule`: its cards hold no emission and it lights nothing. Its
    /// power is reported as `disabled_power`, so the account still sums.
    Disabled,
};

[[nodiscard]] const char* emitter_class_name(EmitterClass classification) noexcept;

// ================================================================================================
// AN AUTHOR'S OR A GAME'S WORD OVER THE MEASUREMENT
// ================================================================================================
//
// The measurement is right for the common case and wrong for two: a large emitter an author knows
// is the room's key light (a window card, a skylight panel — promote it whatever its area), and a
// small, bright one whose shadows nobody will see (a dashboard's LEDs — leave it to the cache and
// save the lights). And a game switches lamps off. An `EmitterRule` says so per material, per
// instance, or per (instance, material); the most specific rule that matches an emitter decides,
// and among equally specific ones the last given. With no rule the classification is the
// measurement's, bit for bit.

/// What a rule makes of the emitters it matches.
enum class EmitterOverride : u8 {
    /// The measurement decides: the classification without the rule.
    Automatic = 0,
    /// Never promoted, whatever its power and area.
    CacheOnly,
    /// Promoted whatever its power and area — still within the cell's light budget, so an emitter
    /// past it is `Demoted` and reported like any other.
    Promote,
    /// Its emission is removed: no light, no cache radiance.
    Off,
    Count,
};

[[nodiscard]] const char* emitter_override_name(EmitterOverride mode) noexcept;

/// Matches every instance, or every material.
inline constexpr u32 kAnyEmitter = 0xFFFFFFFFU;

struct EmitterRule {
    /// The `Surfel::instance_id` it applies to, or `kAnyEmitter`.
    u32 instance_id = kAnyEmitter;
    /// The `Surfel::material_id` it applies to, or `kAnyEmitter`.
    u32 material_id = kAnyEmitter;
    EmitterOverride mode = EmitterOverride::Automatic;
};

/// The rule `rules` applies to (instance, material): the most specific match — both ids, then the
/// instance alone, then the material alone, then a rule matching every emitter — the last of
/// equals. `Automatic` with none.
[[nodiscard]] EmitterOverride emitter_override_for(Span<const EmitterRule> rules, u32 instance_id,
                                                   u32 material_id) noexcept;

/// One emitter: the cards of one (instance, material) with non-zero emission.
struct EmitterRecord {
    u64 cell = 0;
    u32 instance_id = 0;
    u32 material_id = 0;
    u32 cards = 0;
    f32 area_m2 = 0.0F;
    f32 power = 0.0F;
    EmitterClass classification = EmitterClass::CacheOnly;
    /// The rule that decided it, `Automatic` when the measurement did.
    EmitterOverride override_mode = EmitterOverride::Automatic;
};

/// Where every watt went.
struct EmissiveReport {
    u32 emitters = 0;
    u32 cached_emitters = 0;
    u32 promoted_emitters = 0;
    u32 demoted_emitters = 0;
    u32 promoted_lights = 0;
    f32 total_power = 0.0F;
    f32 cached_power = 0.0F;
    f32 promoted_power = 0.0F;
    f32 demoted_power = 0.0F;
    /// Energy removed by clamping. Zero by construction; see the header comment.
    f32 clamped_power = 0.0F;
    /// Emitters an `EmitterRule` switched off, and the power they would emit. `total_power` is
    /// `cached_power + promoted_power + demoted_power + disabled_power`.
    u32 disabled_emitters = 0;
    f32 disabled_power = 0.0F;
};

/// Rec. 709 luminance of an emission.
[[nodiscard]] f32 emission_luminance(Vec3 emission) noexcept;

/// A Lambertian card's radiant power: pi * Y(emission) * area.
[[nodiscard]] f32 emitted_power(const Surfel& surfel) noexcept;

/// The distance at which a promoted light's on-axis irradiance falls to `threshold`, clamped to
/// [1, 64] metres. The half-extent of the region its arrival or departure invalidates.
[[nodiscard]] f32 emitter_influence_radius(const GiLight& light, f32 threshold) noexcept;

/// Classify one cell's surfels. `cache_surfels` receives every surfel, with the emission of each
/// promoted card zeroed — what the GI scene and the surface cache should hold. `lights` receives
/// one light per promoted card, with ids from `light_id_base`, and `records` one entry per emitter.
/// All three are appended to; `report` is accumulated into.
[[nodiscard]] Status classify_emitters(Span<const Surfel> surfels, const EmissiveSettings& settings,
                                       u64 cell, u64 light_id_base, Array<Surfel>& cache_surfels,
                                       Array<GiLight>& lights, Array<EmitterRecord>& records,
                                       EmissiveReport& report) noexcept;

/// The same under `rules`: each emitter's class is its rule's (`emitter_override_for`) where one
/// matches, and the measurement's elsewhere. An `Off` emitter's cards reach `cache_surfels` with
/// no emission. Empty `rules` is the overload above, bit for bit.
[[nodiscard]] Status classify_emitters(Span<const Surfel> surfels, const EmissiveSettings& settings,
                                       Span<const EmitterRule> rules, u64 cell, u64 light_id_base,
                                       Array<Surfel>& cache_surfels, Array<GiLight>& lights,
                                       Array<EmitterRecord>& records,
                                       EmissiveReport& report) noexcept;

/// The classification for every resident cell: the subsystem `IlluminationSystem` owns, and the
/// list of promoted lights it adds to a frame's.
class EmissiveLights {
public:
    EmissiveLights() noexcept = default;

    void configure(const EmissiveSettings& settings) noexcept { settings_ = settings; }
    [[nodiscard]] const EmissiveSettings& settings() const noexcept { return settings_; }

    /// The rules the next `ingest` classifies under, copied. A cell already classified keeps its
    /// classes until it is ingested again: `IlluminationSystem::set_emitter_rules` does that.
    [[nodiscard]] Status set_rules(Span<const EmitterRule> rules) noexcept;
    [[nodiscard]] Span<const EmitterRule> rules() const noexcept { return rules_.span(); }

    /// Classify `surfels` as cell `cell`. `cache_surfels` is cleared and filled with what the GI
    /// scene should ingest. A cell already classified is refused.
    [[nodiscard]] Status ingest(u64 cell, Span<const Surfel> surfels,
                                Array<Surfel>& cache_surfels) noexcept;

    /// Forget a cell's emitters and lights. A cell that was never ingested is a no-op.
    void evict(u64 cell) noexcept;

    /// Every promoted light of every resident cell, in ingestion order.
    [[nodiscard]] Span<const GiLight> lights() const noexcept { return lights_.span(); }
    [[nodiscard]] Span<const EmitterRecord> records() const noexcept { return records_.span(); }
    /// Totals over the resident cells.
    [[nodiscard]] EmissiveReport report() const noexcept;
    /// The lights a cell's last ingest or eviction added or removed, for invalidation.
    [[nodiscard]] Span<const GiLight> last_changed() const noexcept { return changed_.span(); }

private:
    EmissiveSettings settings_{};
    Array<EmitterRule> rules_;
    Array<GiLight> lights_;
    Array<u64> light_cells_;
    Array<EmitterRecord> records_;
    Array<GiLight> changed_;
};

}  // namespace cy::rendering::gi
