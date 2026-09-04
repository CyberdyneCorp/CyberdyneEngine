// A frame through the render graph on a real Vulkan device, with validation on. Tasks 2.3.1 and
// 2.3.3.
//
// IT LIVES WITH THE GRAPH RATHER THAN WITH THE BACKEND, and the layer order is why: the RHI is
// layer 3 and the graph is layer 4, so a suite that needs both belongs at the higher of the two.
// That is also the honest description of what it tests — not "does Vulkan work" but "does the frame
// the graph derived run on a device and produce the bytes it should".
//
// `rhi-and-render-graph`: "Development builds SHALL enable backend validation layers", and "a frame
// that renders but trips validation is not a frame that works". So this suite runs the layers, runs
// SYNCHRONISATION VALIDATION with them — which is off by default and without which none of the
// hazard checks fire — and asserts that a whole frame produced zero validation errors.
//
// IT SKIPS RATHER THAN FAILS WHEN THERE IS NO DEVICE. Most continuous integration machines have no
// GPU, and a suite that failed there would be a suite somebody disables. The skip is loud: it
// reports which backend was selected and why, so "the Vulkan suite passed" and "the Vulkan suite
// found no Vulkan" are never confusable.

#include <cy/test/test.h>

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/graph/visualise.h>

#include <cstdio>

namespace {

using cy::rendering::ResourceId;
using cy::rhi::Access;
using cy::rhi::QueueKind;

/// A Vulkan device, or nothing. Every case builds its own: validation keeps per-queue state for the
/// process's lifetime, and recycled handles across two devices in one process produce phantom
/// cross-test hazards that look damning and are not. (M3 spike, gotcha 6e.)
void print_validation(cy::rhi::ValidationSeverity severity, const char* message,
                      void* /*user*/) noexcept {
    const char* label = "info";
    if (severity == cy::rhi::ValidationSeverity::Error) {
        label = "error";
    } else if (severity == cy::rhi::ValidationSeverity::Warning) {
        label = "warning";
    }
    std::fprintf(stderr, "vulkan validation %s: %s\n", label, message != nullptr ? message : "");
}

class VulkanFixture {
public:
    VulkanFixture() noexcept : allocator_(cy::system_allocator(cy::MemoryDomain::Gpu)) {
        (void)cy::rhi::vulkan::register_vulkan_backend();
        (void)cy::rhi::null::register_null_backend();

        cy::rhi::DeviceDescription description;
        description.application_name = "cy_test_smoke_vulkan";
        description.enable_validation = true;
        // Without this the layers are loaded and the hazard checks are silent.
        description.enable_synchronisation_validation = true;
        description.request_async_compute = true;
        device_ = cy::rhi::create_device(allocator_, "vulkan", description, selection_);
        if (device_.has_value()) {
            // `rhi-and-render-graph`: a validation error is logged rather than counted. Printing it
            // is what makes a failing assertion on validation_errors actionable — a count alone
            // says a frame is wrong without saying how.
            device_.value()->set_validation_callback(&print_validation, nullptr);
        }
    }

    ~VulkanFixture() {
        if (device_.has_value()) {
            (void)device_.value()->wait_idle();
            cy::rhi::destroy_device(allocator_, device_.value());
        }
    }

    VulkanFixture(const VulkanFixture&) = delete;
    VulkanFixture& operator=(const VulkanFixture&) = delete;
    VulkanFixture(VulkanFixture&&) = delete;
    VulkanFixture& operator=(VulkanFixture&&) = delete;

    /// True when a real Vulkan device was created. False means the machine has no loader or no
    /// device, and the case reports the reason and returns.
    [[nodiscard]] bool have_vulkan() const noexcept {
        return device_.has_value() &&
               device_.value()->capabilities().backend() == cy::rhi::BackendKind::Vulkan;
    }
    [[nodiscard]] cy::rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] cy::Allocator& allocator() const noexcept { return allocator_; }
    [[nodiscard]] const cy::rhi::BackendSelection& selection() const noexcept { return selection_; }

private:
    cy::Allocator& allocator_;
    cy::rhi::BackendSelection selection_{};
    cy::Expected<cy::rhi::Device*, cy::Error> device_ =
        cy::fail(cy::ErrorCode::Unavailable, "not created");
};

