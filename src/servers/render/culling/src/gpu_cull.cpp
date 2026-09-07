#include <cy/servers/render/culling/gpu_cull.h>

#include <cmath>

namespace cy::render::culling {
namespace {

[[nodiscard]] Vec3 centre_of(const GpuInstance& instance) noexcept {
    return Vec3{instance.bounds_center[0], instance.bounds_center[1], instance.bounds_center[2]};
}

/// The level a chain selects, with the hysteresis band applied against the level chosen last frame.
///
/// The band is asymmetric on purpose: coverage must fall BELOW `threshold * (1 - hysteresis)` to
/// coarsen and rise ABOVE `threshold` to refine, so an instance sitting exactly on a threshold does
/// not flip level every frame as the camera breathes.
struct LodPick {
    u32 level = 0;
    u32 fade_to = kNoLodFade;
    f32 fade = 0.0F;
};

[[nodiscard]] LodPick select_level(Span<const GpuMeshLod> chain, f32 coverage, f32 bias,
                                   const GpuCullView& view, u32 previous) noexcept {
    LodPick pick;
    if (chain.empty()) {
        return pick;
    }
    const f32 adjusted = coverage * std::exp2(bias + view.lod_bias);

    u32 level = static_cast<u32>(chain.size()) - 1U;
    for (u32 index = 0; index < chain.size(); ++index) {
        f32 threshold = chain[index].screen_coverage_threshold;
        // Coarsening past the level chosen last frame costs the band; refining does not.
        if (previous != kNoLodFade && index > previous) {
            threshold *= 1.0F - view.lod_hysteresis;
        }
        if (adjusted >= threshold) {
            level = index;
            break;
        }
    }
    pick.level = level;

    // The cross-fade band: inside it both levels are drawn with complementary dither masks and
    // temporal antialiasing resolves the blend.
    if (view.lod_cross_fade_band > 0.0F && level + 1U < chain.size()) {
        const f32 threshold = chain[level].screen_coverage_threshold;
        const f32 band = threshold * view.lod_cross_fade_band;
        if (band > 0.0F && adjusted < threshold + band && adjusted >= threshold) {
            pick.fade_to = level + 1U;
            pick.fade = 1.0F - ((adjusted - threshold) / band);
        }
    }
    return pick;
}

/// Resolve one instance's place in its visibility hierarchy, walking to the root.
///
/// Returns the alpha the instance is drawn at, and 0 when a parent has taken over. The walk is
/// bounded by the slot count rather than by a visited set: a cycle is a content error the importer
/// should have caught, and a compute thread cannot allocate one anyway. Hitting the bound answers
/// "not visible", which drops the branch rather than hanging the dispatch.
[[nodiscard]] f32 resolve_visibility(const GpuCullScene& scene, const GpuCullView& view, u32 slot,
                                     f32 distance) noexcept {
    if (scene.ranges.empty() || slot >= scene.ranges.size()) {
        return 1.0F;
    }
    const f32 own = visibility_range_alpha(scene.ranges[slot], distance);
    if (own <= 0.0F) {
        return 0.0F;
    }

    // A parent's visibility REPLACES its children's, so an instance whose parent is itself visible
    // is not drawn — except during a `Dependents` cross-fade, where both are, which is the HLOD
    // swap the requirement describes.
    u32 walk = visibility_parent_of(scene.ranges[slot].mode_and_parent);
    for (u32 step = 0; step < scene.ranges.size() && walk != kNoVisibilityParent; ++step) {
        if (walk >= scene.ranges.size()) {
            return own;
        }
        const GpuVisibilityRange& parent = scene.ranges[walk];
        const Vec3 parent_centre = walk < scene.instances.size() ? centre_of(scene.instances[walk])
                                                                 : Vec3{0.0F, 0.0F, 0.0F};
        const Vec3 eye{view.camera_position[0], view.camera_position[1], view.camera_position[2]};
        const f32 parent_alpha = visibility_range_alpha(parent, length(parent_centre - eye));
        if (parent_alpha >= 1.0F) {
            return 0.0F;
        }
        if (parent_alpha > 0.0F &&
            fade_mode_of(parent.mode_and_parent) == GpuFadeMode::Dependents) {
            return own * (1.0F - parent_alpha);
        }
        if (parent_alpha > 0.0F) {
            return 0.0F;
        }
        walk = visibility_parent_of(parent.mode_and_parent);
    }
    return own;
}

/// Whether a caster inside the light's volume can cast into the camera's frustum.
///
/// The swept volume is approximated by the union of the caster's sphere and the same sphere
/// displaced along the light direction — conservative, cheap, and exact enough that the only thing
/// it can do wrong is keep a caster that could have been dropped.
[[nodiscard]] bool casts_into_view(const Frustum& frustum, Vec3 centre, f32 radius,
                                   Vec3 light_direction, f32 sweep) noexcept {
    const Sphere here{centre, radius};
    if (frustum.intersects(here)) {
        return true;
    }
    const Sphere swept{centre + (light_direction * sweep), radius};
    return frustum.intersects(swept);
}

}  // namespace

// --- The view -----------------------------------------------------------------------------------

void write_frustum(GpuCullView& view, const Frustum& frustum) noexcept {
    for (u32 index = 0; index < Frustum::kCount; ++index) {
        view.planes[index][0] = frustum.planes[index].normal.x;
        view.planes[index][1] = frustum.planes[index].normal.y;
        view.planes[index][2] = frustum.planes[index].normal.z;
        view.planes[index][3] = frustum.planes[index].d;
    }
}

void write_camera_frustum(GpuCullView& view, const Frustum& frustum) noexcept {
    for (u32 index = 0; index < Frustum::kCount; ++index) {
        view.camera_planes[index][0] = frustum.planes[index].normal.x;
        view.camera_planes[index][1] = frustum.planes[index].normal.y;
        view.camera_planes[index][2] = frustum.planes[index].normal.z;
        view.camera_planes[index][3] = frustum.planes[index].d;
    }
}

void write_field_of_view(GpuCullView& view, f32 fov_y_radians) noexcept {
    const f32 half = fov_y_radians * 0.5F;
    const f32 tangent = std::tan(half);
    view.inverse_tan_half_fov = tangent > 1.0e-6F ? 1.0F / tangent : 1.0e6F;
}

f32 screen_coverage(f32 radius, f32 view_depth, f32 inverse_tan_half_fov) noexcept {
    if (!(view_depth > 0.0F)) {
        return 1.0F;
    }
    return (radius * inverse_tan_half_fov) / view_depth;
}

f32 visibility_range_alpha(const GpuVisibilityRange& range, f32 distance) noexcept {
    const bool bounded = range.end > 0.0F;
    if (distance < range.begin) {
        if (range.fade_margin <= 0.0F) {
            return 0.0F;
        }
        const f32 into = (distance - (range.begin - range.fade_margin)) / range.fade_margin;
        if (into <= 0.0F) {
            return 0.0F;
        }
        return into >= 1.0F ? 1.0F : into;
    }
    if (bounded && distance > range.end) {
        if (range.fade_margin <= 0.0F) {
            return 0.0F;
        }
        const f32 out = ((range.end + range.fade_margin) - distance) / range.fade_margin;
        if (out <= 0.0F) {
            return 0.0F;
        }
        return out >= 1.0F ? 1.0F : out;
    }
    return 1.0F;
}

// --- The output ---------------------------------------------------------------------------------

GpuCullOutput::GpuCullOutput(Allocator& allocator) noexcept
    : commands_(allocator), payloads_(allocator), virtual_geometry_(allocator) {}

Status GpuCullOutput::reserve(u32 capacity) noexcept {
    if (capacity <= capacity_) {
        return ok();
    }
    if (Status reserved = commands_.reserve(capacity); !reserved) {
        return reserved;
    }
    if (Status reserved = payloads_.reserve(capacity); !reserved) {
        return reserved;
    }
    if (Status reserved = virtual_geometry_.reserve(capacity); !reserved) {
        return reserved;
    }
    capacity_ = capacity;
    return ok();
}

void GpuCullOutput::clear() noexcept {
    commands_.clear();
    payloads_.clear();
    virtual_geometry_.clear();
    counters_ = GpuCullCounters{};
}

Span<const GpuDrawIndexedIndirect> GpuCullOutput::commands() const noexcept {
    return commands_.span();
}

Span<const GpuDrawPayload> GpuCullOutput::payloads() const noexcept {
    return payloads_.span();
}

Span<const u32> GpuCullOutput::virtual_geometry() const noexcept {
    return virtual_geometry_.span();
}

Status GpuCullOutput::emit(const GpuDrawIndexedIndirect& command,
                           const GpuDrawPayload& payload) noexcept {
    if (commands_.size() >= capacity_) {
        return fail(ErrorCode::OutOfRange,
                    "the culling output is full; reserve it for the scene's high water mark, "
                    "because on the device this buffer cannot grow");
    }
    if (Status pushed = commands_.push_back(command); !pushed) {
        return pushed;
    }
    if (Status pushed = payloads_.push_back(payload); !pushed) {
        return pushed;
    }
    ++counters_.draws;
    return ok();
}

Status GpuCullOutput::emit_virtual_geometry(u32 instance_slot) noexcept {
    if (virtual_geometry_.size() >= capacity_) {
        return fail(ErrorCode::OutOfRange, "the culling output's cluster list is full");
    }
    if (Status pushed = virtual_geometry_.push_back(instance_slot); !pushed) {
        return pushed;
    }
    ++counters_.virtual_geometry;
    return ok();
}

// --- The reference cull -------------------------------------------------------------------------

namespace {

/// Everything one dispatch reads that does not change per instance, unpacked once.
///
/// A struct rather than eleven arguments, because the per-instance function below takes all of it
/// and a compute shader would read the same block from a constant buffer.
struct DispatchState {
    Frustum frustum;
    Frustum camera_frustum;
    Vec3 eye{0.0F, 0.0F, 0.0F};
    Vec3 forward{0.0F, 0.0F, -1.0F};
    Vec3 light{0.0F, -1.0F, 0.0F};
    bool occlusion_on = false;
    bool ranges_on = false;
    bool shadow_view = false;
    bool moved_only = false;
};

[[nodiscard]] DispatchState unpack(const GpuCullView& view,
                                   const GpuCullOptions& options) noexcept {
    DispatchState state;
    for (u32 index = 0; index < Frustum::kCount; ++index) {
        state.frustum.planes[index].normal =
            Vec3{view.planes[index][0], view.planes[index][1], view.planes[index][2]};
        state.frustum.planes[index].d = view.planes[index][3];
        state.camera_frustum.planes[index].normal =
            Vec3{view.camera_planes[index][0], view.camera_planes[index][1],
                 view.camera_planes[index][2]};
        state.camera_frustum.planes[index].d = view.camera_planes[index][3];
    }
    state.frustum.refresh_corner_masks();
    state.camera_frustum.refresh_corner_masks();
    state.eye = Vec3{view.camera_position[0], view.camera_position[1], view.camera_position[2]};
    state.forward = Vec3{view.camera_forward[0], view.camera_forward[1], view.camera_forward[2]};
    state.light = Vec3{view.light_direction[0], view.light_direction[1], view.light_direction[2]};
    state.occlusion_on = (view.flags & kGpuCullOcclusion) != 0U && options.occlusion != nullptr;
    state.ranges_on = (view.flags & kGpuCullVisibilityRanges) != 0U;
    state.shadow_view = (view.flags & kGpuCullShadowCasters) != 0U;
    state.moved_only = (view.flags & kGpuCullMovedOnly) != 0U;
    return state;
}

/// Why an instance did not survive, or that it did. The counter each answer increments is the
/// diagnostic `rendering-culling-and-lod` asks for, so the enumeration and the counter block are
/// one list read two ways.
enum class Verdict : u8 { Skipped, Layer, Frustum, Range, Occlusion, Visible };

/// What survives the tests, and what the tests computed on the way.
struct Survivor {
    Verdict verdict = Verdict::Skipped;
    f32 view_depth = 0.0F;
    f32 alpha = 1.0F;
};

/// The three required tests in their required order, then visibility ranges, then occlusion.
///
/// THE ORDER IS THE REQUIREMENT, not an optimisation: "layer mask against the view's mask, then
/// conservative frustum-versus-AABB using precomputed plane sign masks, then optional per-instance
/// distance limits". A layer test is one AND over a word already loaded; a frustum test is six dot
/// products, and running them the other way round would cost six times as much on the instances a
/// view does not draw.
[[nodiscard]] Survivor test_instance(const GpuCullScene& scene, const GpuCullView& view,
                                     const DispatchState& state, const GpuCullOptions& options,
                                     u32 slot) noexcept {
    Survivor result;
    const GpuInstance& instance = scene.instances[slot];
    if (!instance.active() || (instance.flags & kInstanceVisible) == 0U) {
        return result;
    }
    if (state.shadow_view && (instance.flags & kInstanceCastsShadow) == 0U) {
        return result;
    }
    if (state.moved_only && (instance.flags & kInstanceMoved) == 0U) {
        return result;
    }

    // 1. Layer.
    if ((instance.layer_mask & view.layer_mask) == 0U) {
        result.verdict = Verdict::Layer;
        return result;
    }

    // 2. Geometry.
    const Vec3 centre = centre_of(instance);
    const f32 radius = instance.bounds_radius;
    if (!state.frustum.intersects(Sphere{centre, radius})) {
        result.verdict = Verdict::Frustum;
        return result;
    }
    // "Casters SHALL additionally be rejected when they cannot cast into the camera frustum." A
    // sweep distance of zero disables the tighter test, which is what a light whose shadow serves
    // several camera views must do.
    if (state.shadow_view && view.sweep_distance > 0.0F &&
        !casts_into_view(state.camera_frustum, centre, radius, state.light, view.sweep_distance)) {
        result.verdict = Verdict::Frustum;
        return result;
    }

    // 3. Distance, view-wide and per instance, the smaller winning.
    const f32 distance = length(centre - state.eye);
    if (view.max_distance > 0.0F && distance - radius > view.max_distance) {
        result.verdict = Verdict::Range;
        return result;
    }

    // 4. Visibility ranges and the HLOD hierarchy.
    if (state.ranges_on) {
        result.alpha = resolve_visibility(scene, view, slot, distance);
        if (!(result.alpha > 0.0F)) {
            result.verdict = Verdict::Range;
            return result;
        }
    }

    // 5. Occlusion. Last, because it is the most expensive test.
    if (state.occlusion_on && options.occlusion->occluded(centre, radius)) {
        result.verdict = Verdict::Occlusion;
        return result;
    }

    result.view_depth = dot(centre - state.eye, state.forward);
    result.verdict = Verdict::Visible;
    return result;
}

void count(GpuCullCounters& counters, Verdict verdict) noexcept {
    switch (verdict) {
        case Verdict::Layer:
            ++counters.rejected_by_layer;
            break;
        case Verdict::Frustum:
            ++counters.rejected_by_frustum;
            break;
        case Verdict::Range:
            ++counters.rejected_by_range;
            break;
        case Verdict::Occlusion:
            ++counters.rejected_by_occlusion;
            break;
        case Verdict::Visible:
            ++counters.visible;
            break;
        case Verdict::Skipped:
            break;
    }
}

/// Turn a survivor into a draw: select its level, remember it for the next frame's hysteresis, and
/// emit the chosen level's arguments.
[[nodiscard]] Status emit_draw(const GpuCullScene& scene, const GpuCullView& view, u32 slot,
                               const Survivor& survivor, GpuCullOutput& output) noexcept {
    const GpuInstance& instance = scene.instances[slot];
    const GpuLodChain chain =
        instance.lod_chain < scene.chains.size() ? scene.chains[instance.lod_chain] : GpuLodChain{};
    if (chain.count == 0 || chain.first + chain.count > scene.mesh_lods.size()) {
        return ok();
    }
    const Span<const GpuMeshLod> levels(scene.mesh_lods.data() + chain.first, chain.count);
    const f32 coverage = view.ortho_height > 0.0F
                             ? (instance.bounds_radius * 2.0F) / view.ortho_height
                             : screen_coverage(instance.bounds_radius, survivor.view_depth,
                                               view.inverse_tan_half_fov);
    const u32 previous =
        slot < scene.previous_levels.size() ? scene.previous_levels[slot] : kNoLodFade;
    const LodPick pick = select_level(levels, coverage, 0.0F, view, previous);
    if (slot < scene.previous_levels.size()) {
        scene.previous_levels[slot] = pick.level;
    }
    output.counters().lod_histogram[pick.level < 8U ? pick.level : 7U] += 1;

    const GpuMeshLod& level = levels[pick.level];
    GpuDrawIndexedIndirect command;
    command.index_count = level.index_count;
    command.instance_count = 1;
    command.first_index = level.first_index;
    command.vertex_offset = level.vertex_offset;
    command.first_instance = static_cast<u32>(output.commands().size());

    GpuDrawPayload payload;
    payload.instance_slot = slot;
    payload.material = level.material;
    payload.lod_level = pick.level;
    payload.lod_fade_to = pick.fade_to;
    payload.lod_fade = pick.fade;
    payload.alpha = survivor.alpha;
    payload.view_depth = survivor.view_depth;
    payload.coverage = coverage;
    return output.emit(command, payload);
}

}  // namespace

Status cpu_reference_cull(const GpuCullScene& scene, const GpuCullView& view,
                          const GpuCullOptions& options, GpuCullOutput& output) noexcept {
    output.clear();
    const DispatchState state = unpack(view, options);
    const u32 count_of = view.instance_count < scene.instances.size()
                             ? view.instance_count
                             : static_cast<u32>(scene.instances.size());
    GpuCullCounters& counters = output.counters();

    for (u32 slot = 0; slot < count_of; ++slot) {
        const Survivor survivor = test_instance(scene, view, state, options, slot);
        if (survivor.verdict == Verdict::Skipped) {
            continue;
        }
        ++counters.tested;
        count(counters, survivor.verdict);
        if (survivor.verdict != Verdict::Visible) {
            continue;
        }

        // Virtual geometry does not emit an indexed draw: cluster traversal owns it. The list is
        // produced now so that M7 consumes something that already exists.
        if ((scene.instances[slot].flags & kInstanceVirtualGeometry) != 0U) {
            if (Status emitted = output.emit_virtual_geometry(slot); !emitted) {
                return emitted;
            }
            continue;
        }
        if (Status emitted = emit_draw(scene, view, slot, survivor, output); !emitted) {
            return emitted;
        }
    }
    return ok();
}

}  // namespace cy::render::culling
