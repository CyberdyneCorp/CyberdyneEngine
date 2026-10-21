// SPDX-License-Identifier: MIT
#pragma once
// A synthetic scene the host reference upscales: an analytic picture evaluated at every jittered
// sample a render extent takes of it, so a case knows exactly what each sample saw and what the
// supersampled output pixel IS. `integration.rendering_upscale`.
//
// THE SAMPLE POSITIONS ARE THE FRAME'S. Render texel i's sample is at i + 0.5 + (-jitter.x,
// +jitter.y) render pixels — `UpscaleConstants::sample`, which `pipeline::upload_for` jitters the
// projection by — and the jitter is the temporal framework's own sequence at the upscale's phase
// count. A reconstruction that placed the samples anywhere else converges to a displaced picture,
// which the convergence case measures against the supersampled one.

#include <cy/core/math/projection.h>
#include <cy/rendering/temporal/jitter.h>
#include <cy/rendering/upscale/upscale.h>

#include <cmath>
#include <functional>
#include <vector>

namespace cy::upscale_test {

using namespace cy::rendering;
using namespace cy::rendering::upscale;

/// What the scene shows at one point.
struct Surface {
    Vec3 colour{0.0F, 0.0F, 0.0F};
    /// Metres from the camera.
    f32 metres = 10.0F;
    /// Normalised screen space, pointing at where this surface was last frame.
    Vec2 velocity{0.0F, 0.0F};
    /// What was under the translucency at this point, when some was drawn over it.
    bool translucent = false;
    Vec3 opaque{0.0F, 0.0F, 0.0F};
};

/// The scene at a normalised screen position, this frame.
using SceneFn = std::function<Surface(Vec2 uv)>;

[[nodiscard]] inline Mat4 test_projection() noexcept {
    return perspective_reversed_z(0.9F, 16.0F / 9.0F, 0.1F, 100.0F);
}

/// The reversed-Z depth a view depth is written as: the inverse of `m32 / (d + m22)`.
[[nodiscard]] inline f32 device_depth(f32 metres) noexcept {
    const Mat4 p = test_projection();
    return (p.columns[3].z / metres) - p.columns[2].z;
}

/// One frame's samples at a render extent.
struct Samples {
    std::vector<Vec4> color;
    std::vector<f32> depth;
    std::vector<Vec2> velocity;
    std::vector<Vec4> opaque;
};

[[nodiscard]] inline Samples sample_scene(const SceneFn& scene, Extent2D render, Vec2 jitter) {
    Samples out;
    const usize pixels = static_cast<usize>(render.width) * render.height;
    out.color.resize(pixels);
    out.depth.resize(pixels);
    out.velocity.resize(pixels);
    out.opaque.resize(pixels);
    for (u32 y = 0; y < render.height; ++y) {
        for (u32 x = 0; x < render.width; ++x) {
            const Vec2 uv{
                (static_cast<f32>(x) + 0.5F - jitter.x) / static_cast<f32>(render.width),
                (static_cast<f32>(y) + 0.5F + jitter.y) / static_cast<f32>(render.height)};
            const Surface surface = scene(uv);
            const usize at = (static_cast<usize>(y) * render.width) + x;
            out.color[at] = Vec4{surface.colour.x, surface.colour.y, surface.colour.z, 1.0F};
            out.depth[at] = device_depth(surface.metres);
            out.velocity[at] = surface.velocity;
            const Vec3 under = surface.translucent ? surface.opaque : surface.colour;
            out.opaque[at] = Vec4{under.x, under.y, under.z, 1.0F};
        }
    }
    return out;
}

/// The output extent supersampled `grid` x `grid` times a pixel and box filtered: the native
/// picture an upscaler converges toward.
[[nodiscard]] inline std::vector<Vec3> supersample(const SceneFn& scene, Extent2D output,
                                                   u32 grid = 8) {
    std::vector<Vec3> out(static_cast<usize>(output.width) * output.height);
    const f32 inverse = 1.0F / static_cast<f32>(grid * grid);
    for (u32 y = 0; y < output.height; ++y) {
        for (u32 x = 0; x < output.width; ++x) {
            Vec3 sum{0.0F, 0.0F, 0.0F};
            for (u32 b = 0; b < grid; ++b) {
                for (u32 a = 0; a < grid; ++a) {
                    const Vec2 uv{(static_cast<f32>(x) +
                                   ((static_cast<f32>(a) + 0.5F) / static_cast<f32>(grid))) /
                                      static_cast<f32>(output.width),
                                  (static_cast<f32>(y) +
                                   ((static_cast<f32>(b) + 0.5F) / static_cast<f32>(grid))) /
                                      static_cast<f32>(output.height)};
                    const Vec3 c = scene(uv).colour;
                    sum = Vec3{sum.x + c.x, sum.y + c.y, sum.z + c.z};
                }
            }
            out[(static_cast<usize>(y) * output.width) + x] =
                Vec3{sum.x * inverse, sum.y * inverse, sum.z * inverse};
        }
    }
    return out;
}

/// The host reference run frame after frame, as the device runs it: the framework's jitter
/// sequence, the history and the depth it keeps.
class HostRun {
public:
    HostRun(Extent2D output, Extent2D render, const UpscaleSettings& settings)
        : output_(output), render_(render), settings_(settings) {
        jitter_.configure(jitter_config_for(output, render));
        history_.resize(static_cast<usize>(output.width) * output.height);
        state_.resize(history_.size(), Vec2{0.0F, 0.0F});
    }

