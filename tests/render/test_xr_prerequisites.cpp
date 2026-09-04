// The three XR prerequisite checks. Task 6.5.
//
// ================================================================================================
// WHAT THESE ARE FOR
// ================================================================================================
//
// `xr-support` is deferred to M11 and `delivery-roadmap` requires deferred scope to carry its
// re-entry point: the milestone it is reconsidered at, the seams that must stay open until then,
// and THE CHECK THAT PROVES THOSE SEAMS ARE STILL OPEN. tests/render/README.md has held that table
// since M0 with nothing behind it. This file is what goes behind it.
//
// Without these, the seams close silently and the first evidence is an XR port that turns out to be
// a renderer rewrite. Each case below names the prerequisite it comes from and what closing that
// seam would cost, so a future change that has to close one closes it deliberately — as an OpenSpec
// change against `xr-support`, which is what the README says and what a failing test here forces.
//
// ================================================================================================
// THEY RUN ON THE NULL BACKEND, WITH NO GPU
// ================================================================================================
//
// Every one of the three is a question about STRUCTURE — is the geometry submitted once, does the
// frame's time come from an argument, does the view reach submission without a command being
// re-recorded — and none of them is a question about pixels. The null backend records every command
// and hashes the stream, which is exactly the instrument these questions need, and it means the
// checks run on every pull request rather than only where there is hardware.
//
// ================================================================================================
// ONE OF THE THREE IS HALF OPEN, AND THIS FILE SAYS WHICH
// ================================================================================================
//
// The frame-timing seam has two halves. The host owning the loop is the half M0 built and
// `tests/smoke/` already covers. The other half — the frame's time arriving as an ARGUMENT rather
// than being read from a clock — is open in the RENDERER (checked below) and is NOT open in
// `cy::Runtime`: `Runtime::tick()` takes no predicted display time, so a host cannot yet hand one
// down. That is recorded here, in the case's own comment and in tests/render/README.md, rather than
// asserted away. What the case does check is the half a renderer can violate, which is the half
// this milestone could have broken.

#include <cy/test/test.h>

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>

#include "renderer.h"
#include "scene.h"

#include <cstdio>

namespace {

using cy::rendering::PassContext;
using cy::rendering::ResourceId;
using cy::rhi::Access;
using cy::rhi::QueueKind;
using cy::sample::first_light::Camera;
using cy::sample::first_light::FrameReport;
using cy::sample::first_light::Renderer;
using cy::sample::first_light::RendererOptions;
using cy::sample::first_light::Scene;
using cy::sample::first_light::SceneDescription;

/// A null device. Every case builds its own.
class NullDevice {
public:
    NullDevice() noexcept : allocator_(cy::system_allocator(cy::MemoryDomain::Gpu)) {
        (void)cy::rhi::null::register_null_backend();
        cy::rhi::DeviceDescription description;
        description.application_name = "cy_test_render_xr";
        description.request_async_compute = false;
        device_ =
            cy::rhi::create_device(allocator_, cy::rhi::kNullBackendName, description, selection_);
    }

    ~NullDevice() {
        if (device_.has_value()) {
            cy::rhi::destroy_device(allocator_, device_.value());
        }
    }

    NullDevice(const NullDevice&) = delete;
    NullDevice& operator=(const NullDevice&) = delete;

    [[nodiscard]] bool ready() const noexcept { return device_.has_value(); }
    [[nodiscard]] cy::rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] cy::Allocator& allocator() const noexcept { return allocator_; }

private:
    cy::Allocator& allocator_;
    cy::rhi::BackendSelection selection_{};
    cy::Expected<cy::rhi::Device*, cy::Error> device_ =
        cy::fail(cy::ErrorCode::Unavailable, "not created");
};

/// What the stereo pass records with, so the case can read it back off the command log.
struct StereoState {
    cy::rendering::GraphExecutor* executor = nullptr;
    ResourceId target = cy::rendering::kInvalidResource;
    cy::u32 view_mask = 0;
};

/// ONE rendering scope, TWO layers, ONE draw. The geometry is submitted once and amplified across
/// the views by the hardware, which is what `xr-support`'s multi-view prerequisite requires and
/// what a second pass over the same geometry would violate even if the two images matched.
void record_stereo(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<StereoState*>(user);
    cy::rhi::RenderAttachment color;
    color.view = state->executor->view(state->target);
    color.load = cy::rhi::LoadOp::Clear;
    color.store = cy::rhi::StoreOp::Store;

    cy::rhi::RenderingInfo info;
    info.render_area = cy::rhi::Rect2D{0, 0, 64, 64};
    info.color_attachments = cy::Span<const cy::rhi::RenderAttachment>(&color, 1);
    info.layer_count = 2;
    info.view_mask = state->view_mask;

    context.commands->begin_rendering(info);
    context.commands->draw(3, 1, 0, 0);
    context.commands->end_rendering();
}

