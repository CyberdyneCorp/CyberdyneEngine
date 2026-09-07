// Virtual texturing on the device: feedback written by a shader, resolved on the GPU, and a page
// table sampled by a shader. M7 tasks 4.1 and 4.2.
//
// ================================================================================================
// WHAT M6 LEFT, AND WHAT THESE CASES ASSERT
// ================================================================================================
//
// M6's closing gate recorded, of this capability: nothing in virtual texturing is on a GPU — no
// feedback buffer written by a shader, no page table sampled by one. Both halves are here, and each
// is checked against the CPU implementation that has existed since M6 rather than against a
// picture:
//
//   Task 4.1. `record_feedback` runs one thread per pixel over a 256x256 grid and increments one
//   word per PAGE. `resolve_feedback` compacts the non-zero words on the device. The case builds
//   the same map on the CPU from `address_of_pixel` and asserts the two agree entry for entry — and
//   then asserts what the CPU actually MAPPED, in bytes, against the pixel count. "A per-pixel
//   request stream SHALL NOT reach the CPU" is a measurement here, not a claim.
//
//   Task 4.2. `sample_pages` walks the page table from the level asked for towards the coarsest,
//   over the WHOLE address space, and the case compares every entry against
//   `VirtualTextureSystem::sample()`. It runs TWICE: once before the mip tail is made resident,
//   where `missing` must be true somewhere, and once after, where it must be false everywhere. The
//   second assertion is worth nothing without the first.

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/virtual_texturing/frame.h>
#include <cy/servers/render/virtual_texturing/system.h>
#include <cy/test/test.h>

#include <cstdio>
#include <map>
#include <vector>

using cy::f32;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::render::vt::TileCacheDesc;
using cy::render::vt::VirtualAddress;
using cy::render::vt::VirtualTextureDesc;
using cy::render::vt::VirtualTextureSystem;
using cy::rendering::vt::FeedbackSettings;
using cy::rendering::vt::GpuSampleResult;
using cy::rendering::vt::VirtualTextureFrame;
using cy::rendering::vt::VirtualTextureFrameReadback;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

void count_validation(cy::rhi::ValidationSeverity severity, const char* message,
                      void* user) noexcept {
    if (severity == cy::rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
    }
    std::fprintf(stderr, "vulkan validation %s: %s\n",
                 severity == cy::rhi::ValidationSeverity::Error ? "error" : "warning",
                 message != nullptr ? message : "");
}

/// A device of its own per case, for the reason tests/render/device.h gives.
class DeviceFixture {
public:
    DeviceFixture() noexcept : allocator_(cy::system_allocator(cy::MemoryDomain::Gpu)) {
        (void)cy::rhi::vulkan::register_vulkan_backend();
        (void)cy::rhi::null::register_null_backend();
        cy::rhi::DeviceDescription description;
        description.application_name = "cy_test_render_virtual_texturing";
        description.enable_validation = true;
        description.enable_synchronisation_validation = true;
        description.request_async_compute = false;
        device_ = cy::rhi::create_device(allocator_, "vulkan", description, selection_);
        if (device_.has_value()) {
            device_.value()->set_validation_callback(&count_validation, &validation_errors_);
        }
    }

    ~DeviceFixture() {
        if (device_.has_value()) {
            (void)device_.value()->wait_idle();
            cy::rhi::destroy_device(allocator_, device_.value());
        }
    }

    DeviceFixture(const DeviceFixture&) = delete;
    DeviceFixture& operator=(const DeviceFixture&) = delete;

    [[nodiscard]] bool has_gpu() const noexcept {
        return device_.has_value() &&
               device_.value()->capabilities().backend() == cy::rhi::BackendKind::Vulkan;
    }
    [[nodiscard]] cy::rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] u32 validation_errors() const noexcept { return validation_errors_; }

    void report_skip() const noexcept {
        std::fprintf(stderr,
                     "no Vulkan device on this machine; the backend selected was '%s' because %s\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)",
                     selection_.reason != nullptr ? selection_.reason : "(no reason given)");
    }