constexpr cy::u32 kSide = 16;
constexpr cy::u32 kLayers = 2;
constexpr cy::u32 kTexels = kSide * kSide;

struct FrameState {
    cy::rhi::Device* device = nullptr;
    cy::rendering::GraphExecutor* executor = nullptr;
    ResourceId upload = cy::rendering::kInvalidResource;
    ResourceId image = cy::rendering::kInvalidResource;
    ResourceId readback = cy::rendering::kInvalidResource;
    cy::u32 layer = 0;
};

/// Copy one layer of the upload buffer into one array layer of the transient image. A transfer, so
/// it needs no shader — which is what lets this suite exercise the whole graph before the shader
/// system lands.
void record_upload(const cy::rendering::PassContext& context, void* user) noexcept {
    auto* state = static_cast<FrameState*>(user);
    cy::rhi::BufferTextureCopy region;
    region.buffer_offset = static_cast<cy::u64>(state->layer) * kTexels * sizeof(cy::u32);
    region.base_layer = static_cast<cy::u16>(state->layer);
    region.layer_count = 1;
    region.texture_extent = cy::rhi::Extent3D{kSide, kSide, 1};
    context.commands->copy_buffer_to_texture(
        state->executor->buffer(state->upload), state->executor->texture(state->image),
        cy::Span<const cy::rhi::BufferTextureCopy>(&region, 1));
}

void record_download(const cy::rendering::PassContext& context, void* user) noexcept {
    auto* state = static_cast<FrameState*>(user);
    cy::rhi::BufferTextureCopy regions[kLayers];
    for (cy::u32 layer = 0; layer < kLayers; ++layer) {
        regions[layer] = cy::rhi::BufferTextureCopy{};
        regions[layer].buffer_offset = static_cast<cy::u64>(layer) * kTexels * sizeof(cy::u32);
        regions[layer].base_layer = static_cast<cy::u16>(layer);
        regions[layer].layer_count = 1;
        regions[layer].texture_extent = cy::rhi::Extent3D{kSide, kSide, 1};
    }
    context.commands->copy_texture_to_buffer(
        state->executor->texture(state->image), state->executor->buffer(state->readback),
        cy::Span<const cy::rhi::BufferTextureCopy>(regions, kLayers));
}

}  // namespace

CY_TEST_CASE("a Vulkan device reports itself by capability, never by identity") {
    VulkanFixture fixture;
    if (!fixture.have_vulkan()) {
        CY_TEST_MESSAGE("no Vulkan device on this machine; the backend selected was '"
                        << fixture.selection().selected << "' because "
                        << fixture.selection().reason);
        return;
    }
    const cy::rhi::DeviceCapabilities& capabilities = fixture.device().capabilities();

    CY_CHECK_EQ(capabilities.backend(), cy::rhi::BackendKind::Vulkan);
    CY_CHECK(capabilities.device_name()[0] != '\0');
    CY_CHECK(capabilities.driver_version()[0] != '\0');
    // Printed rather than passed through doctest's stringifier, which renders a const char* as its
    // address rather than its text.
    std::fprintf(stderr, "device: %s, driver %s\n", capabilities.device_name(),
                 capabilities.driver_version());

    // Vulkan 1.3 is the baseline precisely so that these are not optional.
    CY_CHECK(capabilities.has(cy::rhi::Capability::DynamicRendering));
    CY_CHECK(capabilities.has(cy::rhi::Capability::ComputeShaders));

    // The engine's hard limits are a portability contract, checked once at device creation. A
    // device that did not meet them would have failed to create, so reaching here proves it.
    cy::rhi::ValidationMessage message;
    CY_CHECK(cy::rhi::validate_device_limits(capabilities.limits(), message).has_value());
    CY_CHECK_GE(capabilities.limits().max_bound_descriptor_sets, cy::rhi::kMaxDescriptorSets);

    // Queues: whatever this device has. Async compute is "has COMPUTE and NOT GRAPHICS", so a
    // device without a dedicated family reports false and the graph folds onto graphics.
    CY_CHECK(fixture.device().has_queue(QueueKind::Graphics));
    if (fixture.device().has_queue(QueueKind::AsyncCompute)) {
        CY_CHECK_NE(fixture.device().queue_family(QueueKind::AsyncCompute),
                    fixture.device().queue_family(QueueKind::Graphics));
    }
}

