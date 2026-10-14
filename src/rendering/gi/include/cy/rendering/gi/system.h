#pragma once
// The composition: the named subsystems, wired, and one frame of illumination. Tasks 9.1 and 9.2.
//
// ================================================================================================
// THIS IS NOT THE SINGLE ILLUMINATION MODULE THE SPECIFICATION FORBIDS
// ================================================================================================
//
// `rendering-global-illumination` requires the system to be "decomposed into named subsystems with
// separate ownership — GI scene, surface cache, distance field, radiance cache, tracers (screen,
// software, hardware), resolve, reflections, denoising, baking, and diagnostics — rather than a
// single illumination module."
//
// Every one of those is a type of its own in a file of its own, and this class holds them and wires
// them. It contains no illumination logic: `update()` is a sequence of calls into the subsystems in
// the order their data flows, and `indirect_diffuse()` gathers samples and hands them to
// `resolve.h`'s `combine`. The test that keeps that honest is that the caches reach each other only
// through the abstract seams in lighting.h — replace `RadianceCache` with a baked irradiance volume
// and this file changes by one line.
//
// ================================================================================================
// THE ORDER IN `update()` IS THE DATA FLOW AND NOT A PREFERENCE
// ================================================================================================
//
//   1. the budget, because every step below reads a lever value from it;
//   2. invalidations, so nothing below updates something that is about to be invalidated;
//   3. the distance field, because the software tier traces it;
//   4. the acceleration structures, because the hardware tier traces them;
//   5. probe placement, because the probes updated in 7 must exist;
//   6. the surface cache, because the probes gather from it;
//   7. the radiance cache, because the surface cache's next update gathers from it — which is the
//      feedback loop that makes the bounces accumulate;
//   8. the reflection probes, which capture through the same tracer and the same surface cache;
//   9. convergence, which observes what 6 and 7 reported.
//
// Swapping 6 and 7 costs one frame of latency in the bounce and is not otherwise wrong; every other
// pair is ordered by a real dependency.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/denoise/denoiser.h>
#include <cy/rendering/gi/budget.h>
#include <cy/rendering/gi/card_lighting.h>
#include <cy/rendering/gi/classification.h>
#include <cy/rendering/gi/diagnostics.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/emissive.h>
#include <cy/rendering/gi/gi_volume.h>
#include <cy/rendering/gi/lighting.h>
#include <cy/rendering/gi/media.h>
#include <cy/rendering/gi/radiance_cache.h>
#include <cy/rendering/gi/reflections.h>
#include <cy/rendering/gi/resolve.h>
#include <cy/rendering/gi/scene.h>
#include <cy/rendering/gi/surface_cache.h>
#include <cy/rendering/gi/tracing.h>
#include <cy/rendering/raytracing/acceleration.h>

