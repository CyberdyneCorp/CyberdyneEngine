// SPDX-License-Identifier: MIT
// CyberGI on the device, stage 4: the resolve's two signals through the shared denoiser. Issue #35.
//
//   cyGiReproject           `screen_history`: the pixel of last frame each pixel lands on, through
//                           `derive_surface_motion` and `classify_history`.
//   cyGiDenoiseAccumulate   `Denoiser::accumulate`: the temporal blend over the reprojected history.
//   cyGiDenoiseVariance     `Denoiser::estimate_variance`, and the cascade's first input.
//   cyGiDenoiseFilter       one pass of `Denoiser::filter`'s a-trous cascade, `filter_pixel` and
//                           `tap_weight` expression for expression.
//
// ================================================================================================
// THIS IS `cy::rendering::denoise::Denoiser`, NOT A SECOND FILTER — GTAO's PATTERN, WITH HISTORY
// ================================================================================================
//
// `gtao_filter.slang` transcribed the denoiser's spatial stage for a signal that keeps no history.
// The resolve's signals do keep one, so this transcribes all four stages: accumulation over the
// reprojected history with the blend weight falling as the sample count rises and floored by the
// signal's history length; the luminance moments and the variance they give, taken spatially where
// a pixel has fewer than four samples; and the cascade, whose passes stop on depth, normal,
// roughness and the value itself at `sigma_value` standard deviations, and which leaves a
// converged pixel alone. Each signal's `SignalConfig` and the quality ladder's position arrive as
// constant words read off `Denoiser::config()` and `quality_ladder()` — the numbers are the
// host's, not this file's — and `render.gi_gpu_resolve` runs the same noisy input through
// `Denoiser::denoise` and compares the two buffers.
//
// `SignalKind` carries no behaviour here either: a dispatch covers both signals, and the only thing
// the signal index selects is a configuration and a slice of each buffer.

// Self-contained: the denoiser reads no scene, and its set is set 0 of its own pipelines.

static const uint kGdCounts = 0u;          // width, height, pixels, current slot
static const uint kGdIntensity = 5u;       // diffuse, specular, depth tolerance, history valid
static const uint kGdWorldToClip = 6u;     // four columns
static const uint kGdPreviousToClip = 10u; // four columns
static const uint kGdSignals = 20u;        // three uint4 per signal
static const uint kGdInvalid = 26u;        // x region count; then two uint4 a region: min, max
static const uint kGdMaxRegions = 4u;      // `kScreenHistoryRegions`

static const uint kGdSignalCount = 2u;
static const float kGdEpsilon = 1.0e-6f;
/// `kConvergedSamples` and `kConvergedRelativeDeviation` in denoiser.cpp.
static const float kGdConvergedSamples = 12.0f;
static const float kGdConvergedDeviation = 0.02f;

struct GiDenoiseSet {
    StructuredBuffer<uint4> constants;
    /// The resolve's surface slots: [world position, view depth], [normal, roughness].
    StructuredBuffer<float4> surfaces;
    /// The resolve's two signals: [radiance, samples] per pixel per signal, interleaved.
    StructuredBuffer<float4> noisy;
    /// Two words a pixel: the history pixel as int bits (-1 none), and its confidence as float bits.
    RWStructuredBuffer<uint> sources;
    /// Two slots, each two signals, each two float4 a pixel: [accumulated, samples], [m1, m2, 0, 0].
    RWStructuredBuffer<float4> history;
    /// One float a pixel per signal.
    RWStructuredBuffer<float> variance;
    /// The cascade's two buffers, one float4 a pixel per signal.
    RWStructuredBuffer<float4> filterA;
    RWStructuredBuffer<float4> filterB;
};

struct GiDenoisePush {
    /// Items: pixels for the reprojection, pixels times signals for the rest.
    uint count;
    /// The cascade pass, and with it the step: 1 << pass.
    uint pass;
    /// 0: read A write B; 1: read B write A.
    uint direction;
    uint reserved;
};

[[vk::binding(0, 0)]] ParameterBlock<GiDenoiseSet> giDenoise;
[[vk::push_constant]] GiDenoisePush gdPush;

// --- Small things ---------------------------------------------------------------------------------