private:
    cy::Allocator& allocator_;
    cy::rhi::BackendSelection selection_{};
    u32 validation_errors_ = 0;
    cy::Expected<cy::rhi::Device*, cy::Error> device_ =
        cy::fail(cy::ErrorCode::Unavailable, "not created");
};

/// A 2048-texel terrain page set: 128-texel tiles, five mips, two of them the tail. 341 pages over
/// the pyramid, which is small enough to dispatch one thread per page and large enough that a
/// feedback pass touches several levels.
VirtualTextureDesc terrain_desc() noexcept {
    VirtualTextureDesc desc;
    desc.id = 11;
    desc.width = 2048;
    desc.height = 2048;
    desc.tile_size = 128;
    desc.border = 4;
    desc.mip_count = 5;
    desc.layers = 1;
    desc.mip_tail_levels = 2;
    desc.bytes_per_tile = 16 * 1024;
    return desc;
}

/// Run one frame of the three dispatches.
///
/// EVERY STEP IS A REQUIRE, INCLUDING `begin_frame` AND `end_frame`. An earlier draft of this
/// helper returned false when `begin_frame` failed and the callers wrote `if (!run_frame(...))
/// return;` — so when the missing `end_frame` made the SECOND frame of a case fail to begin, two
/// cases returned early, printed nothing, and reported success over the half they had run. That is
/// M7 task 5b.5's rule ("an artefact that reports a gap SHALL NOT exit zero") in a test's clothing,
/// and it cost the page-table case's whole after-the-tail half before the assertion count gave it
/// away.
void run_frame(DeviceFixture& gpu, VirtualTextureFrame& frame) {
    CY_REQUIRE(gpu.device().begin_frame().has_value());
    {
        cy::rendering::RenderGraph graph(allocator());
        CY_REQUIRE(frame.declare(graph).has_value());
        cy::rendering::GraphExecutor executor(allocator(), gpu.device());
        CY_REQUIRE(
            executor
                .execute(graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{})
                .has_value());
        CY_REQUIRE(gpu.device().wait_idle().has_value());
    }
    // The frame has to be ENDED, not merely waited on: `begin_frame` recycles the oldest in-flight
    // frame's pools and a device whose frames are all still open refuses to start another.
    CY_REQUIRE(gpu.device().end_frame().has_value());
}

}  // namespace

