// THE SIMULATION, PHOTOGRAPHED. M8.c section 2, and rule 14 of this milestone's brief.
//
// ================================================================================================
// WHY THIS SUITE EXISTS AND WHAT IT WRITES
// ================================================================================================
//
// `integration.vfx` proves the world spawns, moves and kills particles. It cannot prove that what
// it measured is what a renderer would draw, because that is one publication and one upload away —
// and this project went six milestones without looking at the difference.
//
// So this suite steps the SAME cooked effect, publishes it through `publish_sprites`, hands the
// records to `cy::rendering::particles::ParticleRenderer` and reads the frame's output image back
// off a Vulkan device with validation and synchronisation validation on. Three pictures, written
// every run rather than only on failure, because a picture that only exists when something is
// broken is not a comparison:
//
//   vfx-simulation.png             the effect mid-flight, simulated by `SimulationWorld`
//   vfx-budget-full.png            the same effect at its authored quality
//   vfx-budget-degraded.png        the same effect after the budget controller has reduced it
//                                  under a synthetic overload — the same frame, the same camera,
//                                  the same seed, one number changed
//
// The committed copies are in `docs/design/images/`.
//
// ================================================================================================
// EVERY ASSERTION HERE CAN FAIL
// ================================================================================================
//
// The budget pair is two runs of one code path with the controller's measurement changed, so there
// is no tolerance to tune and nothing to regenerate. Take the levers out of `simulate_instance` and
// the two pictures become identical and the case goes red; that is the check, and it was run.

#include "vfx_scene.h"

#include "golden.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstdio>

using namespace cy;
using namespace cy::vfx;
using namespace cy::vfx_test;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
    }
    std::fprintf(stderr, "vulkan validation %s: %s\n",
                 severity == rhi::ValidationSeverity::Error ? "error" : "warning",
                 message != nullptr ? message : "");
}

/// A device per case, for the reason tests/render/device.h gives: synchronisation validation keeps
/// per-queue state for the process's lifetime, and recycled handles across two devices in one
/// process produce phantom cross-test hazards that look damning and are not.
class DeviceFixture {
public:
    DeviceFixture() noexcept : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_test_render_vfx";
        description.enable_validation = true;
        description.enable_synchronisation_validation = true;
        device_ = rhi::create_device(allocator_, "vulkan", description, selection_);
        if (device_.has_value()) {
            device_.value()->set_validation_callback(&count_validation, &errors_);
        }
    }

    ~DeviceFixture() {
        if (device_.has_value()) {
            (void)device_.value()->wait_idle();
            rhi::destroy_device(allocator_, device_.value());
        }
    }

    DeviceFixture(const DeviceFixture&) = delete;
    DeviceFixture& operator=(const DeviceFixture&) = delete;

    [[nodiscard]] bool has_gpu() const noexcept {
        return device_.has_value() &&
               device_.value()->capabilities().backend() == rhi::BackendKind::Vulkan;
    }
    [[nodiscard]] rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] u32 validation_errors() const noexcept { return errors_; }

    void report_skip() const noexcept {
        std::fprintf(stderr,
                     "no Vulkan device on this machine; the backend selected was '%s' because %s\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)",
                     selection_.reason != nullptr ? selection_.reason : "(no reason given)");
    }

private:
    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    u32 errors_ = 0;
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

void save(const char* name, Span<const u32> texels) noexcept {
    render_test::Image image(allocator());
    if (!render_test::adopt(image, texels, kSceneWidth, kSceneHeight).has_value()) {
        return;
    }
    if (render_test::write_png(name, image).has_value()) {
        std::fprintf(stderr, "wrote %s (%ux%u)\n", name, kSceneWidth, kSceneHeight);
    }
}

/// How many texels the effect lit. The frame clears to a dark neutral, so "a particle was here" and
/// "nothing was" are different numbers rather than the same one.
[[nodiscard]] u32 lit_texels(Span<const u32> texels) noexcept {
    u32 lit = 0;
    for (const u32 texel : texels) {
        const u32 red = texel & 0xFFU;
        const u32 green = (texel >> 8U) & 0xFFU;
        const u32 blue = (texel >> 16U) & 0xFFU;
        if (red > 24U || green > 24U || blue > 24U) {
            ++lit;
        }
    }
    return lit;
}

