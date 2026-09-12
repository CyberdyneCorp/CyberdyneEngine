// The three dispatches that are the SAME for every effect, and therefore the only ones that can be
// checked-in SPIR-V. M10 task 5.1 and 5.2.
//
// ================================================================================================
// WHY THESE THREE ARE FIXED AND THE SIMULATION KERNEL IS NOT
// ================================================================================================
//
// A VFX kernel is generated from an authored graph and its code is different for every effect —
// `emit_slang.cpp` writes it, and `src/vfx/gpu/`'s host compiles it through `cy::shader`'s Slang
// front end at load time. The scheduler's own work is not: zeroing the step's counters, compacting
// the live and free lists, granting a spawn against the free count, writing the indirect arguments
// and ordering the population are the same arithmetic whatever the effect does. So they live here,
// are compiled once by hand, and are embedded — which is also what lets a GPU simulation exist in a
// build with `CY_SHADER_SLANG=OFF`, where the generated half must be supplied pre-cooked.
//
// ================================================================================================
// ASCENDING ORDER IS A REQUIREMENT, NOT A PREFERENCE
// ================================================================================================
//
// `vfx_compact` is ONE workgroup walking the alive array in ascending slot order with a shared-
// memory prefix scan, exactly as `gpu_cull.slang`'s `compact_draws` does and for the same reason:
// an atomic append produces whatever order the hardware scheduled the groups in, and a live list in
// that order cannot be compared against the CPU path's. The CPU executor builds its live set by
// `for (particle = 0; particle < capacity; ++particle)`, so the GPU's list is the same list and
// `test_vfx_gpu_pass.cpp` can compare populations and per-slot liveness EXACTLY rather than
// statistically.
//
// The free list is built in the same pass and in the same order, which is what makes a spawn
// deterministic too: `vfx-system` accepts that GPU VFX is not reproducible across devices, and that
// is not a reason to make it unreproducible on one.
//
// ================================================================================================
// THE INDIRECT ARGUMENTS ARE WRITTEN HERE AND READ BY THE DEVICE
// ================================================================================================
//
// "Simulation SHALL use indirect dispatch driven by live particle counts MAINTAINED ON THE GPU, so
// dispatch size tracks actual population without CPU knowledge of it", and "THEN dispatch sizes
// SHALL follow via indirect arguments, with NO CPU READBACK of counts."
//
// `vfx_compact`'s lane 0 writes `cyVfxArgs` and the host records `dispatch_indirect` against it.
// The host never reads `cyVfxCounts` to size anything: the only read-back in this module is the
// counter block copied for the diagnostics `vfx-system` asks for, one frame late, and nothing
// branches on it.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   slangc vfx_support.slang -target spirv -profile spirv_1_5 \
//          -entry vfx_reset   -stage compute -o vfx_reset.spv
//   slangc vfx_support.slang -target spirv -profile spirv_1_5 \
//          -entry vfx_compact -stage compute -o vfx_compact.spv
//   slangc vfx_support.slang -target spirv -profile spirv_1_5 \
//          -entry vfx_sort    -stage compute -o vfx_sort.spv
//   python3 embed_spirv.py ../src/vfx_support_spirv.h \
//          kVfxResetSpirv=vfx_reset.spv \
//          kVfxCompactSpirv=vfx_compact.spv \
//          kVfxSortSpirv=vfx_sort.spv
//   clang-format -i ../src/vfx_support_spirv.h

// --- The binding set. `cy::vfx::GpuBinding`, and it is the SAME set the generated prelude
//     declares — see src/vfx/src/emit_slang.cpp. A support dispatch reads what a kernel wrote and
//     a kernel reads what a support dispatch wrote, so a second set would be a second thing to
//     keep in step. ---
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> cyVfxParticles;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> cyVfxAlive;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> cyVfxIndices;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> cyVfxCounts;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> cyVfxFree;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> cyVfxParamWords;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> cyVfxKeys;

struct CyVfxEvent {
    uint source;
    uint depth;
    float rank;
    float payload;
};
[[vk::binding(7, 0)]] RWStructuredBuffer<CyVfxEvent> cyVfxEvents;
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> cyVfxArgs;

