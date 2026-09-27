// SPDX-License-Identifier: MIT
#pragma once
// Card lighting's host half: the shadow map, the card lookup grid, and the card-to-card gather.
// Issue #35, stages 1 and 2.
//
// ================================================================================================
// WHY THIS FILE EXISTS: IT IS THE ORACLE A DEVICE SURFACE CACHE IS CHECKED AGAINST
// ================================================================================================
//
// `cy::rendering-gi-gpu` shades surface cards in compute. Every term it computes has a host
// implementation here, written first and written so the device can transcribe it line for line:
//
//   `ShadowMap`          the directional light's depth map, captured from the distance field (or
//                        assigned from a renderer's shadow pass), and the depth test a card's
//                        direct term is shadowed by.
//   `ShadowMapOccluder`  an `Occluder` that answers a segment toward the shadow map's light from
//                        the map and every other segment from a fallback — the distance field, for
//                        a punctual light. The device applies the same rule to the same segments.
//   `CardGrid`           a hashed uniform grid over the live cards, the one lookup structure both
//                        sides index, so "which card answers a hit" has one answer.
//   `CardSnapshot`       a `RadianceLookup` over a copy of the cache's outgoing radiance, taken
//                        BEFORE an update. The device reads last frame's radiance while it writes
//                        this frame's; a host update that gathered from the live cache would see
//                        pages it had already shaded this frame and disagree by a bounce.
//   `CardGather`         the `IndirectSource` a card's multi-bounce term comes from: cosine rays
//                        through the distance field, a hit resolved through the snapshot, a miss
//                        answered by the sky term.
//
// That makes the device path Lumen's surface-cache radiosity rather than a gather from the radiance
// cache, and the difference is deliberate for this slice: the radiance cache is still host-side
// (issue #35 stage 3), and a device cache that read it would need it on the device first.
//
// NO DEVICE, NO SHADER. Everything here is arithmetic over `DistanceField` and `SurfaceCache`, and
// runs headless like the rest of this module.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/lighting.h>
#include <cy/rendering/gi/surface_cache.h>

#include <cmath>

namespace cy::rendering::gi {

// --- The shadow map
// -------------------------------------------------------------------------------

/// An orthographic depth map along one directional light. Depths are metres along `direction` from
/// the near plane, which sits `depth_range_metres / 2` before `centre`; a texel nothing was found
/// under holds `depth_range_metres`.
struct ShadowMapSettings {
    /// The direction the light TRAVELS, as `GiLight::direction` holds it.
    Vec3 direction{0.0F, -1.0F, 0.0F};
    Vec3 centre{0.0F, 0.0F, 0.0F};
    f32 half_extent_metres = 8.0F;
    f32 depth_range_metres = 32.0F;
    u32 resolution = 128;
    /// A point is in shadow when the map holds a depth this much nearer the light than its own.
    /// It covers the capture's half-voxel hit threshold and a texel's slope.
    f32 bias_metres = 0.3F;
};

/// How closely a shadow ray must point back along the map's light before the map answers for it.
/// `shaded_direct` sends a directional light's shadow ray along exactly that direction, so the
/// comparison only has to be tight enough that a punctual light's ray is never mistaken for one.
inline constexpr f32 kShadowMapAlignment = 0.9999F;

class ShadowMap {
public:
    ShadowMap() noexcept = default;

    [[nodiscard]] Status configure(const ShadowMapSettings& settings) noexcept;
    [[nodiscard]] const ShadowMapSettings& settings() const noexcept { return settings_; }

    /// Fill the map by sphere tracing `field` from the near plane, one ray per texel centre. The
    /// stand-in for a rasterised shadow pass in a scene that has only a field.
    void capture(const DistanceField& field) noexcept;

    /// Take a renderer's depths instead: `resolution^2` metres along the light, row-major from the
    /// texel at (-half_extent, -half_extent) in (right, up).
    [[nodiscard]] Status assign(Span<const f32> depths) noexcept;

