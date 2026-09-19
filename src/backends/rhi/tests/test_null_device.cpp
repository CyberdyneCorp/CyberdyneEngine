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
#include <cy/core/assets/file.h>
#include <cy/core/memory/domain.h>
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

CY_TEST_CASE("publishing memory pressure reports the device heap into MemoryDomain::Gpu") {
    // M11.d task 6.2. `MemoryDomain::Gpu` has been budgeted since M1 — 768 MiB soft on the desktop
    // profile — and until now **no backend reported a byte into it**, so the budget was compared
    // against zero and a per-domain report showed a GPU holding nothing. `rhi-and-render-graph`
    // requires that "GPU memory SHALL appear in the same domain and budget model as CPU memory";
    // the pressure half of that sentence was implemented and the domain half was not.
    //
    // The two reads below bracket ONE call that allocates nothing, so the difference between them
    // is the reported device heap and cannot be host memory the fixture happened to take — which
    // matters here because this suite's devices are deliberately built on a Gpu-domain allocator.
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    Device& device = fixture.device();

    cy::Expected<cy::rhi::TextureHandle, cy::Error> texture =
        device.create_texture(target("domain-reported"));
    CY_REQUIRE(texture.has_value());
    const cy::u64 heap = device.memory_report().device_heap_used;

    const cy::u64 before = cy::domain_stats(cy::MemoryDomain::Gpu).live_bytes;
    device.publish_memory_pressure();
    const cy::u64 after = cy::domain_stats(cy::MemoryDomain::Gpu).live_bytes;
    CY_CHECK_GT(after, before);
    CY_CHECK_EQ(after - before, heap);

    // And a heap that FALLS is reported as a free rather than accumulating, which is what makes a
    // rising row a leak rather than an artefact of the reporting.
    device.destroy_texture(*texture);
    device.publish_memory_pressure();
    const cy::u64 released = cy::domain_stats(cy::MemoryDomain::Gpu).live_bytes;
    CY_CHECK_LT(released, after);
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

CY_TEST_CASE("the global texture table is nameable in a pipeline layout and is the device's own") {
    // THE DEVICE-FREE HALF OF M11.c TASK 3.7. What no machine without a GPU can settle is whether a
    // shader's sample came back with the texture's texels — `render.material_binding` does that.
    // What every machine can settle is the CONTRACT that made the sample possible: the table has a
    // set layout handle and a set handle, a pipeline layout takes the layout, and the device keeps
    // ownership of both. Before M11.c none of those existed: `bindless_layout_` and `bindless_set_`
    // were Vulkan objects with no handle, so nothing above the backend could name either one and
    // every index the table handed out addressed a descriptor no shader could reach.
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    Device& device = fixture.device();

    const cy::rhi::DescriptorSetLayoutHandle layout = device.global_texture_table_layout();
    const cy::rhi::DescriptorSetHandle set = device.global_texture_table();
    CY_REQUIRE_FALSE(layout.is_null());
    CY_REQUIRE_FALSE(set.is_null());

    // The whole point: a pipeline layout accepts it at set 0, which is where `cy/material.slang`
    // declares `cyMaterialTextures[]`.
    cy::rhi::PipelineLayoutDescription description;
    description.name = "global table only";
    description.set_layouts = cy::Span<const cy::rhi::DescriptorSetLayoutHandle>(&layout, 1);
    cy::Expected<cy::rhi::PipelineLayoutHandle, cy::Error> pipeline_layout =
        device.create_pipeline_layout(description);
    CY_REQUIRE(pipeline_layout.has_value());
    device.destroy_pipeline_layout(*pipeline_layout);

    // And the device keeps it. Every caller that names the table holds the SAME handle, so one of
    // them destroying it would take the table down under all the others.
    device.destroy_descriptor_set_layout(layout);
    CY_CHECK_EQ(device.global_texture_table_layout(), layout);
    cy::Expected<cy::rhi::PipelineLayoutHandle, cy::Error> after =
        device.create_pipeline_layout(description);
    CY_CHECK(after.has_value());
    if (after.has_value()) {
        device.destroy_pipeline_layout(*after);
    }
}

CY_TEST_CASE("the global table reads through one sampler, and says so rather than replacing it") {
    // `cy/material.slang` declares `SamplerState cyMaterialSampler` — a scalar. A shader sampling
    // slot `i` has no second sampler to choose, so a device that quietly accepted a different one
    // would change the filtering of every texture already in the table.
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    Device& device = fixture.device();

    cy::rhi::SamplerDescription description;
    description.name = "table sampler";
    cy::Expected<cy::rhi::SamplerHandle, cy::Error> first = device.create_sampler(description);
    CY_REQUIRE(first.has_value());
    CY_CHECK(device.set_global_sampler(*first).has_value());
    // The same one again is not a change and is not an error.
    CY_CHECK(device.set_global_sampler(*first).has_value());

    description.name = "a second table sampler";
    description.max_anisotropy = 16.0F;
    cy::Expected<cy::rhi::SamplerHandle, cy::Error> second = device.create_sampler(description);
    CY_REQUIRE(second.has_value());
    CY_CHECK_FALSE(device.set_global_sampler(*second).has_value());

    device.destroy_sampler(*second);
    device.destroy_sampler(*first);
}

CY_TEST_CASE("the pipeline cache takes a path, and an absent one is a cold start") {
    // METAL GAP 6. `save_pipeline_cache` used to hand back a blob, which `MTLBinaryArchive` cannot
    // produce without writing a file and reading it back; it takes a path now. The contract every
    // backend holds is checked HERE, on the one backend every test run has: the file is written
    // even when there is nothing to persist, an absent file loads as a cold start rather than an
    // error, and a written one loads back.
    //
    // WHAT THIS DOES NOT SHOW, and it is the finding beside the signature: NOTHING IN THE ENGINE
    // CALLS EITHER OF THEM. `rhi-and-render-graph` requires the cache to be "persisted across runs,
    // so a warm start compiles nothing", and that is unimplemented above the RHI. Changing a
    // signature does not implement it and this case does not claim it does.
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    Device& device = fixture.device();

    const char* path = "cy-null-pipeline-cache.bin";
    (void)cy::assets::fs::remove_file(path);

    // A cold start: no file at all.
    CY_CHECK(device.load_pipeline_cache(path).has_value());

    CY_CHECK(device.save_pipeline_cache(path).has_value());
    CY_CHECK(cy::assets::fs::exists(path));
    CY_CHECK(device.load_pipeline_cache(path).has_value());

    // A path a caller forgot to fill in is refused rather than silently doing nothing.
    CY_CHECK_FALSE(device.save_pipeline_cache("").has_value());
    CY_CHECK_FALSE(device.load_pipeline_cache(nullptr).has_value());

    (void)cy::assets::fs::remove_file(path);
}

CY_TEST_CASE(
    "the null backend states the two answers Metal gaps 1 and 5 turned into capabilities") {
    // A capability nothing SETS is a capability every device answers the same way by accident —
    // which is exactly the defect `RayTracingObservation` was built to make impossible after
    // `Capability::RayTracing` went eight milestones with no writer. These two are new at M11.d, so
    // they are asserted the day they are added rather than the milestone somebody notices.
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    const cy::rhi::DeviceCapabilities& caps = fixture.device().capabilities();

    // Gap 1: this device consumes SPIR-V, and says so rather than leaving the default to mean it.
    CY_CHECK_EQ(caps.native_shader_format(), cy::rhi::ShaderFormat::Spirv);
    // Gap 5: the null backend implements Vulkan's secondary-command-buffer model — it is the
    // reference — so the graph's parallel path is exercised on a machine with no GPU.
    CY_CHECK(caps.has(cy::rhi::Capability::ParallelPassRecording));
    // Gap 4: one ownership domain for every queue, which is what "no dedicated async compute"
    // means and what makes the single-queue fold emit zero transfers.
    CY_CHECK(caps.needs_queue_ownership_transfer());
    CY_CHECK_EQ(caps.queue_ownership_domain(cy::rhi::QueueKind::AsyncCompute),
                caps.queue_ownership_domain(cy::rhi::QueueKind::Graphics));
}

CY_TEST_CASE("a depth target the device cannot support is refused, not substituted") {
    // METAL GAP 7's REFUSAL. The per-format query has answered since M3 with no consumer above
    // `src/backends/rhi/`; a backend was free to swap in something of a different precision and a
    // different footprint with nothing saying so. Now the device says no and names the call that
    // picks the substitute, which is the engine making the decision rather than the backend.
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    Device& device = fixture.device();

    cy::rhi::TextureDescription depth;
    depth.name = "depth";
    depth.format = cy::rhi::Format::D32Sfloat;
    depth.extent = {64, 64, 1};
    depth.usage = cy::rhi::TextureUsage::DepthStencilAttachment;
    cy::Expected<cy::rhi::TextureHandle, cy::Error> supported = device.create_texture(depth);
    CY_REQUIRE(supported.has_value());
    device.destroy_texture(*supported);

    // A colour format declared as a depth attachment is the same refusal from the other side, and
    // it is the one this backend can produce without lying about what it supports.
    depth.format = cy::rhi::Format::Rgba8Unorm;
    CY_CHECK_FALSE(device.create_texture(depth).has_value());
}
