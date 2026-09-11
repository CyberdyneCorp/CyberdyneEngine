// THE LAYER, WITH NO GPU AT ALL. M8.c tasks 1b.1 and 1b.2.
//
// What this suite asks is what was DECIDED rather than what was drawn: how many stages the sinks
// carry a callback for, how many of them ran, how many draws each recorded, and how many bytes the
// Prepare transfer moved. Every one of those is answerable on a machine with no device, which is
// most continuous-integration machines — and design.md §1's argument for the null backend is that a
// rendering milestone whose only gate is a photograph is not gated on the machines that build it.
//
// THE CONTROL IS THE FIRST CASE AND IT IS THE POINT. `RecordMode::None` is the identical frame with
// an empty `FrameSinks`, which is what every caller in the tree supplied before this milestone.
// If the layer's callbacks were removed, the two would agree — so the assertion that they DISAGREE
// is what this milestone's task 1b.2 is.
//
// It is `integration` and not `unit`, deliberately: every case builds a spatial index, a material
// table, a device and a render graph, and the taxonomy names all of those as what does not belong
// in `unit`.

#include "frame_scene.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::pipeline_test;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

/// A null device per case. The null backend records nothing to a driver but runs the whole
/// derivation — compile, realise, schedule, record — so a record callback that was never attached
/// shows here exactly as it would on a GPU.
class NullFixture {
public:
    NullFixture() noexcept : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_test_render_pipeline";
        device_ = rhi::create_device(allocator_, "null", description, selection_);
    }

    ~NullFixture() {
        if (device_.has_value()) {
            (void)device_.value()->wait_idle();
            rhi::destroy_device(allocator_, device_.value());
        }
    }

    NullFixture(const NullFixture&) = delete;
    NullFixture& operator=(const NullFixture&) = delete;

    [[nodiscard]] bool ok() const noexcept { return device_.has_value(); }
    [[nodiscard]] rhi::Device& device() const noexcept { return *device_.value(); }

private:
    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

[[nodiscard]] u32 attached_callbacks(const rendering::assembly::FrameSinks& sinks) noexcept {
    u32 count = 0;
    for (const rendering::FramePassCallback& callback : sinks.passes) {
        count += callback.record != nullptr ? 1U : 0U;
    }
    return count;
}

}  // namespace

CY_TEST_CASE("the layer's sinks carry a record callback and an empty FrameSinks does not") {
    NullFixture fixture;
    CY_REQUIRE(fixture.ok());
    FrameScene scene(allocator());
    CY_REQUIRE(scene.build(fixture.device()).has_value());

    // The state of the tree before M8.c: `FrameSinks{}` is what `samples/08-vertical-slice`,
    // `src/rendering/assembly/tests/` and everything else handed the assembly.
    CY_CHECK_EQ(attached_callbacks(rendering::assembly::FrameSinks{}), 0U);

    rendering::assembly::AssemblyReport report;
    CY_REQUIRE(scene.render(RecordMode::None, report).has_value());
    // Five stages: Prepare, DepthPrepass, Opaque, Transparent, PostProcess. A sixth would be a
    // stage this layer decided to own and it would show here first.
    auto& recorder = const_cast<FrameRecorder&>(scene.recorder());
    CY_CHECK_EQ(attached_callbacks(recorder.sinks()), 5U);
}