float gdClamp(float value, float lo, float hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

/// `signal_scalar` for a radiance signal; a visibility one reads its first channel.
float gdScalar(float3 value, uint domain) {
    if (domain == 1u) {
        return value.x;
    }
    return (0.2126f * value.x) + (0.7152f * value.y) + (0.0722f * value.z);
}

float gdSpline(int offset) {
    const int magnitude = offset < 0 ? -offset : offset;
    return magnitude == 0 ? 6.0f / 16.0f
                          : (magnitude == 1 ? 4.0f / 16.0f : (magnitude == 2 ? 1.0f / 16.0f : 0.0f));
}

/// `stable_variance`.
float gdStableVariance(float first, float second) {
    const float variance = max(0.0f, second - (first * first));
    const float roundoff = kGdEpsilon * max(1.0f, abs(second));
    return variance <= roundoff ? 0.0f : variance;
}

uint gdPixels() {
    return giDenoise.constants[kGdCounts].z;
}

uint gdHistoryRecord(uint slot, uint signal, uint pixel) {
    return (((slot * kGdSignalCount) + signal) * gdPixels() + pixel) * 2u;
}

uint gdSurfaceRecord(uint pixel) {
    const uint4 counts = giDenoise.constants[kGdCounts];
    return ((counts.w * counts.z) + pixel) * 2u;
}

// --- The reprojection: `screen_history` ------------------------------------------------------------

/// `invalidated_at` in screen_resolve.cpp.
bool gdInvalidated(float3 point) {
    const uint count = min(giDenoise.constants[kGdInvalid].x, kGdMaxRegions);
    for (uint region = 0u; region < count; ++region) {
        const float3 lo = asfloat(giDenoise.constants[kGdInvalid + 1u + (region * 2u)].xyz);
        const float3 hi = asfloat(giDenoise.constants[kGdInvalid + 2u + (region * 2u)].xyz);
        if (point.x >= lo.x && point.y >= lo.y && point.z >= lo.z && point.x <= hi.x &&
            point.y <= hi.y && point.z <= hi.z) {
            return true;
        }
    }
    return false;
}

/// `to_screen` in temporal/src/motion.cpp: clip space to [0,1]^2, y up, or false behind the eye.
bool gdToScreen(uint first, float3 world, out float2 uv) {
    const float4 c0 = asfloat(giDenoise.constants[first]);
    const float4 c1 = asfloat(giDenoise.constants[first + 1u]);
    const float4 c2 = asfloat(giDenoise.constants[first + 2u]);
    const float4 c3 = asfloat(giDenoise.constants[first + 3u]);
    const float4 clip = c0 * world.x + c1 * world.y + c2 * world.z + c3 * 1.0f;
    uv = float2(0.0f, 0.0f);
    if (clip.w <= 1e-8f) {
        return false;
    }
    const float inverse = 1.0f / clip.w;
    uv = float2(((clip.x * inverse) * 0.5f) + 0.5f, ((clip.y * inverse) * 0.5f) + 0.5f);
    return true;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cyGiReproject(uint3 thread : SV_DispatchThreadID) {
    const uint pixel = thread.x;
    const uint4 counts = giDenoise.constants[kGdCounts];
    if (pixel >= counts.z) {
        return;
    }
    giDenoise.sources[pixel * 2u] = asuint(-1);
    giDenoise.sources[(pixel * 2u) + 1u] = asuint(0.0f);
    const float4 at = giDenoise.surfaces[gdSurfaceRecord(pixel)];
    const bool history = giDenoise.constants[kGdIntensity].w != 0u;
    if (!history || at.w <= 0.0f) {
        return;
    }
    // `derive_surface_motion`, the surface taken to be where it was.
    float2 current;
    float2 previous;
    bool representable = false;
    float2 currentUv = float2(0.0f, 0.0f);
    float2 motion = float2(0.0f, 0.0f);
    if (gdToScreen(kGdWorldToClip, at.xyz, current)) {
        currentUv = current;
        if (gdToScreen(kGdPreviousToClip, at.xyz, previous)) {
            motion = float2(previous.x - current.x, previous.y - current.y);
            representable = true;
        }
    }
    const float2 uv = float2(currentUv.x + motion.x, currentUv.y + motion.y);
    const float width = float(counts.x);
    const float height = float(counts.y);
    const float fx = floor(uv.x * width);
    const float fy = floor((1.0f - uv.y) * height);
    const bool onScreen = fx >= 0.0f && fy >= 0.0f && fx < width && fy < height;
    const uint landing = onScreen ? (uint(fy) * counts.x) + uint(fx) : 0u;
    const uint previousSlot = 1u - counts.w;
    const float historyDepth =
        onScreen ? giDenoise.surfaces[((previousSlot * counts.z) + landing) * 2u].w : 0.0f;
    // `classify_history`.
    if (!representable) {
        return;
    }
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) {
        return;
    }
    const float tolerance = asfloat(giDenoise.constants[kGdIntensity].z);
    const float reference = max(min(at.w, historyDepth), 1e-4f);
    const float difference = abs(at.w - historyDepth);
    if (difference > reference * tolerance || !onScreen || historyDepth <= 0.0f ||
        gdInvalidated(at.xyz)) {
        return;
    }
    giDenoise.sources[pixel * 2u] = landing;
    giDenoise.sources[(pixel * 2u) + 1u] = asuint(1.0f);
}

// --- Accumulation: `Denoiser::accumulate` ----------------------------------------------------------

[shader("compute")]
[numthreads(64, 1, 1)]
void cyGiDenoiseAccumulate(uint3 thread : SV_DispatchThreadID) {
    const uint pixels = gdPixels();
    if (thread.x >= gdPush.count) {
        return;
    }
    const uint signal = thread.x / pixels;
    const uint pixel = thread.x - (signal * pixels);
    const uint4 words = giDenoise.constants[kGdSignals + (signal * 3u) + 1u];
    const uint domain = giDenoise.constants[kGdSignals + (signal * 3u) + 2u].y;
    const float historyLength = asfloat(words.y);
    const float alphaFloor = historyLength > 0.0f ? 1.0f / historyLength : 1.0f;
    const uint current = giDenoise.constants[kGdCounts].w;

    const float4 noisy = giDenoise.noisy[(pixel * 2u) + signal];
    const float3 value = noisy.xyz;
    const float scalar = gdScalar(value, domain);
    const float fresh = noisy.w;

    int source = -1;
    float confidence = 0.0f;
    if (giDenoise.constants[kGdIntensity].w != 0u) {
        source = asint(giDenoise.sources[pixel * 2u]);
        if (source >= 0 && uint(source) < pixels) {
            confidence = gdClamp(asfloat(giDenoise.sources[(pixel * 2u) + 1u]), 0.0f, 1.0f);
        } else {
            source = -1;
        }
    }

    const uint out = gdHistoryRecord(current, signal, pixel);
    if (source < 0 || confidence <= 0.0f) {
        giDenoise.history[out] = float4(value, fresh);
        giDenoise.history[out + 1u] = float4(scalar, scalar * scalar, 0.0f, 0.0f);
        return;
    }
    const uint from = gdHistoryRecord(1u - current, signal, uint(source));
    const float4 colour = giDenoise.history[from];
    const float4 moments = giDenoise.history[from + 1u];
    const float carried = colour.w * confidence;
    const float count = carried + fresh;
    const float alpha = max(count > 0.0f ? fresh / count : 1.0f, alphaFloor);
    const float3 blended = colour.xyz + ((value - colour.xyz) * alpha);
    const float first = moments.x + ((scalar - moments.x) * alpha);
    const float second = moments.y + (((scalar * scalar) - moments.y) * alpha);
    giDenoise.history[out] = float4(blended, min(count, historyLength));
    giDenoise.history[out + 1u] = float4(first, second, 0.0f, 0.0f);
}

// --- Variance: `Denoiser::estimate_variance` ---------------------------------------------------------

[shader("compute")]
[numthreads(64, 1, 1)]
void cyGiDenoiseVariance(uint3 thread : SV_DispatchThreadID) {
    const uint pixels = gdPixels();
    if (thread.x >= gdPush.count) {
        return;
    }
    const uint signal = thread.x / pixels;
    const uint pixel = thread.x - (signal * pixels);
    const uint4 counts = giDenoise.constants[kGdCounts];
    const uint current = counts.w;
    const uint record = gdHistoryRecord(current, signal, pixel);
    const float4 colour = giDenoise.history[record];
    const float4 moments = giDenoise.history[record + 1u];
    float variance = gdStableVariance(moments.x, moments.y);
    if (colour.w < 4.0f) {
        // A pixel with almost no history takes its variance spatially, from the 3x3 of the first
        // moment around it.
        const int x = int(pixel % counts.x);
        const int y = int(pixel / counts.x);
        float sum = 0.0f;
        float squares = 0.0f;
        float taken = 0.0f;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int sx = x + dx;
                const int sy = y + dy;
                if (sx < 0 || sy < 0 || sx >= int(counts.x) || sy >= int(counts.y)) {
                    continue;
                }
                const uint neighbour = (uint(sy) * counts.x) + uint(sx);
                const float value = giDenoise.history[gdHistoryRecord(current, signal, neighbour) + 1u].x;
                sum += value;
                squares += value * value;
                taken += 1.0f;
            }
        }
        const float mean = sum / taken;
        variance = max(variance, gdStableVariance(mean, squares / taken));
    }
    giDenoise.variance[(signal * pixels) + pixel] = variance;
    // The cascade starts from the accumulated signal.
    giDenoise.filterA[(signal * pixels) + pixel] = float4(colour.xyz, 0.0f);
}

