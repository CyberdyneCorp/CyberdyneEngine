// The null backend as a reference implementation. Task 2.1.2.
//
// `rhi-and-render-graph` requires the null backend to satisfy "resource creation and command
// recording as no-ops while preserving handle semantics and validation". These cases are what makes
// "preserving" mean something: a stale handle fails, a limit is enforced, a frame paces, a
// swapchain hands out images, and the recorded command stream hashes the same twice.

#include <cy/test/test.h>

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/core/memory/system_allocator.h>

namespace {

using cy::rhi::Device;

/// One device, destroyed with the test. Every case builds its own: a device carries handle
/// generations and a command log, and sharing one between cases would make each case depend on the
/// order the others ran in.
class Fixture {
public:
    Fixture() noexcept
        : allocator_(cy::system_allocator(cy::MemoryDomain::Gpu)),
          device_(cy::rhi::null::create_null_device(allocator_, description())) {}

    ~Fixture() {
        if (device_.has_value()) {
            cy::rhi::null::destroy_null_device(allocator_, device_.value());
        }
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture&&) = delete;

    [[nodiscard]] bool ok() const noexcept { return device_.has_value(); }
    [[nodiscard]] Device& device() const noexcept { return *device_.value(); }

private:
    static cy::rhi::DeviceDescription description() noexcept {
        cy::rhi::DeviceDescription desc;
        desc.enable_validation = true;
        return desc;
    }

    cy::Allocator& allocator_;
    cy::Expected<Device*, cy::Error> device_;
};

cy::rhi::TextureDescription target(const char* name) noexcept {
    cy::rhi::TextureDescription desc;
    desc.name = name;
    desc.format = cy::rhi::Format::Rgba8Unorm;
    desc.extent = cy::rhi::Extent3D{64, 64, 1};
    desc.usage = cy::rhi::TextureUsage::ColorAttachment | cy::rhi::TextureUsage::Sampled;
    return desc;
}

}  // namespace

CY_TEST_CASE("the null backend reports itself, and reports no async compute") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    const cy::rhi::DeviceCapabilities& capabilities = fixture.device().capabilities();

    CY_CHECK_EQ(capabilities.backend(), cy::rhi::BackendKind::Null);
    CY_CHECK(capabilities.has(cy::rhi::Capability::ComputeShaders));
    CY_CHECK(capabilities.has(cy::rhi::Capability::DynamicRendering));
    CY_CHECK(capabilities.supports_gpu_driven());

    // Deliberate, and argued where it is set: continuous integration then exercises the
    // single-queue fold, which is the path most machines actually run.
    CY_CHECK_FALSE(capabilities.has(cy::rhi::Capability::AsyncCompute));
    CY_CHECK_FALSE(fixture.device().has_queue(cy::rhi::QueueKind::AsyncCompute));
    CY_CHECK(fixture.device().has_queue(cy::rhi::QueueKind::Graphics));

    // A device that met fewer than the engine's hard limits would be one the engine refuses.
    cy::rhi::ValidationMessage message;
    CY_CHECK(cy::rhi::validate_device_limits(capabilities.limits(), message).has_value());
}

CY_TEST_CASE("a stale handle fails validation rather than aliasing the resource that replaced it") {
    // `rhi-and-render-graph`, "Handle-based resources", stated as a scenario. This is the property
    // the whole generational-handle model exists for, and the null backend has to hold it as
    // strictly as a device does or continuous integration is testing something else.
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    Device& device = fixture.device();

    cy::Expected<cy::rhi::TextureHandle, cy::Error> first = device.create_texture(target("first"));
    CY_REQUIRE(first.has_value());
    const cy::rhi::TextureHandle stale = *first;
    CY_CHECK(device.is_valid(stale));

    device.destroy_texture(stale);
    CY_CHECK_FALSE(device.is_valid(stale));

    // The slot is reused; the generation is not.
    cy::Expected<cy::rhi::TextureHandle, cy::Error> second =
        device.create_texture(target("second"));
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(second->index(), stale.index());
    CY_CHECK_NE(second->generation(), stale.generation());
    CY_CHECK_FALSE(device.is_valid(stale));
    CY_CHECK(device.is_valid(*second));
    CY_CHECK(device.texture_description(stale) == nullptr);
}

