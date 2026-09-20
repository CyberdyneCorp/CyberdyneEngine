// Generated prelude for emitter `sparks0`. Everything below is derived from this effect's own declarations, which is the
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
struct CyVfxEvent {
    uint source;
    uint depth;
    float rank;
    float payload;
};
struct CyVfxSet {
    RWStructuredBuffer<uint> particles;
    RWStructuredBuffer<uint> alive;
    RWStructuredBuffer<uint> indices;
    RWStructuredBuffer<uint> counts;
    RWStructuredBuffer<uint> free_list;
    RWStructuredBuffer<uint> parameter_words;
    RWStructuredBuffer<uint> keys;
    RWStructuredBuffer<CyVfxEvent> events;
    RWStructuredBuffer<uint> args;
};
[[vk::binding(0, 0)]] ParameterBlock<CyVfxSet> cyVfxSet;
#define cyVfxParticles cyVfxSet.particles
#define cyVfxAlive cyVfxSet.alive
#define cyVfxIndices cyVfxSet.indices
#define cyVfxCounts cyVfxSet.counts
#define cyVfxFree cyVfxSet.free_list
#define cyVfxParamWords cyVfxSet.parameter_words
#define cyVfxKeys cyVfxSet.keys
#define cyVfxEvents cyVfxSet.events
#define cyVfxArgs cyVfxSet.args

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
    float intensity;
    float3 gravity;
};
static CyVfxParams cyVfxParams;
void cyVfxLoadParams() {
    cyVfxParams.intensity = asfloat(cyVfxParamWords[0u]);
    cyVfxParams.gravity = float3(asfloat(cyVfxParamWords[4u]), asfloat(cyVfxParamWords[5u]), asfloat(cyVfxParamWords[6u]));
}

// --- The derived attribute layout. 7 attribute(s) allocated, 1 elided,
// 44 bytes a particle. ---
// `position` at Float32: 3 component(s), 12 byte(s) a particle on the CPU, 3 word(s) here.
// array base = 0 word(s), capacity 512.
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
// `velocity` at Float32: 3 component(s), 12 byte(s) a particle on the CPU, 3 word(s) here.
// array base = 1536 word(s), capacity 512.
static const uint cyVfxBase_velocity = 1536u;
float3 cyVfxLoad_velocity(uint particle) {
    uint base = particle * 3u;
    return float3(cyVfxUnpackF32(cyVfxParticles[cyVfxBase_velocity + base + 0u]), cyVfxUnpackF32(cyVfxParticles[cyVfxBase_velocity + base + 1u]), cyVfxUnpackF32(cyVfxParticles[cyVfxBase_velocity + base + 2u]));
}
void cyVfxStore_velocity(uint particle, float3 value) {
    uint base = particle * 3u;
    uint w0 = 0u;
    uint w1 = 0u;
    uint w2 = 0u;
    cyVfxPackLane(w0, 0u, asuint(value[0]), 32u);
    cyVfxPackLane(w1, 0u, asuint(value[1]), 32u);
    cyVfxPackLane(w2, 0u, asuint(value[2]), 32u);
    cyVfxParticles[cyVfxBase_velocity + base + 0u] = w0;
    cyVfxParticles[cyVfxBase_velocity + base + 1u] = w1;
    cyVfxParticles[cyVfxBase_velocity + base + 2u] = w2;
}
// `age` at Float32: 1 component(s), 4 byte(s) a particle on the CPU, 1 word(s) here.
// array base = 3072 word(s), capacity 512.
static const uint cyVfxBase_age = 3072u;
float cyVfxLoad_age(uint particle) {
    uint base = particle * 1u;
    return cyVfxUnpackF32(cyVfxParticles[cyVfxBase_age + base + 0u]);
}
void cyVfxStore_age(uint particle, float value) {
    uint base = particle * 1u;
    uint w0 = 0u;
    cyVfxPackLane(w0, 0u, asuint(value), 32u);
    cyVfxParticles[cyVfxBase_age + base + 0u] = w0;
}
// `lifetime` at Float32: 1 component(s), 4 byte(s) a particle on the CPU, 1 word(s) here.
// array base = 3584 word(s), capacity 512.
static const uint cyVfxBase_lifetime = 3584u;
float cyVfxLoad_lifetime(uint particle) {
    uint base = particle * 1u;
    return cyVfxUnpackF32(cyVfxParticles[cyVfxBase_lifetime + base + 0u]);
}
void cyVfxStore_lifetime(uint particle, float value) {
    uint base = particle * 1u;
    uint w0 = 0u;
    cyVfxPackLane(w0, 0u, asuint(value), 32u);
    cyVfxParticles[cyVfxBase_lifetime + base + 0u] = w0;
}
// `size` at Float32: 1 component(s), 4 byte(s) a particle on the CPU, 1 word(s) here.
// array base = 4096 word(s), capacity 512.
static const uint cyVfxBase_size = 4096u;
float cyVfxLoad_size(uint particle) {
    uint base = particle * 1u;
    return cyVfxUnpackF32(cyVfxParticles[cyVfxBase_size + base + 0u]);
}
void cyVfxStore_size(uint particle, float value) {
    uint base = particle * 1u;
    uint w0 = 0u;
    cyVfxPackLane(w0, 0u, asuint(value), 32u);
    cyVfxParticles[cyVfxBase_size + base + 0u] = w0;
}
// `color` at Unorm8: 4 component(s), 4 byte(s) a particle on the CPU, 1 word(s) here.
// array base = 4608 word(s), capacity 512.
static const uint cyVfxBase_color = 4608u;
float4 cyVfxLoad_color(uint particle) {
    uint base = particle * 1u;
    return float4(cyVfxUnpackUnorm8(cyVfxParticles[cyVfxBase_color + base + 0u], 0u), cyVfxUnpackUnorm8(cyVfxParticles[cyVfxBase_color + base + 0u], 1u), cyVfxUnpackUnorm8(cyVfxParticles[cyVfxBase_color + base + 0u], 2u), cyVfxUnpackUnorm8(cyVfxParticles[cyVfxBase_color + base + 0u], 3u));
}
void cyVfxStore_color(uint particle, float4 value) {
    uint base = particle * 1u;
    uint w0 = 0u;
    cyVfxPackLane(w0, 0u, cyVfxPackUnorm8(value[0]), 8u);
    cyVfxPackLane(w0, 1u, cyVfxPackUnorm8(value[1]), 8u);
    cyVfxPackLane(w0, 2u, cyVfxPackUnorm8(value[2]), 8u);
    cyVfxPackLane(w0, 3u, cyVfxPackUnorm8(value[3]), 8u);
    cyVfxParticles[cyVfxBase_color + base + 0u] = w0;
}
// `emission` at Float32: 1 component(s), 4 byte(s) a particle on the CPU, 1 word(s) here.
// array base = 5120 word(s), capacity 512.
static const uint cyVfxBase_emission = 5120u;
float cyVfxLoad_emission(uint particle) {
    uint base = particle * 1u;
    return cyVfxUnpackF32(cyVfxParticles[cyVfxBase_emission + base + 0u]);
}
void cyVfxStore_emission(uint particle, float value) {
    uint base = particle * 1u;
    uint w0 = 0u;
    cyVfxPackLane(w0, 0u, asuint(value), 32u);
    cyVfxParticles[cyVfxBase_emission + base + 0u] = w0;
}
// `scratch` was written and never read: not allocated.