namespace cy::rendering::gi {

/// Where the surface cache's bounce comes from.
enum class SurfaceBounce : u8 {
    /// The radiance cache's probes, interpolated at each card: the host's bounce since M7.
    Probes = 0,
    /// A gather of the cards themselves through the field (`card_lighting.h`'s `CardGather`):
    /// what the device surface cache does. Each card sees its own side of a wall, where a probe a
    /// metre away interpolates across it — which is why the per-pixel resolve's frame uses it.
    Cards,
};

struct IlluminationSettings {
    GiMode mode = GiMode::Dynamic;
    ErrorTargets error_targets{};
    ClipmapSettings field{};
    ProbeCacheSettings probes{};
    ReflectionThresholds reflection_thresholds{};
    GiBudgetSettings budget{};
    SkyTerm sky{};
    rt::AccelerationService::ServiceConfig ray_tracing{};
    /// How far a world ray reaches, in metres.
    f32 max_ray_distance_metres = 40.0F;
    /// The convergence tracker's region size.
    f32 convergence_region_metres = 4.0F;
    /// Rays a probe gathers per update at lever position 0. The budget scales it down from here.
    u32 rays_per_probe = 32;
    /// How `ingest_cell` classifies emissive cards (emissive.h).
    EmissiveSettings emissive{};
    /// The surface cache's bounce, and the card gather's settings when it is `Cards` (its sky is
    /// the system's). Appended, so a brace-initialised settings block keeps its meaning.
    SurfaceBounce surface_bounce = SurfaceBounce::Probes;
    CardGatherSettings card_gather{};
    /// How `add_object` classes an object and what each class's moves cost (classification.h).
    ClassificationSettings classification{};
};

struct FrameContext {
    Vec3 camera{0.0F, 0.0F, 0.0F};
    Span<const GiLight> lights;
    u64 frame = 0;
    /// THIS SYSTEM'S OWN measured cost in milliseconds. Never a frame time — see budget.h.
    f32 measured_gi_ms = 0.0F;
    /// Set by the renderer budget arbiter when it grants this system a step back up.
    bool permit_relaxation = false;
    /// The screen tier's buffers, or null on a view that has none.
    const ScreenView* screen = nullptr;
    Aabb importance_region{};
    bool has_importance_region = false;
    /// Forces every scheduler to full rates. `advance_to_convergence` sets it; a frame does not.
    bool converged_mode = false;
};

struct IlluminationFrameReport {
    ScrollReport field{};
    rt::FrameReport acceleration{};
    ProbePlacementReport placement{};
    SurfaceUpdateReport surfaces{};
    ProbeUpdateReport probes{};
    ReflectionCaptureReport reflections{};
    GiBudgetReport budget{};
    /// Which world tier answered this frame's rays. For the diagnostics, and for the fallback test.
    RadianceSource world_tier = RadianceSource::None;
    /// Invalidation records serviced this frame, and what they cost.
    u32 invalidations_serviced = 0;
    u32 invalidated_probes = 0;
    u32 invalidated_surface_pages = 0;
    u32 invalidated_field_bricks = 0;
    /// The least converged region, in [0, 1].
    f32 convergence = 0.0F;
    /// The registered objects by class, and what their moves cost this frame.
    ObjectsFrameReport objects{};
};

/// What a game or an editor tunes while the world runs, as one value. `IlluminationSettings` is the
/// configuration a renderer builds the system with; this is the part of it a player's options menu
/// or a level's settings may change without `configure()`'s full rebuild.
///
/// The defaults are the system's own: a system nobody called `set_runtime_settings` on computes
/// exactly what it computed before this existed.
struct GiRuntimeSettings {
    GiMode mode = GiMode::Dynamic;
    /// The global probe quality, as a GI volume's (gi_volume.h): its ray multiplier.
    GiQuality quality = GiQuality::Medium;
    /// The global tier preference, as a GI volume's.
    GiTierPreference tier = GiTierPreference::Automatic;
    /// Whether the hardware tier may be used where the device has it. Off behaves exactly like a
    /// device without ray queries (`rt::AccelerationService::set_enabled_by_profile`).
    bool hardware_ray_tracing = true;
    /// The GI allocation, in milliseconds (`GiBudget::set_allocation_ms`).
    f32 budget_ms = 3.0F;
};

/// Refuses a mode, quality or tier out of range, and a budget that is not a positive finite number.
[[nodiscard]] Status validate_gi_runtime_settings(const GiRuntimeSettings& settings) noexcept;

/// What the surface being resolved carries. The double-counting decision needs it.
struct SurfaceProperties {
    bool has_lightmap = false;
    bool has_irradiance_volume = false;
    Vec3 lightmap_radiance{0.0F, 0.0F, 0.0F};
    Vec3 irradiance_volume_radiance{0.0F, 0.0F, 0.0F};
    /// Distance from the camera, for the far-field transition.
    f32 camera_distance_metres = 0.0F;
    /// THIN TRANSMISSION (media.h): the tinted fraction of the light arriving at the back face
    /// that comes through. Zero — the default — is an opaque surface, resolved as it always was.
    Vec3 transmission{0.0F, 0.0F, 0.0F};
};

/// The subsystems, wired.
class IlluminationSystem {
public:
    IlluminationSystem() noexcept;