CY_TEST_CASE("vt feedback: a shader writes it, the GPU resolves it, and no pixel reaches the CPU") {
    DeviceFixture gpu;
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }

    VirtualTextureFrame frame;
    FeedbackSettings settings;
    settings.grid_width = 256;
    settings.grid_height = 256;
    settings.density = 1;
    settings.request_capacity = 1024;
    CY_REQUIRE(frame.create(allocator(), gpu.device(), terrain_desc(), settings).has_value());

    // The page table is uploaded even though this case is about feedback: `sample_pages` runs in
    // the same graph and reads it, and a dispatch reading an uninitialised storage buffer is a
    // validation error rather than a value nobody looks at.
    VirtualTextureSystem system(allocator());
    CY_REQUIRE(system.register_texture(terrain_desc()).has_value());
    CY_REQUIRE(frame.upload_page_table(*system.page_table(terrain_desc().id)).has_value());

    run_frame(gpu, frame);
    cy::Expected<VirtualTextureFrameReadback, cy::Error> readback = frame.read_back();
    CY_REQUIRE(readback.has_value());

    // THE CPU MIRROR. `address_of_pixel` is `record_feedback`'s arithmetic, line for line, and
    // `samples_pixel` is `FeedbackBuffer::samples_pixel`'s rule. Building the map here is what
    // makes the device's compaction checkable rather than merely plausible.
    std::map<u64, u32> expected;
    u32 reporting_pixels = 0;
    for (u32 y = 0; y < settings.grid_height; ++y) {
        for (u32 x = 0; x < settings.grid_width; ++x) {
            if (!frame.samples_pixel(x, y)) {
                continue;
            }
            ++reporting_pixels;
            expected[frame.address_of_pixel(x, y).encode()] += 1;
        }
    }
    CY_CHECK(reporting_pixels == settings.grid_width * settings.grid_height);

    CY_CHECK(readback->dropped == 0);
    CY_REQUIRE(readback->requests.size() == expected.size());
    CY_CHECK(readback->total_samples == reporting_pixels);

    // Entry for entry, and IN ORDER: the device compacts in ascending page index, which is
    // ascending mip-major address, and `std::map` iterates in ascending key order. A compaction
    // that appended atomically would pass a set comparison and fail this one, which is why the
    // compaction is a prefix scan.
    auto want = expected.begin();
    bool spans_several_mips = false;
    u8 first_mip = 0xFF;
    for (cy::usize index = 0; index < readback->requests.size(); ++index, ++want) {
        const cy::render::vt::FeedbackRequest& got = readback->requests[index];
        CY_CHECK(got.address == want->first);
        CY_CHECK(got.samples == want->second);
        const VirtualAddress address = VirtualAddress::decode(got.address);
        CY_CHECK(address.texture == terrain_desc().id);
        if (first_mip == 0xFF) {
            first_mip = address.mip;
        } else if (address.mip != first_mip) {
            spans_several_mips = true;
        }
    }
    // NOT VACUOUS. A feedback pass that asked for one page at one level would satisfy every
    // assertion above and would demonstrate nothing about a pyramid.
    CY_CHECK(spans_several_mips);
    CY_CHECK(readback->requests.size() > 4);

    // ================================================================================================
    // THE REQUIREMENT, AS A MEASUREMENT
    // ================================================================================================
    //
    // "A per-pixel request stream SHALL NOT reach the CPU." What the CPU mapped is
    // `bytes_read`; what a per-pixel stream would have been is four bytes per reporting pixel. The
    // ratio is printed so that a change which quietly started reading more shows up as a number
    // that moved rather than as a test that still passes.
    const u64 per_pixel_stream = static_cast<u64>(reporting_pixels) * sizeof(u32);
    CY_CHECK(readback->bytes_read * 16 < per_pixel_stream);
    std::fprintf(stderr,
                 "vt feedback: %u pixels reported, %zu pages compacted on the device, %llu bytes "
                 "mapped by the CPU against %llu for a per-pixel stream (%.0fx smaller)\n",
                 reporting_pixels, static_cast<size_t>(readback->requests.size()),
                 static_cast<unsigned long long>(readback->bytes_read),
                 static_cast<unsigned long long>(per_pixel_stream),
                 static_cast<double>(per_pixel_stream) / static_cast<double>(readback->bytes_read));

    CY_CHECK(gpu.validation_errors() == 0);
}

CY_TEST_CASE("vt feedback: the density lever is a quality lever and it moves the sample count") {
    DeviceFixture gpu;
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }
    VirtualTextureFrame frame;
    FeedbackSettings settings;
    settings.grid_width = 256;
    settings.grid_height = 256;
    settings.density = 1;
    CY_REQUIRE(frame.create(allocator(), gpu.device(), terrain_desc(), settings).has_value());
    VirtualTextureSystem system(allocator());
    CY_REQUIRE(system.register_texture(terrain_desc()).has_value());
    CY_REQUIRE(frame.upload_page_table(*system.page_table(terrain_desc().id)).has_value());

    run_frame(gpu, frame);
    cy::Expected<VirtualTextureFrameReadback, cy::Error> dense = frame.read_back();
    CY_REQUIRE(dense.has_value());
    const u32 dense_samples = dense->total_samples;
    const u64 dense_bytes = dense->bytes_read;

    // One sample per sixteen pixels. `virtual-texturing`: density "SHALL be adjustable — one sample
    // per pixel block rather than per pixel — and SHALL be a quality lever".
    frame.set_density(16);
    CY_CHECK(frame.density() == 16);
    run_frame(gpu, frame);
    cy::Expected<VirtualTextureFrameReadback, cy::Error> sparse = frame.read_back();
    CY_REQUIRE(sparse.has_value());

    CY_CHECK(sparse->total_samples * 16 == dense_samples);
    // The lever costs coverage, not correctness: fewer samples ask for the same or fewer pages, and
    // a page nobody sampled is a page nobody needs this frame.
    CY_CHECK(sparse->requests.size() <= dense->requests.size());
    CY_CHECK(sparse->bytes_read <= dense_bytes);
    std::fprintf(stderr,
                 "vt density: 1/pixel gave %u samples over %zu pages; 1/16 gave %u over %zu\n",
                 dense_samples, static_cast<size_t>(dense->requests.size()), sparse->total_samples,
                 static_cast<size_t>(sparse->requests.size()));

    // The clamp is the requirement's own range, and it is enforced rather than trusted.
    frame.set_density(0);
    CY_CHECK(frame.density() == cy::render::vt::kMinFeedbackDensity);
    frame.set_density(100000);
    CY_CHECK(frame.density() == cy::render::vt::kMaxFeedbackDensity);

    CY_CHECK(gpu.validation_errors() == 0);
}