CY_TEST_CASE(
    "a frame WITH the layer's callbacks records, and the identical frame WITHOUT does not") {
    NullFixture fixture;
    CY_REQUIRE(fixture.ok());
    FrameScene scene(allocator());
    CY_REQUIRE(scene.build(fixture.device()).has_value());

    // --- The control: the frame every caller in the tree assembled before this milestone.
    rendering::assembly::AssemblyReport blank;
    CY_REQUIRE(scene.render(RecordMode::None, blank).has_value());
    CY_CHECK(blank.executed);
    CY_CHECK_GT(blank.passes_declared, 0U);
    CY_CHECK_GT(blank.draws, 0U);
    // The frame is real and nothing recorded into it. THIS is the wall M8.b's gate named.
    CY_CHECK_EQ(scene.recorded().passes, 0U);
    CY_CHECK_EQ(scene.recorded().draws(), 0U);

    // --- The same frame, with the layer.
    rendering::assembly::AssemblyReport recorded;
    CY_REQUIRE(scene.render(RecordMode::Callbacks, recorded).has_value());
    CY_CHECK(recorded.executed);
    // The same frame: the assembly's own numbers are unchanged, because the layer decides nothing
    // the assembly decides. Only the recording differs.
    CY_CHECK_EQ(recorded.draws, blank.draws);
    CY_CHECK_EQ(recorded.passes_declared, blank.passes_declared);
    CY_CHECK_EQ(recorded.lights, blank.lights);

    CY_CHECK_EQ(scene.recorded().passes, 5U);
    // Every visible instance drawn twice — once into the depth prepass and once shaded — and the
    // prepass draws are not counted in `opaque_draws`, so this is the shaded count.
    CY_CHECK_EQ(scene.recorded().opaque_draws, recorded.draws);
    // Every visible instance is drawn TWICE — once into the depth prepass and once shaded — which
    // is what `rendering-forward-clustered`'s prepass is, and the two counts say so separately.
    CY_CHECK_EQ(scene.recorded().prepass_draws, recorded.draws);
    CY_CHECK_EQ(scene.recorded().skipped_draws, 0U);
    // The Prepare stage moved bytes: `ForwardFrame` calls it "the only pass that writes them" and
    // before this milestone nothing wrote them.
    CY_CHECK_GT(scene.recorded().uploaded_bytes, 0U);
}

CY_TEST_CASE("the particle renderer reaches the frame through the layer's own seam") {
    NullFixture fixture;
    CY_REQUIRE(fixture.ok());
    FrameScene scene(allocator());
    CY_REQUIRE(scene.build(fixture.device()).has_value());

    rendering::assembly::AssemblyReport without;
    CY_REQUIRE(scene.render(RecordMode::Callbacks, without).has_value());
    CY_CHECK_EQ(scene.recorded().extensions_run, 0U);
    CY_CHECK_EQ(scene.particle_report().draws, 0U);

    rendering::assembly::AssemblyReport with;
    CY_REQUIRE(scene.render(RecordMode::CallbacksAndParticles, with).has_value());
    // The extension ran inside the frame's transparent stage, and it issued ONE draw for the whole
    // effect. Neither number can move without the seam existing.
    CY_CHECK_EQ(scene.recorded().extensions_run, 1U);
    CY_CHECK_EQ(scene.particle_report().draws, 1U);
    CY_CHECK_EQ(scene.particle_report().particles, kParticleCount);
    CY_CHECK_EQ(scene.particle_report().dropped, 0U);
    // The particles changed nothing about the frame the assembly built. `vfx-system`'s firewall is
    // section 1's subject; this is the rendering half of the same statement.
    CY_CHECK_EQ(with.draws, without.draws);
}

CY_TEST_CASE("the ring turns over many frames and tears down with the device still busy") {
    NullFixture fixture;
    CY_REQUIRE(fixture.ok());
    auto* scene = new FrameScene(allocator());
    CY_REQUIRE(scene->build(fixture.device()).has_value());

    // More than `frames_in_flight` turns of the ring, so a buffer or a descriptor set that was
    // recycled under a frame still reading it would be reached. M4's `render.frames` suite exists
    // because M3 shipped exactly that defect and a one-frame test could not see it.
    const u32 turns = (fixture.device().frames_in_flight() * 4U) + 1U;
    for (u32 frame = 0; frame < turns; ++frame) {
        rendering::assembly::AssemblyReport report;
        CY_REQUIRE(scene->render(RecordMode::CallbacksAndParticles, report).has_value());
        CY_CHECK_EQ(scene->recorded().passes, 5U);
        CY_CHECK_EQ(scene->recorded().skipped_draws, 0U);
    }
    // TEARDOWN UNDER LOAD: destroyed without a `wait_idle` of its own from the case. `release()`
    // does one, which is the contract every device-owning object in this tree states, and the
    // fixture's destructor does another.
    delete scene;
}