    [[nodiscard]] Status configure(const IlluminationSettings& settings) noexcept;
    [[nodiscard]] const IlluminationSettings& settings() const noexcept { return settings_; }

    void set_mode(GiMode mode) noexcept { settings_.mode = mode; }
    [[nodiscard]] GiMode mode() const noexcept { return settings_.mode; }

    /// Replace the sky term without reconfiguring the system.
    ///
    /// THE SEAM [dependency cycle 2](docs/roadmap/dependencies.md) IS ABOUT, and the reason it is
    /// not `configure()`. `configure()` rebuilds the clipmap, discards the probe cache and
    /// reconstructs the acceleration service — a full recomputation — and a sun that moved a
    /// quarter of a degree must not cost that. The sky is read from exactly two places, the
    /// settings and the tiered tracer's config, and this writes both and touches nothing else.
    ///
    /// It does not invalidate anything. What a sky change invalidates is a decision about a REGION,
    /// which belongs to the caller that knows where the sky is visible —
    /// `cy::rendering-sky-illumination` is the composition point that makes it, and
    /// `InvalidationCause::SkyChanged` is the cause it files the record under.
    void set_sky_term(const SkyTerm& sky) noexcept;
    [[nodiscard]] const SkyTerm& sky_term() const noexcept { return settings_.sky; }

    /// Shade the surface cache somewhere else, or on the host again with null.
    ///
    /// THE CPU-OR-GPU CHOICE, and the only one this class makes. The cache keeps its allocation,
    /// invalidation, selection and lookup; the backend — `cy::rendering-gi-gpu`'s device shading
    /// is one — does the arithmetic on the pages the cache selected, and its results land at the
    /// next `update()`. That is one frame of latency in the bounce, the same latency swapping steps
    /// 6 and 7 below would cost. See `SurfaceShadingBackend` for the contract.
    void set_surface_shading(SurfaceShadingBackend* backend) noexcept {
        surfaces_.set_shading_backend(backend);
    }

    /// Gather the radiance cache's probes somewhere else, or on the host again with null. The same
    /// choice for the radiance cache (issue #35 stage 3): the cache keeps its placement, scrolling,
    /// invalidation, selection and query, and the backend — `cy::rendering-gi-gpu`'s
    /// `GpuRadianceCache` — traces the rays of the probes it selected. See `ProbeGatherBackend`.
    void set_probe_gather(ProbeGatherBackend* backend) noexcept {
        radiance_.set_gather_backend(backend);
    }

    /// Ingest one world cell THROUGH THE EMISSIVE CLASSIFICATION, and allocate a surface page for
    /// each of its cards.
    ///
    /// `scene().ingest_cell` followed by `surfaces().allocate_from` is what a caller did before
    /// emitters were classified, and it still works: every emissive card then lights the room
    /// through the cache alone. This is the same with `EmissiveLights` in front, so a small, bright
    /// emitter arrives as a light in `frame_lights()` with its emission out of the cache, and its
    /// arrival invalidates the region it lights (`InvalidationCause::LightChanged`, attributed to
    /// the light's id). The pages are remembered by cell, and `evict_cell` releases them.
    [[nodiscard]] Status ingest_cell(u64 cell, const Aabb& bounds, Span<const Surfel> surfels,
                                     u64 frame) noexcept;

    /// Evict a cell `ingest_cell` took: from the GI scene, its promoted lights, and its pages.
    void evict_cell(u64 cell, u64 frame) noexcept;

    /// The emitter classification of every cell `ingest_cell` took.
    [[nodiscard]] const EmissiveLights& emitters() const noexcept { return emissive_; }

    /// The lights the last `update()` shaded with: the frame's own, then every promoted emitter.
    /// Exactly the frame's span, not a copy, when nothing is promoted.
    [[nodiscard]] Span<const GiLight> frame_lights() const noexcept { return last_lights_; }

