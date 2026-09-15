#pragma once
// THE FIVE STOCHASTIC SIGNALS, PRODUCED. M11.c task 2.5.
//
// ================================================================================================
// WHAT WAS WRONG, STATED AS A FACT ABOUT THE TREE RATHER THAN AS A GAP
// ================================================================================================
//
// `denoising` declares five signals — indirect diffuse, indirect specular, ray-traced shadows,
// ambient occlusion and stochastic direct lighting — and gives each of them a `SignalConfig`, a
// history slot and a per-signal diagnostic. **Nothing in this tree ever called
// `Denoiser::denoise()` outside the denoiser's own two suites.** `src/rendering/gi/` was the only
// module that named `Denoiser` at all and what it did with it was set its quality position from a
// budget lever. So the framework's per-signal requirements — the visibility domain, the lobe shape,
// the roughness widening, the history length — were exercised by buffers written by the test that
// asserted on them, and "the framework handles five signals" was readable off an enumerator.
//
// `denoising`'s own new requirement says the honest choices are two: *"Every signal the framework
// declares SHALL have a producer that routes through the framework, or SHALL be removed from the
// declaration."* This file is the first choice, taken for all five.
//
// ================================================================================================
// WHY THE PRODUCER LIVES HERE AND NOT IN THE DENOISER
// ================================================================================================
//
// Because a producer written inside the denoiser is the synthetic buffer again with a longer name.
// Every one of these five is a stochastic estimate of something the ILLUMINATION SYSTEM knows: a
// traced ray, a sampled light, a visibility query. They are produced from the GI scene, the tiered
// tracer and the surface cache — the same objects `indirect_diffuse()` resolves through — and are
// handed to `denoise::Denoiser` across the plain-span seam the denoiser already declares. The
// denoiser stays a module that computes no history and knows no scene.
//
// ================================================================================================
// ONE RAY A PIXEL, AND WHY THAT IS THE POINT RATHER THAN A SHORTCUT
// ================================================================================================
//
// A denoiser exists because a renderer cannot afford many. Each producer below takes ONE stochastic
// sample per pixel per frame and lets temporal accumulation and the a-trous cascade do the rest,
// which is what the framework is for and what makes `Diagnostics::mean_variance` mean something. A
// producer that averaged sixteen samples before handing the buffer over would be measuring a
// filter's behaviour on a signal no frame produces.
//
// Every direction is drawn from a DETERMINISTIC sequence keyed by pixel and frame — the same
// golden-ratio construction the probe gather uses — so two runs of a frame agree. A stochastic
// signal whose noise is not reproducible is a signal no golden image and no regression test can
// judge.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/denoise/denoiser.h>
#include <cy/rendering/gi/lighting.h>
#include <cy/rendering/gi/system.h>

namespace cy::rendering::gi {

/// The visibility buffer the producers read. All spans are `width * height` and indexed
/// `y * width + x`, which is `denoise::GuidanceBuffers`' convention because these become it.
struct SignalSurfaces {
    u32 width = 0;
    u32 height = 0;
    /// View-space depth in metres, positive in front of the eye. Zero or negative means sky, and a
    /// sky pixel produces no signal at all rather than a black one — the difference is what
    /// `SignalProduction::pixels` counts.
    Span<const f32> depth;
    /// World position of the surface at each pixel.
    Span<const Vec3> position;
    Span<const Vec3> normal;
    Span<const f32> roughness;
    /// The visibility buffer's identities, where the renderer has them. Empty is allowed and the
    /// filter falls back to depth, normal and roughness — `Diagnostics::identity_available` says
    /// which happened.
    Span<const u32> instance_id;
    Span<const u32> material_id;
    Vec3 camera{0.0F, 0.0F, 0.0F};
    /// How far an ambient-occlusion ray reaches, in metres. AO is a local term; a ray that reached
    /// the whole scene would be a second, worse indirect diffuse.
    f32 occlusion_radius_metres = 2.0F;
};

/// What one frame of production did, per signal.
///
/// `denoising`: *"The check SHALL name which signals have producers and which do not, so that 'the
/// framework handles five signals' cannot be read off an enumerator."* `produced` is that list, and
/// it is filled in from what actually ran rather than from the enumerator's own range.
struct SignalProduction {
    bool produced[denoise::kSignalCount] = {};
    /// Pixels that carried a surface and therefore a sample. Zero with `produced` true would be a
    /// producer that ran over an empty frame, which is a different report from one that did not run.
    u32 pixels[denoise::kSignalCount] = {};
    u32 rays[denoise::kSignalCount] = {};
    /// The luminance variance of the noisy input and of what came back. The second must be lower or
    /// the framework did nothing, which is the only claim a denoiser really makes.
    f32 noisy_variance[denoise::kSignalCount] = {};
    f32 reconstructed_variance[denoise::kSignalCount] = {};
};

/// The producers.
///
/// Not thread-safe and one per view, for the same reason `denoise::Denoiser` is: the buffers are
/// this view's and a second view gets a second instance.
class StochasticSignals {
public:
    StochasticSignals() noexcept = default;