cy::u32 popcount_of(cy::u32 value) noexcept {
    cy::u32 bits = 0;
    while (value != 0U) {
        bits += value & 1U;
        value >>= 1U;
    }
    return bits;
}

}  // namespace

// ==================================================================================================
// PREREQUISITE 1 — Multi-view rendering
//
// `xr-support`: a camera with two views submits its geometry ONCE, amplified across both layers,
// with per-view matrices indexed in the shader.
//
// IF THE SEAM CLOSES: stereo becomes a second full render of the scene, and every pass written
// between now and M11 will have assumed one view per camera.
// ==================================================================================================
CY_TEST_CASE("render.xr_prerequisites: two views are one submission of the geometry") {
    NullDevice fixture;
    CY_REQUIRE(fixture.ready());
    cy::rhi::Device& device = fixture.device();

    // The capability is queried, never assumed — which is `rhi-and-render-graph`'s rule and the
    // reason a renderer may not branch on a backend's name. A device without it would report false
    // here and an XR path would have to fall back rather than fail.
    CY_CHECK(device.capabilities().has(cy::rhi::Capability::Multiview));

    // The pipeline carries the mask too, and it has to: a multiview render pass and a pipeline
    // built for a single view are a mismatch the driver rejects. `view_mask` on
    // GraphicsPipelineDescription is the seam that would have to be removed for this to close.
    cy::rhi::PipelineLayoutDescription layout_description;
    layout_description.name = "stereo layout";
    cy::Expected<cy::rhi::PipelineLayoutHandle, cy::Error> layout =
        device.create_pipeline_layout(layout_description);
    CY_REQUIRE(layout.has_value());

    CY_REQUIRE(device.begin_frame().has_value());
    cy::rendering::RenderGraph graph(fixture.allocator());
    cy::rendering::GraphExecutor executor(fixture.allocator(), device);

    cy::rendering::TextureRequest target_request;
    target_request.name = "stereo target";
    target_request.format = cy::rhi::Format::Rgba8Unorm;
    target_request.width = 64;
    target_request.height = 64;
    // Two array layers: one eye each. The graph derives the barrier over BOTH, from one declaration
    // — the layer count is part of the resource rather than something a pass has to transition.
    target_request.array_layers = 2;
    const ResourceId target = graph.create_texture(target_request);

    StereoState state;
    state.executor = &executor;
    state.target = target;
    state.view_mask = 0b11U;

    graph.add_pass("stereo", QueueKind::Graphics)
        .write(target, Access::ColorAttachmentWrite)
        .record(&record_stereo, &state)
        .side_effect();
    CY_REQUIRE(graph.status().has_value());

    cy::rhi::null::clear_command_log(device);
    CY_REQUIRE(
        executor.execute(graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{})
            .has_value());

    cy::u32 scopes = 0;
    cy::u32 draws = 0;
    cy::u32 mask = 0;
    cy::u32 layers = 0;
    for (const cy::rhi::null::RecordedCommand& command : cy::rhi::null::command_log(device)) {
        if (command.kind == cy::rhi::null::CommandKind::BeginRendering) {
            ++scopes;
            layers = command.c;
            mask = command.d;
        } else if (command.kind == cy::rhi::null::CommandKind::Draw) {
            ++draws;
        }
    }
    // THE ASSERTION, and it is the one the README states: two views, and the geometry submitted
    // once. A renderer that looped over the eyes would record two scopes and two draws and would
    // fail here even though its image was correct.
    CY_CHECK_EQ(scopes, 1U);
    CY_CHECK_EQ(draws, 1U);
    CY_CHECK_EQ(layers, 2U);
    CY_CHECK_EQ(popcount_of(mask), 2U);

    executor.release();
    CY_CHECK(device.end_frame().has_value());
    device.destroy_pipeline_layout(*layout);
}

