#include <cy/rendering/gi/system.h>

#include <cy/core/jobs/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cy::rendering::gi {
namespace {

/// The number of samples `indirect_diffuse` combines. One per source that can answer a diffuse
/// query: the two baked ones, the radiance cache, and the sky.
constexpr usize kDiffuseSampleSlots = 4;
static_assert(kDiffuseSampleSlots == kMaxReportedSources);

/// How many serviced invalidation records the diagnostics keep.
constexpr usize kRecentInvalidations = 64;

/// The confidence a reflection ray that met a proxy carries. A proxy is a sphere standing in for a
/// body, so its answer is never as trustworthy as a traced surface's, and the radiance cache it
/// is lit by is a probe or two away.
constexpr f32 kProxyConfidence = 0.5F;

[[nodiscard]] f64 elapsed_ms(i64 started) noexcept {
    return static_cast<f64>(jobs::monotonic_now_ns() - started) * 1.0e-6;
}

[[nodiscard]] f32 luminance(Vec3 colour) noexcept {
    return (0.2126F * colour.x) + (0.7152F * colour.y) + (0.0722F * colour.z);
}

/// The coarse proxy: one sphere at the bounds' centre with the bounds' volume. A bounding sphere
/// would make a standing character an occluder twice its width.
[[nodiscard]] Sphere coarse_proxy(const Aabb& world) noexcept {
    const Vec3 size = world.size();
    const f32 volume = std::max(size.x * size.y * size.z, 0.0F);
    const f32 radius = std::cbrt((3.0F * volume) / (4.0F * math::kPi));
    return Sphere{world.center(), radius};
}

/// A counter's growth since a baseline; a counter reset in between counts from zero.
template <class T>
[[nodiscard]] T since(T now, T then) noexcept {
    return now >= then ? now - then : now;
}

/// How much a card facing `normal` grows under `transform`'s linear part: the length of the
/// cofactor matrix applied to the normal, `|n.x (c1 x c2) + n.y (c2 x c0) + n.z (c0 x c1)|`, which
/// is one for a rotation and the area ratio for any scale or shear.
[[nodiscard]] f32 card_area_scale(const Mat4& transform, Vec3 normal) noexcept {
    const Vec3 c0{transform.columns[0].x, transform.columns[0].y, transform.columns[0].z};
    const Vec3 c1{transform.columns[1].x, transform.columns[1].y, transform.columns[1].z};
    const Vec3 c2{transform.columns[2].x, transform.columns[2].y, transform.columns[2].z};
    return length((cross(c1, c2) * normal.x) + (cross(c2, c0) * normal.y) +
                  (cross(c0, c1) * normal.z));
}

/// A rigid transform's card areas, to the rounding of its rotation, are left as they are: an
/// object placed without a scale holds exactly the cards it was given.
constexpr f32 kRigidAreaTolerance = 1.0e-5F;

[[nodiscard]] f32 hit_rate(u32 hits, u32 total, f32 none) noexcept {
    return total == 0 ? none : static_cast<f32>(hits) / static_cast<f32>(total);
}

}  // namespace

IlluminationSystem::IlluminationSystem() noexcept = default;

Status IlluminationSystem::configure(const IlluminationSettings& settings) noexcept {
    settings_ = settings;
    scene_.set_error_targets(settings.error_targets);
    if (Status configured = field_.configure(settings.field); !configured) {
        return configured;
    }
    if (Status configured = radiance_.configure(settings.probes); !configured) {
        return configured;
    }
    budget_.configure(settings.budget);
    emissive_.configure(settings.emissive);
    convergence_.configure(settings.convergence_region_metres, 0.25F);

    acceleration_ = rt::AccelerationService{settings.ray_tracing};

    software_.bind(field_, &surfaces_);
    hardware_.bind(acceleration_, &surfaces_);
    world_.bind(&software_, &hardware_);

    TieredTracer::Config config;
    config.sky = settings.sky;
    config.max_distance_metres = settings.max_ray_distance_metres;
    tracer_.set_config(config);
    tracer_.set_software(&software_);
    tracer_.set_hardware(&hardware_);
    return ok();
}

void IlluminationSystem::set_sky_term(const SkyTerm& sky) noexcept {
    settings_.sky = sky;
    TieredTracer::Config config = tracer_.config();
    config.sky = sky;
    tracer_.set_config(config);
}

Status IlluminationSystem::ingest_cell(u64 cell, const Aabb& bounds, Span<const Surfel> surfels,
                                       u64 frame) noexcept {
    if (scene_.cell_resident(cell)) {
        return fail(ErrorCode::InvalidArgument,
                    "IlluminationSystem::ingest_cell: the cell is already resident");
    }
    if (Status classified = emissive_.ingest(cell, surfels, cache_surfels_); !classified) {
        return classified;
    }
    if (Status ingested = scene_.ingest_cell(cell, bounds, cache_surfels_.span(), frame);
        !ingested) {
        emissive_.evict(cell);
        return ingested;
    }
    invalidate_emitters(emissive_.last_changed(), frame);
    if (Status kept = keep_emissive_cell(cell, bounds, surfels); !kept) {
        return kept;
    }
    // A page per card this ingest added, remembered by cell so `evict_cell` releases exactly them.
    Status allocated = ok();
    for (const Surfel& surfel : cache_surfels_) {
        Expected<u32, Error> handle = surfaces_.allocate(surfel);
        if (!handle) {
            allocated = make_unexpected(handle.error());
            break;
        }
        if (Status pushed = cell_pages_.push_back(CellPage{cell, handle.value()}); !pushed) {
            surfaces_.release(handle.value());
            allocated = pushed;
            break;
        }
    }
    return allocated;
}

Status IlluminationSystem::keep_emissive_cell(u64 cell, const Aabb& bounds,
                                              Span<const Surfel> surfels) noexcept {
    const bool emits = std::ranges::any_of(surfels, [](const Surfel& surfel) {
        return surfel.emission.x > 0.0F || surfel.emission.y > 0.0F || surfel.emission.z > 0.0F;
    });
    if (!emits) {
        return ok();
    }
    EmissiveCell entry;
    entry.cell = cell;
    entry.bounds = bounds;
    if (Status copied = entry.surfels.append(surfels); !copied) {
        return copied;
    }
    return emissive_cells_.push_back(std::move(entry));
}

void IlluminationSystem::evict_cell(u64 cell, u64 frame) noexcept {
    scene_.evict_cell(cell, frame);
    emissive_.evict(cell);
    for (usize index = emissive_cells_.size(); index > 0; --index) {
        if (emissive_cells_[index - 1].cell == cell) {
            emissive_cells_.erase(index - 1);
        }
    }
    invalidate_emitters(emissive_.last_changed(), frame);
    // Compacted in order: `reclassify_cell` pairs a cell's pages with its cards by their order
    // here, so releasing one cell must not move another's pages past each other.
    usize kept = 0;
    for (const CellPage& entry : cell_pages_) {
        const CellPage page = entry;
        if (page.cell == cell) {
            surfaces_.release(page.page);
        } else {
            cell_pages_[kept++] = page;
        }
    }
    while (cell_pages_.size() > kept) {
        cell_pages_.pop_back();
    }
}

