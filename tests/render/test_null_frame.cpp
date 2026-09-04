// The milestone's frame, through the null backend, on a machine with no GPU. Task 6.4.
//
// ================================================================================================
// WHY THIS MATTERS MORE THAN THE GOLDEN IMAGES
// ================================================================================================
//
// design.md §1: the null backend "is what makes every later milestone's rendering work testable in
// CI on a machine with no GPU, which is most CI machines". `render.golden` needs a device and skips
// without one. THIS suite needs nothing: it is declared unconditionally, it runs on every pull
// request on every platform, and what it checks is the part of a frame that is decided rather than
// drawn — which passes survive, which order they fall into, how many barriers the derivation
// emitted, and whether two runs agree.
//
// A rendering milestone whose only gate is a photograph is a rendering milestone that is not gated
// on the machines that build it.
//
// ================================================================================================
// IT RUNS THE SAMPLE'S OWN RENDERER
// ================================================================================================
//
// Not a scene written for a test: `cy::sample-first-light`, the library half of
// `samples/03-first-light`. The renderer names no backend and takes a `cy::rhi::Device&`, so the
// SAME code that produced the golden image runs here against a device that executes nothing. There
// is no `#if` between the two paths and there is no second renderer to keep in step — which is the
// argument design.md §1 makes, checked rather than asserted.

#include <cy/test/test.h>

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/core/memory/system_allocator.h>

#include "renderer.h"
#include "scene.h"

#include <cstdio>

namespace {

using cy::sample::first_light::FrameReport;
using cy::sample::first_light::Renderer;
using cy::sample::first_light::RendererOptions;
using cy::sample::first_light::Scene;
using cy::sample::first_light::SceneDescription;

constexpr cy::u32 kWidth = 192;
constexpr cy::u32 kHeight = 108;

/// A null device and the sample's renderer over it. One per case, like every other render fixture
/// here — a device per case is what keeps two cases from sharing recycled handles.
class NullFrame {
public:
    explicit NullFrame(bool aliasing = true) noexcept
        : allocator_(cy::system_allocator(cy::MemoryDomain::Gpu)) {
        (void)cy::rhi::null::register_null_backend();
        cy::rhi::DeviceDescription description;
        description.application_name = "cy_test_render_null_frame";
        description.request_async_compute = false;
        device_ =
            cy::rhi::create_device(allocator_, cy::rhi::kNullBackendName, description, selection_);
        options_.width = kWidth;
        options_.height = kHeight;
        options_.aliasing = aliasing;
    }

    ~NullFrame() {
        if (device_.has_value()) {
            cy::rhi::destroy_device(allocator_, device_.value());
        }
    }

    NullFrame(const NullFrame&) = delete;
    NullFrame& operator=(const NullFrame&) = delete;

    [[nodiscard]] bool ready() const noexcept {
        return device_.has_value() &&
               device_.value()->capabilities().backend() == cy::rhi::BackendKind::Null;
    }
    [[nodiscard]] cy::rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] cy::Allocator& allocator() const noexcept { return allocator_; }
    [[nodiscard]] RendererOptions& options() noexcept { return options_; }

private:
    cy::Allocator& allocator_;
    cy::rhi::BackendSelection selection_{};
    RendererOptions options_{};
    cy::Expected<cy::rhi::Device*, cy::Error> device_ =
        cy::fail(cy::ErrorCode::Unavailable, "not created");
};

/// How many commands of one kind the null backend recorded.
cy::u32 count_of(cy::rhi::Device& device, cy::rhi::null::CommandKind kind) noexcept {
    cy::u32 total = 0;
    for (const cy::rhi::null::RecordedCommand& command : cy::rhi::null::command_log(device)) {
        if (command.kind == kind) {
            ++total;
        }
    }
    return total;
}

}  // namespace

