#pragma once
// The surface cache: shaded radiance for world surfaces, so a traced hit is a lookup. Task 9.1.
//
// `rendering-global-illumination` — "Surface cache".
//
// ================================================================================================
// WHAT MAKES HYBRID TRACING AFFORDABLE AT ALL
// ================================================================================================
//
// A ray that hits a surface has to come back with radiance. Doing that by decoding the geometry,
// sampling the material's textures and evaluating its closures makes a secondary ray cost what a
// primary pixel costs, and there are far more secondary rays than pixels. So the cache holds the
// ANSWER — position, normal, albedo, roughness, emission, the direct lighting and the accumulated
// indirect — and a hit reads it. That is the requirement "a secondary hit is a lookup, not a
// material evaluation", and the enforcement is that the tracers hold a `RadianceLookup` and have no
// access to a material at all.
//
// ================================================================================================
// MULTI-BOUNCE IS THE FEEDBACK, AND IT COSTS NOTHING
// ================================================================================================
//
//     accumulated = direct + albedo * gather(indirect)
//
// where `gather` reads the radiance cache, which was itself built from `accumulated` last frame.
// One bounce of path length, iterated once per update, converges toward the multi-bounce solution
// over frames. `SurfaceCache::update()` is where that single line lives and it is the whole of
// "successive frames SHALL approximate additional bounces".
//
// The consequence worth knowing: a page updated once has one bounce in it, and the convergence
// metric in resolve.h is what says whether the frame you are looking at has more.
//
// ================================================================================================
// THE UPDATE IS BUDGETED AND PRIORITISED, WHICH IS WHY A PAGE HAS AN AGE
// ================================================================================================
//
// Every page carries a last-update frame, a validity flag and an error estimate, and the update
// selects by "visible, recently invalidated, and high-error regions" in that order of weight. A
// cache that refreshed everything would be a cache that costs what it saves.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/bvh.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/lighting.h>
#include <cy/rendering/gi/scene.h>