void IlluminationSystem::invalidate_emitters(Span<const GiLight> lights, u64 frame) noexcept {
    // A promoted light lights beyond its own cell, so its arrival or departure invalidates the
    // region it reaches above the threshold — attributed to it, as any light change is.
    for (const GiLight& light : lights) {
        const f32 reach =
            emitter_influence_radius(light, settings_.emissive.invalidation_irradiance);
        scene_.invalidate(Aabb::from_center_extents(light.position, Vec3{reach, reach, reach}),
                          InvalidationCause::LightChanged, light.id, frame);
    }
}

Span<const GiLight> IlluminationSystem::lights_for(Span<const GiLight> frame) noexcept {
    const Span<const GiLight> promoted = emissive_.lights();
    if (promoted.empty()) {
        return frame;
    }
    frame_lights_.clear();
    if (!frame_lights_.append(frame) || !frame_lights_.append(promoted)) {
        // Out of memory: shade with the frame's own lights rather than a partial list.
        return frame;
    }
    return frame_lights_.span();
}

Status IlluminationSystem::set_volumes(Span<const GiVolume> volumes) noexcept {
    for (const GiVolume& volume : volumes) {
        if (Status valid = validate_gi_volume(volume); !valid) {
            return valid;
        }
    }
    retire_volume_regions();
    if (Status assigned = volumes_.assign(volumes); !assigned) {
        return assigned;
    }
    retire_volume_regions();
    return ok();
}

void IlluminationSystem::retire_volume_regions() noexcept {
    // Only what is decided when a probe is placed needs the probes placed again. An intensity or a
    // far-field distance is resolved per sample, and a volume setting only those touches no probe.
    constexpr u32 kPlacement = gi_volume_bit(GiVolumeParameter::Quality) |
                               gi_volume_bit(GiVolumeParameter::ProbeDensity) |
                               gi_volume_bit(GiVolumeParameter::UpdateRate) |
                               gi_volume_bit(GiVolumeParameter::TierPreference);
    for (const GiVolume& volume : volumes_.volumes()) {
        if ((volume.sets & kPlacement) == 0U) {
            continue;
        }
        const f32 blend = volume.blend_distance_metres;
        const Aabb reach = Aabb::from_min_max(volume.bounds.min - Vec3{blend, blend, blend},
                                              volume.bounds.max + Vec3{blend, blend, blend});
        (void)radiance_.retire_region(reach);
    }
}

GiRegionSettings IlluminationSystem::global_region() const noexcept {
    GiRegionSettings global;
    global.far_field_begins_metres = settings_.error_targets.far_field_begins_metres;
    global.ray_scale = gi_quality_ray_scale(global_quality_);
    global.tier = global_tier_;
    return global;
}

GiRegionSettings IlluminationSystem::region_settings(Vec3 point) const noexcept {
    const GiRegionSettings global = global_region();
    return volumes_.empty() ? global : volumes_.resolve(point, global);
}

// --- Run-time settings ---------------------------------------------------------------------------

Status validate_gi_runtime_settings(const GiRuntimeSettings& settings) noexcept {
    if (settings.mode >= GiMode::Count || settings.quality >= GiQuality::Count ||
        settings.tier >= GiTierPreference::Count) {
        return fail(ErrorCode::InvalidArgument,
                    "GiRuntimeSettings: a mode, quality or tier out of range");
    }
    if (!std::isfinite(settings.budget_ms) || settings.budget_ms <= 0.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "GiRuntimeSettings: the budget must be a positive number of milliseconds");
    }
    return ok();
}

GiRuntimeSettings IlluminationSystem::runtime_settings() const noexcept {
    GiRuntimeSettings out;
    out.mode = settings_.mode;
    out.quality = global_quality_;
    out.tier = global_tier_;
    out.hardware_ray_tracing = settings_.ray_tracing.enabled_by_profile;
    out.budget_ms = budget_.settings().allocation_ms;
    return out;
}

Status IlluminationSystem::set_runtime_settings(const GiRuntimeSettings& settings) noexcept {
    if (Status valid = validate_gi_runtime_settings(settings); !valid) {
        return valid;
    }
    settings_.mode = settings.mode;
    if (settings.hardware_ray_tracing != settings_.ray_tracing.enabled_by_profile) {
        settings_.ray_tracing.enabled_by_profile = settings.hardware_ray_tracing;
        acceleration_.set_enabled_by_profile(settings.hardware_ray_tracing);
    }
    settings_.budget.allocation_ms = settings.budget_ms;
    budget_.set_allocation_ms(settings.budget_ms);
    if (settings.quality != global_quality_ || settings.tier != global_tier_) {
        global_quality_ = settings.quality;
        global_tier_ = settings.tier;
        // Both are recorded on a probe at placement, and neither moves one: every live probe is
        // told its new rays and tier in place, keeping what it gathered, so the light does not
        // drop while the probes gather at the new setting.
        (void)radiance_.record_region_settings(volumes_.empty() ? nullptr : &volumes_,
                                               global_region());
    }
    return ok();
}

// --- Emitter rules -------------------------------------------------------------------------------

Status IlluminationSystem::set_emitter_rules(Span<const EmitterRule> rules, u64 frame) noexcept {
    if (Status set = emissive_.set_rules(rules); !set) {
        return set;
    }
    Status result = ok();
    for (const EmissiveCell& entry : emissive_cells_) {
        if (Status classified = reclassify_cell(entry, frame); !classified && result) {
            result = classified;
        }
    }
    return result;
}

Status IlluminationSystem::reclassify_cell(const EmissiveCell& entry, u64 frame) noexcept {
    // IN PLACE: the cell's cards and pages keep everything they gathered and only their emission
    // changes, so a lamp put out darkens what it lit and nothing else of the cell flickers. Its
    // promoted lights leave and arrive as a streamed emitter's do, invalidating what they light.
    emissive_.evict(entry.cell);
    invalidate_emitters(emissive_.last_changed(), frame);
    if (Status classified = emissive_.ingest(entry.cell, entry.surfels.span(), cache_surfels_);
        !classified) {
        return classified;
    }
    invalidate_emitters(emissive_.last_changed(), frame);
    if (Status emitted = scene_.set_cell_emission(entry.cell, cache_surfels_.span()); !emitted) {
        return emitted;
    }
    // A card that now emits through the cache, or no longer does, changes the light it sends to
    // what gathers it: that region is invalidated as a material change, attributed to the cell.
    Aabb changed{};
    bool any = false;
    f32 strongest = 0.0F;
    usize index = 0;
    for (const CellPage& page : cell_pages_) {
        if (page.cell != entry.cell || index >= cache_surfels_.size()) {
            continue;
        }
        const Surfel& card = cache_surfels_[index++];
        const Vec3 before = surfaces_.page(page.page).emission;
        if (!surfaces_.set_emission(page.page, card.emission)) {
            continue;
        }
        changed = any ? Aabb::from_min_max(cwise_min(changed.min, card.position),
                                           cwise_max(changed.max, card.position))
                      : Aabb::from_min_max(card.position, card.position);
        any = true;
        const Surfel& brightest = emission_luminance(before) > emission_luminance(card.emission)
                                      ? entry.surfels[index - 1]
                                      : card;
        strongest = std::max(strongest, emitted_power(brightest));
    }
    if (any) {
        GiLight reach;
        reach.colour = Vec3{1.0F, 1.0F, 1.0F};
        reach.intensity = strongest / math::kPi;
        reach.emitter_area = 1.0F;
        const f32 radius =
            emitter_influence_radius(reach, settings_.emissive.invalidation_irradiance);
        scene_.invalidate(Aabb::from_min_max(changed.min - Vec3{radius, radius, radius},
                                             changed.max + Vec3{radius, radius, radius}),
                          InvalidationCause::MaterialChanged, entry.cell, frame);
    }
    return ok();
}