// --- The event channels this emitter raises on, with their declared per-frame bounds. ---
static const uint cyVfxChannel_collision = 0u;
void cyVfxRaise_collision(uint particle, float rank) {
    uint slot = 0;
    InterlockedAdd(cyVfxCounts[CY_VFX_COUNT_EVENTS_BASE + 0u], 1u, slot);
    if (slot >= 64u || slot >= CY_VFX_EVENT_STRIDE) { return; }
    CyVfxEvent raised;
    raised.source = particle;
    raised.depth = 0u;
    raised.rank = rank;
    raised.payload = rank;
    cyVfxEvents[0u * CY_VFX_EVENT_STRIDE + slot] = raised;
}

// Generated by cy::vfx. Emitter `sparks0`, stages Spawn, IR digest 0xd05cdcda04162c60.
void cyVfxKernel_sparks0_1(uint particle) {
    // Uniform across the dispatch.
    float v0 = (cyVfxParams.intensity * 16.0);
    cyVfxCounts[CY_VFX_COUNT_SPAWN_REQUEST] = uint(max(v0, 0.0));
}

// Generated by cy::vfx. Emitter `sparks0`, stages Initialise Update, IR digest 0x11492d7a38be0d2a.
void cyVfxKernel_sparks0_6(uint particle) {
    // Uniform across the dispatch.
    float3 v0 = (cyVfxInput.dt * float3(0.0, -9.81000042, 0.0));
    // Per particle.
    float v1 = (2.0 * cyVfxRandom(particle, 22u));
    float v2 = (v1 - 1.0);
    float v3 = (v2 * 0.620000005);
    float v4 = (cyVfxRandom(particle, 17u) * 3.5999999);
    float v5 = (2.5999999 + v4);
    float v6 = (2.0 * cyVfxRandom(particle, 26u));
    float v7 = (v6 - 1.0);
    float v8 = (0.620000005 * v7);
    float3 v9 = float3(v3, v5, v8);
    float3 v10 = (v9 + v0);
    float3 v11 = (cyVfxInput.dt * v10);
    float v12 = (2.0 * cyVfxRandom(particle, 4u));
    float v13 = (v12 - 1.0);
    float v14 = (2.0 * cyVfxRandom(particle, 7u));
    float v15 = (v14 - 1.0);
    float v16 = (2.0 * cyVfxRandom(particle, 10u));
    float v17 = (v16 - 1.0);
    float3 v18 = float3(v13, v15, v17);
    float3 v19 = (v18 * 0.159999996);
    float3 v20 = (v11 + v19);
    float v21 = (cyVfxRandom(particle, 35u) * 0.899999976);
    float v22 = (v21 + 0.550000012);
    float v23 = (cyVfxInput.dt / v22);
    float v24 = saturate(v23);
    float v25 = lerp(0.219999999, 0.0199999996, v24);
    float4 v26 = float4(1.0, 0.419999987, 0.119999997, 0.899999976);
    float v27 = lerp(14000.0, 300.0, v24);
    float v28 = dot(v19, float3(0.0, 1.0, 0.0));
    bool v29 = (cyVfxInput.dt > v22);
    cyVfxStore_position(particle, v20);
    cyVfxStore_velocity(particle, v10);
    cyVfxStore_age(particle, cyVfxInput.dt);
    cyVfxStore_lifetime(particle, v22);
    cyVfxStore_size(particle, v25);
    cyVfxStore_color(particle, v26);
    cyVfxStore_emission(particle, v27);
    // `scratch` is written and never read: elided by attribute liveness.
    if (v28 != 0.0) { cyVfxRaise_collision(particle, v28); }
    if (v29) { cyVfxKill(particle); }
}

