// GPU-driven culling, as two compute entry points. M7 task 5.1.
//
// `rendering-culling-and-lod` — "GPU-driven culling": "instance bounds are uploaded once, culling
// runs as a compute pass producing compacted draw arguments, and drawing uses indirect commands."
//
// ================================================================================================
// THIS FILE IS THE SECOND IMPLEMENTATION OF ONE ALGORITHM, AND THAT IS THE POINT
// ================================================================================================
//
// `cy::render::culling::cpu_reference_cull` (src/servers/render/culling/src/gpu_cull.cpp) computes
// exactly what these two entry points compute. It exists so that this file can be checked against a
// buffer rather than against a screenshot, which is what `tests/test_gpu_cull_pass.cpp` does: it
// runs both over one scene and compares the emitted commands, the payloads, the counters, the
// virtual-geometry list and the LOD-hysteresis state word for word.
//
// So EVERY expression below is written in the order the reference writes it, including the ones a
// shader author would naturally reassociate. `screen_coverage` is `(radius * inverse_tan) / depth`
// and not `radius * (inverse_tan / depth)`; the frustum test is `!(distance < -radius)` and not
// `distance >= -radius`, because the two differ for a NaN and the reference's answer for a
// degenerate input is to keep the instance. A "simplification" here is a test failure there.
//
// ================================================================================================
// WHY TWO ENTRY POINTS AND NOT ONE
// ================================================================================================
//
// A one-pass cull appends survivors with an atomic counter, and the order the survivors land in is
// then whatever order the hardware happened to schedule the groups in. That is fine for a frame and
// useless as an expected value: the reference emits in ASCENDING SLOT ORDER and says so, and a
// comparison against a buffer whose order is a race is not a comparison.
//
//   cull_instances   one thread per GPU scene slot. Runs the tests, selects the level, writes a
//                    per-slot verdict and a per-slot draw record, and accumulates the counters —
//                    which are sums, and so are order-independent whatever the atomics do.
//   compact_draws    ONE workgroup. Walks the verdict array in ascending slot order in chunks of
//                    kCompactGroupSize, prefix-sums each chunk in shared memory, and scatters. The
//                    result is bit-for-bit the reference's array, and `first_instance` — which is
//                    the draw's own index — is therefore a number and not a lottery.
//
// The cost is one extra dispatch over a byte-per-slot array. What it buys is that "the shader agrees
// with the reference" is a memcmp.
//
// ================================================================================================
// WHAT IS NOT HERE
// ================================================================================================
//
// The occlusion test. `hzb.h` is a CPU model of a depth pyramid and there is no pyramid on a device
// yet; `GpuCullPass` refuses a view with `kGpuCullOcclusion` set rather than silently culling
// nothing, so the day the pyramid lands the omission is a compile-time task and not a wrong frame.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   slangc gpu_cull.slang -target spirv -profile spirv_1_5 \
//          -entry cull_instances -stage compute -o gpu_cull_instances.spv
//   slangc gpu_cull.slang -target spirv -profile spirv_1_5 \
//          -entry compact_draws  -stage compute -o gpu_cull_compact.spv
//   python3 embed_spirv.py gpu_cull_spirv.h \
//          kGpuCullInstancesSpirv=gpu_cull_instances.spv \
//          kGpuCullCompactSpirv=gpu_cull_compact.spv
//   clang-format -i gpu_cull_spirv.h

// --- The records, laid out to match the C++ exactly ---------------------------------------------
//
// Every one of these has a `static_assert` on its size in the C++ header that declares it. The
// float4 spelling is not decoration: it forces the same offsets under std430 that `alignas(16)`
// forces in C++, and a `float transform[12]` here would be legal and would be a different struct.

/// `cy::render::GpuInstance`, 160 bytes.
struct Instance {
    float4 transform[3];
    float4 previous_transform[3];
    /// xyz world-space bounding-sphere centre, w its radius.
    float4 bounds;
    float4 previous_bounds;
    /// x mesh, y material, z lod_chain, w layer_mask.
    uint4 mesh_material_chain_layer;
    uint flags;
    float importance;
    uint stable_id_low;
    uint stable_id_high;
};