CY_TEST_CASE("creation runs the same validation a device runs") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    Device& device = fixture.device();

    cy::rhi::TextureDescription bad = target("no format");
    bad.format = cy::rhi::Format::Undefined;
    CY_CHECK_FALSE(device.create_texture(bad).has_value());

    cy::rhi::BufferDescription empty;
    empty.name = "empty";
    empty.size = 0;
    empty.usage = cy::rhi::BufferUsage::Storage;
    CY_CHECK_FALSE(device.create_buffer(empty).has_value());

    // A shader module whose first word is not SPIR-V's magic number is rejected in continuous
    // integration rather than on the one machine with a GPU.
    const cy::u32 not_spirv[] = {0xDEADBEEFU, 0U, 0U};
    cy::rhi::ShaderModuleDescription module;
    module.name = "bogus";
    module.stage = cy::rhi::ShaderStage::Compute;
    module.spirv = cy::Span<const cy::u32>(not_spirv, 3);
    CY_CHECK_FALSE(device.create_shader_module(module).has_value());
}

CY_TEST_CASE("a mapped buffer has storage, and a device-local one does not") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    Device& device = fixture.device();

    cy::rhi::BufferDescription upload;
    upload.name = "staging";
    upload.size = 256;
    upload.usage = cy::rhi::BufferUsage::TransferSource;
    upload.memory = cy::rhi::MemoryUse::Upload;
    cy::Expected<cy::rhi::BufferHandle, cy::Error> mapped = device.create_buffer(upload);
    CY_REQUIRE(mapped.has_value());
    void* pointer = device.buffer_mapped_pointer(*mapped);
    CY_REQUIRE(pointer != nullptr);
    // Writing through it must be legal; a null backend that handed back null here would make every
    // upload path untestable without a GPU.
    static_cast<cy::u8*>(pointer)[255] = 0x5A;
    CY_CHECK_EQ(static_cast<cy::u8*>(pointer)[255], 0x5A);

    cy::rhi::BufferDescription local;
    local.name = "vertices";
    local.size = 256;
    local.usage = cy::rhi::BufferUsage::Vertex;
    cy::Expected<cy::rhi::BufferHandle, cy::Error> device_local = device.create_buffer(local);
    CY_REQUIRE(device_local.has_value());
    CY_CHECK(device.buffer_mapped_pointer(*device_local) == nullptr);
}

CY_TEST_CASE("frames pace, and a frame slot's command buffers are recycled") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    Device& device = fixture.device();
    CY_REQUIRE_EQ(device.frames_in_flight(), cy::rhi::kDefaultFramesInFlight);

    cy::rhi::CommandBufferHandle first_frame_buffer;
    for (cy::u32 frame = 0; frame < 3; ++frame) {
        cy::Expected<cy::u32, cy::Error> slot = device.begin_frame();
        CY_REQUIRE(slot.has_value());
        CY_CHECK_EQ(*slot, frame % device.frames_in_flight());

        cy::Expected<cy::rhi::CommandBufferHandle, cy::Error> commands =
            device.acquire_command_buffer(cy::rhi::QueueKind::Graphics, false);
        CY_REQUIRE(commands.has_value());
        if (frame == 0) {
            first_frame_buffer = *commands;
        }
        CY_REQUIRE(device.begin_command_buffer(*commands).has_value());
        CY_REQUIRE(device.end_command_buffer(*commands).has_value());
        CY_REQUIRE(device.end_frame().has_value());
    }

    // Frame 2 reuses frame 0's slot, so frame 0's command buffer is gone and its handle is stale.
    CY_CHECK(device.command_buffer(first_frame_buffer) == nullptr);
    CY_CHECK_EQ(device.frame_index(), 3U);
    CY_CHECK_EQ(device.statistics().frames_completed, 3U);
}