CY_TEST_CASE("a frame through the graph moves real bytes and trips no validation") {
    // THE CASE THE MILESTONE IS ABOUT, on a device. Two passes write different array layers of one
    // image on one queue; a pass on another queue reads both; a recordless pass declares the host
    // boundary. Every barrier, the queue-family ownership transfer and the semaphore between the
    // submits are derived — no pass emits one, and none could.
    VulkanFixture fixture;
    if (!fixture.have_vulkan()) {
        CY_TEST_MESSAGE("no Vulkan device on this machine; skipping");
        return;
    }
    cy::rhi::Device& device = fixture.device();
    CY_REQUIRE(device.begin_frame().has_value());

    // Upload: a pattern per layer, so a wrong barrier shows up as wrong bytes rather than as a
    // crash. Layer 0 is 0xA0000 + index, layer 1 is 0xB0000 + index — the spike's own pattern.
    cy::rhi::BufferDescription upload_description;
    upload_description.name = "upload";
    upload_description.size = static_cast<cy::u64>(kLayers) * kTexels * sizeof(cy::u32);
    upload_description.usage = cy::rhi::BufferUsage::TransferSource;
    upload_description.memory = cy::rhi::MemoryUse::Upload;
    cy::Expected<cy::rhi::BufferHandle, cy::Error> upload =
        device.create_buffer(upload_description);
    CY_REQUIRE(upload.has_value());
    auto* upload_bytes = static_cast<cy::u32*>(device.buffer_mapped_pointer(*upload));
    CY_REQUIRE(upload_bytes != nullptr);
    for (cy::u32 layer = 0; layer < kLayers; ++layer) {
        for (cy::u32 index = 0; index < kTexels; ++index) {
            upload_bytes[(layer * kTexels) + index] = (layer == 0 ? 0xA0000U : 0xB0000U) + index;
        }
    }

    cy::rhi::BufferDescription readback_description;
    readback_description.name = "readback";
    readback_description.size = upload_description.size;
    readback_description.usage = cy::rhi::BufferUsage::TransferDestination;
    readback_description.memory = cy::rhi::MemoryUse::Readback;
    cy::Expected<cy::rhi::BufferHandle, cy::Error> readback =
        device.create_buffer(readback_description);
    CY_REQUIRE(readback.has_value());

    cy::rendering::RenderGraph graph(fixture.allocator());
    cy::rendering::GraphExecutor executor(fixture.allocator(), device);

    cy::rendering::TextureRequest image_request;
    image_request.name = "layers";
    image_request.format = cy::rhi::Format::R32Uint;
    image_request.width = kSide;
    image_request.height = kSide;
    image_request.array_layers = kLayers;
    const ResourceId image = graph.create_texture(image_request);

    cy::rendering::BufferRequest upload_request;
    upload_request.name = "upload";
    upload_request.size = upload_description.size;
    upload_request.extra_usage = cy::rhi::BufferUsage::TransferSource;
    const ResourceId upload_id = graph.import_buffer(upload_request, *upload);

    cy::rendering::BufferRequest readback_request;
    readback_request.name = "readback";
    readback_request.size = readback_description.size;
    readback_request.extra_usage = cy::rhi::BufferUsage::TransferDestination;
    const ResourceId readback_id = graph.import_buffer(readback_request, *readback);

    // The producing queue: the dedicated async-compute family where the device has one, and
    // graphics where it does not. Branching on the capability rather than on the device.
    const QueueKind producer =
        device.has_queue(QueueKind::AsyncCompute) ? QueueKind::AsyncCompute : QueueKind::Graphics;

    FrameState states[kLayers + 1];
    for (cy::u32 layer = 0; layer < kLayers; ++layer) {
        states[layer] = FrameState{&device, &executor, upload_id, image, readback_id, layer};
        graph.add_pass("upload layer", producer)
            .read(upload_id, Access::TransferRead)
            .write(image, Access::TransferWrite,
                   cy::rhi::SubresourceRange::layer(static_cast<cy::u16>(layer)))
            .record(&record_upload, &states[layer]);
    }
    states[kLayers] = FrameState{&device, &executor, upload_id, image, readback_id, 0};
    graph.add_pass("download", QueueKind::Graphics)
        .read(image, Access::TransferRead)
        .write(readback_id, Access::TransferWrite)
        .record(&record_download, &states[kLayers]);
    // The host boundary declared as a dependency, so the graph emits the transfer-to-host barrier.
    graph.add_pass("host", QueueKind::Graphics).read(readback_id, Access::HostRead).side_effect();
    CY_REQUIRE(graph.status().has_value());

    cy::Expected<cy::rendering::ExecutionResult, cy::Error> result =
        executor.execute(graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{});
    CY_REQUIRE(result.has_value());
    CY_CHECK_EQ(result->passes_recorded, kLayers + 2);
    CY_CHECK_GT(result->barriers, 0U);
    if (producer == QueueKind::AsyncCompute) {
        // Two submits, one semaphore, and one coalesced ownership release over layers [0, 2).
        CY_CHECK_EQ(result->submits, 2U);
        CY_CHECK_GE(result->queue_ownership_transfers, 1U);
    } else {
        CY_CHECK_EQ(result->submits, 1U);
        CY_CHECK_EQ(result->queue_ownership_transfers, 0U);
    }

    CY_REQUIRE(device.wait_idle().has_value());
    CY_REQUIRE(device.end_frame().has_value());

    // THE BYTES. A wrong barrier here is wrong data, not a crash: this is what the spike measured
    // as 256/256 pixels exactly correct, and it is the assertion a validation layer cannot make.
    const auto* readback_bytes =
        static_cast<const cy::u32*>(device.buffer_mapped_pointer(*readback));
    CY_REQUIRE(readback_bytes != nullptr);
    cy::u32 wrong = 0;
    for (cy::u32 layer = 0; layer < kLayers; ++layer) {
        for (cy::u32 index = 0; index < kTexels; ++index) {
            const cy::u32 expected = (layer == 0 ? 0xA0000U : 0xB0000U) + index;
            if (readback_bytes[(layer * kTexels) + index] != expected) {
                ++wrong;
            }
        }
    }
    CY_CHECK_EQ(wrong, 0U);

    // AND NO VALIDATION ERROR. With synchronisation validation on, a missing barrier between the
    // two queues would be SYNC-HAZARD-WRITE-RACING-WRITE here.
    CY_CHECK_EQ(device.statistics().validation_errors, 0U);
    CY_CHECK_EQ(device.statistics().validation_warnings, 0U);

    executor.release();
    device.destroy_buffer(*upload);
    device.destroy_buffer(*readback);
}