/// `cy::render::culling::GpuMeshLod`, 32 bytes.
struct MeshLod {
    uint index_count;
    uint first_index;
    uint vertex_offset;
    uint material;
    float screen_coverage_threshold;
    float radius;
    uint reserved0;
    uint reserved1;
};

/// `cy::render::culling::GpuLodChain`, 8 bytes.
struct LodChain {
    uint first;
    uint count;
};

/// `cy::render::culling::GpuVisibilityRange`, 16 bytes.
struct VisibilityRange {
    float begin;
    float end;
    float fade_margin;
    uint mode_and_parent;
};

/// `cy::render::culling::GpuDrawIndexedIndirect`, 20 bytes. Fixed by the graphics APIs.
struct DrawIndexedIndirect {
    uint index_count;
    uint instance_count;
    uint first_index;
    uint vertex_offset;
    uint first_instance;
};

/// `cy::render::culling::GpuDrawPayload`, 32 bytes.
struct DrawPayload {
    uint instance_slot;
    uint material;
    uint lod_level;
    uint lod_fade_to;
    float lod_fade;
    float alpha;
    float view_depth;
    float coverage;
};

/// `cy::render::culling::GpuCullView`, 272 bytes, packed into float4 rows so that std140 and the
/// C++ struct agree without a single padding byte having to be reasoned about twice.
struct CullView {
    float4 planes[6];
    /// xyz camera_position, w inverse_tan_half_fov.
    float4 eye_inverse_tan;
    /// xyz camera_forward, w max_distance.
    float4 forward_max_distance;
    /// xyz light_direction, w sweep_distance.
    float4 light_sweep;
    float4 camera_planes[6];
    /// x layer_mask, y flags, z instance_count, w lod_bias reinterpreted as a word.
    uint4 layer_flags_count_bias;
    /// x lod_hysteresis, y lod_cross_fade_band, z ortho_height, w reserved.
    float4 hysteresis_band_ortho;
};

// --- The flags, spelled once ---------------------------------------------------------------------

static const uint kInstanceActive = 1u << 0u;
static const uint kInstanceVisible = 1u << 1u;
static const uint kInstanceCastsShadow = 1u << 2u;
static const uint kInstanceMoved = 1u << 4u;
static const uint kInstanceVirtualGeometry = 1u << 7u;

static const uint kCullOcclusion = 1u << 0u;
static const uint kCullVisibilityRanges = 1u << 1u;
static const uint kCullShadowCasters = 1u << 2u;
static const uint kCullMovedOnly = 1u << 3u;

static const uint kNoLodFade = 0xFFFFFFFFu;
static const uint kNoVisibilityParent = 0xFFFFFFFFu;

static const uint kFadeModeDependents = 2u;

/// The per-slot verdict, and the index of the counter each answer increments. The enumeration and
/// `GpuCullCounters`'s field order are one list read two ways, exactly as they are in the reference.
static const uint kVerdictSkipped = 0u;
static const uint kVerdictLayer = 1u;
static const uint kVerdictFrustum = 2u;
static const uint kVerdictRange = 3u;
static const uint kVerdictOcclusion = 4u;
static const uint kVerdictVisible = 5u;

/// What `compact_draws` does with a slot: nothing, emit a draw, or append to the cluster list.
static const uint kEmitNothing = 0u;
static const uint kEmitDraw = 1u;
static const uint kEmitVirtualGeometry = 2u;

/// Word indices into `GpuCullCounters`, which is sixteen words and one cache line.
static const uint kCounterTested = 0u;
static const uint kCounterRejectedLayer = 1u;
static const uint kCounterRejectedFrustum = 2u;
static const uint kCounterRejectedOcclusion = 3u;
static const uint kCounterRejectedRange = 4u;
static const uint kCounterVisible = 5u;
static const uint kCounterDraws = 6u;
static const uint kCounterVirtualGeometry = 7u;
static const uint kCounterLodHistogram = 8u;

static const uint kCullGroupSize = 64u;
static const uint kCompactGroupSize = 256u;

// --- Bindings -------------------------------------------------------------------------------------
//
// One set, because a cull has one view and reads one scene. `shader-system` fixes 0 global, 1 view,
// 2 pass, 3 draw; a dispatch that is entirely one pass's business uses set 0 the way the M3 sample
// does, and the C++ side declares exactly these twelve bindings in this order.