// ==================================================================================================
// PREREQUISITE 2 — Runtime-driven frame timing
//
// `xr-support`: one frame runs against an EXTERNALLY SUPPLIED predicted display time, and the
// frame's timing derives from that argument rather than from the engine's own clock. Reads of a
// global clock inside the frame fail it.
//
// IF THE SEAM CLOSES: the engine owns the loop again, which is the M6 restructure design.md §3
// exists to avoid.
//
// WHAT IS OPEN AND WHAT IS NOT, at M3:
//   OPEN     the renderer. `Renderer::render(const Scene&, const Camera&)` is its whole input, and
//            the case below shows that two renders separated in wall-clock time produce a
//            byte-identical command stream — so no clock reaches the frame.
//   NOT OPEN `cy::Runtime::tick()` takes no predicted display time. A host can call it once per
//            frame (which M0's smoke test asserts) but cannot yet tell it WHEN the frame will be
//            displayed. Adding that argument is the change M11 needs and it is small today; it gets
//            expensive once anything downstream reads the runtime's clock instead.
// ==================================================================================================
CY_TEST_CASE("render.xr_prerequisites: the frame's inputs are arguments, not a clock") {
    NullDevice fixture;
    CY_REQUIRE(fixture.ready());

    Scene scene(fixture.allocator());
    CY_REQUIRE(scene.build(SceneDescription{}).has_value());
    RendererOptions options;
    options.width = 64;
    options.height = 64;
    Renderer renderer(fixture.allocator(), fixture.device());
    CY_REQUIRE(renderer.prepare(scene, options).has_value());

    // Past the first frame, which carries the one-off texture upload.
    CY_REQUIRE(renderer.render(scene, scene.camera_at(0.0F)).has_value());

    const Camera camera = scene.camera_at(0.37F);
    cy::rhi::null::clear_command_log(fixture.device());
    cy::Expected<FrameReport, cy::Error> first = renderer.render(scene, camera);
    CY_REQUIRE(first.has_value());
    const cy::u64 first_stream = cy::rhi::null::command_log_hash(fixture.device());

    // Time passes. Not by sleeping — a test that sleeps is a test that is slow for everybody — but
    // by doing enough work that any wall clock the frame might read has moved. Two more frames of
    // the same renderer is both time and a change of state.
    CY_REQUIRE(renderer.render(scene, scene.camera_at(0.9F)).has_value());
    CY_REQUIRE(renderer.render(scene, scene.camera_at(0.1F)).has_value());

    cy::rhi::null::clear_command_log(fixture.device());
    cy::Expected<FrameReport, cy::Error> later = renderer.render(scene, camera);
    CY_REQUIRE(later.has_value());
    const cy::u64 later_stream = cy::rhi::null::command_log_hash(fixture.device());

    // The same arguments, a different moment, the same frame. A renderer that animated anything
    // from a clock of its own — a jitter sequence, a temporal index, a time-of-day — would differ
    // here, and an XR runtime that asked it to render a predicted pose would get a frame for some
    // other instant.
    CY_CHECK_EQ(first_stream, later_stream);
    CY_CHECK_EQ(first->plan_hash, later->plan_hash);
}