// `cy::vfx::GpuCountWord`.
static const uint CY_VFX_COUNT_LIVE = 0u;
static const uint CY_VFX_COUNT_FREE = 1u;
static const uint CY_VFX_COUNT_SPAWN_REQUEST = 2u;
static const uint CY_VFX_COUNT_SPAWN_GRANTED = 3u;
static const uint CY_VFX_COUNT_SPAWNED = 4u;
static const uint CY_VFX_COUNT_KILLED = 5u;
static const uint CY_VFX_COUNT_REPORTED_LIVE = 6u;
static const uint CY_VFX_COUNT_SORT_PASSES = 7u;
static const uint CY_VFX_COUNT_EVENTS_BASE = 8u;
static const uint CY_VFX_MAX_EVENT_CHANNELS = 8u;

// `cy::vfx::GpuPushConstants`. The generated kernels read the same forty-eight bytes under the name
// `cyVfxPush`; the first five fields are the ones a kernel body reads and the rest are the
// scheduler's.
struct CyVfxInput {
    float dt;
    float emitter_age;
    float particle_index;
    float spawn_index;
    float normalised_age;
    uint pass;
    uint capacity;
    uint spawn_scale_fixed;
    uint block_capacity;
    uint sort_n;
    uint sort_passes;
    uint reserved;
};
[[vk::push_constant]] ConstantBuffer<CyVfxInput> cyVfxPush;

static const uint CY_VFX_GROUP_SIZE = 64u;
static const uint CY_VFX_SORT_GROUP_SIZE = 256u;
static const uint CY_VFX_SORT_CAPACITY = 2048u;

// --- vfx_reset: the state a freshly created emitter starts from -----------------------------------
//
// The particle block itself is NOT zeroed: an attribute is read only after a kernel has written it,
// and zeroing four megabytes at creation to satisfy a reader that does not exist would be four
// megabytes of bandwidth for a property nothing has. The LIVENESS array is zeroed, and that is what
// makes every slot free and every unwritten attribute unreachable.

[shader("compute")]
[numthreads(64, 1, 1)]
void vfx_reset(uint3 thread: SV_DispatchThreadID) {
    const uint slot = thread.x;
    if (slot < cyVfxPush.block_capacity) {
        cyVfxAlive[slot] = 0u;
        cyVfxIndices[slot] = 0u;
        cyVfxFree[slot] = 0u;
        cyVfxKeys[slot] = 0xFFFFFFFFu;
    }
    if (slot == 0u) {
        for (uint word = 0u; word < CY_VFX_COUNT_EVENTS_BASE + CY_VFX_MAX_EVENT_CHANNELS; ++word) {
            cyVfxCounts[word] = 0u;
        }
        for (uint arg = 0u; arg < 9u; ++arg) {
            cyVfxArgs[arg] = 0u;
        }
    }
}

// --- vfx_compact: the step's bookkeeping, in one workgroup ---------------------------------------

groupshared uint gs_scan[CY_VFX_GROUP_SIZE];
groupshared uint gs_live_base;
groupshared uint gs_free_base;