[[vk::binding(0, 0)]] ConstantBuffer<CullView> view;
[[vk::binding(1, 0)]] StructuredBuffer<Instance> instances;
[[vk::binding(2, 0)]] StructuredBuffer<LodChain> chains;
[[vk::binding(3, 0)]] StructuredBuffer<MeshLod> mesh_lods;
[[vk::binding(4, 0)]] StructuredBuffer<VisibilityRange> ranges;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> previous_levels;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> slot_emit;
[[vk::binding(7, 0)]] RWStructuredBuffer<DrawIndexedIndirect> slot_command;
[[vk::binding(8, 0)]] RWStructuredBuffer<DrawPayload> slot_payload;
[[vk::binding(9, 0)]] RWStructuredBuffer<uint> counters;
[[vk::binding(10, 0)]] RWStructuredBuffer<DrawIndexedIndirect> commands;
[[vk::binding(11, 0)]] RWStructuredBuffer<DrawPayload> payloads;
[[vk::binding(12, 0)]] RWStructuredBuffer<uint> virtual_geometry;

// --- Sizes, as the C++ wrote them ----------------------------------------------------------------
//
// A structured buffer knows its own length on the device, but the reference's spans are the CPU's
// truth and an empty `ranges` span means "no instance declares a visibility range" — which a
// zero-length buffer cannot express, because Vulkan has no zero-length buffer. The counts travel in
// push constants so that a scene with no ranges and a scene with one range are two dispatches over
// two different code paths, exactly as they are two calls over two different spans.

struct Sizes {
    uint chain_count;
    uint mesh_lod_count;
    uint range_count;
    uint previous_level_count;
    uint draw_capacity;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};

[[vk::push_constant]] Sizes sizes;

// --- The arithmetic, in the reference's order -----------------------------------------------------

float signed_distance(float4 plane, float3 point) {
    return dot(plane.xyz, point) + plane.w;
}

/// Exact for a sphere. Spelled as the negation of "behind this plane" rather than as `>= -radius`:
/// the two differ when the distance is NaN, and the outward answer for a degenerate input is that
/// the sphere is kept. `cy::Frustum::intersects(const Sphere&)` says the same sentence.
bool frustum_intersects(float4 planes[6], float3 centre, float radius) {
    for (uint index = 0u; index < 6u; ++index) {
        if (signed_distance(planes[index], centre) < -radius) {
            return false;
        }
    }
    return true;
}

bool casts_into_view(float3 centre, float radius, float3 light_direction, float sweep) {
    if (frustum_intersects(view.camera_planes, centre, radius)) {
        return true;
    }
    return frustum_intersects(view.camera_planes, centre + (light_direction * sweep), radius);
}

float screen_coverage(float radius, float view_depth, float inverse_tan_half_fov) {
    if (!(view_depth > 0.0f)) {
        return 1.0f;
    }
    return (radius * inverse_tan_half_fov) / view_depth;
}

float visibility_range_alpha(VisibilityRange range, float distance) {
    const bool bounded = range.end > 0.0f;
    if (distance < range.begin) {
        if (range.fade_margin <= 0.0f) {
            return 0.0f;
        }
        const float into = (distance - (range.begin - range.fade_margin)) / range.fade_margin;
        if (into <= 0.0f) {
            return 0.0f;
        }
        return into >= 1.0f ? 1.0f : into;
    }
    if (bounded && distance > range.end) {
        if (range.fade_margin <= 0.0f) {
            return 0.0f;
        }
        const float out = ((range.end + range.fade_margin) - distance) / range.fade_margin;
        if (out <= 0.0f) {
            return 0.0f;
        }
        return out >= 1.0f ? 1.0f : out;
    }
    return 1.0f;
}

uint visibility_parent_of(uint packed) {
    const uint biased = packed >> 2u;
    return biased == 0u ? kNoVisibilityParent : biased - 1u;
}

uint fade_mode_of(uint packed) {
    return packed & 3u;
}