// Generated by cy::vfx. Emitter `sparks0`, stages Update, IR digest 0x91f6c2e27a15489f.
void cyVfxKernel_sparks0_4(uint particle) {
    // Uniform across the dispatch.
    float3 v0 = (cyVfxInput.dt * float3(0.0, -9.81000042, 0.0));
    // Per particle.
    float3 v1 = (v0 + cyVfxLoad_velocity(particle));
    float3 v2 = (cyVfxInput.dt * v1);
    float3 v3 = (v2 + cyVfxLoad_position(particle));
    float v4 = (cyVfxInput.dt + cyVfxLoad_age(particle));
    float v5 = (v4 / cyVfxLoad_lifetime(particle));
    float v6 = saturate(v5);
    float v7 = lerp(0.219999999, 0.0199999996, v6);
    float v8 = lerp(14000.0, 300.0, v6);
    float v9 = dot(float3(0.0, 1.0, 0.0), cyVfxLoad_position(particle));
    bool v10 = (v4 > cyVfxLoad_lifetime(particle));
    cyVfxStore_velocity(particle, v1);
    cyVfxStore_position(particle, v3);
    cyVfxStore_age(particle, v4);
    cyVfxStore_size(particle, v7);
    cyVfxStore_emission(particle, v8);
    if (v9 != 0.0) { cyVfxRaise_collision(particle, v9); }
    if (v10) { cyVfxKill(particle); }
}


// --- The dispatch entry point. One program, four passes, selected by the pushed
//     `pass` word; the two that run over the population are dispatched INDIRECTLY from
//     counts this program and the support dispatches maintain. ---
[shader("compute")]
[numthreads(64, 1, 1)]
void cyVfxKernel(uint3 thread : SV_DispatchThreadID) {
    cyVfxInput = cyVfxPush;
    cyVfxLoadParams();
    uint tid = thread.x;
    if (cyVfxInput.pass == CY_VFX_PASS_SPAWN) {
        if (tid != 0u) { return; }
        cyVfxInput.particle_index = 0.0;
        cyVfxKernel_sparks0_1(0u);
        return;
    }
    if (cyVfxInput.pass == CY_VFX_PASS_INITIALISE) {
        if (tid >= cyVfxCounts[CY_VFX_COUNT_SPAWN_GRANTED]) { return; }
        uint particle = cyVfxFree[tid];
        cyVfxInput.particle_index = float(particle);
        cyVfxInput.spawn_index = float(tid);
        cyVfxInput.normalised_age = 0.0;
        cyVfxKernel_sparks0_6(particle);
        cyVfxAlive[particle] = 1u;
        uint ignored = 0u;
        InterlockedAdd(cyVfxCounts[CY_VFX_COUNT_SPAWNED], 1u, ignored);
        return;
    }
    if (cyVfxInput.pass == CY_VFX_PASS_KEYS) {
        if (tid >= cyVfxCounts[CY_VFX_COUNT_LIVE]) { return; }
        uint particle = cyVfxIndices[tid];
        float3 p = cyVfxLoad_position(particle);
        float d = dot(p, p);
        cyVfxKeys[tid] = ~asuint(d);
        return;
    }
    if (tid >= cyVfxCounts[CY_VFX_COUNT_LIVE]) { return; }
    uint particle = cyVfxIndices[tid];
    cyVfxInput.particle_index = float(particle);
    cyVfxKernel_sparks0_4(particle);
}