    /// Place the GI volumes (gi_volume.h), replacing any placed before. Every probe inside the
    /// old and the new volumes' reach is retired, so the next `update()` places those regions
    /// again at the density, rate and quality now asked for. A refused volume changes nothing.
    [[nodiscard]] Status set_volumes(Span<const GiVolume> volumes) noexcept;
    [[nodiscard]] const GiVolumeSet& volumes() const noexcept { return volumes_; }

    /// What the volumes resolve to at `point`, from the global settings. The global settings,
    /// bit for bit, where no volume reaches.
    [[nodiscard]] GiRegionSettings region_settings(Vec3 point) const noexcept;

    // --- Run-time settings, emitter rules and object classes ----------------------------------

    /// The settings `set_runtime_settings` last applied, or the configured ones.
    [[nodiscard]] GiRuntimeSettings runtime_settings() const noexcept;

    /// Apply a level's or a player's settings without reconfiguring. The mode takes effect at the
    /// next `update()`; hardware ray tracing switches the acceleration service's profile (its
    /// structures are kept); the budget is the allocation the next `update()` holds to. A change of
    /// quality or tier retires every probe, because both are decided when a probe is placed, and the
    /// next `update()` places them again at the new setting. A refused value changes nothing.
    [[nodiscard]] Status set_runtime_settings(const GiRuntimeSettings& settings) noexcept;

    /// Classify emitters under `rules` from now on (emissive.h's `EmitterRule`), and classify every
    /// resident cell that has an emitter again: its promoted lights leave or arrive and the regions
    /// they light are invalidated, as an emitter that streams in or out. Each such cell is evicted
    /// and ingested from the cards `ingest_cell` was first given, so its pages converge again. A
    /// refused rule changes nothing.
    [[nodiscard]] Status set_emitter_rules(Span<const EmitterRule> rules, u64 frame) noexcept;
    [[nodiscard]] Span<const EmitterRule> emitter_rules() const noexcept {
        return emissive_.rules();
    }

    /// Change a registered object's class: `has_override` false returns it to the derivation from
    /// its mobility and size. An object entering `Static` or `SlowDynamic` takes its cards and its
    /// field into the GI scene now; one leaving them takes them out now, invalidating where it was.
    /// The same class is a no-op. Refused for an unknown id.
    [[nodiscard]] Status set_object_class(u64 id, bool has_override, IlluminationClass klass,
                                          u64 frame) noexcept;

    // --- Objects and their illumination classes (classification.h) ----------------------------

    /// Register an object. Its class is the override or the derivation from its mobility and the
    /// size of its world bounds, decided here and kept. `Static` and `SlowDynamic` objects enter
    /// the GI scene (their cards, as a cell `object_cell_id(id)`) and the field (their own, when
    /// they bring one); a `FastDynamic` one becomes a proxy; a `TransientIgnored` one is recorded
    /// and nothing else. Refused for an id already registered.
    [[nodiscard]] Status add_object(const GiObjectDesc& desc, u64 frame) noexcept;

    /// Move an object. `Static`: its cards and field move now and their region is invalidated now.
    /// `SlowDynamic`: the move is coalesced, and the next `update()` at least
    /// `slow_dynamic_interval_frames` after the last flush moves them and invalidates the union of
    /// everywhere it went, once. `FastDynamic`: the proxy follows; nothing is invalidated.
    /// `TransientIgnored`: nothing.
    [[nodiscard]] Status move_object(u64 id, const Mat4& transform, u64 frame) noexcept;

    /// A skinned object's deformed proxy, posed: world-space spheres from the device pose. Used
    /// while `select_proxy_fidelity` says `Deformed`; the bounding sphere otherwise.
    [[nodiscard]] Status pose_object(u64 id, Span<const Sphere> deformed) noexcept;

    /// Unregister an object, taking its cards and field placement with it.
    void remove_object(u64 id, u64 frame) noexcept;

    [[nodiscard]] const GiObject* object(u64 id) const noexcept;
    [[nodiscard]] Span<const GiObject> objects() const noexcept { return objects_.span(); }

