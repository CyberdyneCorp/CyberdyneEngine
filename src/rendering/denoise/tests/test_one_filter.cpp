// The structural rule: `SignalKind` carries no behaviour. Task 9.3.
//
// `denoising` — "One denoising framework": every stochastic signal goes through one subsystem
// "rather than each effect implementing its own filtering", and "GI and reflections... SHALL use
// the same accumulation, variance estimation, and edge-stopping rules".
//
// A prose requirement like that is kept by nothing. This file is what keeps it: it runs the SAME
// input through TWO different signal kinds carrying the SAME configuration, and asserts the two
// images are identical bit for bit. Any special case anywhere in denoiser.cpp that reads the signal
// kind — a branch, a table lookup, an "except for shadows" — fails this case immediately.
//
// The control at the end is the other half. Two kinds carrying DIFFERENT configurations must
// produce different images, or the case above would pass on a filter that ignored its own
// configuration too.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/denoise/denoiser.h>

#include "support.h"

#include <vector>

namespace {

using cy::rendering::denoise::default_config;
using cy::rendering::denoise::Denoiser;
using cy::rendering::denoise::NoisySignal;
using cy::rendering::denoise::SignalConfig;
using cy::rendering::denoise::SignalKind;
using denoise_support::Scene;
using denoise_support::StillHistory;

/// Run a scene through one signal kind of a freshly built denoiser configured with `config`.
std::vector<cy::Vec3> run(const Scene& scene, SignalKind kind, const SignalConfig& config,
                          cy::u32 frames) {
    Denoiser denoiser;
    CY_REQUIRE(denoiser.resize(scene.width, scene.height).has_value());
    denoiser.configure(kind, config);
    const StillHistory history(scene.width * scene.height);
    std::vector<cy::Vec3> image;
    for (cy::u32 frame = 0; frame < frames; ++frame) {
        const std::vector<cy::Vec3> values = scene.sample(frame);
        NoisySignal noisy;
        noisy.values = {values.data(), values.size()};
        const auto result = denoiser.denoise(kind, noisy, scene.guidance(), history.guidance());
        CY_REQUIRE(result.has_value());
        image.assign(result.value().begin(), result.value().end());
    }
    return image;
}

[[nodiscard]] bool identical(const std::vector<cy::Vec3>& a, const std::vector<cy::Vec3>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (cy::usize index = 0; index < a.size(); ++index) {
        if (a[index] != b[index]) {
            return false;
        }
    }
    return true;
}

}  // namespace

CY_TEST_CASE("the signal kind selects a configuration and carries no behaviour of its own") {
    const Scene scene(24);
    const SignalConfig diffuse = default_config(SignalKind::IndirectDiffuse);

    const std::vector<cy::Vec3> as_diffuse = run(scene, SignalKind::IndirectDiffuse, diffuse, 6);

    // Every other signal, denoised under the diffuse configuration, produces the diffuse image.
    // Bit for bit — not "within tolerance", because a tolerance is where a second filter hides.
    CY_CHECK(identical(as_diffuse, run(scene, SignalKind::IndirectSpecular, diffuse, 6)));
    CY_CHECK(identical(as_diffuse, run(scene, SignalKind::RayTracedShadow, diffuse, 6)));
    CY_CHECK(identical(as_diffuse, run(scene, SignalKind::AmbientOcclusion, diffuse, 6)));
    CY_CHECK(identical(as_diffuse, run(scene, SignalKind::StochasticDirect, diffuse, 6)));
}

CY_TEST_CASE("the declared configuration is what makes two signals differ") {
    // The control for the case above.
    const Scene scene(24);
    const std::vector<cy::Vec3> as_diffuse =
        run(scene, SignalKind::IndirectDiffuse, default_config(SignalKind::IndirectDiffuse), 6);
    const std::vector<cy::Vec3> as_shadow =
        run(scene, SignalKind::IndirectDiffuse, default_config(SignalKind::RayTracedShadow), 6);
    CY_CHECK_FALSE(identical(as_diffuse, as_shadow));
}

CY_TEST_CASE("GI and reflections denoise through the same instance and one set of rules") {
    // Two signals live at once in one denoiser: their histories are separate and the filter is not.
    // The requirement is "they SHALL use the same accumulation, variance estimation and
    // edge-stopping rules", and the check is that each one's answer does not depend on whether the
    // other ran beside it.
    const Scene scene(24);
    Denoiser shared;
    CY_REQUIRE(shared.resize(scene.width, scene.height).has_value());
    const StillHistory history(scene.width * scene.height);

    std::vector<cy::Vec3> diffuse_from_shared;
    for (cy::u32 frame = 0; frame < 6; ++frame) {
        const std::vector<cy::Vec3> values = scene.sample(frame);
        NoisySignal noisy;
        noisy.values = {values.data(), values.size()};
        const auto specular = shared.denoise(SignalKind::IndirectSpecular, noisy, scene.guidance(),
                                             history.guidance());
        CY_REQUIRE(specular.has_value());
        const auto diffuse = shared.denoise(SignalKind::IndirectDiffuse, noisy, scene.guidance(),
                                            history.guidance());
        CY_REQUIRE(diffuse.has_value());
        diffuse_from_shared.assign(diffuse.value().begin(), diffuse.value().end());
    }

    const std::vector<cy::Vec3> diffuse_alone =
        run(scene, SignalKind::IndirectDiffuse, default_config(SignalKind::IndirectDiffuse), 6);
    CY_CHECK(identical(diffuse_from_shared, diffuse_alone));
}