namespace cy::rendering::gi {

/// How well a card's normal must agree with a hit's before the card may answer for it. A quarter is
/// a 75-degree cone: wide enough that a hit whose normal the field blended at an edge still finds
/// its own surface, and narrow enough that a card facing across the hit cannot. Public because the
/// device lookup (`card_lighting.h`, `cy::rendering-gi-gpu`) must apply the same cone.
inline constexpr f32 kCardLookupAlignment = 0.25F;

/// One cached surface. The fields are the specification's list, in its order.
struct SurfacePage {
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 normal{0.0F, 1.0F, 0.0F};
    Vec3 albedo{0.5F, 0.5F, 0.5F};
    f32 roughness = 1.0F;
    Vec3 emission{0.0F, 0.0F, 0.0F};
    Vec3 direct{0.0F, 0.0F, 0.0F};
    /// The gathered indirect, which is what feeds back to make the next bounce.
    Vec3 accumulated{0.0F, 0.0F, 0.0F};
    f32 area = 1.0F;
    u32 instance_id = 0;
    u32 material_id = 0;
    /// The frame this page was last shaded. `age` is the frame minus this.
    u64 last_update_frame = 0;
    /// How much the last update changed the answer, relative to the answer. The error estimate the
    /// scheduler prioritises on, and the convergence signal the resolve reads.
    f32 error = 1.0F;
    /// False until the page has been shaded once, and again after an invalidation.
    bool valid = false;
    /// Set by the renderer's feedback: this page affects a visible surface.
    bool visible = false;
    bool live = false;
    /// Bumped by every change the host makes to the page — allocation, release, invalidation — and
    /// by nothing a shading pass does. A device mirror compares it against the revision it last
    /// uploaded, which is what makes the card upload incremental; a shading backend compares it
    /// against the revision it submitted, which is how a result for a page that was invalidated
    /// while it was in flight is dropped rather than undoing the invalidation.
    u32 revision = 0;
};

struct SurfaceUpdateReport {
    u32 pages_updated = 0;
    u32 pages_invalid = 0;
    u32 queue_depth = 0;
    /// The largest age among the pages that were NOT serviced. The starvation number.
    u64 oldest_unserviced_age = 0;
    f32 mean_error = 0.0F;
};

struct SurfaceCacheDiagnostics {
    u32 page_count = 0;
    u32 valid_pages = 0;
    u64 bytes = 0;
    u32 lookups = 0;
    u32 lookup_misses = 0;
    SurfaceUpdateReport last_update{};
};

/// What one update needs in order to shade a page. Both members may be null: no lights is a black
/// direct term and no indirect source is a single-bounce cache, and neither is an error.
struct SurfaceUpdateContext {
    Span<const GiLight> lights;
    const Occluder* occluder = nullptr;
    const IndirectSource* indirect = nullptr;
    /// Pages per update. The GI budget's `SurfaceCacheRate` lever sets it.
    u32 budget = 64;
    u64 frame = 0;
    /// A page not serviced within this many frames is forced into the update set. The progress
    /// guarantee: no valid page is starved indefinitely by higher-priority work.
    u64 max_age_frames = 240;
};

/// WHO SHADES THE PAGES THE SCHEDULER PICKED. The seam a device surface cache replaces the host one
/// behind (`cy::rendering-gi-gpu`), so `system.h` chooses CPU or GPU by installing one pointer and
/// nothing else in this module learns a device exists.
///
/// The cache keeps everything that is a DECISION — allocation, invalidation, the prioritised,
/// budgeted selection, the lookup a traced hit resolves through — and hands the backend only the
/// arithmetic. That is what keeps the host implementation below the oracle: the selection a device
/// shades is the selection this file would have shaded, computed by the same function.
///
/// Asynchronous by construction. `submit` hands over this update's selection; the work runs when
/// the caller executes its frame, and `retire` folds the results back into the pages. The cache
/// calls `retire` at the start of its next update, so the caller's contract is that the frame that
/// ran the previous submission has completed by then (its fence was waited on). A result for a page
/// whose `revision` moved while it was in flight is discarded.
class SurfaceShadingBackend {
public:
    SurfaceShadingBackend() = default;
    SurfaceShadingBackend(const SurfaceShadingBackend&) = delete;
    SurfaceShadingBackend(SurfaceShadingBackend&&) = delete;
    SurfaceShadingBackend& operator=(const SurfaceShadingBackend&) = delete;
    SurfaceShadingBackend& operator=(SurfaceShadingBackend&&) = delete;
    virtual ~SurfaceShadingBackend() = default;

    /// Take this update's selection. `pages` is every page the cache holds, indexed by handle, so a
    /// backend may mirror what changed; `selected` is the handles to shade, in priority order and
    /// no longer than the budget. Returns how many it accepted: a backend sized for fewer pages
    /// than the budget takes a prefix, which is still the highest-priority work.
    [[nodiscard]] virtual u32 submit(Span<const SurfacePage> pages, Span<const u32> selected,
                                     const SurfaceUpdateContext& context) noexcept = 0;

    /// Write the results of the last submission into `pages`: `direct`, `accumulated`, `error`,
    /// `valid` and `last_update_frame`. Returns how many pages it wrote.
    virtual u32 retire(Span<SurfacePage> pages) noexcept = 0;
};

/// Shaded radiance for world surfaces.
class SurfaceCache : public RadianceLookup {
public:
    SurfaceCache() noexcept;
    ~SurfaceCache() override;

    SurfaceCache(const SurfaceCache&) = delete;
    SurfaceCache(SurfaceCache&&) = delete;
    SurfaceCache& operator=(const SurfaceCache&) = delete;
    SurfaceCache& operator=(SurfaceCache&&) = delete;

    /// Allocate a page for one surface card. Returns its handle.
    [[nodiscard]] Expected<u32, Error> allocate(const Surfel& surfel) noexcept;

    /// Allocate a page per surfel of a GI scene cell that has just been ingested.
    [[nodiscard]] Status allocate_from(const GiScene& scene, const Aabb& region) noexcept;