bool IlluminationSystem::dynamic_enabled() const noexcept {
    return settings_.mode == GiMode::Dynamic || settings_.mode == GiMode::Hybrid;
}

void IlluminationSystem::service_invalidations(IlluminationFrameReport& report,
                                               u64 frame) noexcept {
    (void)frame;
    for (InvalidationRecord& record : scene_.invalidations_mutable()) {
        // Each subsystem services the record and writes back what it cost, so one record accounts
        // for the whole of one change. That is "the system SHALL report which changes caused which
        // invalidations", as a number rather than a log line.
        //
        // AND THE CAUSE DECIDES WHO IS ASKED. The sparse distance field is a representation of
        // WHERE SURFACES ARE; a sun that rotated moved none of them, so rebuilding its bricks would
        // be work attributed to a change that cannot have caused it. `SkyChanged` is the one cause
        // that skips it, and `record.field_bricks` staying zero is how
        // "invalidating only the illumination that depends on it" is checked rather than asserted.
        const bool geometry_may_have_moved = record.cause != InvalidationCause::SkyChanged;
        record.field_bricks = geometry_may_have_moved ? field_.invalidate(record.region) : 0;
        record.surface_pages = surfaces_.invalidate(record.region);
        record.probes = radiance_.invalidate(record.region);
        (void)reflections_.invalidate(record.region);
        report.invalidated_field_bricks += record.field_bricks;
        report.invalidated_surface_pages += record.surface_pages;
        report.invalidated_probes += record.probes;
        report.invalidations_serviced += 1;
        record_attribution(record);
        if (recent_.size() >= kRecentInvalidations) {
            recent_.erase(0);
        }
        (void)recent_.push_back(record);
    }
    scene_.clear_invalidations();
}

void IlluminationSystem::record_attribution(const InvalidationRecord& record) noexcept {
    // One entry per (cause, source), costliest first. A record whose source is already listed adds
    // to it; a new one displaces the cheapest when the table is full and it costs more.
    const u32 cost = record.probes + record.surface_pages + record.field_bricks;
    GiCostAttribution* table = profile_.attribution;
    u32 slot = profile_.attribution_count;
    for (u32 index = 0; index < profile_.attribution_count; ++index) {
        if (table[index].source_id == record.source_id && table[index].cause == record.cause) {
            slot = index;
            break;
        }
    }
    if (slot == profile_.attribution_count) {
        if (profile_.attribution_count < kMaxCostAttributions) {
            profile_.attribution_count += 1;
        } else if (cost > table[kMaxCostAttributions - 1].cost) {
            slot = kMaxCostAttributions - 1;
        } else {
            return;
        }
        table[slot] = GiCostAttribution{};
        table[slot].source_id = record.source_id;
        table[slot].cause = record.cause;
    }
    GiCostAttribution& entry = table[slot];
    entry.records += 1;
    entry.probes += record.probes;
    entry.surface_pages += record.surface_pages;
    entry.field_bricks += record.field_bricks;
    entry.cost += cost;
    // Keep the table sorted by cost, then by source, so two runs list the same order.
    std::sort(table, table + profile_.attribution_count,
              [](const GiCostAttribution& a, const GiCostAttribution& b) {
                  if (a.cost != b.cost) {
                      return a.cost > b.cost;
                  }
                  if (a.source_id != b.source_id) {
                      return a.source_id < b.source_id;
                  }
                  return a.cause < b.cause;
              });
}

void IlluminationSystem::begin_profile(u64 frame) noexcept {
    profile_ = GiProfile{};
    profile_.frame = frame;
    baseline_.tracer = tracer_.diagnostics();
    baseline_.surface_lookups = surfaces_.diagnostics().lookups;
    baseline_.surface_misses = surfaces_.diagnostics().lookup_misses;
    baseline_.probe_gathers = radiance_.diagnostics().gathers;
    baseline_.probe_misses = radiance_.diagnostics().gather_misses;
    gather_tier_ = RadianceSource::None;
    gather_rays_ = 0;
}

void IlluminationSystem::add_stage_time(GiStage stage, u64 nanoseconds) noexcept {
    if (stage < GiStage::Count) {
        profile_.stage_ms[static_cast<u32>(stage)] += static_cast<f64>(nanoseconds) * 1.0e-6;
    }
}

GiProfile IlluminationSystem::profile() const noexcept {
    GiProfile out = profile_;
    const TracingDiagnostics& now = tracer_.diagnostics();
    const TracingDiagnostics& then = baseline_.tracer;
    out.stage_ms[static_cast<u32>(GiStage::ScreenTrace)] +=
        static_cast<f64>(since(now.screen_ns, then.screen_ns)) * 1.0e-6;
    out.stage_ms[static_cast<u32>(GiStage::WorldTrace)] +=
        static_cast<f64>(since(now.world_ns, then.world_ns)) * 1.0e-6;
    out.screen_rays = since(now.screen_rays, then.screen_rays);
    out.software_rays = since(now.software_rays, then.software_rays);
    out.hardware_rays = since(now.hardware_rays, then.hardware_rays);
    if (gather_tier_ == RadianceSource::HardwareTrace) {
        out.hardware_rays += gather_rays_;
    } else if (gather_tier_ == RadianceSource::SoftwareTrace) {
        out.software_rays += gather_rays_;
    }
    out.screen_hit_rate = hit_rate(
        std::min(since(now.screen_hits, then.screen_hits), out.screen_rays), out.screen_rays, 0.0F);
    const u32 lookups = since(surfaces_.diagnostics().lookups, baseline_.surface_lookups);
    const u32 surface_misses =
        std::min(since(surfaces_.diagnostics().lookup_misses, baseline_.surface_misses), lookups);
    out.surface_cache_hit_rate = hit_rate(lookups - surface_misses, lookups, 1.0F);
    const u32 gathers = since(radiance_.diagnostics().gathers, baseline_.probe_gathers);
    const u32 probe_misses =
        std::min(since(radiance_.diagnostics().gather_misses, baseline_.probe_misses), gathers);
    out.radiance_cache_hit_rate = hit_rate(gathers - probe_misses, gathers, 1.0F);
    out.probes = radiance_.probe_count();
    out.valid_probes = radiance_.diagnostics().valid_probes;
    out.surface_pages = surfaces_.page_count();
    return out;
}

// --- Objects ---------------------------------------------------------------------------------

u32 IlluminationSystem::find_object(u64 id) const noexcept {
    for (usize index = 0; index < objects_.size(); ++index) {
        if (objects_[index].id == id) {
            return static_cast<u32>(index);
        }
    }
    return ~0U;
}

const GiObject* IlluminationSystem::object(u64 id) const noexcept {
    const u32 index = find_object(id);
    return index == ~0U ? nullptr : &objects_[index];
}

