// Generated prelude for emitter `probe`. Everything below is derived from this effect's own declarations, which is the
// only place it can come from: a parameter block and an attribute layout are
// different for every effect and cannot live in a shared standard library.
// This unit imports nothing, so it compiles on its own.

// --- Storage helpers. One spelling of each conversion, so a load and a store agree. ---
float cyVfxUnpackF32(uint word) { return asfloat(word); }
float cyVfxUnpackF16(uint word, uint lane) { return f16tof32((word >> (lane * 16u)) & 0xFFFFu); }
float cyVfxUnpackUnorm8(uint word, uint lane) {
    return float((word >> (lane * 8u)) & 0xFFu) * (1.0 / 255.0);
}
float cyVfxUnpackSnorm16(uint word, uint lane) {
    int raw = int((word >> (lane * 16u)) & 0xFFFFu);
    raw = (raw & 0x8000) != 0 ? raw - 65536 : raw;
    return max(float(raw) * (1.0 / 32767.0), -1.0);
}
uint cyVfxPackF16(float value) { return f32tof16(value); }
uint cyVfxPackUnorm8(float value) { return uint(saturate(value) * 255.0 + 0.5); }
uint cyVfxPackSnorm16(float value) { return uint(int(clamp(value, -1.0, 1.0) * 32767.0)) & 0xFFFFu; }
void cyVfxPackLane(inout uint word, uint lane, uint bits, uint width) {
    if (width >= 32u) { word = bits; return; }
    uint shift = lane * width;
    uint mask = ((1u << width) - 1u) << shift;
    word = (word & ~mask) | ((bits << shift) & mask);
}

// --- The binding set. FIXED for every effect, which is what lets one descriptor set layout serve
//     every generated kernel and the fixed support dispatches at the same time. The numbers are
//     `cy::vfx::GpuBinding` in <cy/vfx/gpu_layout.h> and are spelled there once. ---
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> cyVfxParticles;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> cyVfxAlive;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> cyVfxIndices;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> cyVfxCounts;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> cyVfxFree;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> cyVfxParamWords;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> cyVfxKeys;

// The counter words, `cy::vfx::GpuCountWord`. Every one of them is written by a dispatch and read
// by a dispatch: this is what "live particle counts MAINTAINED ON THE GPU" means as addresses.
static const uint CY_VFX_COUNT_LIVE = 0u;
static const uint CY_VFX_COUNT_FREE = 1u;
static const uint CY_VFX_COUNT_SPAWN_REQUEST = 2u;
static const uint CY_VFX_COUNT_SPAWN_GRANTED = 3u;
static const uint CY_VFX_COUNT_SPAWNED = 4u;
static const uint CY_VFX_COUNT_KILLED = 5u;
static const uint CY_VFX_COUNT_REPORTED_LIVE = 6u;
static const uint CY_VFX_COUNT_SORT_PASSES = 7u;
static const uint CY_VFX_COUNT_EVENTS_BASE = 8u;
static const uint CY_VFX_EVENT_STRIDE = 256u;

// --- Per-dispatch inputs. PUSHED rather than bound, because `pass` changes between two dispatches
//     in one command buffer and a word in device memory cannot. Matches
//     `cy::vfx::GpuPushConstants` field for field. ---
struct CyVfxInput {
    float dt;
    float emitter_age;
    // Per-thread, and therefore overwritten by the entry point after it reads the block. They are
    // members so that the pushed struct and the struct a kernel body reads are one struct.
    float particle_index;
    float spawn_index;
    float normalised_age;
    uint pass;
    uint capacity;
    uint spawn_scale_fixed;
};
[[vk::push_constant]] ConstantBuffer<CyVfxInput> cyVfxPush;

// What a kernel body reads. A `static` global is per-thread in this model, which is exactly what
// `particle_index` and `spawn_index` need to be — the CPU executor sets them per particle too, and
// a uniform block could not have expressed that at all.
static CyVfxInput cyVfxInput;

static const uint CY_VFX_PASS_SPAWN = 0u;
static const uint CY_VFX_PASS_INITIALISE = 1u;
static const uint CY_VFX_PASS_UPDATE = 2u;
static const uint CY_VFX_PASS_KEYS = 3u;

// A per-particle random draw. The stream index is the authoring node's own key, so two `random`
// nodes are two streams and one node is one value per particle per evaluation.
float cyVfxRandom(uint particle, uint stream) {
    uint state = particle * 747796405u + stream * 2891336453u + 1u;
    state ^= state >> 16u;
    state *= 2246822519u;
    state ^= state >> 13u;
    state *= 3266489917u;
    state ^= state >> 16u;
    return float(state >> 8u) * (1.0 / 16777216.0);
}