    void release(u32 handle) noexcept;

    /// Invalidate every page inside `region`. Returns how many, which is what an invalidation
    /// record's `surface_pages` is filled in with.
    u32 invalidate(const Aabb& region) noexcept;

    /// Mark the pages inside `region` as affecting visible surfaces. The renderer's feedback.
    u32 mark_visible(const Aabb& region, bool visible) noexcept;

    /// Shade up to `context.budget` pages, chosen by priority.
    SurfaceUpdateReport update(const SurfaceUpdateContext& context) noexcept;

    /// Force every page to be shaded, however many there are. Converged mode's demand on this
    /// subsystem, and what a bake's seeding uses.
    SurfaceUpdateReport update_all(const SurfaceUpdateContext& context) noexcept;

    /// The prioritised selection one update shades, in order: every live page older than
    /// `context.max_age_frames` first (the progress guarantee), then by visibility, validity, error
    /// and age, then by handle. `all` takes every live page and otherwise the first
    /// `context.budget`. Writes `pages_invalid`, `queue_depth` and `oldest_unserviced_age` into
    /// `report`. A static function over the page array so a device backend, and a test holding one
    /// to its budget, run the one selection there is.
    [[nodiscard]] static Status select(Span<const SurfacePage> pages,
                                       const SurfaceUpdateContext& context, bool all,
                                       Array<u32>& selected, SurfaceUpdateReport& report) noexcept;

    /// Shade the selected pages somewhere else. Null restores the host shading. See
    /// `SurfaceShadingBackend` for the contract; installing one does not touch any page.
    void set_shading_backend(SurfaceShadingBackend* backend) noexcept { backend_ = backend; }
    [[nodiscard]] SurfaceShadingBackend* shading_backend() const noexcept { return backend_; }

    /// Fold the backend's completed results into the pages now, rather than at the next update.
    /// A no-op with no backend. Returns how many pages it wrote.
    u32 collect() noexcept;

    /// Every page slot, live or not, indexed by handle. What a device mirror uploads from.
    [[nodiscard]] Span<const SurfacePage> pages() const noexcept { return pages_.span(); }

    // --- RadianceLookup --------------------------------------------------------------------------

    [[nodiscard]] bool radiance_at(Vec3 position, Vec3 normal, Vec3& radiance,
                                   u32& age_frames) const noexcept override;

    /// The outgoing radiance of one page: emission plus direct plus the accumulated indirect.
    [[nodiscard]] static Vec3 outgoing(const SurfacePage& page) noexcept;

    [[nodiscard]] const SurfacePage& page(u32 handle) const noexcept { return pages_[handle]; }
    [[nodiscard]] u32 page_count() const noexcept { return live_pages_; }
    [[nodiscard]] const SurfaceCacheDiagnostics& diagnostics() const noexcept {
        return diagnostics_;
    }

    /// The lookup radius, in metres. A hit further than this from any page finds nothing, which is
    /// a low confidence rather than a wrong answer.
    void set_lookup_radius(f32 radius) noexcept { lookup_radius_ = radius; }
    [[nodiscard]] f32 lookup_radius() const noexcept { return lookup_radius_; }

private:
    [[nodiscard]] static f32 priority_of(const SurfacePage& page, u64 frame) noexcept;
    static void shade(SurfacePage& page, const SurfaceUpdateContext& context) noexcept;
    SurfaceUpdateReport service(const SurfaceUpdateContext& context, bool all) noexcept;
    void account() noexcept;

    Array<SurfacePage> pages_;
    Array<u32> proxies_;
    SurfaceShadingBackend* backend_ = nullptr;
    Array<u32> free_pages_;
    DynamicBvh index_;
    u32 live_pages_ = 0;
    u64 current_frame_ = 0;
    f32 lookup_radius_ = 0.5F;
    mutable SurfaceCacheDiagnostics diagnostics_{};
};

}  // namespace cy::rendering::gi