/// Resolve one instance's place in its visibility hierarchy, walking to the root.
///
/// Bounded by the range count rather than by a visited set: a cycle is a content error the importer
/// should have caught, and a compute thread cannot allocate one. Hitting the bound answers "not
/// visible", which drops the branch rather than hanging the dispatch.
float resolve_visibility(uint slot, float distance, uint instance_count) {
    if (sizes.range_count == 0u || slot >= sizes.range_count) {
        return 1.0f;
    }
    const float own = visibility_range_alpha(ranges[slot], distance);
    if (own <= 0.0f) {
        return 0.0f;
    }

    const float3 eye = view.eye_inverse_tan.xyz;
    uint walk = visibility_parent_of(ranges[slot].mode_and_parent);
    for (uint step = 0u; step < sizes.range_count && walk != kNoVisibilityParent; ++step) {
        if (walk >= sizes.range_count) {
            return own;
        }
        const VisibilityRange parent = ranges[walk];
        const float3 parent_centre =
            walk < instance_count ? instances[walk].bounds.xyz : float3(0.0f, 0.0f, 0.0f);
        const float parent_alpha = visibility_range_alpha(parent, length(parent_centre - eye));
        if (parent_alpha >= 1.0f) {
            return 0.0f;
        }
        if (parent_alpha > 0.0f && fade_mode_of(parent.mode_and_parent) == kFadeModeDependents) {
            return own * (1.0f - parent_alpha);
        }
        if (parent_alpha > 0.0f) {
            return 0.0f;
        }
        walk = visibility_parent_of(parent.mode_and_parent);
    }
    return own;
}

struct Survivor {
    uint verdict;
    float view_depth;
    float alpha;
};

/// The three required tests in their required order, then visibility ranges, then occlusion.
///
/// "Culling SHALL reject instances by, in order: layer mask against the view's mask, then
/// conservative frustum-versus-AABB using precomputed plane sign masks, then optional per-instance
/// distance limits." A layer test is one AND over a word already loaded; a frustum test is six dot
/// products, and running them the other way round would cost six times as much on the instances a
/// view does not draw.
Survivor test_instance(uint slot, uint instance_count) {
    Survivor result;
    result.verdict = kVerdictSkipped;
    result.view_depth = 0.0f;
    result.alpha = 1.0f;

    const Instance instance = instances[slot];
    const uint flags = view.layer_flags_count_bias.y;
    const bool shadow_view = (flags & kCullShadowCasters) != 0u;
    const bool moved_only = (flags & kCullMovedOnly) != 0u;
    const bool ranges_on = (flags & kCullVisibilityRanges) != 0u;

    if ((instance.flags & kInstanceActive) == 0u || (instance.flags & kInstanceVisible) == 0u) {
        return result;
    }
    if (shadow_view && (instance.flags & kInstanceCastsShadow) == 0u) {
        return result;
    }
    if (moved_only && (instance.flags & kInstanceMoved) == 0u) {
        return result;
    }

    // 1. Layer.
    if ((instance.mesh_material_chain_layer.w & view.layer_flags_count_bias.x) == 0u) {
        result.verdict = kVerdictLayer;
        return result;
    }

    // 2. Geometry.
    const float3 centre = instance.bounds.xyz;
    const float radius = instance.bounds.w;
    if (!frustum_intersects(view.planes, centre, radius)) {
        result.verdict = kVerdictFrustum;
        return result;
    }
    const float sweep = view.light_sweep.w;
    if (shadow_view && sweep > 0.0f &&
        !casts_into_view(centre, radius, view.light_sweep.xyz, sweep)) {
        result.verdict = kVerdictFrustum;
        return result;
    }

    // 3. Distance, view-wide and per instance, the smaller winning.
    const float3 eye = view.eye_inverse_tan.xyz;
    const float distance = length(centre - eye);
    const float max_distance = view.forward_max_distance.w;
    if (max_distance > 0.0f && distance - radius > max_distance) {
        result.verdict = kVerdictRange;
        return result;
    }

    // 4. Visibility ranges and the HLOD hierarchy.
    if (ranges_on) {
        result.alpha = resolve_visibility(slot, distance, instance_count);
        if (!(result.alpha > 0.0f)) {
            result.verdict = kVerdictRange;
            return result;
        }
    }

    // 5. Occlusion would be here, and is refused by the C++ rather than skipped silently.

    result.view_depth = dot(centre - eye, view.forward_max_distance.xyz);
    result.verdict = kVerdictVisible;
    return result;
}

