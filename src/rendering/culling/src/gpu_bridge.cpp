#include <cy/rendering/culling/gpu_bridge.h>

#include <algorithm>

namespace cy::rendering {
namespace {

using render::culling::GpuCullCounters;
using render::culling::GpuCullView;
using render::culling::GpuDrawPayload;
using render::culling::GpuFadeMode;
using render::culling::GpuVisibilityRange;
using render::culling::kGpuCullVisibilityRanges;
using render::culling::kNoLodFade;
using render::culling::pack_visibility_parent;

/// The spatial flags and the GPU scene flags are two spellings of one set of facts, and this is the
/// one place that translates. A second translation somewhere else is how the two drift.
[[nodiscard]] u32 gpu_flags_of(u32 spatial_flags) noexcept {
    u32 flags = 0;
    if ((spatial_flags & kSpatialActive) != 0U) {
        flags |= render::kInstanceActive;
    }
    if ((spatial_flags & kSpatialVisible) != 0U) {
        flags |= render::kInstanceVisible;
    }
    if ((spatial_flags & kSpatialCastsShadow) != 0U) {
        flags |= render::kInstanceCastsShadow;
    }
    if ((spatial_flags & kSpatialMoved) != 0U) {
        flags |= render::kInstanceMoved;
    }
    return flags;
}

}  // namespace

// --- The publication --------------------------------------------------------------------------

GpuCullPublication::GpuCullPublication(Allocator& allocator) noexcept
    : instances(allocator), ranges(allocator), spatial_slots(allocator) {}

void GpuCullPublication::clear() noexcept {
    instances.clear();
    ranges.clear();
    spatial_slots.clear();
    high_water = 0;
    any_ranges = false;
}

Status publish_gpu_cull_scene(const SpatialIndex& index, GpuCullPublication& out) noexcept {
    out.clear();

    const Span<const SpatialEntry> entries = index.entries();
    const Span<const u32> flags = index.flags();
    const Span<const Aabb> bounds = index.bounds();

    u32 high_water = 0;
    for (u32 slot = 0; slot < index.slot_count(); ++slot) {
        if ((flags[slot] & kSpatialActive) == 0U ||
            entries[slot].domain != SpatialDomain::Renderable) {
            continue;
        }
        high_water = std::max(entries[slot].gpu_slot + 1U, high_water);
    }
    if (high_water == 0) {
        return ok();
    }

    if (Status sized = out.instances.resize(high_water); !sized) {
        return sized;
    }
    if (Status sized = out.spatial_slots.resize(high_water); !sized) {
        return sized;
    }
    for (u32 gpu_slot = 0; gpu_slot < high_water; ++gpu_slot) {
        out.instances[gpu_slot] = render::GpuInstance{};
        out.spatial_slots[gpu_slot] = kNoSpatialSlot;
    }

    // Ranges are sized only when something declares one, because an EMPTY span is what tells the
    // cull that visibility ranges are not in play at all. A span of zeroed records says the
    // opposite — every instance declares "always visible with no parent" — and the two produce
    // different counters.
    bool any_ranges = false;
    for (u32 slot = 0; slot < index.slot_count(); ++slot) {
        if ((flags[slot] & kSpatialActive) == 0U ||
            entries[slot].domain != SpatialDomain::Renderable) {
            continue;
        }
        any_ranges = any_ranges || entries[slot].max_draw_distance > 0.0F;
    }
    if (any_ranges) {
        if (Status sized = out.ranges.resize(high_water); !sized) {
            return sized;
        }
        for (u32 gpu_slot = 0; gpu_slot < high_water; ++gpu_slot) {
            out.ranges[gpu_slot] = GpuVisibilityRange{};
            out.ranges[gpu_slot].mode_and_parent =
                pack_visibility_parent(GpuFadeMode::None, render::culling::kNoVisibilityParent);
        }
    }

    for (u32 slot = 0; slot < index.slot_count(); ++slot) {
        if ((flags[slot] & kSpatialActive) == 0U ||
            entries[slot].domain != SpatialDomain::Renderable) {
            continue;
        }
        const SpatialEntry& entry = entries[slot];
        const u32 gpu_slot = entry.gpu_slot;
        render::GpuInstance& instance = out.instances[gpu_slot];

        const Vec3 centre = bounds[slot].center();
        instance.bounds_center[0] = centre.x;
        instance.bounds_center[1] = centre.y;
        instance.bounds_center[2] = centre.z;
        instance.bounds_radius = entry.radius;
        instance.previous_center[0] = centre.x;
        instance.previous_center[1] = centre.y;
        instance.previous_center[2] = centre.z;
        instance.previous_radius = entry.radius;
        instance.layer_mask = index.layer_masks()[slot];
        instance.flags = gpu_flags_of(flags[slot]);
        instance.lod_chain = entry.lod_chain;
        instance.importance = entry.importance;
        instance.set_stable_id(entry.stable_id);
        out.spatial_slots[gpu_slot] = slot;

        if (any_ranges && entry.max_draw_distance > 0.0F) {
            out.ranges[gpu_slot].end = entry.max_draw_distance;
        }
    }

    out.high_water = high_water;
    out.any_ranges = any_ranges;
    return ok();
}

// --- The view ----------------------------------------------------------------------------------

void write_gpu_cull_view(const CullView& view, u32 instance_count, GpuCullView& out) noexcept {
    render::culling::write_frustum(out, view.frustum);
    out.camera_position[0] = view.camera_position.x;
    out.camera_position[1] = view.camera_position.y;
    out.camera_position[2] = view.camera_position.z;
    out.camera_forward[0] = view.camera_forward.x;
    out.camera_forward[1] = view.camera_forward.y;
    out.camera_forward[2] = view.camera_forward.z;
    render::culling::write_field_of_view(out, view.fov_y_radians);
    out.layer_mask = view.layer_mask;
    out.max_distance = view.max_distance;
    out.instance_count = instance_count;
    // The two biases are summed here rather than on the device, because a shader adding two numbers
    // per instance is two numbers it did not need — `GpuCullView::lod_bias` documents itself as
    // already summed.
    out.lod_bias = view.lod.global_bias + view.lod.view_bias;
    out.lod_hysteresis = view.lod.hysteresis;
    out.lod_cross_fade_band = view.lod.cross_fade_band;
    out.ortho_height = view.orthographic ? view.ortho_height : 0.0F;
    out.flags = kGpuCullVisibilityRanges;
}

// --- The results
// ----------------------------------------------------------------------------------

Status apply_gpu_cull(const SpatialIndex& index, const GpuCullPublication& published,
                      Span<const GpuDrawPayload> payloads, const GpuCullCounters& counters,
                      CullResults& results) noexcept {
    results.clear();

    for (const GpuDrawPayload& payload : payloads) {
        if (payload.instance_slot >= published.spatial_slots.size()) {
            return fail(ErrorCode::OutOfRange,
                        "a culling payload named a GPU slot outside the publication; the dispatch "
                        "and the publication were built from different frames");
        }
        const u32 slot = published.spatial_slots[payload.instance_slot];
        if (slot == kNoSpatialSlot) {
            // A GPU slot this index did not publish. Not an error — a renderer with two publishers
            // has two of them filling one array — but not this cull's to route either.
            continue;
        }
        const SpatialEntry& entry = index.entry(slot);

        VisibleInstance visible;
        visible.slot = slot;
        visible.gpu_slot = payload.instance_slot;
        visible.stable_id = entry.stable_id;
        visible.view_depth = payload.view_depth;
        visible.coverage = payload.coverage;
        visible.lod_level = payload.lod_level;
        // The two modules spell "no fade" differently and this is the one place that translates:
        // `kNoLodFade` on the shader-visible record, `kInvalidLod` on the CPU one. Both are ~0U,
        // and relying on that rather than saying it is how they stop being both.
        visible.lod_fade_to = payload.lod_fade_to == kNoLodFade ? kInvalidLod : payload.lod_fade_to;
        visible.lod_fade = payload.lod_fade;
        visible.importance = entry.importance;

        const u32 flags = index.flags()[slot];
        Array<VisibleInstance>& list =
            (flags & kSpatialTransparent) != 0U ? results.transparent : results.opaque;
        if (Status pushed = list.push_back(visible); !pushed) {
            return pushed;
        }
        if ((flags & kSpatialMoved) != 0U) {
            if (Status pushed = results.motion.push_back(visible); !pushed) {
                return pushed;
            }
        }
    }

    // The diagnostics are the dispatch's, verbatim. `rendering-culling-and-lod` requires the GPU
    // result to be "read back with one frame of latency, documented as such" — the latency is the
    // caller's arrangement, and what arrives is these counters unmodified. Copying them rather than
    // recomputing them is the point: a second count would be a second answer.
    results.stats.tested = counters.tested;
    results.stats.rejected_by_layer = counters.rejected_by_layer;
    results.stats.rejected_by_frustum = counters.rejected_by_frustum;
    results.stats.rejected_by_occlusion = counters.rejected_by_occlusion;
    results.stats.rejected_by_range = counters.rejected_by_range;
    results.stats.visible = counters.visible;
    for (u32 level = 0; level < 8; ++level) {
        results.stats.lod_histogram[level] = counters.lod_histogram[level];
    }
    results.stats.partitions = 1;
    return ok();
}

}  // namespace cy::rendering