// --- The cascade: `Denoiser::filter_pixel` and `tap_weight` ----------------------------------------

float4 gdFilterRead(uint index) {
    return gdPush.direction == 0u ? giDenoise.filterA[index] : giDenoise.filterB[index];
}

void gdFilterWrite(uint index, float4 value) {
    if (gdPush.direction == 0u) {
        giDenoise.filterB[index] = value;
    } else {
        giDenoise.filterA[index] = value;
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cyGiDenoiseFilter(uint3 thread : SV_DispatchThreadID) {
    const uint pixels = gdPixels();
    if (thread.x >= gdPush.count) {
        return;
    }
    const uint signal = thread.x / pixels;
    const uint pixel = thread.x - (signal * pixels);
    const uint4 counts = giDenoise.constants[kGdCounts];
    const uint base = signal * pixels;
    const float4 centre = gdFilterRead(base + pixel);
    const uint4 shape = giDenoise.constants[kGdSignals + (signal * 3u) + 1u];
    // A signal whose cascade is shorter than the other's carries its answer through the passes it
    // does not take, so both end in the same buffer.
    if (gdPush.pass >= shape.z) {
        gdFilterWrite(base + pixel, centre);
        return;
    }
    const float4 surface = giDenoise.surfaces[gdSurfaceRecord(pixel)];
    const float depth = surface.w;
    if (depth <= 0.0f) {
        gdFilterWrite(base + pixel, centre);  // a sky pixel has no surface to stop on
        return;
    }
    const float4 sigma = asfloat(giDenoise.constants[kGdSignals + (signal * 3u)]);
    const uint domain = giDenoise.constants[kGdSignals + (signal * 3u) + 2u].y;
    const float centreValue = gdScalar(centre.xyz, domain);
    const float deviation = sqrt(giDenoise.variance[base + pixel]);
    const float samples = giDenoise.history[gdHistoryRecord(counts.w, signal, pixel)].w;
    if (samples >= kGdConvergedSamples &&
        deviation <= kGdConvergedDeviation * (abs(centreValue) + kGdEpsilon)) {
        gdFilterWrite(base + pixel, centre);
        return;
    }
    const float4 facing = giDenoise.surfaces[gdSurfaceRecord(pixel) + 1u];
    const float roughness = facing.w;
    const float3 normal = facing.xyz;
    const float widening = asfloat(shape.x);
    const float widened = float(1u << gdPush.pass) * (1.0f + (widening * roughness));
    const int step = int(max(1u, uint(floor(widened + 0.5f))));
    const int extent = int(shape.w);
    const int x = int(pixel % counts.x);
    const int y = int(pixel / counts.x);

    float3 sum = float3(0.0f, 0.0f, 0.0f);
    float weightSum = 0.0f;
    for (int dy = -extent; dy <= extent; ++dy) {
        for (int dx = -extent; dx <= extent; ++dx) {
            const int sx = x + (dx * step);
            const int sy = y + (dy * step);
            if (sx < 0 || sy < 0 || sx >= int(counts.x) || sy >= int(counts.y)) {
                continue;
            }
            const uint tap = (uint(sy) * counts.x) + uint(sx);
            const uint tapRecord = gdSurfaceRecord(tap);
            const float tapDepth = giDenoise.surfaces[tapRecord].w;
            if (tapDepth <= 0.0f) {
                continue;
            }
            float weight = gdSpline(dx) * gdSpline(dy);
            const float depthDifference = abs(tapDepth - depth);
            weight *= exp(-depthDifference / ((sigma.x * abs(depth)) + kGdEpsilon));
            const float4 tapFacing = giDenoise.surfaces[tapRecord + 1u];
            const float alignment = max(0.0f, dot(normal, tapFacing.xyz));
            weight *= pow(alignment, sigma.y);
            weight *= exp(-abs(tapFacing.w - roughness) / (sigma.w + kGdEpsilon));
            const float4 value = gdFilterRead(base + tap);
            const float tapValue = gdScalar(value.xyz, domain);
            weight *= exp(-abs(tapValue - centreValue) / ((sigma.z * deviation) + kGdEpsilon));
            weight = max(weight, 0.0f);
            if (weight <= 0.0f) {
                continue;
            }
            sum = sum + (value.xyz * weight);
            weightSum += weight;
        }
    }
    gdFilterWrite(base + pixel, weightSum > 0.0f ? float4(sum / weightSum, 0.0f) : centre);
}