Status IlluminationSystem::add_object(const GiObjectDesc& desc, u64 frame) noexcept {
    if (find_object(desc.id) != ~0U) {
        return fail(ErrorCode::AlreadyExists, "IlluminationSystem::add_object: the id is taken");
    }
    if ((desc.id & kObjectCellTag) != 0U) {
        return fail(ErrorCode::InvalidArgument,
                    "IlluminationSystem::add_object: the top bit of an object id is reserved");
    }
    GiObject entry;
    entry.id = desc.id;
    entry.mobility = desc.mobility;
    entry.local_bounds = desc.local_bounds;
    entry.transform = desc.transform;
    entry.committed_transform = desc.transform;
    entry.skinned = desc.skinned;
    entry.importance = desc.importance;
    entry.proxy_albedo = desc.proxy_albedo;
    entry.has_class_override = desc.has_class_override;
    entry.class_override = desc.class_override;
    entry.klass = desc.has_class_override ? desc.class_override : derived_class(entry);
    entry.last_flush_frame = frame;
    const bool participates = participating(entry.klass);
    if (participates) {
        if (Status prepared = place_object(desc, entry); !prepared) {
            return prepared;
        }
    } else if (Status parked = park_object(desc, entry); !parked) {
        // A class that takes no part keeps its cards and its field, so `set_object_class` can
        // give it one that does.
        return parked;
    }
    if (Status pushed = objects_.push_back(std::move(entry)); !pushed) {
        if (participates && desc.field != nullptr) {
            field_.remove(object_cell_id(desc.id));
        }
        return pushed;
    }
    return participates ? commit_object(objects_.back(), frame) : ok();
}

IlluminationClass IlluminationSystem::derived_class(const GiObject& entry) const noexcept {
    return classify_illumination(entry.mobility,
                                 object_size_metres(entry.local_bounds, entry.transform),
                                 settings_.classification);
}

Status IlluminationSystem::park_object(const GiObjectDesc& desc, GiObject& entry) noexcept {
    if (Status copied = entry.surfels.append(desc.surfels); !copied) {
        return copied;
    }
    if (desc.field == nullptr) {
        return ok();
    }
    entry.parked_dimension = desc.field->dimension;
    entry.parked_bounds = desc.field->bounds;
    return entry.parked_distances.append(desc.field->distances);
}

Status IlluminationSystem::set_object_class(u64 id, bool has_override, IlluminationClass klass,
                                            u64 frame) noexcept {
    if (has_override && klass >= IlluminationClass::Count) {
        return fail(ErrorCode::InvalidArgument,
                    "IlluminationSystem::set_object_class: a class out of range");
    }
    const u32 index = find_object(id);
    if (index == ~0U) {
        return fail(ErrorCode::NotFound, "IlluminationSystem::set_object_class: no such object");
    }
    GiObject& entry = objects_[index];
    entry.has_class_override = has_override;
    entry.class_override = klass;
    const IlluminationClass next = has_override ? klass : derived_class(entry);
    const bool was = participating(entry.klass);
    const bool will = participating(next);
    entry.klass = next;
    if (was == will) {
        // Static and SlowDynamic hold the same state, and so do the other two; a pending
        // SlowDynamic move becoming Static is flushed now, as a Static move would be.
        return was && entry.pending ? commit_object(entry, frame) : ok();
    }
    return will ? enter_scene(entry, frame) : leave_scene(entry, frame);
}

Status IlluminationSystem::leave_scene(GiObject& entry, u64 frame) noexcept {
    const u64 cell = object_cell_id(entry.id);
    if (entry.has_field) {
        entry.parked_distances.clear();
        if (Status copied = field_.copy_placement(cell, entry.parked_dimension, entry.parked_bounds,
                                                  entry.parked_distances);
            !copied) {
            return copied;
        }
        field_.remove(cell);
        entry.has_field = false;
    }
    if (scene_.cell_resident(cell)) {
        evict_cell(cell, frame);
    }
    entry.pending = false;
    entry.pending_region = Aabb{};
    return ok();
}

Status IlluminationSystem::enter_scene(GiObject& entry, u64 frame) noexcept {
    if (!entry.parked_distances.empty()) {
        AssetDistanceField parked;
        parked.dimension = entry.parked_dimension;
        parked.bounds = entry.parked_bounds;
        parked.distances = entry.parked_distances.span();
        if (Status placed = field_.place(object_cell_id(entry.id), parked, entry.transform);
            !placed) {
            return placed;
        }
        entry.has_field = true;
        entry.parked_distances.clear();
    }
    return commit_object(entry, frame);
}

Status IlluminationSystem::place_object(const GiObjectDesc& desc, GiObject& entry) noexcept {
    if (Status copied = entry.surfels.append(desc.surfels); !copied) {
        return copied;
    }
    if (desc.field == nullptr) {
        return ok();
    }
    if (Status placed = field_.place(object_cell_id(desc.id), *desc.field, desc.transform);
        !placed) {
        return placed;
    }
    entry.has_field = true;
    return ok();
}

Status IlluminationSystem::commit_object(GiObject& entry, u64 frame) noexcept {
    // The object's cards, in the world, through the same door a streamed cell takes — so the
    // emissive classification, the pages and the invalidation records are a cell's.
    const u64 cell = object_cell_id(entry.id);
    Array<Surfel> world;
    if (Status reserved = world.reserve(entry.surfels.size()); !reserved) {
        return reserved;
    }
    for (const Surfel& local : entry.surfels) {
        Surfel placed = local;
        placed.position = transform_point(entry.transform, local.position);
        placed.normal =
            normalized_or(transform_direction(entry.transform, local.normal), local.normal);
        // A scaled object's cards cover its scaled surface: their area, and with it an emitter's
        // power (emissive.h), is the world's, not the object space's.
        const f32 area_scale =
            card_area_scale(entry.transform, normalized_or(local.normal, Vec3{}));
        if (std::fabs(area_scale - 1.0F) > kRigidAreaTolerance) {
            placed.area = local.area * area_scale;
        }
        (void)world.push_back(placed);
    }
    if (scene_.cell_resident(cell)) {
        evict_cell(cell, frame);
    }
    if (entry.has_field) {
        if (Status moved = field_.move(cell, entry.transform); !moved) {
            return moved;
        }
    }
    entry.committed_transform = entry.transform;
    entry.pending = false;
    entry.pending_region = Aabb{};
    entry.last_flush_frame = frame;
    if (world.empty()) {
        return ok();
    }
    return ingest_cell(cell, transformed(entry.local_bounds, entry.transform), world.span(), frame);
}

Status IlluminationSystem::move_object(u64 id, const Mat4& transform, u64 frame) noexcept {
    const u32 index = find_object(id);
    if (index == ~0U) {
        return fail(ErrorCode::NotFound, "IlluminationSystem::move_object: no such object");
    }
    GiObject& entry = objects_[index];
    switch (entry.klass) {
        case IlluminationClass::Static:
            entry.transform = transform;
            return commit_object(entry, frame);
        case IlluminationClass::SlowDynamic:
            // Coalesced: the union of everywhere it has been since the last flush is what the
            // flush invalidates, once.
            if (!entry.pending) {
                entry.pending_region = transformed(entry.local_bounds, entry.committed_transform);
            }
            entry.pending = true;
            entry.transform = transform;
            entry.pending_region.grow(transformed(entry.local_bounds, transform));
            moves_coalesced_ += 1;
            return ok();
        case IlluminationClass::FastDynamic:
        case IlluminationClass::TransientIgnored:
        case IlluminationClass::Count:
            entry.transform = transform;
            entry.committed_transform = transform;
            return ok();
    }
    return ok();
}