    [[nodiscard]] Span<const f32> depths() const noexcept { return depths_.span(); }
    [[nodiscard]] Vec3 direction() const noexcept { return direction_; }
    [[nodiscard]] Vec3 right() const noexcept { return right_; }
    [[nodiscard]] Vec3 up() const noexcept { return up_; }

    /// Whether the map answers for a shadow ray leaving along `toward_light`.
    [[nodiscard]] bool covers(Vec3 toward_light) const noexcept;
    /// The depth test. A point outside the map's footprint or depth range is unshadowed.
    [[nodiscard]] bool shadowed(Vec3 point) const noexcept;

private:
    ShadowMapSettings settings_{};
    Vec3 direction_{0.0F, -1.0F, 0.0F};
    Vec3 right_{1.0F, 0.0F, 0.0F};
    Vec3 up_{0.0F, 0.0F, 1.0F};
    Array<f32> depths_;
};

/// The shadow map for a segment toward its light, and `fallback` for every other segment. Either
/// may be null: no map means every segment goes to the fallback, and no fallback means a segment
/// the map does not cover is unshadowed — `shaded_direct`'s own degradation.
class ShadowMapOccluder : public Occluder {
public:
    ShadowMapOccluder() noexcept = default;
    ShadowMapOccluder(const ShadowMap* map, const Occluder* fallback) noexcept
        : map_(map), fallback_(fallback) {}

    void bind(const ShadowMap* map, const Occluder* fallback) noexcept {
        map_ = map;
        fallback_ = fallback;
    }

    [[nodiscard]] bool occluded(Vec3 from, Vec3 to) const noexcept override;

private:
    const ShadowMap* map_ = nullptr;
    const Occluder* fallback_ = nullptr;
};

// --- The card lookup
// ------------------------------------------------------------------------------

/// A hashed uniform grid over the live pages of a surface cache: the lookup a traced hit resolves
/// through, in a form a device can index. A bucket lists the cards of every cell that hashes to
/// it, so a lookup that visits a colliding cell sees extra candidates and rejects them by distance
/// — which is why the grid needs no per-cell key.
class CardGrid {
public:
    CardGrid() noexcept = default;

    /// Index every live page of `pages` by position, at `cell_metres` a cell.
    [[nodiscard]] Status build(Span<const SurfacePage> pages, f32 cell_metres) noexcept;

    [[nodiscard]] f32 cell_metres() const noexcept { return cell_; }
    /// A power of two, at least 64 and at least twice the card count.
    [[nodiscard]] u32 bucket_count() const noexcept {
        return static_cast<u32>(ranges_.size() / 2U);
    }
    /// Two words per bucket: the first item and the count.
    [[nodiscard]] Span<const u32> ranges() const noexcept { return ranges_.span(); }
    /// Page handles, grouped by bucket.
    [[nodiscard]] Span<const u32> items() const noexcept { return items_.span(); }

    [[nodiscard]] static u32 bucket_of(i32 x, i32 y, i32 z, u32 bucket_count) noexcept {
        const u32 hash = (static_cast<u32>(x) * 73856093U) ^ (static_cast<u32>(y) * 19349663U) ^
                         (static_cast<u32>(z) * 83492791U);
        return hash & (bucket_count - 1U);
    }
    [[nodiscard]] static i32 cell_of(f32 value, f32 cell) noexcept {
        return static_cast<i32>(std::floor(value / cell));
    }