CY_TEST_CASE("transient aliasing reduces the device's own reported heap usage") {
    // Task 7.3, measured rather than asserted. M3's spike measured 64.00 MiB -> 8.00 MiB on the
    // device with VK_EXT_memory_budget, and the plan and the device agreed exactly. Here the plan
    // is checked against itself and, where the device reports a budget, against the device.
    VulkanFixture fixture;
    if (!fixture.have_vulkan()) {
        CY_TEST_MESSAGE("no Vulkan device on this machine; skipping");
        return;
    }
    cy::rhi::Device& device = fixture.device();
    CY_REQUIRE(device.begin_frame().has_value());

    constexpr cy::u32 kChain = 8;
    constexpr cy::u64 kMebibyte = 1024ULL * 1024ULL;

    cy::rendering::RenderGraph graph(fixture.allocator());
    cy::rendering::GraphExecutor executor(fixture.allocator(), device);

    ResourceId links[kChain] = {};
    for (ResourceId& link : links) {
        cy::rendering::TextureRequest request;
        request.name = "link";
        request.format = cy::rhi::Format::R32Uint;
        request.width = 512;
        request.height = 512;
        link = graph.create_texture(request);
    }
    cy::rendering::BufferRequest out_request;
    out_request.name = "result";
    out_request.size = 4096;
    out_request.extra_usage = cy::rhi::BufferUsage::TransferDestination;

    cy::rhi::BufferDescription out_description;
    out_description.name = "result";
    out_description.size = 4096;
    out_description.usage = cy::rhi::BufferUsage::TransferDestination;
    out_description.memory = cy::rhi::MemoryUse::Readback;
    cy::Expected<cy::rhi::BufferHandle, cy::Error> out = device.create_buffer(out_description);
    CY_REQUIRE(out.has_value());
    const ResourceId out_id = graph.import_buffer(out_request, *out);

    graph.add_pass("seed", QueueKind::Graphics).write(links[0], Access::TransferWrite);
    for (cy::u32 index = 1; index < kChain; ++index) {
        graph.add_pass("step", QueueKind::Graphics)
            .read(links[index - 1], Access::TransferRead)
            .write(links[index], Access::TransferWrite);
    }
    graph.add_pass("copy out", QueueKind::Graphics)
        .read(links[kChain - 1], Access::TransferRead)
        .write(out_id, Access::TransferWrite);
    CY_REQUIRE(graph.status().has_value());

    cy::Expected<cy::rendering::CompiledGraph, cy::Error> plan =
        executor.compile_only(graph, cy::rendering::CompileOptions{});
    CY_REQUIRE(plan.has_value());

    // Eight 1 MiB images that ping-pong between two slots: 8 MiB without aliasing, 2 MiB with it.
    CY_CHECK_EQ(plan->memory.naive_bytes, static_cast<cy::u64>(kChain) * kMebibyte);
    CY_CHECK_EQ(plan->memory.heap_bytes, 2ULL * kMebibyte);
    CY_CHECK_EQ(device.transient_pool_bytes(), plan->memory.heap_bytes);
    CY_CHECK(cy::rendering::validate_plan(graph, *plan).has_value());

    const cy::rhi::GpuMemoryReport report = device.memory_report();
    const auto transient = static_cast<cy::u32>(cy::rhi::GpuMemoryCategory::Transient);
    CY_CHECK_EQ(report.live_bytes[transient], plan->memory.heap_bytes);
    if (device.capabilities().has(cy::rhi::Capability::MemoryBudgetReporting)) {
        // The device's own heap figure, which is the permanent gate an aliasing regression trips.
        CY_TEST_MESSAGE("device heap used " << report.device_heap_used << " of "
                                            << report.device_heap_budget);
        CY_CHECK_GT(report.device_heap_budget, 0U);
    }

    executor.release();
    CY_REQUIRE(device.end_frame().has_value());
    device.destroy_buffer(*out);
    CY_CHECK_EQ(device.statistics().validation_errors, 0U);
}