Status IlluminationSystem::pose_object(u64 id, Span<const Sphere> deformed) noexcept {
    const u32 index = find_object(id);
    if (index == ~0U) {
        return fail(ErrorCode::NotFound, "IlluminationSystem::pose_object: no such object");
    }
    GiObject& entry = objects_[index];
    entry.posed.clear();
    return entry.posed.append(deformed);
}

void IlluminationSystem::remove_object(u64 id, u64 frame) noexcept {
    const u32 index = find_object(id);
    if (index == ~0U) {
        return;
    }
    const u64 cell = object_cell_id(id);
    if (scene_.cell_resident(cell)) {
        evict_cell(cell, frame);
    }
    if (objects_[index].has_field) {
        field_.remove(cell);
    }
    objects_.erase(index);
}

void IlluminationSystem::push_proxy(const Sphere& sphere, Vec3 albedo) noexcept {
    // The two arrays stay index for index: a sphere whose albedo could not be stored is dropped.
    if (proxies_.push_back(sphere) && !proxy_albedo_.push_back(albedo)) {
        proxies_.pop_back();
    }
}

void IlluminationSystem::add_proxies(GiObject& entry, Vec3 camera,
                                     ObjectsFrameReport& objects) noexcept {
    const Aabb world = transformed(entry.local_bounds, entry.transform);
    bool deformed = false;
    if (entry.skinned) {
        entry.fidelity = select_proxy_fidelity(length(world.center() - camera), entry.importance,
                                               settings_.classification);
        deformed = entry.fidelity == ProxyFidelity::Deformed && !entry.posed.empty();
        objects.deformed_proxies += deformed ? 1U : 0U;
        objects.coarse_proxies += deformed ? 0U : 1U;
    }
    if (!deformed) {
        push_proxy(coarse_proxy(world), entry.proxy_albedo);
        return;
    }
    for (const Sphere& bone : entry.posed) {
        push_proxy(bone, entry.proxy_albedo);
    }
}

void IlluminationSystem::update_objects(IlluminationFrameReport& report,
                                        const FrameContext& context) noexcept {
    ObjectsFrameReport& objects = report.objects;
    objects.moves_coalesced = moves_coalesced_;
    moves_coalesced_ = 0;
    proxies_.clear();
    proxy_albedo_.clear();
    const u64 interval = settings_.classification.slow_dynamic_interval_frames;
    for (GiObject& entry : objects_) {
        objects.objects[static_cast<u32>(entry.klass)] += 1;
        const bool due = entry.klass == IlluminationClass::SlowDynamic && entry.pending &&
                         context.frame >= entry.last_flush_frame + interval;
        // The flush files its invalidation records with the scene, which step 2 services.
        if (due && commit_object(entry, context.frame)) {
            objects.flushes += 1;
        }
        if (entry.klass == IlluminationClass::FastDynamic) {
            add_proxies(entry, context.camera, objects);
        }
    }
    objects.proxy_spheres = static_cast<u32>(proxies_.size());
}

f32 IlluminationSystem::proxy_occlusion(Vec3 position, Vec3 normal) const noexcept {
    // Independent occluders: what is left is the product of what each leaves.
    f32 visible = 1.0F;
    for (const Sphere& sphere : proxies_) {
        visible *= 1.0F - sphere_occlusion(sphere, position, normal);
    }
    return 1.0F - visible;
}

bool IlluminationSystem::proxy_hit(Vec3 origin, Vec3 direction, f32& distance,
                                   Vec3& radiance) const noexcept {
    f32 nearest = -1.0F;
    usize which = 0;
    for (usize index = 0; index < proxies_.size(); ++index) {
        const f32 t = sphere_hit_distance(proxies_[index], origin, direction);
        if (t >= 0.0F && (nearest < 0.0F || t < nearest)) {
            nearest = t;
            which = index;
        }
    }
    if (nearest < 0.0F || nearest > settings_.max_ray_distance_metres) {
        return false;
    }
    radiance = proxy_radiance(which, origin + (direction * nearest));
    distance = nearest;
    return true;
}

void IlluminationSystem::see_proxies(Vec3 origin, Vec3 direction,
                                     RadianceSample& answer) const noexcept {
    // REFLECTION PRESENCE: a FastDynamic proxy nearer than what the world answered is what the ray
    // sees. None registered, nothing here.
    f32 distance = 0.0F;
    Vec3 radiance{0.0F, 0.0F, 0.0F};
    if (proxies_.empty() || !proxy_hit(origin, direction, distance, radiance)) {
        return;
    }
    if (answer.hit_distance >= 0.0F && distance >= answer.hit_distance) {
        return;
    }
    answer.radiance = radiance;
    answer.hit_distance = distance;
    answer.confidence = std::max(answer.confidence, kProxyConfidence);
}

Vec3 IlluminationSystem::proxy_radiance(usize proxy, Vec3 toward) const noexcept {
    // A Lambertian body reflects its albedo times the mean radiance arriving at it: the radiance
    // cache's answer on the side of the sphere that faces `toward`, or the sky where no probe is.
    const Sphere& sphere = proxies_[proxy];
    const Vec3 outward = normalized_or(toward - sphere.center, Vec3{0.0F, 1.0F, 0.0F});
    const Vec3 surface = sphere.center + (outward * (sphere.radius + 1.0e-2F));
    const RadianceSample lit = radiance_.sample(surface, outward, 0);
    const Vec3 incoming = lit.confidence > 0.0F ? lit.radiance : settings_.sky.radiance(outward);
    return cwise_mul(proxy_albedo_[proxy], incoming);
}

// --- Media -----------------------------------------------------------------------------------

Status IlluminationSystem::set_refraction_solver(RefractionSolver* solver) noexcept {
    (void)solver;
    return fail(ErrorCode::NotImplemented,
                "true refraction is DEFERRED (rendering-global-illumination, \"Transparency and "
                "participating media\"): RefractionSolver is the reserved interface and no solver "
                "is accepted yet");
}

Status IlluminationSystem::set_caustics_source(CausticsSource* source) noexcept {
    (void)source;
    return fail(ErrorCode::NotImplemented,
                "caustics are DEFERRED (rendering-global-illumination, \"Transparency and "
                "participating media\"): CausticsSource is the reserved interface and no source "
                "is accepted yet");
}

// --- Diagnostics -----------------------------------------------------------------------------

void IlluminationSystem::explain_sources(const DiffuseSamples& gathered,
                                         IlluminationQuery& query) noexcept {
    f32 total_confidence = 0.0F;
    for (usize index = 0; index < gathered.count; ++index) {
        if ((gathered.excluded & source_bit(gathered.samples[index].source)) == 0U) {
            total_confidence += std::max(gathered.samples[index].confidence, 0.0F);
        }
    }
    for (usize index = 0; index < gathered.count; ++index) {
        const RadianceSample& sample = gathered.samples[index];
        SourceReport& entry = query.sources[query.source_count++];
        entry.source = sample.source;
        entry.radiance = sample.radiance;
        entry.confidence = sample.confidence;
        entry.excluded = (gathered.excluded & source_bit(sample.source)) != 0U;
        entry.weight_share = (entry.excluded || total_confidence <= 0.0F)
                                 ? 0.0F
                                 : std::max(sample.confidence, 0.0F) / total_confidence;
    }
}