    /// The proxy spheres the last `update()` chose for the FastDynamic objects.
    [[nodiscard]] Span<const Sphere> proxy_spheres() const noexcept { return proxies_.span(); }

    /// The fraction of the indirect light at a point the proxies take away, in [0, 1].
    [[nodiscard]] f32 proxy_occlusion(Vec3 position, Vec3 normal) const noexcept;

    // --- Transparency and participating media (media.h) ----------------------------------------

    /// RESERVED AND DEFERRED. Refused with `ErrorCode::NotImplemented`, naming the deferral.
    [[nodiscard]] static Status set_refraction_solver(RefractionSolver* solver) noexcept;
    /// RESERVED AND DEFERRED. Refused with `ErrorCode::NotImplemented`, naming the deferral.
    [[nodiscard]] static Status set_caustics_source(CausticsSource* source) noexcept;

    /// The low-frequency indirect a participating medium takes, on a grid over `bounds`.
    [[nodiscard]] Status media_indirect(const Aabb& bounds, f32 spacing, Vec3 fallback,
                                        MediaIndirectGrid& out) const noexcept {
        return build_media_indirect(radiance_, bounds, spacing, fallback, out);
    }

    // --- Diagnostics (diagnostics.h) ---------------------------------------------------------

    /// What lit this point, and why it is the colour it is. `diffuse` is exactly what
    /// `indirect_diffuse` returns for the same arguments.
    [[nodiscard]] IlluminationQuery explain(Vec3 position, Vec3 normal,
                                            const SurfaceProperties& surface,
                                            const ExplainSettings& settings) const noexcept;

    /// The probe half of `explain` alone — the contributing probes, their terms, ages and leak
    /// suspicion, and the probes in reach — for a caller asking it of every pixel.
    [[nodiscard]] IlluminationQuery explain_probes(Vec3 position, Vec3 normal,
                                                   const ExplainSettings& settings) const noexcept;

    /// The same question asked of a box: its probes, their ages and errors, its convergence and
    /// the invalidations touching it.
    [[nodiscard]] IlluminationRegionReport explain_region(
        const Aabb& region, const ExplainSettings& settings) const noexcept;

    /// The frame the last `update()` began, including the queries made since.
    [[nodiscard]] GiProfile profile() const noexcept;

    /// Account work a producer outside `update()` did for this frame — the stochastic signals'
    /// denoising is one — to a stage of the profile.
    void add_stage_time(GiStage stage, u64 nanoseconds) noexcept;

    /// The invalidation records serviced most recently, oldest first, at most 64.
    [[nodiscard]] Span<const InvalidationRecord> recent_invalidations() const noexcept {
        return recent_.span();
    }

    // The named subsystems. Public because they are separate subsystems and not private state:
    // a renderer ingests cells into the scene, a residency system places distance fields, and an
    // editor inspects the caches, and none of that should go through a forwarding method here.
    [[nodiscard]] GiScene& scene() noexcept { return scene_; }
    [[nodiscard]] const GiScene& scene() const noexcept { return scene_; }
    [[nodiscard]] DistanceField& field() noexcept { return field_; }
    [[nodiscard]] const DistanceField& field() const noexcept { return field_; }
    [[nodiscard]] SurfaceCache& surfaces() noexcept { return surfaces_; }
    [[nodiscard]] const SurfaceCache& surfaces() const noexcept { return surfaces_; }
    [[nodiscard]] RadianceCache& radiance() noexcept { return radiance_; }
    [[nodiscard]] const RadianceCache& radiance() const noexcept { return radiance_; }
    [[nodiscard]] ReflectionProbeSet& reflection_probes() noexcept { return reflections_; }
    [[nodiscard]] const ReflectionProbeSet& reflection_probes() const noexcept {
        return reflections_;
    }
    [[nodiscard]] GiBudget& budget() noexcept { return budget_; }
    [[nodiscard]] const GiBudget& budget() const noexcept { return budget_; }
    [[nodiscard]] denoise::Denoiser& denoiser() noexcept { return denoiser_; }
    [[nodiscard]] const denoise::Denoiser& denoiser() const noexcept { return denoiser_; }
    [[nodiscard]] rt::AccelerationService& acceleration() noexcept { return acceleration_; }
    [[nodiscard]] TieredTracer& tracer() noexcept { return tracer_; }
    [[nodiscard]] const WorldTracer& world_tracer() const noexcept { return world_; }
    /// The two world tiers on their own: what a per-pixel resolve traces its final-gather ray
    /// through (screen_resolve.h's `ScreenResolveSources`).
    [[nodiscard]] const SoftwareTracer& software_tracer() const noexcept { return software_; }
    [[nodiscard]] const HardwareTracer& hardware_tracer() const noexcept { return hardware_; }
    [[nodiscard]] const ConvergenceTracker& convergence() const noexcept { return convergence_; }