CY_TEST_CASE(
    "vt page table: a shader samples it, and the mip tail's guarantee holds on the device") {
    DeviceFixture gpu;
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }
    const VirtualTextureDesc desc = terrain_desc();

    VirtualTextureSystem system(allocator());
    TileCacheDesc cache;
    cache.format = desc.format_class();
    cache.tile_size = desc.tile_size;
    cache.border = desc.border;
    cache.bytes_per_tile = desc.bytes_per_tile;
    cache.tile_capacity = 64;
    CY_REQUIRE(system.configure_cache(cache).has_value());
    CY_REQUIRE(system.register_texture(desc).has_value());

    VirtualTextureFrame frame;
    FeedbackSettings settings;
    settings.grid_width = 128;
    settings.grid_height = 128;
    CY_REQUIRE(frame.create(allocator(), gpu.device(), desc, settings).has_value());

    // ================================================================================================
    // BEFORE THE TAIL. The case is worth nothing without this half: an assertion that nothing is
    // missing, over a table where nothing could be missing, is an assertion about an empty set.
    // ================================================================================================
    CY_REQUIRE(frame.upload_page_table(*system.page_table(desc.id)).has_value());
    run_frame(gpu, frame);
    cy::Expected<cy::Span<const GpuSampleResult>, cy::Error> before = frame.sample_results();
    CY_REQUIRE(before.has_value());
    CY_REQUIRE(before->size() == frame.entry_count());
    u32 missing_before = 0;
    for (const GpuSampleResult& result : *before) {
        missing_before += result.missing != 0 ? 1U : 0U;
    }
    CY_CHECK(missing_before == frame.entry_count());
    std::fprintf(stderr, "vt page table: %u of %u pages missing before the tail is resident\n",
                 missing_before, frame.entry_count());

    // ================================================================================================
    // AFTER THE TAIL. "A surface is never missing, only blurry" — held on the device, over the
    // WHOLE address space rather than at three sampled points.
    // ================================================================================================
    CY_REQUIRE(system.make_mip_tail_resident(desc.id).has_value());
    CY_REQUIRE(frame.upload_page_table(*system.page_table(desc.id)).has_value());
    run_frame(gpu, frame);
    cy::Expected<cy::Span<const GpuSampleResult>, cy::Error> after = frame.sample_results();
    CY_REQUIRE(after.has_value());

    // Every entry, compared against `VirtualTextureSystem::sample()` — which is the CPU
    // implementation of the same walk, and the one M6's suite already asserts the tail's guarantee
    // against. The two agreeing is what makes this a second implementation of a known answer.
    const cy::render::vt::PageTable& table = *system.page_table(desc.id);
    u32 checked = 0;
    u32 fallbacks = 0;
    for (u8 mip = 0; mip < desc.mip_count; ++mip) {
        for (u8 layer = 0; layer < desc.layers; ++layer) {
            for (u32 y = 0; y < desc.tiles_y(mip); ++y) {
                for (u32 x = 0; x < desc.tiles_x(mip); ++x) {
                    VirtualAddress address;
                    address.texture = desc.id;
                    address.mip = mip;
                    address.layer = layer;
                    address.tile_x = static_cast<cy::u16>(x);
                    address.tile_y = static_cast<cy::u16>(y);
                    const cy::usize index = table.linear_index(address);
                    CY_REQUIRE(index < after->size());
                    const GpuSampleResult& got = (*after)[index];
                    const cy::render::vt::SampleResult want = system.sample(address);

                    CY_CHECK(got.missing == 0);
                    CY_CHECK(want.missing == false);
                    CY_CHECK(got.resident_mip == want.resident_mip);
                    CY_CHECK(got.physical_tile == want.physical_tile);
                    CY_CHECK(got.deficit == want.deficit);
                    fallbacks += want.fallback ? 1U : 0U;
                    ++checked;
                }
            }
        }
    }
    CY_CHECK(checked == frame.entry_count());
    // Most of the pyramid resolves to a COARSER level than it asked for, which is the guarantee
    // doing its work rather than the table happening to be full.
    CY_CHECK(fallbacks > 0);
    std::fprintf(stderr,
                 "vt page table: %u pages sampled on the device, 0 missing, %u resolved to a "
                 "coarser resident level\n",
                 checked, fallbacks);

    CY_CHECK(gpu.validation_errors() == 0);
}