CY_TEST_CASE("passes recorded on job workers reach the device with the same bytes") {
    // THE REGRESSION TEST FOR A REAL DEFECT. Until M3's close this executor acquired every
    // secondary command buffer on one thread and handed them to workers to record into. A backend's
    // command allocator is externally synchronised — a VkCommandPool may not be touched by two
    // threads at once, and that covers recording into any buffer allocated from it — so that shape
    // is a data race. It was found by running the null backend's parallel-recording case
    // repeatedly, where the same defect corrupted the heap; the fix is one command pool per (frame,
    // queue, recording thread), taken by the thread that will record.
    //
    // Here it is checked where it matters: on a device, with the validation layers on, with the
    // bytes verified.
    VulkanFixture fixture;
    if (!fixture.have_vulkan()) {
        CY_TEST_MESSAGE("no Vulkan device on this machine; skipping");
        return;
    }
    cy::jobs::JobSystemConfig config;
    config.worker_count = 4;
    config.task_slots_per_participant = 2048;
    config.deque_capacity = 2048;
    config.scratch_bytes_per_participant = cy::usize{256} * 1024;
    cy::jobs::JobSystem jobs;
    CY_REQUIRE(jobs.start(config).has_value());

    cy::rhi::Device& device = fixture.device();
    CY_REQUIRE(device.begin_frame().has_value());

    cy::rhi::BufferDescription upload_description;
    upload_description.name = "upload";
    upload_description.size = static_cast<cy::u64>(kLayers) * kTexels * sizeof(cy::u32);
    upload_description.usage = cy::rhi::BufferUsage::TransferSource;
    upload_description.memory = cy::rhi::MemoryUse::Upload;
    cy::Expected<cy::rhi::BufferHandle, cy::Error> upload =
        device.create_buffer(upload_description);
    CY_REQUIRE(upload.has_value());
    auto* upload_bytes = static_cast<cy::u32*>(device.buffer_mapped_pointer(*upload));
    CY_REQUIRE(upload_bytes != nullptr);
    for (cy::u32 layer = 0; layer < kLayers; ++layer) {
        for (cy::u32 index = 0; index < kTexels; ++index) {
            upload_bytes[(layer * kTexels) + index] = (layer == 0 ? 0xC0000U : 0xD0000U) + index;
        }
    }

    cy::rhi::BufferDescription readback_description;
    readback_description.name = "readback";
    readback_description.size = upload_description.size;
    readback_description.usage = cy::rhi::BufferUsage::TransferDestination;
    readback_description.memory = cy::rhi::MemoryUse::Readback;
    cy::Expected<cy::rhi::BufferHandle, cy::Error> readback =
        device.create_buffer(readback_description);
    CY_REQUIRE(readback.has_value());

    cy::rendering::RenderGraph graph(fixture.allocator());
    cy::rendering::GraphExecutor executor(fixture.allocator(), device);

    cy::rendering::TextureRequest image_request;
    image_request.name = "layers";
    image_request.format = cy::rhi::Format::R32Uint;
    image_request.width = kSide;
    image_request.height = kSide;
    image_request.array_layers = kLayers;
    const ResourceId image = graph.create_texture(image_request);

    cy::rendering::BufferRequest upload_request;
    upload_request.name = "upload";
    upload_request.size = upload_description.size;
    upload_request.extra_usage = cy::rhi::BufferUsage::TransferSource;
    const ResourceId upload_id = graph.import_buffer(upload_request, *upload);

    cy::rendering::BufferRequest readback_request;
    readback_request.name = "readback";
    readback_request.size = readback_description.size;
    readback_request.extra_usage = cy::rhi::BufferUsage::TransferDestination;
    const ResourceId readback_id = graph.import_buffer(readback_request, *readback);

    // Everything on one queue, so that what is under test is parallel recording and not the
    // cross-queue plan the case above already covers.
    FrameState states[kLayers + 1];
    for (cy::u32 layer = 0; layer < kLayers; ++layer) {
        states[layer] = FrameState{&device, &executor, upload_id, image, readback_id, layer};
        graph.add_pass("upload layer", QueueKind::Graphics)
            .read(upload_id, Access::TransferRead)
            .write(image, Access::TransferWrite,
                   cy::rhi::SubresourceRange::layer(static_cast<cy::u16>(layer)))
            .record(&record_upload, &states[layer]);
    }
    states[kLayers] = FrameState{&device, &executor, upload_id, image, readback_id, 0};
    graph.add_pass("download", QueueKind::Graphics)
        .read(image, Access::TransferRead)
        .write(readback_id, Access::TransferWrite)
        .record(&record_download, &states[kLayers]);
    graph.add_pass("host", QueueKind::Graphics).read(readback_id, Access::HostRead).side_effect();
    CY_REQUIRE(graph.status().has_value());

    cy::rendering::ExecuteOptions options;
    options.parallel_recording = true;
    options.parallel_pass_threshold = 2;
    options.job_system = &jobs;

    cy::Expected<cy::rendering::ExecutionResult, cy::Error> result =
        executor.execute(graph, cy::rendering::CompileOptions{}, options);
    CY_REQUIRE(result.has_value());
    CY_CHECK_EQ(result->secondary_command_buffers, kLayers + 2);

    CY_REQUIRE(device.wait_idle().has_value());
    CY_REQUIRE(device.end_frame().has_value());

    const auto* readback_bytes =
        static_cast<const cy::u32*>(device.buffer_mapped_pointer(*readback));
    CY_REQUIRE(readback_bytes != nullptr);
    cy::u32 wrong = 0;
    for (cy::u32 layer = 0; layer < kLayers; ++layer) {
        for (cy::u32 index = 0; index < kTexels; ++index) {
            const cy::u32 expected = (layer == 0 ? 0xC0000U : 0xD0000U) + index;
            if (readback_bytes[(layer * kTexels) + index] != expected) {
                ++wrong;
            }
        }
    }
    CY_CHECK_EQ(wrong, 0U);
    CY_CHECK_EQ(device.statistics().validation_errors, 0U);

    executor.release();
    device.destroy_buffer(*upload);
    device.destroy_buffer(*readback);
    jobs.shutdown();
}