/// Step the world until the effect is mid-flight, then render.
[[nodiscard]] Status warm(VfxScene& scene, u32 frames) noexcept {
    for (u32 frame = 0; frame < frames; ++frame) {
        if (Status stepped = scene.simulate(); !stepped) {
            return stepped;
        }
    }
    return ok();
}

}  // namespace

CY_TEST_CASE("a simulated effect reaches the frame and puts lit texels on the device") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // EIGHT PLUMES AND SIXTY FRAMES, deliberately different from the budget pair below: two
    // committed images that are byte-identical are one image and a reader who opens both learns
    // nothing from the second.
    VfxScene scene(allocator());
    CY_REQUIRE(scene.build(fixture.device(), 8).has_value());
    scene.set_read_back(true);
    CY_REQUIRE(warm(scene, 60).has_value());

    assembly::AssemblyReport report;
    CY_REQUIRE(scene.render(report).has_value());
    CY_CHECK(report.executed);

    // The simulation produced particles, the publication turned them into records, and the frame
    // drew them. Every one of those three is a number here rather than a claim.
    CY_CHECK_GT(scene.steps().live_particles, 0U);
    CY_CHECK_EQ(scene.published().particles, scene.steps().live_particles);
    CY_CHECK_EQ(scene.particle_report().particles, scene.published().particles);
    CY_CHECK_EQ(scene.particle_report().dropped, 0U);
    CY_CHECK_EQ(scene.particle_report().draws, 1U);
    CY_CHECK_EQ(scene.recorded().extensions_run, 1U);
    // A frame that renders but trips validation is not a frame that works.
    CY_CHECK_EQ(fixture.validation_errors(), 0U);

    u32 brightest = 0;
    for (const u32 texel : scene.pixels()) {
        for (u32 channel = 0; channel < 3U; ++channel) {
            const u32 value = (texel >> (channel * 8U)) & 0xFFU;
            brightest = value > brightest ? value : brightest;
        }
    }
    const particles::ParticleInstance& first = scene.records()[0];
    // THE FIRST RECORD AND THE BRIGHTEST CHANNEL, printed every run. A frame that comes back black
    // has two possible causes — the simulation produced nothing, or the renderer drew nothing — and
    // a count of particles cannot tell them apart. This line can: the first version of `KernelStep`
    // had three operand slots, `make_float4` silently lost its fourth, every particle's alpha was
    // zero, and every assertion about counts passed while the picture was empty.
    std::fprintf(stderr,
                 "passes recorded %u, brightest channel %u, first record (%f %f %f) size %f colour "
                 "(%f %f %f %f)\n",
                 scene.recorded().passes, brightest, static_cast<double>(first.position[0]),
                 static_cast<double>(first.position[1]), static_cast<double>(first.position[2]),
                 static_cast<double>(first.size), static_cast<double>(first.color[0]),
                 static_cast<double>(first.color[1]), static_cast<double>(first.color[2]),
                 static_cast<double>(first.color[3]));
    const u32 lit = lit_texels(scene.pixels());
    std::fprintf(stderr,
                 "%u live particles published as %u records, drawn in %u draw; %u of %u texels "
                 "lit\n",
                 scene.steps().live_particles, scene.published().particles,
                 scene.particle_report().draws, lit, kSceneWidth * kSceneHeight);
    CY_CHECK_GT(lit, 500U);
    save("vfx-simulation.png", scene.pixels());
}