CY_TEST_CASE("render.null_frame: the sample's frame runs with no GPU at all") {
    NullFrame fixture;
    CY_REQUIRE(fixture.ready());

    Scene scene(fixture.allocator());
    CY_REQUIRE(scene.build(SceneDescription{}).has_value());
    Renderer renderer(fixture.allocator(), fixture.device());
    CY_REQUIRE(renderer.prepare(scene, fixture.options()).has_value());

    cy::rhi::null::clear_command_log(fixture.device());
    cy::Expected<FrameReport, cy::Error> frame = renderer.render(scene, scene.camera_at(0.0F));
    CY_REQUIRE(frame.has_value());

    // The frame the sample declares on its FIRST call: the albedo upload, the shadow pass, the
    // forward pass, the readback copy and the recordless host-boundary pass.
    CY_CHECK_EQ(frame->passes_recorded, 5U);
    CY_CHECK_EQ(frame->passes_culled, 0U);
    // One queue, so one submit and no ownership transfer. The same declarations under a device with
    // async compute would produce more of both, from no change to any pass — which is the whole
    // reason the queue is a declaration rather than a branch.
    CY_CHECK_EQ(frame->submits, 1U);
    CY_CHECK_EQ(frame->queue_ownership_transfers, 0U);
    CY_CHECK_GT(frame->barriers, 0U);

    // Two draws per object: one into the shadow map, one into the colour target. A shadow pass that
    // drew a different set from the forward pass is how a caster goes missing, and it would show up
    // here as an odd number.
    CY_CHECK_EQ(frame->draws, static_cast<cy::u32>(scene.objects().size()) * 2U);
    CY_CHECK_EQ(count_of(fixture.device(), cy::rhi::null::CommandKind::DrawIndexed), frame->draws);
    // Two rendering scopes — the shadow pass and the forward pass — and one texture copy each way.
    CY_CHECK_EQ(count_of(fixture.device(), cy::rhi::null::CommandKind::BeginRendering), 2U);
    CY_CHECK_EQ(count_of(fixture.device(), cy::rhi::null::CommandKind::CopyBufferToTexture), 1U);
    CY_CHECK_EQ(count_of(fixture.device(), cy::rhi::null::CommandKind::CopyTextureToBuffer), 1U);
}

CY_TEST_CASE("render.null_frame: every barrier in the stream is the graph's") {
    NullFrame fixture;
    CY_REQUIRE(fixture.ready());

    Scene scene(fixture.allocator());
    CY_REQUIRE(scene.build(SceneDescription{}).has_value());
    Renderer renderer(fixture.allocator(), fixture.device());
    CY_REQUIRE(renderer.prepare(scene, fixture.options()).has_value());

    cy::rhi::null::clear_command_log(fixture.device());
    cy::Expected<FrameReport, cy::Error> frame = renderer.render(scene, scene.camera_at(0.0F));
    CY_REQUIRE(frame.has_value());

    // THE M3 INVARIANT, COUNTED. The number of barrier batches in the recorded stream equals the
    // number the compiled plan derived — so no barrier came from anywhere else. The renderer could
    // not have added one: `PassContext` hands a pass a command buffer, and a command buffer has no
    // synchronisation primitive on it. This is that fact as a number rather than as a claim.
    //
    // The grep-level gate (task 2.2.4) says no barrier SYMBOL exists outside the graph. This says
    // no barrier COMMAND reached the stream from outside it, which is the same invariant seen from
    // the other end.
    CY_CHECK_EQ(count_of(fixture.device(), cy::rhi::null::CommandKind::Barriers),
                frame->barrier_batches);
    CY_CHECK_GT(frame->barrier_batches, 0U);
}