    /// Advance the jitter and return where this frame's samples are.
    [[nodiscard]] Vec2 next_jitter() noexcept {
        jitter_.advance(true);
        return jitter_.current();
    }

    /// Reconstruct one frame from `samples` taken at `jitter`. `velocity` and `reactive` say
    /// whether the frame hands those inputs over; `history_valid` false is the framework's cut.
    void step(const Samples& samples, Vec2 jitter, bool velocity = true, bool reactive = false,
              bool history_valid = true) {
        UpscaleView view;
        view.render = render_;
        view.output = output_;
        view.jitter = jitter;
        view.projection = test_projection();
        view.relative_to_clip = view.projection;
        view.previous_relative_to_clip = view.projection;
        view.history_valid = written_ && history_valid;
        view.velocity = velocity;
        view.reactive = reactive;
        const Expected<UpscaleConstants, Error> constants = make_upscale_constants(settings_, view);
        if (!constants.has_value()) {
            failed_ = true;
            return;
        }
        constants_ = *constants;
        UpscaleInputs inputs;
        inputs.color = Span<const Vec4>(samples.color.data(), samples.color.size());
        inputs.depth = Span<const f32>(samples.depth.data(), samples.depth.size());
        if (velocity) {
            inputs.velocity = Span<const Vec2>(samples.velocity.data(), samples.velocity.size());
        }
        if (reactive) {
            inputs.opaque = Span<const Vec4>(samples.opaque.data(), samples.opaque.size());
        }
        inputs.history = Span<const Vec4>(history_.data(), history_.size());
        inputs.previous_state = Span<const Vec2>(state_.data(), state_.size());
        std::vector<Vec4> next(history_.size());
        std::vector<Vec2> next_state(history_.size());
        if (!upscale_reference(inputs, constants_, Span<Vec4>(next.data(), next.size()),
                               Span<Vec2>(next_state.data(), next_state.size()))) {
            failed_ = true;
            return;
        }
        history_ = next;
        state_ = next_state;
        written_ = true;
    }

    /// Sample `scene` at the next jitter and reconstruct it.
    void frame(const SceneFn& scene, bool velocity = true, bool reactive = false) {
        const Vec2 jitter = next_jitter();
        step(sample_scene(scene, render_, jitter), jitter, velocity, reactive);
    }

    void set_settings(const UpscaleSettings& settings) noexcept { settings_ = settings; }
    [[nodiscard]] const std::vector<Vec4>& history() const noexcept { return history_; }
    [[nodiscard]] const std::vector<Vec2>& state() const noexcept { return state_; }
    [[nodiscard]] const UpscaleConstants& constants() const noexcept { return constants_; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] Extent2D output() const noexcept { return output_; }
    [[nodiscard]] Extent2D render() const noexcept { return render_; }

private:
    Extent2D output_;
    Extent2D render_;
    UpscaleSettings settings_;
    JitterSequence jitter_;
    std::vector<Vec4> history_;
    std::vector<Vec2> state_;
    UpscaleConstants constants_{};
    bool written_ = false;
    bool failed_ = false;
};

/// Mean absolute difference per channel between a reconstruction and a reference, over the pixels
/// `inside` admits (all of them when null).
[[nodiscard]] inline f32 mean_error(const std::vector<Vec4>& image,
                                    const std::vector<Vec3>& reference, Extent2D extent,
                                    const std::function<bool(u32, u32)>& inside = nullptr) {
    f64 sum = 0.0;
    usize count = 0;
    for (u32 y = 0; y < extent.height; ++y) {
        for (u32 x = 0; x < extent.width; ++x) {
            if (inside && !inside(x, y)) {
                continue;
            }
            const usize at = (static_cast<usize>(y) * extent.width) + x;
            sum += static_cast<f64>(std::fabs(image[at].x - reference[at].x) +
                                    std::fabs(image[at].y - reference[at].y) +
                                    std::fabs(image[at].z - reference[at].z));
            ++count;
        }
    }
    return count == 0 ? 0.0F : static_cast<f32>(sum / (3.0 * static_cast<f64>(count)));
}

}  // namespace cy::upscale_test
