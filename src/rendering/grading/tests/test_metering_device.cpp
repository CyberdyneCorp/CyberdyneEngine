// SPDX-License-Identifier: MIT
// The metering dispatches against their host twin, on a Vulkan device. `render.grading`.
//
// A synthetic image whose pixels sit at known histogram bin centres — a room, a darker band and a
// small bright window — is metered by the three dispatches alone. The device's 256 bins must equal
// the host's `luminance_histogram_bin` of the same half-precision pixels exactly, and the device's
// target and adapted EV100 must be `metered_ev100` and `adapt_ev100` of that histogram, over two
// frames so the state is seen to persist.
//
// And, without a device, the regression case for the duplicate definition this suite found: the
// post chain's exposure and the lighting module's, in one source and one binary.

#include "device_fixture.h"

#include <cy/backends/rhi/command_buffer.h>
#include <cy/rendering/grading/grading_renderer.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/lighting/units.h>
#include <cy/rendering/post/exposure.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace cy;
using cy::grading_test::allocator;
using cy::grading_test::DeviceFixture;
using cy::rendering::AutoExposureSettings;
using cy::rendering::grading::ExposureReadback;
using cy::rendering::grading::GradingRenderer;
using cy::rendering::grading::GradingRendererDescription;

namespace {

constexpr u32 kSyntheticWidth = 61;
constexpr u32 kSyntheticHeight = 37;

/// Upload the synthetic image once per frame and meter it: the graph imports the image, copies the
/// staged pixels in, and hands it to the three dispatches.
struct SyntheticImage {
    rhi::TextureHandle texture;
    rhi::BufferHandle staging;
};

void record_synthetic_upload(const rendering::PassContext& context, void* user) noexcept {
    const auto* image = static_cast<const SyntheticImage*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{kSyntheticWidth, kSyntheticHeight, 1};
    context.commands->copy_buffer_to_texture(image->staging, image->texture,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

[[nodiscard]] Status meter_synthetic(rhi::Device& device, GradingRenderer& grading,
                                     SyntheticImage& image) noexcept {
    const Expected<u32, Error> began = device.begin_frame();
    if (!began.has_value()) {
        return make_unexpected(began.error());
    }
    Status result = ok();
    {
        rendering::RenderGraph graph(allocator());
        rendering::TextureRequest request;
        request.name = "synthetic metering image";
        request.format = rhi::Format::Rgba16Sfloat;
        request.width = kSyntheticWidth;
        request.height = kSyntheticHeight;
        const rendering::ResourceId source =
            graph.import_texture(request, image.texture, rhi::ImageUse::Undefined);
        graph.add_pass("synthetic upload", rhi::QueueKind::Graphics)
            .write(source, rhi::Access::TransferWrite)
            .record(&record_synthetic_upload, &image);
        result = grading.declare_metering_of(graph, source, kSyntheticWidth, kSyntheticHeight);
        if (result) {
            rendering::GraphExecutor executor(allocator(), device);
            if (auto executed = executor.execute(graph, rendering::CompileOptions{},
                                                 rendering::ExecuteOptions{});
                !executed) {
                result = make_unexpected(executed.error());
            } else {
                result = device.wait_idle();
            }
            executor.release();
        }
    }
    if (Status ended = device.end_frame(); !ended && result) {
        result = ended;
    }
    return result;
}

/// binary16 to binary32, for the host twin of what the device reads.
[[nodiscard]] f32 from_half(u16 half) noexcept {
    const u32 sign = static_cast<u32>(half & 0x8000U) << 16U;
    const u32 exponent = (half >> 10U) & 0x1FU;
    const u32 mantissa = half & 0x3FFU;
    u32 bits = sign;
    if (exponent != 0U) {
        bits |= ((exponent + 112U) << 23U) | (mantissa << 13U);
    }
    f32 value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

/// binary32 to binary16 for the normal range the synthetic luminances live in.
[[nodiscard]] u16 to_half(f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const u32 exponent = ((bits >> 23U) & 0xFFU) - 112U;
    u32 half = (exponent << 10U) | ((bits >> 13U) & 0x3FFU);
    if ((bits & 0x1FFFU) > 0x1000U || ((bits & 0x1FFFU) == 0x1000U && (half & 1U) != 0U)) {
        ++half;
    }
    return static_cast<u16>(half);
}

}  // namespace

CY_TEST_CASE(
    "the device histogram is the host's bin for bin, and the metered exposure is the "
    "host's") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    rhi::Device& device = fixture.device();
    GradingRenderer grading;
    GradingRendererDescription description;
    description.readback = true;
    CY_REQUIRE(grading.initialize(device, allocator(), description).has_value());

    // THE ROOM AND THE WINDOW: most pixels at a mid EV, a darker band, and a small bright window,
    // each exactly at a bin centre so a last-bit difference between the device's log2 and the
    // host's cannot move a pixel across a boundary. A compensation curve that is not flat, so the
    // curve is exercised too.
    AutoExposureSettings settings;
    settings.compensation.compensation[1] = 0.5F;
    settings.compensation.compensation[2] = -0.25F;
    const f32 width = (settings.max_ev - settings.min_ev) / 256.0F;
    const auto centre = [&](u32 bin) noexcept {
        return rendering::luminance_for_ev100(settings.min_ev +
                                              (width * (static_cast<f32>(bin) + 0.5F)));
    };
    std::vector<u16> halves(static_cast<usize>(kSyntheticWidth) * kSyntheticHeight * 4U);
    std::vector<u32> expected(256, 0);
    for (u32 y = 0; y < kSyntheticHeight; ++y) {
        for (u32 x = 0; x < kSyntheticWidth; ++x) {
            u32 bin = 150U + (((x * 7U) + (y * 3U)) % 11U);
            if (y < 9U) {
                bin = 70U + (x % 5U);
            }
            if (x > 52U && y > 30U) {
                bin = 236U;
            }
            const u16 grey = to_half(centre(bin));
            const usize texel = (static_cast<usize>(y) * kSyntheticWidth) + x;
            halves[(texel * 4U) + 0U] = grey;
            halves[(texel * 4U) + 1U] = grey;
            halves[(texel * 4U) + 2U] = grey;
            halves[(texel * 4U) + 3U] = to_half(1.0F);
            const f32 read = from_half(grey);
            ++expected[rendering::luminance_histogram_bin(
                rendering::metering_luminance(read, read, read), 256, settings.min_ev,
                settings.max_ev)];
        }
    }

    rhi::TextureDescription texture;
    texture.name = "synthetic metering image";
    texture.format = rhi::Format::Rgba16Sfloat;
    texture.extent = rhi::Extent3D{kSyntheticWidth, kSyntheticHeight, 1};
    texture.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination;
    Expected<rhi::TextureHandle, Error> image = device.create_texture(texture);
    CY_REQUIRE(image.has_value());
    rhi::BufferDescription staging;
    staging.name = "synthetic metering staging";
    staging.size = halves.size() * sizeof(u16);
    staging.usage = rhi::BufferUsage::TransferSource;
    staging.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> buffer = device.create_buffer(staging);
    CY_REQUIRE(buffer.has_value());
    std::memcpy(device.buffer_mapped_pointer(*buffer), halves.data(), staging.size);
    SyntheticImage synthetic{*image, *buffer};

    constexpr f32 kStart = 5.0F;
    constexpr f32 kDelta = 0.1F;
    grading.set_automatic(settings, kStart);
    grading.set_delta_seconds(kDelta);
    CY_REQUIRE(meter_synthetic(device, grading, synthetic).has_value());
    ExposureReadback first;
    CY_REQUIRE(grading.read_exposure(first).has_value());
    CY_REQUIRE(meter_synthetic(device, grading, synthetic).has_value());
    ExposureReadback second;
    CY_REQUIRE(grading.read_exposure(second).has_value());
    (void)device.wait_idle();
    device.destroy_buffer(*buffer);
    device.destroy_texture(*image);

    u32 mismatched = 0;
    u64 total = 0;
    for (u32 bin = 0; bin < 256U; ++bin) {
        mismatched += first.histogram[bin] != expected[bin] ? 1U : 0U;
        total += first.histogram[bin];
    }
    const rendering::LuminanceHistogram host{expected.data(), 256, settings.min_ev,
                                             settings.max_ev};
    const f32 target = rendering::metered_ev100(host, settings);
    const f32 adapted = rendering::adapt_ev100(kStart, target, kDelta, settings);
    const f32 again = rendering::adapt_ev100(adapted, target, kDelta, settings);
    std::fprintf(stderr,
                 "histogram: %u of 256 bins differ from the host's, %llu pixels counted of %u\n"
                 "target: device %.6f host %.6f; adapted: device %.6f host %.6f; again: device "
                 "%.6f host %.6f; frames %.0f then %.0f\n",
                 mismatched, static_cast<unsigned long long>(total),
                 kSyntheticWidth * kSyntheticHeight, static_cast<double>(first.target_ev100),
                 static_cast<double>(target), static_cast<double>(first.current_ev100),
                 static_cast<double>(adapted), static_cast<double>(second.current_ev100),
                 static_cast<double>(again), static_cast<double>(first.frames),
                 static_cast<double>(second.frames));
    CY_CHECK_EQ(mismatched, 0U);
    CY_CHECK_EQ(total, static_cast<u64>(kSyntheticWidth) * kSyntheticHeight);
    // f32 on the device against f64 accumulation on the host, over 2 257 pixels. MEASURED: see the
    // line above.
    CY_CHECK_LE(std::fabs(first.target_ev100 - target), 1e-3F);
    CY_CHECK_LE(std::fabs(first.current_ev100 - adapted), 1e-3F);
    CY_CHECK_LE(std::fabs(second.current_ev100 - again), 1e-3F);
    CY_CHECK_EQ(first.frames, 1.0F);
    CY_CHECK_EQ(second.frames, 2.0F);
    // THE CONTROL: the window is in the histogram and the percentile metering ignored it — the
    // target is the room's, several stops below the window's bin.
    CY_CHECK_GT(first.histogram[236], 0U);
    CY_CHECK_LT(target, settings.min_ev + (width * 180.0F));
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
CY_TEST_CASE("the post chain's exposure and the lighting module's link together and agree") {
    // A REGRESSION CASE, and the binary it is in is half of it. `post/exposure.h` and
    // `lighting/units.h` both declared `cy::rendering::CameraExposure` and both defined
    // `exposure_multiplier(float)`, so this source did not compile and this suite — which links the
    // post module's exposure beside the pipeline scene's lighting — did not link. The post names
    // are `CameraControls` and `multiplier_for_ev100` now; the two must still be the same number.
    for (const f32 ev : {-3.0F, 0.0F, 9.5F, 14.6F}) {
        const f32 post = rendering::multiplier_for_ev100(ev);
        const f32 lighting = rendering::exposure_multiplier(ev);
        CY_CHECK_LE(std::fabs(post - lighting), lighting * 1e-6F);
    }
    rendering::CameraControls controls;
    controls.aperture = 16.0F;
    rendering::CameraExposure camera;
    camera.aperture = 16.0F;
    camera.shutter_seconds = controls.shutter_seconds;
    camera.sensitivity = controls.iso;
    CY_CHECK_LE(
        std::fabs(rendering::ev100_from_camera(controls) - rendering::exposure_value(camera)),
        1e-5F);
}