CY_TEST_CASE("render.null_frame: the same frame twice records the same stream") {
    NullFrame fixture;
    CY_REQUIRE(fixture.ready());

    Scene scene(fixture.allocator());
    CY_REQUIRE(scene.build(SceneDescription{}).has_value());
    Renderer renderer(fixture.allocator(), fixture.device());
    CY_REQUIRE(renderer.prepare(scene, fixture.options()).has_value());

    // The first frame carries the texture upload and the second does not, so the two frames to
    // compare are the second and the third. That difference is itself the check below.
    CY_REQUIRE(renderer.render(scene, scene.camera_at(0.0F)).has_value());

    cy::rhi::null::clear_command_log(fixture.device());
    cy::Expected<FrameReport, cy::Error> second = renderer.render(scene, scene.camera_at(0.25F));
    CY_REQUIRE(second.has_value());
    const cy::u64 second_stream = cy::rhi::null::command_log_hash(fixture.device());

    cy::rhi::null::clear_command_log(fixture.device());
    cy::Expected<FrameReport, cy::Error> third = renderer.render(scene, scene.camera_at(0.25F));
    CY_REQUIRE(third.has_value());
    const cy::u64 third_stream = cy::rhi::null::command_log_hash(fixture.device());

    // Task 7.6: frame submission order is identical across runs. design.md §6 requires it because
    // M9's replay and every golden image after this milestone depend on it, and it costs nothing to
    // hold now and a great deal to retrofit.
    CY_CHECK_EQ(second->plan_hash, third->plan_hash);
    CY_CHECK_EQ(second_stream, third_stream);
    CY_CHECK_EQ(second->passes_recorded, third->passes_recorded);
}

CY_TEST_CASE("render.null_frame: the upload pass exists once and is then not declared") {
    NullFrame fixture;
    CY_REQUIRE(fixture.ready());

    Scene scene(fixture.allocator());
    CY_REQUIRE(scene.build(SceneDescription{}).has_value());
    Renderer renderer(fixture.allocator(), fixture.device());
    CY_REQUIRE(renderer.prepare(scene, fixture.options()).has_value());

    cy::Expected<FrameReport, cy::Error> first = renderer.render(scene, scene.camera_at(0.0F));
    CY_REQUIRE(first.has_value());

    cy::rhi::null::clear_command_log(fixture.device());
    cy::Expected<FrameReport, cy::Error> second = renderer.render(scene, scene.camera_at(0.1F));
    CY_REQUIRE(second.has_value());

    // The absence is not a branch inside a pass — it is a pass that was never declared, so the
    // graph derives nothing around it. That is the mechanism `rendering-forward-clustered` uses for
    // a disabled feature ("their passes SHALL be absent from the graph"), and the sample uses it
    // for a one-off upload.
    CY_CHECK_EQ(first->passes_recorded, 5U);
    CY_CHECK_EQ(second->passes_recorded, 4U);
    CY_CHECK_EQ(count_of(fixture.device(), cy::rhi::null::CommandKind::CopyBufferToTexture), 0U);
    // And the second frame needs fewer barriers than the first, because two of the first frame's
    // were the upload's — derived, not written.
    CY_CHECK_LT(second->barriers, first->barriers);
}

CY_TEST_CASE("render.null_frame: aliasing reduces what the frame's targets cost") {
    // The frame has two transients — the colour target and the depth target — and they overlap in
    // time, so this scene CANNOT alias them: the honest result is that both figures agree. The case
    // is here anyway, because the number it checks is the one task 7.3 measures on a frame that can
    // (`integration.render_graph_scale`), and because a plan that reported a saving here would be
    // reporting a saving it had not made.
    NullFrame aliased(true);
    CY_REQUIRE(aliased.ready());
    Scene scene(aliased.allocator());
    CY_REQUIRE(scene.build(SceneDescription{}).has_value());
    Renderer renderer(aliased.allocator(), aliased.device());
    CY_REQUIRE(renderer.prepare(scene, aliased.options()).has_value());
    cy::Expected<FrameReport, cy::Error> frame = renderer.render(scene, scene.camera_at(0.0F));
    CY_REQUIRE(frame.has_value());

    CY_CHECK_GT(frame->transient_bytes, 0U);
    CY_CHECK_LE(frame->transient_bytes, frame->transient_bytes_without_aliasing);
    std::fprintf(stderr, "null frame: transients %llu B, unaliased %llu B\n",
                 static_cast<unsigned long long>(frame->transient_bytes),
                 static_cast<unsigned long long>(frame->transient_bytes_without_aliasing));
}