CY_TEST_CASE("a submit refuses a command buffer that is still recording, and advances a timeline") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    Device& device = fixture.device();
    CY_REQUIRE(device.begin_frame().has_value());

    cy::Expected<cy::rhi::CommandBufferHandle, cy::Error> commands =
        device.acquire_command_buffer(cy::rhi::QueueKind::Graphics, false);
    CY_REQUIRE(commands.has_value());
    CY_REQUIRE(device.begin_command_buffer(*commands).has_value());

    cy::rhi::SubmitInfo info;
    info.queue = cy::rhi::QueueKind::Graphics;
    info.command_buffers = cy::Span<const cy::rhi::CommandBufferHandle>(&*commands, 1);
    CY_CHECK_FALSE(device.submit(info).has_value());

    CY_REQUIRE(device.end_command_buffer(*commands).has_value());
    cy::Expected<cy::u64, cy::Error> signalled = device.submit(info);
    CY_REQUIRE(signalled.has_value());
    CY_CHECK_EQ(*signalled, 1U);
    CY_CHECK_EQ(device.timeline_value(cy::rhi::QueueKind::Graphics), 1U);

    // A wait on a value nothing signalled is a defect in the plan, and the null backend reports it
    // rather than silently succeeding — which is what makes it useful as a plan checker.
    const cy::rhi::TimelineWait impossible{cy::rhi::QueueKind::Graphics, 99,
                                           cy::rhi::Stage::AllCommands};
    cy::rhi::SubmitInfo waiting;
    waiting.queue = cy::rhi::QueueKind::Graphics;
    waiting.waits = cy::Span<const cy::rhi::TimelineWait>(&impossible, 1);
    CY_CHECK_FALSE(device.submit(waiting).has_value());
    CY_CHECK_GE(device.statistics().validation_errors, 1U);
}

CY_TEST_CASE("the recorded command stream is comparable, and identical for identical recording") {
    // This is what makes "the null backend records the same graph" (task 6.4) a comparison of two
    // numbers rather than an assertion nobody can check.
    //
    // THE STREAM IS ASSEMBLED AT SUBMIT TIME, from per-command-buffer logs. That is not an
    // implementation detail: two job workers recording two passes must not touch one array, and
    // assembling in submission order is what makes the result independent of which worker finished
    // first — `rhi-and-render-graph`'s "the same frame description SHALL produce the same command
    // stream regardless of thread scheduling".
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    Device& device = fixture.device();
    CY_REQUIRE(device.begin_frame().has_value());

    cy::u64 hashes[2] = {0, 0};
    for (cy::u64& hash : hashes) {
        cy::rhi::null::clear_command_log(device);
        cy::Expected<cy::rhi::CommandBufferHandle, cy::Error> handle =
            device.acquire_command_buffer(cy::rhi::QueueKind::Graphics, false);
        CY_REQUIRE(handle.has_value());
        CY_REQUIRE(device.begin_command_buffer(*handle).has_value());
        cy::rhi::CommandBuffer* commands = device.command_buffer(*handle);
        CY_REQUIRE(commands != nullptr);

        commands->begin_debug_label("prepass");
        commands->dispatch(8, 4, 1);
        commands->draw(3, 1, 0, 0);
        commands->end_debug_label();

        // Nothing is in the device's stream until the command buffer is submitted.
        CY_CHECK_EQ(cy::rhi::null::command_log(device).size(), 0U);
        CY_REQUIRE(device.end_command_buffer(*handle).has_value());

        cy::rhi::SubmitInfo info;
        info.queue = cy::rhi::QueueKind::Graphics;
        info.command_buffers = cy::Span<const cy::rhi::CommandBufferHandle>(&*handle, 1);
        CY_REQUIRE(device.submit(info).has_value());

        const cy::Span<const cy::rhi::null::RecordedCommand> log =
            cy::rhi::null::command_log(device);
        CY_REQUIRE_EQ(log.size(), 4U);
        CY_CHECK_EQ(log[0].kind, cy::rhi::null::CommandKind::BeginDebugLabel);
        CY_CHECK_EQ(log[1].kind, cy::rhi::null::CommandKind::Dispatch);
        CY_CHECK_EQ(log[1].a, 8U);
        CY_CHECK_EQ(log[2].kind, cy::rhi::null::CommandKind::Draw);
        hash = cy::rhi::null::command_log_hash(device);
    }
    CY_CHECK_EQ(hashes[0], hashes[1]);
    CY_CHECK_NE(hashes[0], 0U);
    CY_CHECK_EQ(device.statistics().draws, 2U);
    CY_CHECK_EQ(device.statistics().dispatches, 2U);
}