CY_TEST_CASE("the budget controller reducing an effect is visible in the picture") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }

    // THE FULL-QUALITY RUN.
    Array<u32> full(allocator());
    u32 full_particles = 0;
    u32 full_lit = 0;
    {
        VfxScene scene(allocator());
        CY_REQUIRE(scene.build(fixture.device()).has_value());
        scene.set_read_back(true);
        scene.world().budget().set_allocation(2.0F);
        scene.world().budget().measure(0.4F);
        CY_REQUIRE(warm(scene, 45).has_value());
        assembly::AssemblyReport report;
        CY_REQUIRE(scene.render(report).has_value());
        full_particles = scene.steps().live_particles;
        full_lit = lit_texels(scene.pixels());
        CY_REQUIRE(full.resize(scene.pixels().size()).has_value());
        for (usize index = 0; index < full.size(); ++index) {
            full[index] = scene.pixels()[index];
        }
        save("vfx-budget-full.png", scene.pixels());
    }

    // THE OVERLOADED RUN. Same effect, same camera, same seed, same number of frames — the ONE
    // difference is what the controller was told VFX cost. `vfx-system`: "the controller SHALL
    // reduce decorative and ambient cost first, and SHALL report which levers it applied".
    VfxScene scene(allocator());
    CY_REQUIRE(scene.build(fixture.device()).has_value());
    scene.set_read_back(true);
    scene.world().budget().set_allocation(2.0F);
    scene.world().budget().measure(9.0F);
    // THE CONTROLLER IS LET RUN TO ITS FLOOR FIRST. Adjustment proceeds from `Decorative` to
    // `Critical` at a bounded rate — that ordering and that rate are the requirement — so a plume
    // declared `Important` is the third class reached and 45 frames of a 2-per-second ramp do not
    // get there. Pre-converging is what makes this a picture of a REDUCED effect rather than a
    // picture of the controller still working its way down the ranks.
    for (u32 frame = 0; frame < 200U; ++frame) {
        scene.world().budget().update(1.0F / 60.0F);
    }
    for (u32 frame = 0; frame < 45U; ++frame) {
        scene.world().budget().update(1.0F / 60.0F);
        CY_REQUIRE(scene.simulate().has_value());
    }
    assembly::AssemblyReport report;
    CY_REQUIRE(scene.render(report).has_value());
    const u32 degraded_particles = scene.steps().live_particles;
    const u32 degraded_lit = lit_texels(scene.pixels());
    const u32 differing = scene.differing_texels(full.span());
    save("vfx-budget-degraded.png", scene.pixels());

    std::fprintf(stderr,
                 "budget: %u particles and %u lit texels at full quality; %u and %u under a 9.0 ms "
                 "measurement against a 2.0 ms allocation; %u texels differ\n",
                 full_particles, full_lit, degraded_particles, degraded_lit, differing);

    // The cost is BOUNDED BY CONFIGURATION: fewer particles, a visibly different frame, and the
    // effect still on screen rather than switched off. Remove the levers from `simulate_instance`
    // and every one of these goes flat.
    CY_CHECK_LT(degraded_particles, full_particles);
    CY_CHECK_GT(degraded_particles, 0U);
    CY_CHECK_GT(differing, 200U);
    CY_CHECK(scene.world().budget().state().over_budget);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("many frames of a live simulation on the device, and a teardown while it is busy") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    auto* scene = new VfxScene(allocator());
    CY_REQUIRE(scene->build(fixture.device(), 8).has_value());

    // A one-frame suite cannot see a defect that begins at frame 3 — M4's `render.frames` suite's
    // own sentence, and the reason the ring is exercised past its own depth here.
    const u32 turns = (fixture.device().frames_in_flight() * 8U) + 1U;
    for (u32 frame = 0; frame < turns; ++frame) {
        CY_REQUIRE(scene->simulate().has_value());
        assembly::AssemblyReport report;
        CY_REQUIRE(scene->render(report).has_value());
        if (fixture.validation_errors() != 0U) {
            std::fprintf(stderr, "validation first spoke at frame %u\n", frame);
        }
        CY_REQUIRE_EQ(fixture.validation_errors(), 0U);
    }
    std::fprintf(stderr, "%u frames with a live simulation, %u particles at the end\n", turns,
                 scene->published().particles);
    // Deleted with the pool committed, the ring mid-turn and the device still holding the last
    // frame's work.
    delete scene;
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
