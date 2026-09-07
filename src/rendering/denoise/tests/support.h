#pragma once
// Shared fixtures for the denoiser suites. Task 9.3.
//
// A deterministic noisy image over a two-object scene: the left half is one instance and material
// at one depth, the right half another, and the two carry values far enough apart that a filter
// crossing the boundary is visible in one number rather than in an eyeball.

#include <cy/rendering/denoise/denoiser.h>

#include <vector>

namespace denoise_support {

using cy::f32;
using cy::i32;
using cy::u32;
using cy::Vec3;

/// A 32-bit hash used as the sample generator. Deterministic across platforms and compilers, which
/// is what makes a denoiser test a test rather than a mood.
[[nodiscard]] inline f32 hashed_unit(u32 seed) noexcept {
    u32 value = (seed * 747796405U) + 2891336453U;
    value = ((value >> ((value >> 28U) + 4U)) ^ value) * 277803737U;
    value = (value >> 22U) ^ value;
    return static_cast<f32>(value & 0xFFFFFFU) / static_cast<f32>(0x1000000U);
}

struct Scene {
    u32 width = 32;
    u32 height = 32;

    std::vector<f32> depth;
    std::vector<Vec3> normal;
    std::vector<f32> roughness;
    std::vector<u32> instance_id;
    std::vector<u32> material_id;

    /// The value each half of the image converges to.
    f32 left_value = 0.20F;
    f32 right_value = 0.80F;

    explicit Scene(u32 size = 32, f32 left = 0.20F, f32 right = 0.80F)
        : width(size), height(size), left_value(left), right_value(right) {
        const u32 pixels = width * height;
        depth.resize(pixels);
        normal.resize(pixels);
        roughness.resize(pixels);
        instance_id.resize(pixels);
        material_id.resize(pixels);
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                const u32 pixel = (y * width) + x;
                const bool left_half = x < width / 2;
                // The same depth and the same normal on both sides. That is deliberate: it is what
                // makes the identity test a test of identity rather than of depth.
                depth[pixel] = 5.0F;
                normal[pixel] = Vec3{0.0F, 0.0F, 1.0F};
                roughness[pixel] = 0.1F;
                instance_id[pixel] = left_half ? 1U : 2U;
                material_id[pixel] = left_half ? 10U : 20U;
            }
        }
    }

    [[nodiscard]] cy::rendering::denoise::GuidanceBuffers guidance() const noexcept {
        cy::rendering::denoise::GuidanceBuffers buffers;
        buffers.width = width;
        buffers.height = height;
        buffers.depth = {depth.data(), depth.size()};
        buffers.normal = {normal.data(), normal.size()};
        buffers.roughness = {roughness.data(), roughness.size()};
        buffers.instance_id = {instance_id.data(), instance_id.size()};
        buffers.material_id = {material_id.data(), material_id.size()};
        return buffers;
    }

    /// The converged answer: what the noisy samples average to.
    [[nodiscard]] f32 expected(u32 x) const noexcept {
        return x < width / 2 ? left_value : right_value;
    }

    /// One frame of stochastic samples around the converged answer.
    [[nodiscard]] std::vector<Vec3> sample(u32 frame, f32 amplitude = 0.5F) const {
        std::vector<Vec3> values(static_cast<size_t>(width) * height);
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                const u32 pixel = (y * width) + x;
                const f32 noise =
                    (hashed_unit((pixel * 9781U) + (frame * 6151U)) - 0.5F) * 2.0F * amplitude;
                const f32 value = expected(x) + noise;
                values[pixel] = Vec3{value, value, value};
            }
        }
        return values;
    }
};

/// A history that reprojects every pixel onto itself: the still camera, which is the case that
/// isolates the accumulation from the reprojection.
struct StillHistory {
    std::vector<i32> source;
    std::vector<f32> confidence;

    explicit StillHistory(u32 pixels, f32 initial_confidence = 1.0F) {
        source.resize(pixels);
        confidence.assign(pixels, initial_confidence);
        for (u32 pixel = 0; pixel < pixels; ++pixel) {
            source[pixel] = static_cast<i32>(pixel);
        }
    }

    [[nodiscard]] cy::rendering::denoise::HistoryGuidance guidance() const noexcept {
        cy::rendering::denoise::HistoryGuidance history;
        history.source = {source.data(), source.size()};
        history.confidence = {confidence.data(), confidence.size()};
        return history;
    }
};

}  // namespace denoise_support
