#include <cy/rendering/culling/cull.h>

#include <cy/core/base/assert.h>
#include <cy/core/jobs/context.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/math/scalar.h>

#include <chrono>
#include <new>

namespace cy::rendering {
namespace {

/// Everything one partition needs. Copied into no task record — the parallel body is handed a
/// pointer to one of these, which outlives the loop because `cull_view` waits for it.
struct CullContext {
    const SpatialIndex* index = nullptr;
    const CullView* view = nullptr;
    CullWorkspace* workspace = nullptr;
    u64 count = 0;
    u64 chunk = 0;
};

/// Which typed list a survivor belongs in, from the flags word the broad phase already loaded.
/// Derived at merge time rather than carried on the record: it is one bit test, and a field would
/// be eight bytes per survivor to hold what the caller can recompute for free.
[[nodiscard]] Array<VisibleInstance>& list_for(CullResults& results, const SpatialEntry& entry,
                                               u32 flags) noexcept {
    if (entry.domain == SpatialDomain::Volume) {
        switch (entry.volume_kind) {
            case VolumeKind::Light:
                return results.lights;
            case VolumeKind::Decal:
                return results.decals;
            case VolumeKind::ReflectionProbe:
                return results.reflection_probes;
            case VolumeKind::GiVolume:
            case VolumeKind::FogVolume:
                return results.gi_volumes;
            case VolumeKind::None:
            case VolumeKind::Count:
                break;
        }
        return results.gi_volumes;
    }
    return (flags & kSpatialTransparent) != 0U ? results.transparent : results.opaque;
}

/// The three tests, in the order the requirement fixes them: layer, then frustum, then distance.
/// Returns false and counts the rejection; returns true with `out` filled in otherwise.
[[nodiscard]] bool test_slot(const CullContext& context, u32 slot, CullStatistics& stats,
                             VisibleInstance& out) noexcept {
    const SpatialIndex& index = *context.index;
    const CullView& view = *context.view;
    const u32 flags = index.flags()[slot];
    if ((flags & (kSpatialActive | kSpatialVisible)) != (kSpatialActive | kSpatialVisible)) {
        return false;
    }
    ++stats.tested;

    // 1. Layer. One AND over a word the loop already holds.
    if ((index.layer_masks()[slot] & view.layer_mask) == 0U) {
        ++stats.rejected_by_layer;
        return false;
    }

    // 2. Frustum, against the TIGHT bounds. The tree's fat bounds are an accelerator; this is the
    // answer.
    const Aabb& bounds = index.bounds()[slot];
    if (!view.frustum.intersects(bounds)) {
        ++stats.rejected_by_frustum;
        return false;
    }

    // 3. Distance limits, per instance and per view, measured along the view direction.
    const SpatialEntry& entry = index.entries()[slot];
    const f32 depth = dot(bounds.center() - view.camera_position, view.camera_forward);
    const f32 instance_limit = entry.max_draw_distance;
    const f32 view_limit = view.max_distance;
    const bool over_instance_limit = instance_limit > 0.0F && depth > instance_limit;
    const bool over_view_limit = view_limit > 0.0F && depth > view_limit;
    if (over_instance_limit || over_view_limit) {
        ++stats.rejected_by_range;
        return false;
    }

    out.slot = slot;
    out.gpu_slot = entry.gpu_slot;
    out.stable_id = entry.stable_id;
    out.view_depth = math::max(depth, 0.0F);
    out.coverage = view.orthographic
                       ? screen_coverage_orthographic(entry.radius, view.ortho_height)
                       : screen_coverage(entry.radius, out.view_depth, view.fov_y_radians);
    out.importance = entry.importance;
    return true;
}

/// Cull one half-open slot range into one partition list.
void cull_range(const CullContext& context, u64 begin, u64 end, Array<VisibleInstance>& out,
                CullStatistics& stats) noexcept {
    for (u64 slot = begin; slot < end; ++slot) {
        VisibleInstance visible;
        if (!test_slot(context, static_cast<u32>(slot), stats, visible)) {
            continue;
        }
        // A push that fails is a partition list that could not grow. Counting it as not visible is
        // the only honest answer: reporting it as visible would claim a draw nothing recorded.
        if (out.push_back(visible)) {
            ++stats.visible;
        }
    }
}

void cull_partition(const jobs::TaskContext& /*task*/, u64 begin, u64 end, void* user) noexcept {
    auto* context = static_cast<CullContext*>(user);
    // The partition index, recomputed from the range. `JobSystem::partition_range` derives every
    // range as `index * chunk`, so this is exact — and it is what lets a worker find its own output
    // list without the loop having to carry an index.
    const u64 partition = context->chunk == 0 ? 0 : begin / context->chunk;
    const auto index = static_cast<u32>(partition);
    cull_range(*context, begin, end, context->workspace->partition(index),
               context->workspace->partition_statistics(index));
}

void accumulate(CullStatistics& into, const CullStatistics& from) noexcept {
    into.tested += from.tested;
    into.rejected_by_layer += from.rejected_by_layer;
    into.rejected_by_frustum += from.rejected_by_frustum;
    into.rejected_by_occlusion += from.rejected_by_occlusion;
    into.rejected_by_range += from.rejected_by_range;
    into.visible += from.visible;
}

/// Route one partition's survivors into the typed lists, selecting each one's LOD as it lands.
/// Done here rather than in the worker because LOD selection reads the caller's chain table, which
/// a worker would need a second pointer to reach for no benefit — the survivors are a fraction of
/// the instances, and this loop is over survivors.
void merge_partition(const SpatialIndex& index, Span<const VisibleInstance> partition,
                     CullResults& results) noexcept {
    for (const VisibleInstance& visible : partition) {
        const u32 flags = index.flags()[visible.slot];
        const SpatialEntry& entry = index.entries()[visible.slot];
        Array<VisibleInstance>& list = list_for(results, entry, flags);
        if (!list.push_back(visible)) {
            continue;
        }
        if ((flags & kSpatialMoved) != 0U && entry.domain == SpatialDomain::Renderable) {
            (void)results.motion.push_back(visible);
        }
    }
}

/// Apply one selection to a list in place, and count it in the histogram.
void apply_lods(const SpatialIndex& index, const CullView& view, LodChainFn chain_of, void* user,
                Span<u32> previous_levels, Span<VisibleInstance> list, u32* histogram) noexcept {
    for (VisibleInstance& record : list) {
        const SpatialEntry& entry = index.entries()[record.slot];
        const Span<const render::MeshLod> chain = chain_of(entry.lod_chain, user);
        const u32 previous =
            record.slot < previous_levels.size() ? previous_levels[record.slot] : kInvalidLod;
        const LodSelection selection =
            select_lod(chain, record.coverage, entry.lod_bias, view.lod, previous);
        record.lod_level = selection.level;
        record.lod_fade_to = selection.fade_to;
        record.lod_fade = selection.fade;
        if (record.slot < previous_levels.size()) {
            previous_levels[record.slot] = selection.level;
        }
        if (histogram != nullptr) {
            ++histogram[math::min<u32>(selection.level, 7U)];
        }
    }
}

}  // namespace

// --- CullResults ------------------------------------------------------------------------------

CullResults::CullResults(Allocator& allocator) noexcept
    : opaque(allocator),
      transparent(allocator),
      motion(allocator),
      lights(allocator),
      decals(allocator),
      reflection_probes(allocator),
      gi_volumes(allocator) {}

void CullResults::clear() noexcept {
    opaque.clear();
    transparent.clear();
    motion.clear();
    lights.clear();
    decals.clear();
    reflection_probes.clear();
    gi_volumes.clear();
    stats = CullStatistics{};
}

// --- CullWorkspace ----------------------------------------------------------------------------

CullWorkspace::CullWorkspace(Allocator& allocator) noexcept
    : allocator_(&allocator), partitions_(allocator), partition_stats_(allocator) {}

Status CullWorkspace::prepare(u32 partitions) noexcept {
    while (partitions_.size() < partitions) {
        void* memory =
            allocator_->allocate(sizeof(Array<VisibleInstance>), alignof(Array<VisibleInstance>));
        if (memory == nullptr) {
            return fail(ErrorCode::OutOfMemory, "cull workspace: partition list");
        }
        auto* list = new (memory) Array<VisibleInstance>(*allocator_);
        if (Status pushed = partitions_.push_back(list); !pushed) {
            list->~Array();
            allocator_->deallocate(memory, sizeof(Array<VisibleInstance>),
                                   alignof(Array<VisibleInstance>));
            return pushed;
        }
    }
    if (Status sized = partition_stats_.resize(partitions_.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < partitions_.size(); ++index) {
        partitions_[index]->clear();
        partition_stats_[index] = CullStatistics{};
    }
    return ok();
}

CullStatistics& CullWorkspace::partition_statistics(u32 index) noexcept {
    CY_ASSERT_MSG(index < partition_stats_.size(), "cull partition statistics out of range");
    return partition_stats_[index];
}

Array<VisibleInstance>& CullWorkspace::partition(u32 index) noexcept {
    CY_ASSERT_MSG(index < partitions_.size(), "cull partition out of range");
    return *partitions_[index];
}

CullWorkspace::~CullWorkspace() {
    for (Array<VisibleInstance>* partition : partitions_.span()) {
        partition->~Array();
        allocator_->deallocate(partition, sizeof(Array<VisibleInstance>),
                               alignof(Array<VisibleInstance>));
    }
}

// --- The cull ---------------------------------------------------------------------------------

Status cull_view(const SpatialIndex& index, const CullView& view, const CullOptions& options,
                 CullWorkspace& workspace, CullResults& results) noexcept {
    const auto started = std::chrono::steady_clock::now();
    results.clear();

    const u64 count = index.slot_count();
    const bool parallel = options.jobs != nullptr && count >= options.parallel_threshold;
    const u64 grain = options.grain == 0 ? 1 : options.grain;
    const u64 partitions =
        parallel ? math::max<u64>(jobs::JobSystem::partition_count(count, grain), 1) : 1;

    if (Status prepared = workspace.prepare(static_cast<u32>(partitions)); !prepared) {
        return prepared;
    }

    CullContext context;
    context.index = &index;
    context.view = &view;
    context.workspace = &workspace;
    context.count = count;
    context.chunk = partitions == 0 ? 0 : (count + partitions - 1) / partitions;

    if (parallel) {
        Expected<jobs::JobHandle, Error> handle =
            options.jobs->submit_parallel_for(count, grain, &cull_partition, &context, "cull.view");
        if (!handle.has_value()) {
            return make_unexpected(handle.error());
        }
        options.jobs->wait(*handle);
    } else if (count > 0) {
        cull_range(context, 0, count, workspace.partition(0), workspace.partition_statistics(0));
    }

    // The merge, IN PARTITION ORDER. See the header comment: the order is a property of the
    // partitioning, never of which worker finished first.
    for (u64 partition = 0; partition < partitions; ++partition) {
        const auto slot = static_cast<u32>(partition);
        accumulate(results.stats, workspace.partition_statistics(slot));
        merge_partition(index, workspace.partition(slot).span(), results);
    }
    results.stats.partitions = static_cast<u32>(partitions);
    results.stats.wall_nanoseconds =
        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count());
    return ok();
}

Status select_lods(const SpatialIndex& index, const CullView& view, LodChainFn chain_of, void* user,
                   Span<u32> previous_levels, CullResults& results) noexcept {
    if (chain_of == nullptr) {
        return fail(ErrorCode::InvalidArgument, "select_lods: a chain callback is required");
    }
    for (u32& bucket : results.stats.lod_histogram) {
        bucket = 0;
    }
    // The histogram counts each survivor once, so it is filled from the two lists that partition
    // the renderables and not from `motion`, whose entries are already in one of them.
    apply_lods(index, view, chain_of, user, previous_levels, results.opaque.span(),
               results.stats.lod_histogram);
    apply_lods(index, view, chain_of, user, previous_levels, results.transparent.span(),
               results.stats.lod_histogram);
    apply_lods(index, view, chain_of, user, previous_levels, results.motion.span(), nullptr);
    return ok();
}

// --- Shadow caster culling ---------------------------------------------------------------------

bool casts_into_view(const Aabb& caster, const Frustum& camera_frustum, Vec3 light_direction,
                     f32 sweep_distance) noexcept {
    if (sweep_distance <= 0.0F) {
        return camera_frustum.intersects(caster);
    }
    // The union of the box and the same box pushed along the light. Conservative: the real swept
    // volume is contained in it, so the only mistake it can make is keeping a caster that could
    // have been dropped.
    Aabb swept = caster;
    swept.grow(Aabb::from_min_max(caster.min + (light_direction * sweep_distance),
                                  caster.max + (light_direction * sweep_distance)));
    return camera_frustum.intersects(swept);
}

Status cull_shadow_casters(const SpatialIndex& index, const ShadowCullView& view,
                           Array<VisibleInstance>& casters, ShadowCullStatistics& stats) noexcept {
    casters.clear();
    stats = ShadowCullStatistics{};

    const u32 count = index.slot_count();
    for (u32 slot = 0; slot < count; ++slot) {
        const u32 flags = index.flags()[slot];
        const u32 required = kSpatialActive | kSpatialVisible | kSpatialCastsShadow;
        if ((flags & required) != required) {
            continue;
        }
        const SpatialEntry& entry = index.entries()[slot];
        if (entry.domain != SpatialDomain::Renderable) {
            continue;
        }
        ++stats.tested;
        if ((index.layer_masks()[slot] & view.layer_mask) == 0U) {
            ++stats.rejected_by_layer;
            continue;
        }
        const Aabb& bounds = index.bounds()[slot];
        if (!view.shadow_frustum.intersects(bounds)) {
            ++stats.rejected_by_frustum;
            continue;
        }
        if (view.tight && !casts_into_view(bounds, view.camera_frustum, view.light_direction,
                                           view.sweep_distance)) {
            ++stats.rejected_by_sweep;
            continue;
        }
        VisibleInstance caster;
        caster.slot = slot;
        caster.gpu_slot = entry.gpu_slot;
        caster.stable_id = entry.stable_id;
        caster.importance = entry.importance;
        if (Status pushed = casters.push_back(caster); !pushed) {
            return pushed;
        }
        ++stats.casters;
    }
    return ok();
}

}  // namespace cy::rendering