CY_TEST_CASE("a swapchain hands out images and refuses to present one it does not have") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    Device& device = fixture.device();

    cy::rhi::SwapchainDescription description;
    description.name = "window";
    description.extent = cy::rhi::Extent2D{1280, 720};
    description.min_image_count = 3;
    cy::Expected<cy::rhi::SwapchainHandle, cy::Error> swapchain =
        device.create_swapchain(description);
    CY_REQUIRE(swapchain.has_value());

    const cy::rhi::SwapchainInfo info = device.swapchain_info(*swapchain);
    CY_CHECK_EQ(info.image_count, 3U);
    CY_CHECK(info.extent == description.extent);

    cy::Expected<cy::rhi::SemaphoreHandle, cy::Error> acquired = device.create_semaphore();
    CY_REQUIRE(acquired.has_value());
    for (cy::u32 index = 0; index < 5; ++index) {
        cy::Expected<cy::u32, cy::Error> image =
            device.acquire_next_image(*swapchain, *acquired, 0);
        CY_REQUIRE(image.has_value());
        CY_CHECK_EQ(*image, index % 3);
        CY_CHECK_FALSE(device.swapchain_view(*swapchain, *image).is_null());
        CY_CHECK(device.present(*swapchain, *image, cy::rhi::SemaphoreHandle{}).has_value());
    }
    CY_CHECK_FALSE(device.present(*swapchain, 99, cy::rhi::SemaphoreHandle{}).has_value());

    // A swapchain image belongs to the swapchain; destroying it directly is a defect and is caught.
    const cy::u64 before = device.statistics().validation_errors;
    device.destroy_texture(device.swapchain_texture(*swapchain, 0));
    CY_CHECK_GT(device.statistics().validation_errors, before);

    CY_CHECK(device.resize_swapchain(*swapchain, cy::rhi::Extent2D{800, 600}).has_value());
    CY_CHECK(device.swapchain_info(*swapchain).extent == cy::rhi::Extent2D{800, 600});
    device.destroy_swapchain(*swapchain);
}

CY_TEST_CASE("GPU memory is accounted per category and reported as one figure") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    Device& device = fixture.device();

    const cy::rhi::GpuMemoryReport before = device.memory_report();
    cy::Expected<cy::rhi::TextureHandle, cy::Error> texture =
        device.create_texture(target("accounted"));
    CY_REQUIRE(texture.has_value());
    const cy::rhi::GpuMemoryReport after = device.memory_report();

    const auto persistent = static_cast<cy::u32>(cy::rhi::GpuMemoryCategory::Persistent);
    CY_CHECK_GT(after.live_bytes[persistent], before.live_bytes[persistent]);
    // 64x64 RGBA8 is 16 KiB, rounded up to the synthetic allocation alignment.
    CY_CHECK_GE(after.live_bytes[persistent] - before.live_bytes[persistent], 64ULL * 64 * 4);

    device.destroy_texture(*texture);
    CY_CHECK_EQ(device.memory_report().live_bytes[persistent], before.live_bytes[persistent]);
}

CY_TEST_CASE("the backend registry falls back to null, and says that it did") {
    CY_REQUIRE(cy::rhi::null::register_null_backend().has_value());
    CY_REQUIRE(cy::rhi::find_backend(cy::rhi::kNullBackendName) != nullptr);

    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Gpu);
    cy::rhi::BackendSelection selection;
    cy::rhi::DeviceDescription description;
    cy::Expected<cy::rhi::Device*, cy::Error> device =
        cy::rhi::create_device(allocator, "there-is-no-such-backend", description, selection);
    CY_REQUIRE(device.has_value());
    CY_CHECK(selection.fell_back);
    CY_CHECK_EQ(selection.kind, cy::rhi::BackendKind::Null);
    // "asked for X, ran null" is the diagnostic a bug report needs, and it is not recoverable from
    // the device alone.
    CY_CHECK(selection.reason[0] != '\0');
    cy::rhi::destroy_device(allocator, *device);
}