    /// One frame. See the header comment for the order and why it is that order.
    IlluminationFrameReport update(const FrameContext& context) noexcept;

    /// Indirect diffuse at a surface, every source combined by confidence and the double-counting
    /// rule applied.
    [[nodiscard]] ResolveResult indirect_diffuse(Vec3 position, Vec3 normal,
                                                 const SurfaceProperties& surface) const noexcept;

    /// Indirect specular. The same scene, caches, tiers and resolve; a different ray distribution
    /// and a roughness rule, which is the whole difference.
    [[nodiscard]] ResolveResult indirect_specular(Vec3 position, Vec3 normal, Vec3 view_direction,
                                                  f32 roughness, u32 frame,
                                                  TraceBudget& budget) noexcept;

    /// Converged mode: advance frames at full update rates until the convergence metric passes
    /// `threshold` or `max_frames` have run. Returns the frames it took.
    ///
    /// "Converged mode SHALL be used by golden-image tests, cinematic capture, and reference
    /// comparison, so temporal convergence does not make rendering tests unreliable."
    u32 advance_to_convergence(const FrameContext& context, f32 threshold, u32 max_frames) noexcept;

private:
    /// The samples `indirect_diffuse` combines, the exclusion mask and the region's settings. One
    /// function, so `explain` reports the samples that were combined rather than a second set.
    struct DiffuseSamples {
        RadianceSample samples[kMaxReportedSources];
        usize count = 0;
        u32 excluded = 0;
        GiRegionSettings region{};
        f32 openness = 0.0F;
    };
    static void explain_sources(const DiffuseSamples& gathered, IlluminationQuery& query) noexcept;
    [[nodiscard]] DiffuseSamples diffuse_samples(Vec3 position, Vec3 normal,
                                                 const SurfaceProperties& surface) const noexcept;
    [[nodiscard]] ResolveResult resolve_diffuse(const DiffuseSamples& gathered, Vec3 position,
                                                Vec3 normal,
                                                const SurfaceProperties& surface) const noexcept;
    void fill_probes(Vec3 position, Vec3 normal, const ExplainSettings& settings,
                     IlluminationQuery& query) const noexcept;
    /// The distance field blocks the line from a probe to a point.
    [[nodiscard]] bool probe_blocked(Vec3 probe, Vec3 position) const noexcept;
    void region_probes(const Aabb& region, const ExplainSettings& settings,
                       IlluminationRegionReport& report) const noexcept;
    [[nodiscard]] f32 region_convergence(const Aabb& region) const noexcept;
    [[nodiscard]] Status place_object(const GiObjectDesc& desc, GiObject& entry) noexcept;
    void push_proxy(const Sphere& sphere, Vec3 albedo) noexcept;
    void add_proxies(GiObject& entry, Vec3 camera, ObjectsFrameReport& objects) noexcept;
    void see_proxies(Vec3 origin, Vec3 direction, RadianceSample& answer) const noexcept;
    [[nodiscard]] u32 find_object(u64 id) const noexcept;
    [[nodiscard]] Status commit_object(GiObject& entry, u64 frame) noexcept;
    void update_objects(IlluminationFrameReport& report, const FrameContext& context) noexcept;
    [[nodiscard]] bool proxy_hit(Vec3 origin, Vec3 direction, f32& distance,
                                 Vec3& radiance) const noexcept;
    /// What a proxy sphere sends toward `position`: its albedo times the radiance cache's light on
    /// the side facing the point.
    [[nodiscard]] Vec3 proxy_radiance(usize proxy, Vec3 toward) const noexcept;
    void begin_profile(u64 frame) noexcept;
    void record_attribution(const InvalidationRecord& record) noexcept;
    void service_invalidations(IlluminationFrameReport& report, u64 frame) noexcept;
    [[nodiscard]] bool dynamic_enabled() const noexcept;
    [[nodiscard]] Span<const GiLight> lights_for(Span<const GiLight> frame) noexcept;
    void invalidate_emitters(Span<const GiLight> lights, u64 frame) noexcept;
    void retire_volume_regions() noexcept;
    [[nodiscard]] GiRegionSettings global_region() const noexcept;
    [[nodiscard]] IlluminationClass derived_class(const GiObject& entry) const noexcept;
    [[nodiscard]] static bool participating(IlluminationClass klass) noexcept {
        return klass == IlluminationClass::Static || klass == IlluminationClass::SlowDynamic;
    }
    [[nodiscard]] Status park_object(const GiObjectDesc& desc, GiObject& entry) noexcept;
    [[nodiscard]] Status keep_emissive_cell(u64 cell, const Aabb& bounds,
                                            Span<const Surfel> surfels) noexcept;
    [[nodiscard]] Status leave_scene(GiObject& entry, u64 frame) noexcept;
    [[nodiscard]] Status enter_scene(GiObject& entry, u64 frame) noexcept;

