// SPDX-License-Identifier: MIT
// CyberGI on the device: the denoiser with no cascade to run — the screen probes' probe-space
// denoiser, or any signal whose configuration asks for no spatial pass. Issue #35.
//
//   cyGiDenoiseTemporal   `Denoiser::accumulate` for both signals of one pixel, and — because
//                         `Denoiser::filter` with no pass returns the accumulation itself, and the
//                         variance only steers a pass — nothing else of the denoiser: no moments,
//                         no variance, no cascade. On the full-resolution grid the pixel is its own
//                         frame pixel, so the pass is also `screen_upsample` at a divisor of one
//                         and writes the two targets the frame samples; on a coarser grid it hands
//                         the accumulation to `cyGiPublish`, which upsamples it.
//
// ONE PASS FOR WHAT WAS FIVE STAGES, AND THE REASON IS BANDWIDTH. At 1920x1080 the shared
// denoiser's accumulation, variance, one filter pass and publish read and wrote about a gigabyte a
// frame between them, 3.1 ms on the RTX 5060; with no cascade, the moments and the variance are
// never read and the filtered buffer is a copy. This pass reads the integrated signal, the history
// source and the history, and writes the history and the targets: a quarter of the traffic.
//
// THE HISTORY HAS ITS OWN LAYOUT HERE: one float4 a pixel a signal a slot, [accumulated, samples],
// contiguous — `gi_denoise.slang`'s keeps the moments beside it in a second float4, and a pass that
// wrote every other float4 of that layout paid for whole lines it half wrote.
// `GpuScreenResolve` starts the history over whenever a frame moves between the two layouts.

static const uint kGtCounts = 0u;     // grid width, grid height, grid pixels, current slot
static const uint kGtIntensity = 5u;  // diffuse, specular, depth tolerance, history valid
static const uint kGtSignals = 20u;   // three uint4 per signal: `gi_denoise.slang`'s
static const uint kGtFull = 36u;      // frame width, frame height, grid divisor, flags (1: keep a copy)
static const uint kGtSignalCount = 2u;

struct GiTemporalSet {
    StructuredBuffer<uint4> constants;
    /// The resolve's two signals: [radiance, samples] per pixel per signal, interleaved.
    StructuredBuffer<float4> noisy;
    /// Two words a pixel: the history pixel as int bits (-1 none), and its confidence as float bits.
    StructuredBuffer<uint> sources;
    /// Two slots, each two signals, each one float4 a pixel: [accumulated, samples].
    RWStructuredBuffer<float4> history;
    /// The denoised signals on the grid, diffuse then specular: what `cyGiPublish` upsamples on a
    /// coarser grid, and what a test reads back.
    RWStructuredBuffer<float4> denoised;
    /// The published signals a frame pixel, when the flag asks for a copy.
    RWStructuredBuffer<float4> published;
    [format("rgba16f")] RWTexture2D<float4> diffuse;
    [format("rgba16f")] RWTexture2D<float4> specular;
};

[[vk::binding(0, 0)]] ParameterBlock<GiTemporalSet> giTemporal;

uint gtHistoryRecord(uint slot, uint signal, uint pixel) {
    return ((slot * kGtSignalCount) + signal) * giTemporal.constants[kGtCounts].z + pixel;
}

float gtClamp(float value, float lo, float hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

/// `signal_scalar`: luminance for a radiance signal, the first channel for a visibility one.
float gtScalar(float3 value, uint domain) {
    if (domain == 1u) {
        return value.x;
    }
    return (0.2126f * value.x) + (0.7152f * value.y) + (0.0722f * value.z);
}

/// `history_kept` in denoiser.cpp: the share of its count a history keeps against a new sample.
float gtHistoryKept(float3 history, float3 value, uint signal) {
    const uint4 words = giTemporal.constants[kGtSignals + (signal * 3u) + 2u];
    const float rejection = asfloat(words.z);
    if (rejection <= 0.0f) {
        return 1.0f;
    }
    const float before = gtScalar(history, words.y);
    const float now = gtScalar(value, words.y);
    const float deviation = abs(now - before);
    const float allowed = rejection * max(abs(now), abs(before));
    if (deviation <= allowed) {
        return 1.0f;
    }
    const float ratio = allowed / deviation;
    return ratio * ratio;
}

/// `Denoiser::accumulate` for one signal of one pixel, its moments aside.
float3 gtAccumulate(uint signal, uint pixel, int source, float confidence) {
    const uint4 counts = giTemporal.constants[kGtCounts];
    const float historyLength = asfloat(giTemporal.constants[kGtSignals + (signal * 3u) + 1u].y);
    const float alphaFloor = historyLength > 0.0f ? 1.0f / historyLength : 1.0f;
    const float4 noisy = giTemporal.noisy[(pixel * 2u) + signal];
    const float3 value = noisy.xyz;
    const float fresh = noisy.w;
    const uint out = gtHistoryRecord(counts.w, signal, pixel);
    if (source < 0 || confidence <= 0.0f) {
        giTemporal.history[out] = float4(value, fresh);
        return value;
    }
    const float4 colour = giTemporal.history[gtHistoryRecord(1u - counts.w, signal, uint(source))];
    const float carried = (colour.w * confidence) * gtHistoryKept(colour.xyz, value, signal);
    const float count = carried + fresh;
    const float alpha = max(count > 0.0f ? fresh / count : 1.0f, alphaFloor);
    const float3 blended = colour.xyz + ((value - colour.xyz) * alpha);
    giTemporal.history[out] = float4(blended, min(count, historyLength));
    return blended;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cyGiDenoiseTemporal(uint3 thread : SV_DispatchThreadID) {
    const uint4 counts = giTemporal.constants[kGtCounts];
    if (thread.x >= counts.x || thread.y >= counts.y) {
        return;
    }
    const uint pixel = (thread.y * counts.x) + thread.x;
    int source = -1;
    float confidence = 0.0f;
    if (giTemporal.constants[kGtIntensity].w != 0u) {
        source = asint(giTemporal.sources[pixel * 2u]);
        if (source >= 0 && uint(source) < counts.z) {
            confidence = gtClamp(asfloat(giTemporal.sources[(pixel * 2u) + 1u]), 0.0f, 1.0f);
        } else {
            source = -1;
        }
    }
    const float3 diffuse = gtAccumulate(0u, pixel, source, confidence);
    const float3 specular = gtAccumulate(1u, pixel, source, confidence);
    const uint4 full = giTemporal.constants[kGtFull];
    const bool copy = (full.w & 1u) != 0u;
    if (max(full.z, 1u) != 1u || copy) {
        giTemporal.denoised[pixel] = float4(diffuse, 0.0f);
        giTemporal.denoised[counts.z + pixel] = float4(specular, 0.0f);
    }
    if (max(full.z, 1u) != 1u) {
        return;  // `cyGiPublish` upsamples the grid onto the frame's pixels
    }
    // `screen_upsample` at a divisor of one: every pixel is its own sample, and a sky pixel — no
    // surface, no samples — is zero, as the resolve left it.
    giTemporal.diffuse[thread.xy] = float4(diffuse, 1.0f);
    giTemporal.specular[thread.xy] = float4(specular, 1.0f);
    if (copy) {
        giTemporal.published[pixel] = float4(diffuse, 0.0f);
        giTemporal.published[(full.x * full.y) + pixel] = float4(specular, 0.0f);
    }
}