bool IlluminationSystem::probe_blocked(Vec3 probe, Vec3 position) const noexcept {
    // The field's hit test is "within half a voxel of something", so the march stops two voxels
    // short of the point, whose own surface would otherwise answer it.
    const f32 clearance = 2.0F * field_.voxel_size();
    const Vec3 offset = position - probe;
    const f32 span = length(offset);
    if (span <= clearance) {
        return false;
    }
    const Ray ray{probe, offset / span};
    return field_.sphere_trace(ray, span - clearance).hit;
}

void IlluminationSystem::fill_probes(Vec3 position, Vec3 normal, const ExplainSettings& settings,
                                     IlluminationQuery& query) const noexcept {
    ProbeContribution terms[kMaxReportedProbes];
    query.probes_contributing = radiance_.contributions(
        position, normal, Span<ProbeContribution>{terms, kMaxReportedProbes});
    query.probe_count = std::min(query.probes_contributing, kMaxReportedProbes);
    f32 total_weight = 0.0F;
    f32 leaked_weight = 0.0F;
    for (u32 index = 0; index < query.probe_count; ++index) {
        const Probe& probe = radiance_.probe(terms[index].probe);
        ProbeReport& entry = query.probes[index];
        entry.probe = terms[index].probe;
        entry.position = probe.position;
        entry.level = probe.level;
        entry.weight = terms[index].weight;
        entry.falloff = terms[index].falloff;
        entry.facing = terms[index].facing;
        entry.visibility = terms[index].visibility;
        entry.error = probe.error;
        entry.age_frames =
            settings.frame > probe.last_update_frame ? settings.frame - probe.last_update_frame : 0;
        entry.leak_suspect = probe_blocked(probe.position, position);
        total_weight += entry.weight;
        query.leak_suspects += entry.leak_suspect ? 1U : 0U;
        leaked_weight += entry.leak_suspect ? entry.weight : 0.0F;
    }
    query.leak_weight = total_weight > 0.0F ? leaked_weight / total_weight : 0.0F;

    const f32 reach = settings_.probes.base_spacing_metres *
                      static_cast<f32>(1U << (settings_.probes.levels - 1)) * 1.5F;
    for (const Probe& probe : radiance_.probes()) {
        if (probe.live && length(probe.position - position) <= reach) {
            query.probes_in_reach += 1;
            query.invalid_probes_in_reach += probe.valid ? 0U : 1U;
        }
    }
}

namespace {

/// The one cause a query names, in the order a developer asks: is anything answering at all, is it
/// still moving, is it trusted, and only then is it simply dark.
[[nodiscard]] DarknessCause darkness_cause(const IlluminationQuery& query,
                                           const ExplainSettings& settings) noexcept {
    const bool cache_answered = query.probes_contributing > 0;
    if (!cache_answered && query.invalid_probes_in_reach > 0) {
        return DarknessCause::Invalidated;
    }
    if (!cache_answered && query.diffuse.confidence < settings.confidence_floor) {
        return DarknessCause::MissingProbes;
    }
    bool moving = query.convergence < settings.converged_threshold;
    for (u32 index = 0; index < query.probe_count; ++index) {
        moving = moving || query.probes[index].error > settings.probe_error_threshold;
    }
    if (cache_answered && moving) {
        return DarknessCause::Unconverged;
    }
    if (query.diffuse.confidence < settings.confidence_floor) {
        return DarknessCause::LowConfidence;
    }
    return luminance(query.diffuse.radiance) < settings.dark_luminance ? DarknessCause::Occluded
                                                                       : DarknessCause::Lit;
}

}  // namespace

IlluminationQuery IlluminationSystem::explain_probes(
    Vec3 position, Vec3 normal, const ExplainSettings& settings) const noexcept {
    IlluminationQuery query;
    fill_probes(position, normal, settings, query);
    return query;
}

IlluminationQuery IlluminationSystem::explain(Vec3 position, Vec3 normal,
                                              const SurfaceProperties& surface,
                                              const ExplainSettings& settings) const noexcept {
    IlluminationQuery query;
    const DiffuseSamples gathered = diffuse_samples(position, normal, surface);
    query.diffuse = resolve_diffuse(gathered, position, normal, surface);
    explain_sources(gathered, query);
    fill_probes(position, normal, settings, query);
    query.tier_preference = gathered.region.tier;
    query.world_tier = tracer_.world_tier_for(gathered.region.tier);
    query.sky_visibility = gathered.openness;
    query.field_distance = field_.distance(position);
    query.convergence = convergence_.convergence(position);
    query.covered = scene_.has_coverage(position);
    query.cause = darkness_cause(query, settings);
    return query;
}

namespace {

[[nodiscard]] DarknessCause region_cause(const IlluminationRegionReport& report,
                                         const ExplainSettings& settings) noexcept {
    if (report.valid_probes == 0) {
        return report.invalid_probes > 0 ? DarknessCause::Invalidated
                                         : DarknessCause::MissingProbes;
    }
    const bool moving = report.convergence < settings.converged_threshold ||
                        report.max_error > settings.probe_error_threshold;
    return moving ? DarknessCause::Unconverged : DarknessCause::Lit;
}

[[nodiscard]] u32 touching(Span<const InvalidationRecord> records, const Aabb& region) noexcept {
    u32 count = 0;
    for (const InvalidationRecord& record : records) {
        count += record.region.intersects(region) ? 1U : 0U;
    }
    return count;
}

}  // namespace

void IlluminationSystem::region_probes(const Aabb& region, const ExplainSettings& settings,
                                       IlluminationRegionReport& report) const noexcept {
    f64 age_sum = 0.0;
    f64 error_sum = 0.0;
    for (const Probe& probe : radiance_.probes()) {
        if (!probe.live || !region.contains(probe.position)) {
            continue;
        }
        report.probes += 1;
        if (!probe.valid) {
            report.invalid_probes += 1;
            continue;
        }
        report.valid_probes += 1;
        const u64 age =
            settings.frame > probe.last_update_frame ? settings.frame - probe.last_update_frame : 0;
        report.oldest_age_frames = std::max(report.oldest_age_frames, age);
        age_sum += static_cast<f64>(age);
        error_sum += static_cast<f64>(probe.error);
        report.max_error = std::max(report.max_error, probe.error);
    }
    if (report.valid_probes > 0) {
        report.mean_age_frames = static_cast<f32>(age_sum / report.valid_probes);
        report.mean_error = static_cast<f32>(error_sum / report.valid_probes);
    }
}

f32 IlluminationSystem::region_convergence(const Aabb& region) const noexcept {
    // The least converged convergence region the box overlaps, sampled a region apart.
    const f32 step = std::max(settings_.convergence_region_metres, 0.25F);
    const Vec3 size = region.size();
    const auto samples = [step](f32 extent) {
        return static_cast<u32>(std::ceil(std::max(extent, 0.0F) / step)) + 1U;
    };
    const u32 counts[3] = {samples(size.x), samples(size.y), samples(size.z)};
    f32 least = 1.0F;
    for (u32 z = 0; z < counts[2]; ++z) {
        for (u32 y = 0; y < counts[1]; ++y) {
            for (u32 x = 0; x < counts[0]; ++x) {
                const Vec3 offset =
                    Vec3{static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z)} * step;
                const Vec3 point{std::min(region.min.x + offset.x, region.max.x),
                                 std::min(region.min.y + offset.y, region.max.y),
                                 std::min(region.min.z + offset.z, region.max.z)};
                least = std::min(least, convergence_.convergence(point));
            }
        }
    }
    return least;
}