    IlluminationSettings settings_{};
    /// The global quality and tier `set_runtime_settings` chose; neutral until it is called.
    GiQuality global_quality_ = GiQuality::Medium;
    GiTierPreference global_tier_ = GiTierPreference::Automatic;
    /// The cards each cell with an emitter was ingested from, so a change of emitter rules can
    /// classify it again. Only cells that have an emitter are kept.
    struct EmissiveCell {
        explicit EmissiveCell(Allocator& allocator = current_allocator()) noexcept
            : surfels(allocator) {}
        u64 cell = 0;
        Aabb bounds{};
        Array<Surfel> surfels;
    };
    Array<EmissiveCell> emissive_cells_;
    GiScene scene_;
    DistanceField field_;
    SurfaceCache surfaces_;
    RadianceCache radiance_;
    ReflectionProbeSet reflections_;
    GiBudget budget_;
    ConvergenceTracker convergence_;
    denoise::Denoiser denoiser_;
    rt::AccelerationService acceleration_;
    SoftwareTracer software_;
    HardwareTracer hardware_;
    WorldTracer world_;
    TieredTracer tracer_;
    struct CellPage {
        u64 cell = 0;
        u32 page = 0;
    };

    EmissiveLights emissive_;
    /// The cards as the card gather reads them, captured each update when the bounce is `Cards`.
    CardSnapshot card_snapshot_;
    CardGather card_gather_;
    Array<Surfel> cache_surfels_;
    Array<CellPage> cell_pages_;
    Array<GiLight> frame_lights_;
    Span<const GiLight> last_lights_;
    GiVolumeSet volumes_;

    Array<GiObject> objects_;
    Array<Sphere> proxies_;
    /// Each proxy sphere's object's albedo, index for index.
    Array<Vec3> proxy_albedo_;
    u32 moves_coalesced_ = 0;

    /// The profile of the frame the last `update()` began, and the counters it is a delta of.
    struct ProfileBaseline {
        TracingDiagnostics tracer{};
        u32 surface_lookups = 0;
        u32 surface_misses = 0;
        u32 probe_gathers = 0;
        u32 probe_misses = 0;
    };
    GiProfile profile_{};
    ProfileBaseline baseline_{};
    RadianceSource gather_tier_ = RadianceSource::None;
    u32 gather_rays_ = 0;
    Array<InvalidationRecord> recent_;
};

}  // namespace cy::rendering::gi