CY_TEST_CASE("vt frame: the frame is destroyed while its dispatches are still in flight") {
    // TEARDOWN UNDER LOAD, and not only steady state. M5.5's gate found this project's first engine
    // defect that way, and `VirtualTextureFrame::destroy()` frees eleven buffers, three pipelines
    // and a descriptor set while the GPU may still be reading every one of them.
    //
    // `rhi-and-render-graph` requires a resource destroyed during frame N to be released only after
    // frame N's fence has signalled. This case submits, does NOT wait, and destroys. Twenty times,
    // because a deferral that is wrong is wrong intermittently.
    DeviceFixture gpu;
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }
    VirtualTextureSystem system(allocator());
    CY_REQUIRE(system.register_texture(terrain_desc()).has_value());

    for (u32 round = 0; round < 20; ++round) {
        VirtualTextureFrame frame;
        FeedbackSettings settings;
        settings.grid_width = 128;
        settings.grid_height = 128;
        CY_REQUIRE(frame.create(allocator(), gpu.device(), terrain_desc(), settings).has_value());
        CY_REQUIRE(frame.upload_page_table(*system.page_table(terrain_desc().id)).has_value());

        CY_REQUIRE(gpu.device().begin_frame().has_value());
        {
            cy::rendering::RenderGraph graph(allocator());
            CY_REQUIRE(frame.declare(graph).has_value());
            cy::rendering::GraphExecutor executor(allocator(), gpu.device());
            cy::rendering::ExecuteOptions options;
            // Breadcrumbs off, for the defect test_gpu_cull_pass.cpp's teardown case names: the
            // executor resets its breadcrumb slot counter per execution, so overlapping frames fill
            // the same slots of the device's breadcrumb buffer with nothing between them. It is not
            // this module's buffer and not this module's defect.
            options.breadcrumbs = false;
            CY_REQUIRE(
                executor.execute(graph, cy::rendering::CompileOptions{}, options).has_value());
            // NO wait_idle BEFORE THIS.
            frame.destroy();
        }
        CY_REQUIRE(gpu.device().end_frame().has_value());
        // Drained before the next round, because `VirtualTextureFrame` owns ONE set of buffers and
        // two of its frames in flight would be two dispatches writing one feedback array.
        CY_REQUIRE(gpu.device().wait_idle().has_value());
    }
    CY_CHECK(gpu.validation_errors() == 0);
    std::fprintf(stderr,
                 "vt frame: 20 frames destroyed with their dispatches in flight, 0 validation "
                 "errors\n");
}