    /// Every page listed in a cell that overlaps the cube of half-width `radius` around `point`.
    /// `fn(u32 handle)`. A page may be visited twice when two of those cells share a bucket.
    template <class Fn>
    void visit(Vec3 point, f32 radius, Fn&& fn) const {
        if (ranges_.empty()) {
            return;
        }
        const u32 buckets = bucket_count();
        const i32 lo[3] = {cell_of(point.x - radius, cell_), cell_of(point.y - radius, cell_),
                           cell_of(point.z - radius, cell_)};
        const i32 hi[3] = {cell_of(point.x + radius, cell_), cell_of(point.y + radius, cell_),
                           cell_of(point.z + radius, cell_)};
        for (i32 z = lo[2]; z <= hi[2]; ++z) {
            for (i32 y = lo[1]; y <= hi[1]; ++y) {
                for (i32 x = lo[0]; x <= hi[0]; ++x) {
                    const u32 bucket = bucket_of(x, y, z, buckets);
                    const u32 first = ranges_[bucket * 2U];
                    const u32 count = ranges_[(bucket * 2U) + 1U];
                    for (u32 item = first; item < first + count; ++item) {
                        fn(items_[item]);
                    }
                }
            }
        }
    }

private:
    f32 cell_ = 1.0F;
    Array<u32> ranges_;
    Array<u32> items_;
};

/// One page as a lookup sees it.
struct CardSample {
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 normal{0.0F, 1.0F, 0.0F};
    Vec3 outgoing{0.0F, 0.0F, 0.0F};
    u64 last_update_frame = 0;
    /// Live and shaded at least once since its last invalidation.
    bool valid = false;
};

/// A `RadianceLookup` over a copy of a surface cache taken at one instant. See the header comment
/// for why a gather reads a copy.
///
/// The scoring is `SurfaceCache::radiance_at`'s — a card within the lookup radius whose normal is
/// inside the alignment cone, least `distance^2 / alignment` — with one addition the cache does not
/// need: EQUAL SCORES GO TO THE LOWER HANDLE. The cache breaks a tie by whichever card its tree
/// visits first; a device that ran the same comparison in a different order would disagree about
/// coincident cards, and the handle is the one order both sides have.
class CardSnapshot : public RadianceLookup {
public:
    CardSnapshot() noexcept = default;

    /// Copy `cache`'s pages and build the grid at its lookup radius. `frame` is the clock the
    /// lookup reports ages against.
    [[nodiscard]] Status capture(const SurfaceCache& cache, u64 frame) noexcept;

    [[nodiscard]] bool radiance_at(Vec3 position, Vec3 normal, Vec3& radiance,
                                   u32& age_frames) const noexcept override;

    /// The handle `radiance_at` would read, or `~0U`.
    [[nodiscard]] u32 find(Vec3 position, Vec3 normal) const noexcept;

    [[nodiscard]] const CardGrid& grid() const noexcept { return grid_; }
    [[nodiscard]] Span<const CardSample> samples() const noexcept { return samples_.span(); }
    [[nodiscard]] f32 lookup_radius() const noexcept { return radius_; }

private:
    Array<CardSample> samples_;
    CardGrid grid_;
    f32 radius_ = 0.5F;
    u64 frame_ = 0;
};

// --- The gather
// -----------------------------------------------------------------------------------

struct CardGatherSettings {
    /// Cosine-weighted rays per card per update. Zero turns the multi-bounce term off.
    u32 rays = 8;
    f32 max_distance_metres = 24.0F;
    SkyTerm sky{};
};

/// The incoming indirect radiance at a card: the mean over `rays` cosine-weighted directions of
/// what the distance field hits — resolved through `cards` — or, for a ray that escapes, the sky
/// term. The cosine weighting is in the sequence, so the mean is the estimator.
class CardGather : public IndirectSource {
public:
    CardGather() noexcept = default;

    void bind(const DistanceField* field, const RadianceLookup* cards,
              const CardGatherSettings& settings) noexcept {
        field_ = field;
        cards_ = cards;
        settings_ = settings;
    }

    [[nodiscard]] Vec3 gather(Vec3 position, Vec3 normal) const noexcept override;

private:
    const DistanceField* field_ = nullptr;
    const RadianceLookup* cards_ = nullptr;
    CardGatherSettings settings_{};
};

}  // namespace cy::rendering::gi