struct LodPick {
    uint level;
    uint fade_to;
    float fade;
};

/// The level a chain selects, with the hysteresis band applied against the level chosen last frame.
///
/// The band is asymmetric on purpose: coverage must fall BELOW `threshold * (1 - hysteresis)` to
/// coarsen and rise ABOVE `threshold` to refine, so an instance sitting exactly on a threshold does
/// not flip level every frame as the camera breathes.
LodPick select_level(uint first, uint count, float coverage, uint previous) {
    LodPick pick;
    pick.level = 0u;
    pick.fade_to = kNoLodFade;
    pick.fade = 0.0f;
    if (count == 0u) {
        return pick;
    }
    const float lod_bias = asfloat(view.layer_flags_count_bias.w);
    const float hysteresis = view.hysteresis_band_ortho.x;
    const float band_fraction = view.hysteresis_band_ortho.y;
    const float adjusted = coverage * exp2(lod_bias);

    uint level = count - 1u;
    for (uint index = 0u; index < count; ++index) {
        float threshold = mesh_lods[first + index].screen_coverage_threshold;
        if (previous != kNoLodFade && index > previous) {
            threshold *= 1.0f - hysteresis;
        }
        if (adjusted >= threshold) {
            level = index;
            break;
        }
    }
    pick.level = level;

    if (band_fraction > 0.0f && level + 1u < count) {
        const float threshold = mesh_lods[first + level].screen_coverage_threshold;
        const float band = threshold * band_fraction;
        if (band > 0.0f && adjusted < threshold + band && adjusted >= threshold) {
            pick.fade_to = level + 1u;
            pick.fade = 1.0f - ((adjusted - threshold) / band);
        }
    }
    return pick;
}

// --- Pass one: the tests, per slot ----------------------------------------------------------------

[shader("compute")]
[numthreads(64, 1, 1)]
void cull_instances(uint3 thread: SV_DispatchThreadID) {
    const uint instance_count = view.layer_flags_count_bias.z;
    const uint slot = thread.x;
    if (slot >= instance_count) {
        return;
    }

    slot_emit[slot] = kEmitNothing;

    const Survivor survivor = test_instance(slot, instance_count);
    if (survivor.verdict == kVerdictSkipped) {
        return;
    }
    InterlockedAdd(counters[kCounterTested], 1u);
    if (survivor.verdict == kVerdictLayer) {
        InterlockedAdd(counters[kCounterRejectedLayer], 1u);
        return;
    }
    if (survivor.verdict == kVerdictFrustum) {
        InterlockedAdd(counters[kCounterRejectedFrustum], 1u);
        return;
    }
    if (survivor.verdict == kVerdictRange) {
        InterlockedAdd(counters[kCounterRejectedRange], 1u);
        return;
    }
    if (survivor.verdict == kVerdictOcclusion) {
        InterlockedAdd(counters[kCounterRejectedOcclusion], 1u);
        return;
    }
    InterlockedAdd(counters[kCounterVisible], 1u);

    const Instance instance = instances[slot];

    // Virtual geometry does not emit an indexed draw: cluster traversal owns it.
    if ((instance.flags & kInstanceVirtualGeometry) != 0u) {
        slot_emit[slot] = kEmitVirtualGeometry;
        InterlockedAdd(counters[kCounterVirtualGeometry], 1u);
        return;
    }

    const uint chain_index = instance.mesh_material_chain_layer.z;
    LodChain chain;
    chain.first = 0u;
    chain.count = 0u;
    if (chain_index < sizes.chain_count) {
        chain = chains[chain_index];
    }
    if (chain.count == 0u || chain.first + chain.count > sizes.mesh_lod_count) {
        return;
    }

    const float ortho_height = view.hysteresis_band_ortho.z;
    const float radius = instance.bounds.w;
    const float coverage =
        ortho_height > 0.0f
            ? (radius * 2.0f) / ortho_height
            : screen_coverage(radius, survivor.view_depth, view.eye_inverse_tan.w);
    const uint previous = slot < sizes.previous_level_count ? previous_levels[slot] : kNoLodFade;
    const LodPick pick = select_level(chain.first, chain.count, coverage, previous);
    if (slot < sizes.previous_level_count) {
        previous_levels[slot] = pick.level;
    }
    InterlockedAdd(counters[kCounterLodHistogram + (pick.level < 8u ? pick.level : 7u)], 1u);
    InterlockedAdd(counters[kCounterDraws], 1u);

    const MeshLod level = mesh_lods[chain.first + pick.level];
    DrawIndexedIndirect command;
    command.index_count = level.index_count;
    command.instance_count = 1u;
    command.first_index = level.first_index;
    command.vertex_offset = level.vertex_offset;
    // Patched by `compact_draws`, which is the only pass that knows where this draw lands.
    command.first_instance = 0u;
    slot_command[slot] = command;

    DrawPayload payload;
    payload.instance_slot = slot;
    payload.material = level.material;
    payload.lod_level = pick.level;
    payload.lod_fade_to = pick.fade_to;
    payload.lod_fade = pick.fade;
    payload.alpha = survivor.alpha;
    payload.view_depth = survivor.view_depth;
    payload.coverage = coverage;
    slot_payload[slot] = payload;

    slot_emit[slot] = kEmitDraw;
}