IlluminationRegionReport IlluminationSystem::explain_region(
    const Aabb& region, const ExplainSettings& settings) const noexcept {
    IlluminationRegionReport report;
    if (region.is_empty()) {
        report.cause = DarknessCause::MissingProbes;
        return report;
    }
    region_probes(region, settings, report);
    for (const SurfacePage& page : surfaces_.pages()) {
        const bool inside = page.live && region.contains(page.position);
        report.surface_pages += inside ? 1U : 0U;
        report.valid_surface_pages += (inside && page.valid) ? 1U : 0U;
    }
    report.convergence = region_convergence(region);
    report.pending_invalidations = touching(scene_.invalidations(), region);
    report.recent_invalidations = touching(recent_.span(), region);
    report.cause = region_cause(report, settings);
    return report;
}

IlluminationFrameReport IlluminationSystem::update(const FrameContext& context) noexcept {
    IlluminationFrameReport report;
    begin_profile(context.frame);
    // The frame's lights and every promoted emitter, which the direct lighting path below shades
    // like any other light.
    const Span<const GiLight> lights = lights_for(context.lights);
    last_lights_ = lights;

    // 1. The budget. Every step below reads a lever value from it.
    if (context.permit_relaxation) {
        budget_.permit_relaxation();
    }
    report.budget = budget_.update(context.measured_gi_ms, context.frame);
    profile_.allocation_ms = report.budget.allocation_ms;
    profile_.measured_ms = report.budget.measured_ms;

    // 1b. The objects: flush the SlowDynamic moves that are due — which files their invalidation
    //     for step 2 — and choose this frame's proxies.
    update_objects(report, context);

    // 2. Invalidations, before anything updates something that is about to be invalidated.
    service_invalidations(report, context.frame);

    // 3. The distance field: the software tier traces it.
    i64 started = jobs::monotonic_now_ns();
    report.field = field_.scroll_to(context.camera);

    // 4. The acceleration structures: the hardware tier traces them. Inactive is a no-op that
    //    builds nothing, which is what makes the fallback the default rather than a special case.
    report.acceleration = acceleration_.update();
    report.world_tier = world_.tier();
    profile_.stage_ms[static_cast<u32>(GiStage::WorldTrace)] += elapsed_ms(started);

    if (settings_.mode == GiMode::None) {
        // Ambient and sky only. Nothing below is a bounce, so nothing below runs.
        report.convergence = convergence_.worst_convergence();
        return report;
    }

    // 5. Probe placement.
    started = jobs::monotonic_now_ns();
    ProbePlacementContext placement;
    placement.field = &field_;
    placement.lights = lights;
    placement.importance_region = context.importance_region;
    placement.has_importance_region = context.has_importance_region;
    placement.volumes = volumes_.empty() ? nullptr : &volumes_;
    placement.global = global_region();
    report.placement = radiance_.scroll_to(context.camera, placement);
    profile_.stage_ms[static_cast<u32>(GiStage::ProbeUpdate)] += elapsed_ms(started);

    if (!dynamic_enabled()) {
        // `Baked` and `Probe` keep the seeded caches and update neither. They are first-class
        // modes, so this is the whole of them rather than a disabled path.
        report.convergence = convergence_.worst_convergence();
        return report;
    }

    // 6. The surface cache: the probes gather from it.
    started = jobs::monotonic_now_ns();
    SurfaceUpdateContext surfaces;
    surfaces.lights = lights;
    surfaces.occluder = &world_;
    surfaces.indirect = &radiance_;
    if (settings_.surface_bounce == SurfaceBounce::Cards &&
        card_snapshot_.capture(surfaces_, context.frame).has_value()) {
        CardGatherSettings gather = settings_.card_gather;
        gather.sky = settings_.sky;
        card_gather_.bind(&field_, &card_snapshot_, gather);
        surfaces.indirect = &card_gather_;
    }
    surfaces.frame = context.frame;
    surfaces.budget = static_cast<u32>(budget_.value(GiLever::SurfaceCacheRate));
    report.surfaces =
        context.converged_mode ? surfaces_.update_all(surfaces) : surfaces_.update(surfaces);
    profile_.stage_ms[static_cast<u32>(GiStage::SurfaceCache)] += elapsed_ms(started);
    profile_.surface_pages_updated = report.surfaces.pages_updated;

    // 7. The radiance cache: the surface cache's next update gathers from it. This is the feedback
    //    loop that makes successive frames approximate additional bounces.
    ProbeUpdateContext probes;
    probes.tracer = &world_;
    probes.radiance = &surfaces_;
    probes.sky = settings_.sky;
    probes.frame = context.frame;
    probes.max_ray_distance_metres = settings_.max_ray_distance_metres;
    probes.budget = static_cast<u32>(budget_.value(GiLever::ProbeUpdates));
    probes.rays_per_probe =
        std::max(1U, static_cast<u32>(std::min(static_cast<f32>(settings_.rays_per_probe),
                                               budget_.value(GiLever::TracedRays))));
    probes.converged = context.converged_mode;
    probes.software_tracer = &software_;
    started = jobs::monotonic_now_ns();
    report.probes = radiance_.update(probes);
    profile_.stage_ms[static_cast<u32>(GiStage::ProbeUpdate)] += elapsed_ms(started);
    profile_.probes_updated = report.probes.probes_updated;
    profile_.probe_queue_depth = report.probes.queue_depth;
    gather_tier_ = world_.tier();
    gather_rays_ = report.probes.rays;

    // 8. The reflection probes: the same tracer, the same surface cache.
    ReflectionCaptureContext capture;
    capture.tracer = &world_;
    capture.radiance = &surfaces_;
    capture.sky = settings_.sky;
    capture.frame = context.frame;
    capture.max_distance_metres = settings_.max_ray_distance_metres;
    const f32 reflection_scale = budget_.value(GiLever::ReflectionResolution);
    capture.texel_budget =
        context.converged_mode
            ? kReflectionTexels * std::max(1U, reflections_.probe_count())
            : std::max(1U, static_cast<u32>(16.0F * math::clamp(reflection_scale, 0.1F, 1.0F)));
    started = jobs::monotonic_now_ns();
    report.reflections = reflections_.capture(capture);
    profile_.stage_ms[static_cast<u32>(GiStage::ReflectionResolve)] += elapsed_ms(started);

    // The denoiser's quality is one of the declared levers, and this is where the budget's decision
    // reaches it. It is the only place: a second call site would be a second policy.
    denoiser_.set_quality_position(static_cast<u32>(budget_.value(GiLever::DenoiserQuality)));

    // 9. Convergence, from what 6 and 7 reported.
    for (const Probe& probe : radiance_.probes()) {
        if (probe.live && probe.valid) {
            convergence_.observe(probe.position, probe.error);
        }
    }
    report.convergence = convergence_.worst_convergence();
    return report;
}