    /// Size the noisy buffers. Idempotent for an unchanged size.
    [[nodiscard]] Status resize(u32 width, u32 height) noexcept;
    [[nodiscard]] u32 width() const noexcept { return width_; }
    [[nodiscard]] u32 height() const noexcept { return height_; }

    /// Produce all five signals and route each through the denoiser the illumination system holds.
    ///
    /// `history` is `temporal-rendering`'s answer and is passed straight through: this module
    /// computes no reprojection either, for the same reason the denoiser does not.
    [[nodiscard]] Expected<SignalProduction, Error> produce(
        const SignalSurfaces& surfaces, Span<const GiLight> lights, IlluminationSystem& system,
        const denoise::HistoryGuidance& history, u64 frame) noexcept;

    /// The noisy buffer a signal was produced into, before the framework saw it. Valid until the
    /// next `produce`.
    [[nodiscard]] Span<const Vec3> noisy(denoise::SignalKind kind) const noexcept;

    /// What the framework handed back. Valid until the next `produce` for the same signal.
    [[nodiscard]] Span<const Vec3> reconstructed(denoise::SignalKind kind) const noexcept;

private:
    struct SignalBuffer {
        Array<Vec3> values;
        Array<f32> samples;
        Span<const Vec3> reconstructed;
        u32 pixels = 0;
        u32 rays = 0;
    };

    /// One pixel's five samples, so the per-signal loops below are each one expression rather than
    /// five interleaved ones.
    struct PixelInputs {
        Vec3 position;
        Vec3 normal;
        Vec3 view;
        f32 roughness = 0.0F;
        u32 index = 0;
        u64 frame = 0;
    };

    void produce_indirect(const PixelInputs& pixel, IlluminationSystem& system) noexcept;
    void produce_visibility(const PixelInputs& pixel, Span<const GiLight> lights,
                            const IlluminationSystem& system, f32 occlusion_radius) noexcept;
    void produce_direct(const PixelInputs& pixel, Span<const GiLight> lights,
                        const IlluminationSystem& system) noexcept;
    [[nodiscard]] Status route(denoise::SignalKind kind, const SignalSurfaces& surfaces,
                               const denoise::HistoryGuidance& history, denoise::Denoiser& denoiser,
                               SignalProduction& report) noexcept;

    u32 width_ = 0;
    u32 height_ = 0;
    SignalBuffer buffers_[denoise::kSignalCount];
};

/// The luminance variance of a buffer over the pixels a mask marks as carrying a surface. Exposed
/// because the claim "the framework reduced the noise" is checked with it, and a test that computed
/// its own would be checking a second statistic.
[[nodiscard]] f32 luminance_variance(Span<const Vec3> values, Span<const f32> depth) noexcept;

}  // namespace cy::rendering::gi