/// An inclusive Hillis-Steele scan over the group, leaving the total in `gs_scan[63]`. Two barriers
/// a step, which is what makes it correct rather than nearly correct on a machine whose subgroup is
/// not the group — `gpu_cull.slang` records the same reason for the same shape.
uint inclusive_scan(uint lane, uint value) {
    // The leading barrier is not redundant: two scans run back to back below, and the second one's
    // first store would otherwise race the first one's last read of `gs_scan[63]`.
    GroupMemoryBarrierWithGroupSync();
    gs_scan[lane] = value;
    GroupMemoryBarrierWithGroupSync();
    for (uint offset = 1u; offset < CY_VFX_GROUP_SIZE; offset <<= 1u) {
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
[numthreads(64, 1, 1)]
void vfx_compact(uint3 thread: SV_GroupThreadID) {
    const uint lane = thread.x;
    const uint block = cyVfxPush.block_capacity;
    const uint cap = cyVfxPush.capacity;

    if (lane == 0u) {
        gs_live_base = 0u;
        gs_free_base = 0u;
        // ZEROED HERE AND NOT IN A PASS OF ITS OWN, and `kGpuCountSpawnRequest` is deliberately not
        // among them: the Spawn kernel ran before this dispatch and its answer is what the grant
        // below consumes.
        cyVfxCounts[CY_VFX_COUNT_SPAWNED] = 0u;
        cyVfxCounts[CY_VFX_COUNT_KILLED] = 0u;
        for (uint channel = 0u; channel < CY_VFX_MAX_EVENT_CHANNELS; ++channel) {
            cyVfxCounts[CY_VFX_COUNT_EVENTS_BASE + channel] = 0u;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint chunk = 0u; chunk < block; chunk += CY_VFX_GROUP_SIZE) {
        const uint slot = chunk + lane;
        uint state = slot < block ? cyVfxAlive[slot] : 0u;

        // THE PARTICLE-COUNT CAP, and it is the CPU path's own branch: `run_update` kills a live
        // particle whose slot is at or above the cap the budget controller set, because a cap that
        // only stopped new spawns would do nothing to a block that is already full.
        if (slot < block && state != 0u && slot >= cap) {
            cyVfxAlive[slot] = 0u;
            uint ignored = 0u;
            InterlockedAdd(cyVfxCounts[CY_VFX_COUNT_KILLED], 1u, ignored);
            state = 0u;
        }

        // EVERY LIVE SLOT IS LISTED, and the "spawned this sub-step" exclusion the CPU path needs a
        // marker for is STRUCTURAL here.
        //
        // `run_update` walks one array doing both jobs, so a particle the fused initialise has just
        // advanced has to be marked `2` or it would be advanced twice in the sub-step it was born
        // in. This dispatch runs BEFORE the spawn, so a particle born this sub-step is not in the
        // list this sub-step and cannot be advanced by it — no marker required, and the spawn pass
        // therefore writes `1`. Reading `2` here is still handled, because a kernel compiled before
        // this rule existed may write one, and it is promoted rather than dropped.
        const uint is_live = (state != 0u) ? 1u : 0u;
        if (state == 2u) {
            cyVfxAlive[slot] = 1u;
        }

        const uint live_inclusive = inclusive_scan(lane, is_live);
        const uint live_total = gs_scan[CY_VFX_GROUP_SIZE - 1u];
        const uint live_index = gs_live_base + live_inclusive - is_live;
        if (is_live != 0u) {
            cyVfxIndices[live_index] = slot;
        }

        // A FREE SLOT IS ONE BELOW THE CAP. `run_initialise` fills slots by
        // `for (particle = 0; particle < pass.capacity; ...)` and so never spawns above the cap the
        // budget controller set; a free list that offered those slots would spawn particles the
        // very next compaction kills, and the population would settle above the cap.
        const uint is_free = (slot < block && slot < cap && state == 0u) ? 1u : 0u;
        const uint free_inclusive = inclusive_scan(lane, is_free);
        const uint free_total = gs_scan[CY_VFX_GROUP_SIZE - 1u];
        const uint free_index = gs_free_base + free_inclusive - is_free;
        if (is_free != 0u) {
            cyVfxFree[free_index] = slot;
        }

        GroupMemoryBarrierWithGroupSync();
        if (lane == 0u) {
            gs_live_base += live_total;
            gs_free_base += free_total;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane != 0u) {
        return;
    }

    const uint live = gs_live_base;
    const uint free = gs_free_base;
    cyVfxCounts[CY_VFX_COUNT_LIVE] = live;
    cyVfxCounts[CY_VFX_COUNT_FREE] = free;

    // THE GRANT. "WHEN the shared pool is exhausted THEN spawn requests SHALL be REDUCED [...]
    // rather than overwriting live particles or failing the frame" — the reduction is `min` against
    // the free count, and the free count is a number only this dispatch knows.
    //
    // Both operands are clamped to 16 bits before the multiply so the 16.16 product cannot overflow
    // a `uint`. A request above 65535 in one sub-step exceeds every block this module sizes, and the
    // clamp against `free` would have cut it to the block anyway.
    const uint request = min(cyVfxCounts[CY_VFX_COUNT_SPAWN_REQUEST], 0xFFFFu);
    const uint scale = min(cyVfxPush.spawn_scale_fixed, 0x10000u);
    const uint scaled = (request * scale) >> 16u;
    const uint granted = min(scaled, free);
    cyVfxCounts[CY_VFX_COUNT_SPAWN_GRANTED] = granted;

    // What the step ends with: the particles the compaction listed, which is every one that
    // survived, plus the ones about to be spawned.
    cyVfxCounts[CY_VFX_COUNT_REPORTED_LIVE] = live + granted;
    cyVfxCounts[CY_VFX_COUNT_SORT_PASSES] = cyVfxPush.sort_passes;

    // THE INDIRECT ARGUMENTS. `cy::vfx::GpuArgOffset`, in words.
    const uint live_groups = (live + CY_VFX_GROUP_SIZE - 1u) / CY_VFX_GROUP_SIZE;
    const uint spawn_groups = (granted + CY_VFX_GROUP_SIZE - 1u) / CY_VFX_GROUP_SIZE;
    cyVfxArgs[0] = live_groups;
    cyVfxArgs[1] = 1u;
    cyVfxArgs[2] = 1u;
    cyVfxArgs[3] = spawn_groups;
    cyVfxArgs[4] = 1u;
    cyVfxArgs[5] = 1u;
    cyVfxArgs[6] = live_groups;
    cyVfxArgs[7] = 1u;
    cyVfxArgs[8] = 1u;
}

// --- vfx_sort: back-to-front ordering, one workgroup, one graph pass -----------------------------
//
// `vfx-system`: "Distance sorting SHALL be performed on the GPU over the particle population, with
// the sort cost budgeted and REDUCIBLE by the budget controller." The reduction is that the host
// does not declare this pass at all when `BudgetLevers::sorted` is false, and `sort_passes` is then
// zero in the counter block — so "sorted or not" is a number a diagnostic reads rather than a state
// nobody can see.
//
// ONE WORKGROUP over group-shared memory rather than the log^2(n) global passes a device-memory
// bitonic sort needs, because every one of those would be a separate render-graph pass: the RHI
// exposes no barrier outside the graph's executor, deliberately, and sixty-six passes a sub-step an
// emitter is not a schedule. The price is the bound: `CY_VFX_SORT_CAPACITY` elements. An emitter
// whose block exceeds it is refused the sort by the host and told so by name.
//
// The padding is what keeps the live count off the CPU's books: elements at or above the live count
// take the maximal key, sort to the end, and are never read back — so the sort needs to know only
// `sort_n`, which is a power of two the host derives from the BLOCK and not from the population.

groupshared uint gs_keys[CY_VFX_SORT_CAPACITY];
groupshared uint gs_index[CY_VFX_SORT_CAPACITY];

[shader("compute")]
[numthreads(256, 1, 1)]
void vfx_sort(uint3 thread: SV_GroupThreadID) {
    const uint lane = thread.x;
    const uint n = min(cyVfxPush.sort_n, CY_VFX_SORT_CAPACITY);
    const uint live = cyVfxCounts[CY_VFX_COUNT_LIVE];

    for (uint slot = lane; slot < n; slot += CY_VFX_SORT_GROUP_SIZE) {
        const bool real = slot < live;
        gs_keys[slot] = real ? cyVfxKeys[slot] : 0xFFFFFFFFu;
        gs_index[slot] = real ? cyVfxIndices[slot] : 0xFFFFFFFFu;
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint k = 2u; k <= n; k <<= 1u) {
        for (uint j = k >> 1u; j > 0u; j >>= 1u) {
            for (uint i = lane; i < n; i += CY_VFX_SORT_GROUP_SIZE) {
                const uint partner = i ^ j;
                if (partner <= i) {
                    continue;
                }
                const bool ascending = (i & k) == 0u;
                const uint mine = gs_keys[i];
                const uint theirs = gs_keys[partner];
                if ((ascending && mine > theirs) || (!ascending && mine < theirs)) {
                    gs_keys[i] = theirs;
                    gs_keys[partner] = mine;
                    const uint index_mine = gs_index[i];
                    gs_index[i] = gs_index[partner];
                    gs_index[partner] = index_mine;
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }

    // Only the live prefix is written back. The padding sorted to the end by construction, so the
    // live entries are exactly `[0, live)` and the free list — a different array — is untouched.
    for (uint out_slot = lane; out_slot < live; out_slot += CY_VFX_SORT_GROUP_SIZE) {
        cyVfxKeys[out_slot] = gs_keys[out_slot];
        cyVfxIndices[out_slot] = gs_index[out_slot];
    }
}