float cyVfxNoise(float x) {
    float i = floor(x);
    float f = frac(x);
    float a = frac(sin(i * 12.9898) * 43758.5453);
    float b = frac(sin((i + 1.0) * 12.9898) * 43758.5453);
    return lerp(a, b, f * f * (3.0 - 2.0 * f));
}

// A curve is a cook-time resource; until one is bound this is the identity over [0, 1], and the
// generated call names the curve so a binding can replace it without the kernel changing.
float cyVfxCurve(float t) { return saturate(t); }

// An event channel's record. The channel's declared maximum is enforced on the CPU side by
// `EventRouter`; the GPU-side append below is bounded by BOTH that declaration and the ring's own
// region, because a channel declared larger than the ring must not write past it.
struct CyVfxEvent {
    uint source;
    uint depth;
    float rank;
    float payload;
};
[[vk::binding(7, 0)]] RWStructuredBuffer<CyVfxEvent> cyVfxEvents;

// The kill sink the kill root assigns into. THE TWO COUNTERS ARE PART OF THE KILL, not bookkeeping
// beside it.
//
// `CY_VFX_COUNT_KILLED` rising is what makes a falling population visible at all — a kill that did
// not count would leave `StepReport::killed` reading zero while particles disappeared.
//
// `CY_VFX_COUNT_REPORTED_LIVE` falling is what makes the population the number the CPU path means
// by one. The compaction sets it to `live + granted` BEFORE this pass runs, so without this
// decrement it would report the population the sub-step STARTED with plus the spawn, and a run long
// enough for particles to expire would over-count by exactly this sub-step's kills. That is not a
// hypothetical: it is what the ninety-six-step comparison in `test_vfx_gpu_pass.cpp` caught.
//
// The subtraction is spelled as an addition of `0xFFFFFFFF` because `InterlockedAdd` is unsigned.
// It cannot underflow: only a particle the compaction counted into `live`, or one the grant counted
// into `granted`, can reach the guard above.
void cyVfxKill(uint particle) {
    if (cyVfxAlive[particle] != 0u) {
        uint ignored = 0u;
        InterlockedAdd(cyVfxCounts[CY_VFX_COUNT_KILLED], 1u, ignored);
        InterlockedAdd(cyVfxCounts[CY_VFX_COUNT_REPORTED_LIVE], 0xFFFFFFFFu, ignored);
    }
    cyVfxAlive[particle] = 0u;
}

// --- The effect's parameter block. ---
struct CyVfxParams {
    float cyVfxUnused;
};
static CyVfxParams cyVfxParams;
void cyVfxLoadParams() {
    cyVfxParams.cyVfxUnused = 0.0;
}

// --- The derived attribute layout. 1 attribute(s) allocated, 0 elided,
// 12 bytes a particle. ---
// `position` at Float32: 3 component(s), 12 byte(s) a particle on the CPU, 3 word(s) here.
// array base = 0 word(s), capacity 64.
static const uint cyVfxBase_position = 0u;
float3 cyVfxLoad_position(uint particle) {
    uint base = particle * 3u;
    return float3(cyVfxUnpackF32(cyVfxParticles[cyVfxBase_position + base + 0u]), cyVfxUnpackF32(cyVfxParticles[cyVfxBase_position + base + 1u]), cyVfxUnpackF32(cyVfxParticles[cyVfxBase_position + base + 2u]));
}
void cyVfxStore_position(uint particle, float3 value) {
    uint base = particle * 3u;
    uint w0 = 0u;
    uint w1 = 0u;
    uint w2 = 0u;
    cyVfxPackLane(w0, 0u, asuint(value[0]), 32u);
    cyVfxPackLane(w1, 0u, asuint(value[1]), 32u);
    cyVfxPackLane(w2, 0u, asuint(value[2]), 32u);
    cyVfxParticles[cyVfxBase_position + base + 0u] = w0;
    cyVfxParticles[cyVfxBase_position + base + 1u] = w1;
    cyVfxParticles[cyVfxBase_position + base + 2u] = w2;
}

// --- One sampler per data-interface field this effect reads. Unbound here, and
//     an unbound interface reads zero on both paths — see src/vfx/README.md. ---
float cyVfxSample_wind_field_speed(float x) { return float(0);
}

// Generated by cy::vfx. Emitter `probe`, stages Update, IR digest 0xf79ac99b2e8d0fd2.
void cyVfxKernel_probe_4(uint particle) {
    float v0 = cyVfxSample_wind_field_speed(cyVfxInput.dt);
    float3 v1 = float3(v0, v0, v0);
    float3 v2 = (v1 + cyVfxLoad_position(particle));
    cyVfxStore_position(particle, v2);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cyVfxKernel(uint3 thread : SV_DispatchThreadID) {
    cyVfxInput = cyVfxPush;
    cyVfxLoadParams();
    cyVfxKernel_probe_4(thread.x);
}