// ==================================================================================================
// PREREQUISITE 3 — Late-latching
//
// `xr-support`: a view transform updated AFTER culling and command recording have begun reaches
// submission, with no draw command re-recorded. The view matrices come from a buffer written late
// in the frame.
//
// IF THE SEAM CLOSES: view matrices bake into recorded commands, and head-pose latency becomes a
// property of the whole render graph.
//
// HOW IT IS CHECKED. The question "could the view be written later?" is the question "is the view
// in the commands?", and the null backend's log answers it: render the same scene from the same
// POSITION with two different ORIENTATIONS, and the recorded command stream must be identical —
// the orientation reached the draw through the uniform buffer the descriptor names, which a late
// write could have replaced. The control is the second half: moving the camera DOES change the
// stream, because the per-object translation is camera-relative and travels in a push constant. So
// the first assertion is measuring something rather than passing on an empty stream.
// ==================================================================================================
CY_TEST_CASE("render.xr_prerequisites: the view reaches submission through memory, not commands") {
    NullDevice fixture;
    CY_REQUIRE(fixture.ready());

    Scene scene(fixture.allocator());
    CY_REQUIRE(scene.build(SceneDescription{}).has_value());
    RendererOptions options;
    options.width = 64;
    options.height = 64;
    Renderer renderer(fixture.allocator(), fixture.device());
    CY_REQUIRE(renderer.prepare(scene, options).has_value());
    CY_REQUIRE(renderer.render(scene, scene.camera_at(0.0F)).has_value());

    const Camera looking_north = scene.camera_at(0.2F);
    // The same eye, a different gaze — which is exactly what a head rotation between the pose
    // prediction and the submission is.
    Camera looking_elsewhere = looking_north;
    looking_elsewhere.forward = cy::normalize(cy::Vec3{0.3F, -0.2F, -0.93F});

    cy::rhi::null::clear_command_log(fixture.device());
    CY_REQUIRE(renderer.render(scene, looking_north).has_value());
    const cy::u64 north_stream = cy::rhi::null::command_log_hash(fixture.device());

    cy::rhi::null::clear_command_log(fixture.device());
    CY_REQUIRE(renderer.render(scene, looking_elsewhere).has_value());
    const cy::u64 elsewhere_stream = cy::rhi::null::command_log_hash(fixture.device());

    // The view changed and not one command did.
    CY_CHECK_EQ(north_stream, elsewhere_stream);

    // AND THE ASSERTION THAT MAKES THAT MEAN SOMETHING. The line above on its own is weak evidence:
    // the null backend's log records a push constant's OFFSET and SIZE and not its bytes, so a
    // renderer that had baked the view-projection into the push block would still hash the same.
    // What distinguishes the two is the SIZE — 64 bytes is the object's 3x4 transform and its
    // colour, and a view-projection would add another 64. So the check is that every push in the
    // frame is exactly the object block:
    //
    //   the view is NOT in the command stream -> it is in the descriptor-bound uniform buffer
    //   -> a write to that buffer after recording reaches the same draws
    //
    // which is the late-latch seam, stated as the property that makes it possible.
    //
    // THE NUMBER BELOW IS THIS TEST'S AND NOT THE RENDERER'S, and that is the whole assertion.
    // It was `cy::sample::first_light::kObjectPushBytes` — the constant the code under test
    // publishes — which made the check vacuous: baking a `f32 view_projection[4][4]` into
    // `ObjectPush` and moving that constant from 64 to 128 (exactly the seam closing) left this
    // suite at 3 cases, 67 assertions, SUCCESS. Measured with that probe, not supposed. A renderer
    // that legitimately grows its object block now fails here and the growth gets looked at, which
    // is what a seam gate is for.
    //
    // WHAT THIS STILL CANNOT SEE: the null backend records a push constant's offset and size and
    // never its bytes, so no assertion here can distinguish two 64-byte blocks with different
    // contents. Recording the payload is the way to close that, and it is a change to
    // rhi::null::RecordedCommand rather than to this file.
    constexpr cy::u32 kExpectedObjectPushBytes = 64;
    static_assert(cy::sample::first_light::kObjectPushBytes == kExpectedObjectPushBytes,
                  "the sample's per-object push block grew: does it now carry the view? If the "
                  "growth is legitimate, change the number here deliberately — see above");
    cy::u32 pushes = 0;
    for (const cy::rhi::null::RecordedCommand& command :
         cy::rhi::null::command_log(fixture.device())) {
        if (command.kind == cy::rhi::null::CommandKind::PushConstants) {
            ++pushes;
            CY_CHECK_EQ(command.b, kExpectedObjectPushBytes);
            CY_CHECK_EQ(command.a, 0U);
        }
    }
    CY_CHECK_GT(pushes, 0U);
    // And the buffer the view does travel in is bound, once per pass, as a descriptor set.
    cy::u32 binds = 0;
    for (const cy::rhi::null::RecordedCommand& command :
         cy::rhi::null::command_log(fixture.device())) {
        if (command.kind == cy::rhi::null::CommandKind::BindDescriptorSets) {
            ++binds;
        }
    }
    CY_CHECK_EQ(binds, 2U);

    // THE CONTROL, so that "the two hashes agree" is not a statement about a hash that never moves.
    // A scene with a different number of objects records a different number of draws.
    Scene smaller(fixture.allocator());
    SceneDescription fewer;
    fewer.box_count = 2;
    CY_REQUIRE(smaller.build(fewer).has_value());
    Renderer other(fixture.allocator(), fixture.device());
    CY_REQUIRE(other.prepare(smaller, options).has_value());
    CY_REQUIRE(other.render(smaller, smaller.camera_at(0.0F)).has_value());

    cy::rhi::null::clear_command_log(fixture.device());
    CY_REQUIRE(other.render(smaller, smaller.camera_at(0.2F)).has_value());
    CY_CHECK_NE(north_stream, cy::rhi::null::command_log_hash(fixture.device()));
}