// --- Pass two: compaction in ascending slot order --------------------------------------------------

groupshared uint gs_scan[256];
groupshared uint gs_draw_base;
groupshared uint gs_cluster_base;

/// An inclusive Hillis-Steele scan over the group, leaving the total in every lane's `gs_scan` at
/// index kCompactGroupSize - 1. Two barriers per step, which is what makes it correct rather than
/// nearly correct on a machine whose subgroup is not the group.
uint inclusive_scan(uint lane, uint value) {
    // The leading barrier is not redundant. Two scans run back to back in `compact_draws` and the
    // second one's first store would otherwise race the first one's last read of `gs_scan[255]`.
    GroupMemoryBarrierWithGroupSync();
    gs_scan[lane] = value;
    GroupMemoryBarrierWithGroupSync();
    for (uint offset = 1u; offset < kCompactGroupSize; offset <<= 1u) {
        uint addend = 0u;
        if (lane >= offset) {
            addend = gs_scan[lane - offset];
        }
        GroupMemoryBarrierWithGroupSync();
        gs_scan[lane] += addend;
        GroupMemoryBarrierWithGroupSync();
    }
    return gs_scan[lane];
}

[shader("compute")]
[numthreads(256, 1, 1)]
void compact_draws(uint3 thread: SV_GroupThreadID) {
    const uint lane = thread.x;
    const uint instance_count = view.layer_flags_count_bias.z;
    if (lane == 0u) {
        gs_draw_base = 0u;
        gs_cluster_base = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint chunk = 0u; chunk < instance_count; chunk += kCompactGroupSize) {
        const uint slot = chunk + lane;
        const uint emit = slot < instance_count ? slot_emit[slot] : kEmitNothing;

        const uint is_draw = emit == kEmitDraw ? 1u : 0u;
        const uint draw_inclusive = inclusive_scan(lane, is_draw);
        const uint draw_total = gs_scan[kCompactGroupSize - 1u];
        const uint draw_index = gs_draw_base + draw_inclusive - is_draw;
        if (is_draw != 0u && draw_index < sizes.draw_capacity) {
            DrawIndexedIndirect command = slot_command[slot];
            command.first_instance = draw_index;
            commands[draw_index] = command;
            payloads[draw_index] = slot_payload[slot];
        }

        const uint is_cluster = emit == kEmitVirtualGeometry ? 1u : 0u;
        const uint cluster_inclusive = inclusive_scan(lane, is_cluster);
        const uint cluster_total = gs_scan[kCompactGroupSize - 1u];
        const uint cluster_index = gs_cluster_base + cluster_inclusive - is_cluster;
        if (is_cluster != 0u && cluster_index < sizes.draw_capacity) {
            virtual_geometry[cluster_index] = slot;
        }

        GroupMemoryBarrierWithGroupSync();
        if (lane == 0u) {
            gs_draw_base += draw_total;
            gs_cluster_base += cluster_total;
        }
        GroupMemoryBarrierWithGroupSync();
    }
}