IlluminationSystem::DiffuseSamples IlluminationSystem::diffuse_samples(
    Vec3 position, Vec3 normal, const SurfaceProperties& surface) const noexcept {
    DiffuseSamples gathered;
    RadianceSample* samples = gathered.samples;
    usize count = 0;

    if (surface.has_lightmap) {
        samples[count].source = RadianceSource::Lightmap;
        samples[count].radiance = surface.lightmap_radiance;
        samples[count].confidence = 1.0F;
        count += 1;
    }
    if (surface.has_irradiance_volume) {
        samples[count].source = RadianceSource::IrradianceVolume;
        samples[count].radiance = surface.irradiance_volume_radiance;
        samples[count].confidence = 0.9F;
        count += 1;
    }

    // The radiance cache, unless the query is outside every resident cell — in which case the GI
    // scene has no coverage there and the far field answers instead. That is
    // "queries into it SHALL fall back to the far-field representation" as a branch on coverage.
    if (scene_.has_coverage(position)) {
        samples[count] = radiance_.sample(position, normal, 0);
        count += 1;
    }

    // The sky, weighted by how much of it the surface can actually see and by how far into the far
    // field the query is. Both terms are needed: an indoor surface that took full sky would be the
    // leak the field's sky visibility exists to prevent.
    gathered.region = region_settings(position);
    ErrorTargets targets = settings_.error_targets;
    targets.far_field_begins_metres = gathered.region.far_field_begins_metres;
    const f32 far = far_field_weight(surface.camera_distance_metres, targets);
    const f32 openness = field_.sky_visibility(position, normal, 8);
    gathered.openness = openness;
    samples[count].source = RadianceSource::Sky;
    samples[count].radiance = settings_.sky.radiance(normal) * openness;
    samples[count].confidence =
        math::clamp((0.35F * openness) + (0.65F * far * openness), 0.0F, 1.0F);
    count += 1;

    gathered.count = count;
    gathered.excluded =
        exclusion_for(settings_.mode, surface.has_lightmap, surface.has_irradiance_volume);
    return gathered;
}

ResolveResult IlluminationSystem::resolve_diffuse(const DiffuseSamples& gathered, Vec3 position,
                                                  Vec3 normal,
                                                  const SurfaceProperties& surface) const noexcept {
    ResolveResult result =
        combine(Span<const RadianceSample>{gathered.samples, gathered.count}, gathered.excluded);
    // The FastDynamic proxies take their share of the light away and send back what they reflect
    // of the light around them. None registered, nothing here.
    if (!proxies_.empty()) {
        f32 visible = 1.0F;
        Vec3 bounced{0.0F, 0.0F, 0.0F};
        for (usize index = 0; index < proxies_.size(); ++index) {
            const f32 covered = sphere_occlusion(proxies_[index], position, normal);
            if (covered <= 0.0F) {
                continue;
            }
            visible *= 1.0F - covered;
            bounced = bounced + (proxy_radiance(index, position) * covered);
        }
        result.proxy_occlusion = 1.0F - visible;
        result.radiance = (result.radiance * visible) + bounced;
    }
    if (gathered.region.diffuse_intensity != 1.0F) {
        result.radiance = result.radiance * gathered.region.diffuse_intensity;
    }
    // Thin transmission: the back face's irradiance, from a hair behind the surface so the probes
    // on its own side are on the far side of it, tinted. An opaque surface asks for none.
    if (surface.transmission.x > 0.0F || surface.transmission.y > 0.0F ||
        surface.transmission.z > 0.0F) {
        const Vec3 back = normal * -1.0F;
        const Vec3 behind = position + (back * kThinTransmissionBackOffsetMetres);
        const RadianceSample through = radiance_.sample(behind, back, 0);
        result.transmitted =
            cwise_mul(through.radiance, surface.transmission) * gathered.region.diffuse_intensity;
    }
    return result;
}

ResolveResult IlluminationSystem::indirect_diffuse(
    Vec3 position, Vec3 normal, const SurfaceProperties& surface) const noexcept {
    return resolve_diffuse(diffuse_samples(position, normal, surface), position, normal, surface);
}

ResolveResult IlluminationSystem::indirect_specular(Vec3 position, Vec3 normal, Vec3 view_direction,
                                                    f32 roughness, u32 frame,
                                                    TraceBudget& budget) noexcept {
    const i64 started = jobs::monotonic_now_ns();
    const u64 traced_before = tracer_.diagnostics().screen_ns + tracer_.diagnostics().world_ns;
    const Vec3 incident = normalized_or(view_direction, Vec3{0.0F, 0.0F, -1.0F});
    const Vec3 reflection =
        normalized_or(incident - (normal * (2.0F * dot(incident, normal))), normal);
    const u32 rays = reflection_ray_count(roughness, settings_.reflection_thresholds);
    const GiRegionSettings region = region_settings(position);

    RadianceSample samples[3];
    usize count = 0;

    if (rays > 0) {
        // Dedicated rays through the SAME tiered tracer diffuse uses. The only difference is the
        // distribution, which is what "differing only in ray distribution and roughness handling"
        // means and is why there is no second tracer here.
        Vec3 total{0.0F, 0.0F, 0.0F};
        f32 confidence = 0.0F;
        u32 taken = 0;
        for (u32 index = 0; index < rays; ++index) {
            const Vec3 direction =
                reflection_lobe_direction(reflection, normal, roughness, index, rays, frame);
            const Vec3 origin = position + (normal * 1.0e-3F);
            RadianceSample answer = tracer_.trace(
                origin, direction, settings_.max_ray_distance_metres, budget, region.tier);
            see_proxies(origin, direction, answer);
            if (answer.confidence <= 0.0F) {
                continue;
            }
            total = total + answer.radiance;
            confidence = std::max(confidence, answer.confidence);
            taken += 1;
        }
        if (taken > 0) {
            samples[count].radiance = total / static_cast<f32>(taken);
            samples[count].confidence = confidence;
            samples[count].source = tracer_.selected_world_tier();
            count += 1;
        }
    }

    // The radiance cache is the rough answer and the fallback for the smooth one. It costs nothing
    // extra: it is the same cache the diffuse gather reads.
    RadianceSample cached = radiance_.sample(position, reflection, 0);
    if (cached.confidence > 0.0F) {
        // A rough surface trusts the cache as much as anything; a mirror barely at all.
        cached.confidence *= math::clamp(roughness, 0.15F, 1.0F);
        samples[count] = cached;
        count += 1;
    }

    // And the probes, blended in by confidence rather than switched to.
    const RadianceSample probe = reflections_.sample(position, reflection, roughness);
    if (probe.confidence > 0.0F) {
        samples[count] = probe;
        count += 1;
    }

    ResolveResult result = combine(Span<const RadianceSample>{samples, count}, 0);
    if (region.specular_intensity != 1.0F) {
        result.radiance = result.radiance * region.specular_intensity;
    }
    const u64 traced =
        tracer_.diagnostics().screen_ns + tracer_.diagnostics().world_ns - traced_before;
    const i64 spent = jobs::monotonic_now_ns() - started;
    // Exclusive of the rays it traced, which the trace stages already hold.
    add_stage_time(GiStage::ReflectionResolve,
                   static_cast<u64>(std::max<i64>(spent - static_cast<i64>(traced), 0)));
    return result;
}

u32 IlluminationSystem::advance_to_convergence(const FrameContext& context, f32 threshold,
                                               u32 max_frames) noexcept {
    FrameContext local = context;
    local.converged_mode = true;
    for (u32 index = 0; index < max_frames; ++index) {
        local.frame = context.frame + index;
        const IlluminationFrameReport report = update(local);
        if (report.convergence >= threshold && convergence_.region_count() != 0) {
            return index + 1;
        }
    }
    return max_frames;
}

}  // namespace cy::rendering::gi
