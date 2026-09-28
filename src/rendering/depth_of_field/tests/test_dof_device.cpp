// SPDX-License-Identifier: MIT
// Depth of field on a device: what the five dispatches DO to an image, measured on the linear HDR
// they write.
//
// `rendering-post-processing`'s "Depth of field" and its first scenario, and the properties a
// defocus has to have before it is allowed near a frame:
//
//   - the in-focus plane is unchanged: a plane at the focus distance, and one a half-pixel
//     circle away from it, comes out bit for bit;
//   - a far edge blurs by the lens's radius: the step's profile is a disc's of the radius
//     `circle_of_confusion` gives, fitted to a pixel;
//   - near-field bleeding: a defocused foreground's blur extends over the focused plane beside it
//     by its own radius, covering it by the share of the discs that reach each pixel;
//   - the far field stays behind: a focused object in front of a blurred background is not
//     touched by it, and gives it none of its light;
//   - the aperture's shape: six blades draw a hexagon, not a disc;
//   - a pinhole is no stage: f/infinity through the assembled frame is the frame without the
//     stage, byte for byte;
//   - off is the frame before: the frame without the stage against a reference drawn with the
//     shaders from before this module.
//
// THE PASSES ARE THE FRAME'S. The bench declares them with `DepthOfFieldPass::declare` — what
// `ForwardFrame` calls through the stage seam — over colour and depth it uploads, so what is
// measured is what a frame runs. The colour is uploaded as half floats and read back as half
// floats; nothing in between is converted.
//
// Vulkan with validation and synchronisation validation on. Without a GPU the suite SKIPS LOUDLY.

#include "frame_scene.h"
#include "golden.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/depth_of_field/dof_pass.h>
#include <cy/rendering/graph/executor.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

using namespace cy;
using namespace cy::pipeline_test;
using cy::rendering::depth_of_field::DepthOfFieldPass;
using cy::rendering::depth_of_field::DepthOfFieldPassDescription;
using cy::rendering::depth_of_field::DofConstants;
using cy::rendering::depth_of_field::DofSettings;
using cy::rendering::depth_of_field::DofView;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

inline constexpr u32 kSize = 256;
inline constexpr u32 kTexels = kSize * kSize;
/// Where every case's edge is: the first column of the right-hand side.
inline constexpr u32 kEdge = 128;

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
    }
    std::fprintf(stderr, "graphics validation %s: %s\n",
                 severity == rhi::ValidationSeverity::Error ? "error" : "warning",
                 message != nullptr ? message : "");
}

class DeviceFixture {
public:
    DeviceFixture() noexcept : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_test_render_depth_of_field";
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
                     "no requested graphics device on this machine; the backend selected was '%s' "
                     "because %s\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)",
                     selection_.reason != nullptr ? selection_.reason : "(no reason given)");
    }

private:
    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    u32 errors_ = 0;
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

// --- Half floats, both ways, for the upload and the read-back --------------------------------

[[nodiscard]] u16 to_half(f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const u32 sign = (bits >> 16U) & 0x8000U;
    const i32 exponent = static_cast<i32>((bits >> 23U) & 0xFFU) - 127 + 15;
    const u32 mantissa = bits & 0x007FFFFFU;
    if (exponent <= 0) {
        return static_cast<u16>(sign);
    }
    if (exponent >= 0x1F) {
        return static_cast<u16>(sign | 0x7C00U);
    }
    // Every value this suite uploads is exact in a half, so truncation is rounding here.
    return static_cast<u16>(sign | (static_cast<u32>(exponent) << 10U) | (mantissa >> 13U));
}

[[nodiscard]] f32 from_half(u16 half) noexcept {
    const u32 sign = (static_cast<u32>(half) & 0x8000U) << 16U;
    const u32 exponent = (static_cast<u32>(half) >> 10U) & 0x1FU;
    const u32 mantissa = static_cast<u32>(half) & 0x3FFU;
    u32 bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            const f32 value = std::ldexp(static_cast<f32>(mantissa), -24);
            return sign != 0 ? -value : value;
        }
    } else if (exponent == 0x1F) {
        bits = sign | 0x7F800000U | (mantissa << 13U);
    } else {
        bits = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
    }
    f32 value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// --- The lens and the scene -----------------------------------------------------------------

/// A 96 mm lens at f/1.4 focused at 2 m, on a full-frame sensor, drawing a 256-pixel image: the
/// projection's field of view is the lens's, so the circle is the one this camera would see.
inline constexpr f32 kFov = 0.25F;
inline constexpr f32 kFocus = 2.0F;
inline constexpr f32 kFar = 8.0F;
inline constexpr f32 kNear = 1.2F;

[[nodiscard]] Mat4 bench_projection() noexcept {
    return perspective_reversed_z_infinite(kFov, 1.0F, 0.1F);
}

[[nodiscard]] DofSettings bench_settings() noexcept {
    DofSettings settings;
    settings.lens.sensor_height_mm = 24.0F;
    settings.lens.focal_length_mm =
        rendering::depth_of_field::focal_length_for_field_of_view(kFov, 24.0F);
    settings.lens.aperture = 1.4F;
    settings.lens.focus_distance = kFocus;
    settings.max_radius_fraction = 0.1F;
    return settings;
}

/// The depth-buffer value the bench's projection writes for a surface `distance` away.
[[nodiscard]] f32 depth_at(f32 distance) noexcept {
    const Mat4 projection = bench_projection();
    const f32 z = -distance;
    return ((projection.at(2, 2) * z) + projection.at(2, 3)) /
           ((projection.at(3, 2) * z) + projection.at(3, 3));
}

/// One grey value and one distance a texel.
struct Scene {
    std::vector<f32> grey = std::vector<f32>(kTexels, 0.0F);
    std::vector<f32> depth = std::vector<f32>(kTexels, 0.0F);

    void fill(u32 x0, u32 x1, f32 value, f32 distance) {
        for (u32 y = 0; y < kSize; ++y) {
            for (u32 x = x0; x < x1; ++x) {
                grey[(y * kSize) + x] = value;
                depth[(y * kSize) + x] = depth_at(distance);
            }
        }
    }
};

/// What the stage wrote: the grey (red channel) and the raw half-float bits of all four channels.
struct Result {
    std::vector<f32> grey = std::vector<f32>(kTexels, 0.0F);
    std::vector<u16> bits = std::vector<u16>(static_cast<usize>(kTexels) * 4U, 0);
    std::vector<u16> input = std::vector<u16>(static_cast<usize>(kTexels) * 4U, 0);

    [[nodiscard]] f32 at(u32 x, u32 y) const noexcept { return grey[(y * kSize) + x]; }
    /// Whether texel (x, y) came out with every bit it went in with.
    [[nodiscard]] bool unchanged(u32 x, u32 y) const noexcept {
        const usize base = static_cast<usize>((y * kSize) + x) * 4U;
        return std::memcmp(&bits[base], &input[base], 4U * sizeof(u16)) == 0;
    }
    /// The mean over the middle half of the rows, where no row sees the top or bottom border.
    [[nodiscard]] f32 column(u32 x) const noexcept {
        f64 total = 0.0;
        for (u32 y = kSize / 4U; y < (3U * kSize) / 4U; ++y) {
            total += static_cast<f64>(at(x, y));
        }
        return static_cast<f32>(total / static_cast<f64>(kSize / 2U));
    }
};

/// The stage over one scene, on the device, and the target read back.
class Bench {
public:
    explicit Bench(rhi::Device& device) noexcept : device_(&device) {}
    ~Bench() {
        (void)device_->wait_idle();
        pass_.destroy();
        for (rhi::BufferHandle* buffer : {&colour_upload_, &depth_upload_, &readback_}) {
            if (!buffer->is_null()) {
                device_->destroy_buffer(*buffer);
            }
        }
    }

    Bench(const Bench&) = delete;
    Bench& operator=(const Bench&) = delete;

    [[nodiscard]] Status build() noexcept {
        DepthOfFieldPassDescription description;
        description.width = kSize;
        description.height = kSize;
        if (Status made = pass_.create(*device_, description); !made) {
            return made;
        }
        const struct {
            rhi::BufferHandle* handle;
            u64 bytes;
            rhi::BufferUsage usage;
            rhi::MemoryUse memory;
        } buffers[] = {
            {&colour_upload_, static_cast<u64>(kTexels) * 8U, rhi::BufferUsage::TransferSource,
             rhi::MemoryUse::Upload},
            {&depth_upload_, static_cast<u64>(kTexels) * 4U, rhi::BufferUsage::TransferSource,
             rhi::MemoryUse::Upload},
            {&readback_, static_cast<u64>(kTexels) * 8U, rhi::BufferUsage::TransferDestination,
             rhi::MemoryUse::Readback},
        };
        for (const auto& entry : buffers) {
            rhi::BufferDescription buffer;
            buffer.name = "depth of field test buffer";
            buffer.size = entry.bytes;
            buffer.usage = entry.usage;
            buffer.memory = entry.memory;
            Expected<rhi::BufferHandle, Error> made = device_->create_buffer(buffer);
            if (!made.has_value()) {
                return make_unexpected(made.error());
            }
            *entry.handle = *made;
        }
        return ok();
    }

    [[nodiscard]] const DofConstants& constants() const noexcept { return pass_.constants(); }

    [[nodiscard]] Status run(const Scene& scene, const DofSettings& settings,
                             Result& out) noexcept {
        pass_.set_settings(settings);
        if (Status viewed = pass_.set_view(DofView{bench_projection(), kSize, kSize}); !viewed) {
            return viewed;
        }
        stage(scene, out);
        Expected<u32, Error> began = device_->begin_frame();
        if (!began.has_value()) {
            return make_unexpected(began.error());
        }
        RenderGraph graph(allocator());
        TextureRequest request;
        request.width = kSize;
        request.height = kSize;
        request.name = "depth of field test colour";
        request.format = rhi::Format::Rgba16Sfloat;
        colour_ = graph.create_texture(request);
        request.name = "depth of field test depth";
        request.format = rhi::Format::R32Sfloat;
        depth_ = graph.create_texture(request);
        request.name = "depth of field test target";
        request.format = rhi::Format::Rgba16Sfloat;
        request.extra_usage = rhi::TextureUsage::Storage | rhi::TextureUsage::TransferSource;
        target_ = graph.create_texture(request);
        graph.add_pass("depth of field test upload", rhi::QueueKind::Graphics)
            .write(colour_, rhi::Access::TransferWrite)
            .write(depth_, rhi::Access::TransferWrite)
            .record(&record_upload, this);

        ScreenSpaceStageInputs inputs;
        inputs.source = colour_;
        inputs.depth = depth_;
        inputs.target = target_;
        inputs.width = kSize;
        inputs.height = kSize;
        pass_.reset_report();
        if (pass_.declare(graph, inputs) == kInvalidPass) {
            (void)device_->end_frame();
            return fail(ErrorCode::InvalidArgument, "depth of field test: the stage refused");
        }

        BufferRequest readback;
        readback.name = "depth of field test readback";
        readback.size = static_cast<u64>(kTexels) * 8U;
        readback_resource_ = graph.import_buffer(readback, readback_);
        graph.add_pass("depth of field test copy", rhi::QueueKind::Graphics)
            .read(target_, rhi::Access::TransferRead)
            .write(readback_resource_, rhi::Access::TransferWrite)
            .record(&record_readback, this);
        graph.add_pass("depth of field test host", rhi::QueueKind::Graphics)
            .read(readback_resource_, rhi::Access::HostRead)
            .side_effect();

        if (Status executed = execute(graph); !executed) {
            return executed;
        }
        const auto* read = static_cast<const u16*>(device_->buffer_mapped_pointer(readback_));
        if (read == nullptr) {
            return fail(ErrorCode::Internal, "depth of field test: the readback is not mapped");
        }
        std::memcpy(out.bits.data(), read, out.bits.size() * sizeof(u16));
        for (usize texel = 0; texel < kTexels; ++texel) {
            out.grey[texel] = from_half(read[texel * 4U]);
        }
        return ok();
    }

    [[nodiscard]] u32 dispatches() const noexcept { return pass_.report().dispatches; }

private:
    /// Write the scene into the staging buffers, and the colour's bits into `out.input`.
    void stage(const Scene& scene, Result& out) noexcept {
        auto* colour = static_cast<u16*>(device_->buffer_mapped_pointer(colour_upload_));
        auto* depth = static_cast<f32*>(device_->buffer_mapped_pointer(depth_upload_));
        for (u32 texel = 0; texel < kTexels; ++texel) {
            const u16 half = to_half(scene.grey[texel]);
            for (u32 channel = 0; channel < 3; ++channel) {
                out.input[(texel * 4U) + channel] = half;
            }
            out.input[(texel * 4U) + 3U] = to_half(1.0F);
            depth[texel] = scene.depth[texel];
        }
        std::memcpy(colour, out.input.data(), out.input.size() * sizeof(u16));
    }

    [[nodiscard]] Status execute(RenderGraph& graph) noexcept {
        Status frame = graph.status();
        if (frame) {
            GraphExecutor executor(allocator(), *device_);
            const Expected<ExecutionResult, Error> result =
                executor.execute(graph, CompileOptions{}, ExecuteOptions{});
            frame = result.has_value() ? device_->wait_idle() : make_unexpected(result.error());
            executor.release();
        }
        if (Status ended = device_->end_frame(); !ended && frame) {
            frame = ended;
        }
        return frame;
    }

    static void record_upload(const PassContext& context, void* user) noexcept {
        const auto* bench = static_cast<const Bench*>(user);
        rhi::BufferTextureCopy region;
        region.texture_extent = rhi::Extent3D{kSize, kSize, 1};
        context.commands->copy_buffer_to_texture(bench->colour_upload_,
                                                 context.executor->texture(bench->colour_),
                                                 Span<const rhi::BufferTextureCopy>(&region, 1));
        context.commands->copy_buffer_to_texture(bench->depth_upload_,
                                                 context.executor->texture(bench->depth_),
                                                 Span<const rhi::BufferTextureCopy>(&region, 1));
    }

    static void record_readback(const PassContext& context, void* user) noexcept {
        const auto* bench = static_cast<const Bench*>(user);
        rhi::BufferTextureCopy region;
        region.texture_extent = rhi::Extent3D{kSize, kSize, 1};
        context.commands->copy_texture_to_buffer(context.executor->texture(bench->target_),
                                                 bench->readback_,
                                                 Span<const rhi::BufferTextureCopy>(&region, 1));
    }

    rhi::Device* device_ = nullptr;
    DepthOfFieldPass pass_;
    rhi::BufferHandle colour_upload_;
    rhi::BufferHandle depth_upload_;
    rhi::BufferHandle readback_;
    ResourceId colour_ = kInvalidResource;
    ResourceId depth_ = kInvalidResource;
    ResourceId target_ = kInvalidResource;
    ResourceId readback_resource_ = kInvalidResource;
};

/// The share of a disc of radius `radius`, centred `offset` pixels to the right of an edge, that
/// lies to the right of it: the profile a disc blur gives a step.
[[nodiscard]] f32 disc_share(f32 offset, f32 radius) noexcept {
    const f32 s = math::clamp(offset / radius, -1.0F, 1.0F);
    return 0.5F + ((s * std::sqrt(1.0F - (s * s))) + std::asin(s)) / math::kPi;
}

/// The radius whose disc profile best fits the measured step from `low` to `high` about kEdge.
[[nodiscard]] f32 fit_disc_radius(const Result& result, f32 low, f32 high, u32 reach) noexcept {
    f32 best = 0.0F;
    f64 best_error = std::numeric_limits<f64>::max();
    for (f32 radius = 1.0F; radius <= 40.0F; radius += 0.05F) {
        f64 error = 0.0;
        for (u32 x = kEdge - reach; x < kEdge + reach; ++x) {
            // Texel x's centre is x + 1/2; the edge is between texels kEdge - 1 and kEdge.
            const f32 offset = (static_cast<f32>(x) + 0.5F) - static_cast<f32>(kEdge);
            const f32 model = low + ((high - low) * disc_share(offset, radius));
            const f64 delta = static_cast<f64>(result.column(x) - model);
            error += delta * delta;
        }
        if (error < best_error) {
            best_error = error;
            best = radius;
        }
    }
    return best;
}

void write_strip(const char* name, const Result& result) noexcept {
    render_test::Image image(allocator());
    std::vector<u32> texels(kTexels);
    for (usize texel = 0; texel < kTexels; ++texel) {
        const f32 value = math::clamp(result.grey[texel], 0.0F, 1.0F);
        const auto byte = static_cast<u32>(std::lround(value * 255.0F));
        texels[texel] = byte | (byte << 8U) | (byte << 16U) | 0xFF000000U;
    }
    if (!render_test::adopt(image, Span<const u32>(texels.data(), texels.size()), kSize, kSize)
             .has_value()) {
        return;
    }
    const char* directory = std::getenv("CY_TEST_ARTEFACT_DIR");
    if (directory == nullptr || *directory == '\0') {
        directory = CY_TEST_BINARY_DIR;
    }
    char path[1024];
    (void)std::snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (render_test::write_png(path, image).has_value()) {
        std::fprintf(stderr, "wrote %s\n", path);
    }
}

}  // namespace

CY_TEST_CASE("the in-focus plane comes out of the stage bit for bit") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Bench bench(fixture.device());
    CY_REQUIRE(bench.build().has_value());

    // A textured plane at the focus distance: every texel different from its neighbours, so an
    // unwanted blur of any radius shows.
    Scene scene;
    scene.fill(0, kSize, 0.0F, kFocus);
    for (u32 texel = 0; texel < kTexels; ++texel) {
        scene.grey[texel] = static_cast<f32>((texel * 37U) % 64U) / 32.0F;
    }
    Result result;
    CY_REQUIRE(bench.run(scene, bench_settings(), result).has_value());
    CY_CHECK_EQ(bench.dispatches(), 5U);
    u32 unchanged = 0;
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            unchanged += result.unchanged(x, y) ? 1U : 0U;
        }
    }
    CY_CHECK_EQ(unchanged, kTexels);

    // Nor does a plane just off it: at 2.05 m the circle's radius is under half a pixel, inside
    // the one-pixel focus band, and the plane is in focus as a sensor of this resolution sees it.
    const f32 radius = rendering::depth_of_field::coc_radius_pixels(bench.constants(), 2.05F);
    std::fprintf(stderr, "a plane at 2.05 m: circle of radius %.3f px\n", static_cast<f64>(radius));
    CY_REQUIRE(std::fabs(radius) < 1.0F);
    scene.fill(0, kSize, 0.0F, 2.05F);
    for (u32 texel = 0; texel < kTexels; ++texel) {
        scene.grey[texel] = static_cast<f32>((texel * 37U) % 64U) / 32.0F;
    }
    CY_REQUIRE(bench.run(scene, bench_settings(), result).has_value());
    unchanged = 0;
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            unchanged += result.unchanged(x, y) ? 1U : 0U;
        }
    }
    CY_CHECK_EQ(unchanged, kTexels);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("a far edge blurs by the radius the lens gives it, in pixels") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Bench bench(fixture.device());
    CY_REQUIRE(bench.build().has_value());

    // A black-to-white step on a plane 8 m away, behind the 2 m focus.
    Scene scene;
    scene.fill(0, kEdge, 0.0F, kFar);
    scene.fill(kEdge, kSize, 1.0F, kFar);
    Result result;
    CY_REQUIRE(bench.run(scene, bench_settings(), result).has_value());
    write_strip("depth-of-field-far-edge.png", result);

    // The radius `circle_of_confusion` gives this lens at 8 m, as a radius in pixels of a
    // 256-pixel image.
    const f32 expected =
        0.5F * circle_of_confusion(bench_settings().lens, kFar) * static_cast<f32>(kSize);
    CY_REQUIRE(expected > 8.0F);
    CY_REQUIRE(expected < bench.constants().lens[2]);
    const f32 fitted = fit_disc_radius(result, 0.0F, 1.0F, 40);
    std::fprintf(stderr, "far edge: lens radius %.2f px, fitted disc radius %.2f px\n",
                 static_cast<f64>(expected), static_cast<f64>(fitted));
    // A pixel, or a tenth: the half-resolution layer and its upsample add about one pixel of
    // their own, and a blur the lens did not ask for is off by more.
    CY_CHECK_LT(std::fabs(fitted - expected), math::max(1.5F, 0.1F * expected));
    // Far from the edge, the blur of a uniform region is the region.
    CY_CHECK_LT(std::fabs(result.column(kEdge - 40) - 0.0F), 1.0e-3F);
    CY_CHECK_LT(std::fabs(result.column(kEdge + 40) - 1.0F), 1.0e-3F);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the near field's blur extends over the focused plane beside it") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Bench bench(fixture.device());
    CY_REQUIRE(bench.build().has_value());

    // A bright foreground at 1.2 m over the left half; a dim plane in focus at 2 m on the right.
    constexpr f32 kBright = 1.0F;
    constexpr f32 kDim = 0.25F;
    Scene scene;
    scene.fill(0, kEdge, kBright, kNear);
    scene.fill(kEdge, kSize, kDim, kFocus);
    Result result;
    CY_REQUIRE(bench.run(scene, bench_settings(), result).has_value());
    write_strip("depth-of-field-near-edge.png", result);

    const f32 radius =
        -0.5F * circle_of_confusion(bench_settings().lens, kNear) * static_cast<f32>(kSize);
    std::fprintf(stderr, "near edge: the foreground's circle is %.2f px\n",
                 static_cast<f64>(radius));
    CY_REQUIRE(radius > 8.0F);

    // Every focused pixel within the foreground's radius is covered by the share of the
    // foreground's discs that reach it: the disc of that radius about the pixel, on the
    // foreground's side of the edge.
    for (const f32 fraction : {0.1F, 0.3F, 0.5F, 0.7F}) {
        const auto x = kEdge + static_cast<u32>(fraction * radius);
        const f32 offset = (static_cast<f32>(x) + 0.5F) - static_cast<f32>(kEdge);
        const f32 expected = 1.0F - disc_share(offset, radius);
        const f32 coverage = (result.column(x) - kDim) / (kBright - kDim);
        std::fprintf(stderr, "  %.1f r into the focused plane: coverage %.3f, discs %.3f\n",
                     static_cast<f64>(fraction), static_cast<f64>(coverage),
                     static_cast<f64>(expected));
        CY_CHECK_GT(coverage, 0.0F);
        CY_CHECK_LT(std::fabs(coverage - expected), 0.12F);
    }
    // NOT CLIPPED TO THE SILHOUETTE: the first focused column is covered by nearly half.
    CY_CHECK_GT((result.column(kEdge) - kDim) / (kBright - kDim), 0.3F);
    // And beyond the radius, and the tiles' reach, the focused plane is untouched bit for bit.
    const auto clear = kEdge + static_cast<u32>(std::ceil(radius)) + 3U;
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = clear; x < kSize; ++x) {
            if (!result.unchanged(x, y)) {
                CY_TEST_FAIL_CHECK("a focused pixel beyond the near field's reach changed");
                y = kSize;
                break;
            }
        }
    }
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the far field does not bleed onto a nearer focused object") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Bench bench(fixture.device());
    CY_REQUIRE(bench.build().has_value());

    // A bright object in focus on the left; a dim, striped background 8 m away on the right.
    constexpr f32 kObject = 1.0F;
    Scene scene;
    scene.fill(0, kEdge, kObject, kFocus);
    scene.fill(kEdge, kSize, 0.0F, kFar);
    f32 background_high = 0.0F;
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = kEdge; x < kSize; ++x) {
            const f32 value = ((x / 4U) % 2U) == 0U ? 0.0625F : 0.1875F;
            scene.grey[(y * kSize) + x] = value;
            background_high = math::max(background_high, value);
        }
    }
    Result result;
    CY_REQUIRE(bench.run(scene, bench_settings(), result).has_value());
    write_strip("depth-of-field-far-behind.png", result);

    // The focused object is untouched to its silhouette, bit for bit.
    u32 changed = 0;
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kEdge; ++x) {
            changed += result.unchanged(x, y) ? 0U : 1U;
        }
    }
    CY_CHECK_EQ(changed, 0U);
    // The background beside it takes none of its light: nothing there is brighter than the
    // brightest stripe.
    f32 brightest = 0.0F;
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = kEdge; x < kSize; ++x) {
            brightest = math::max(brightest, result.at(x, y));
        }
    }
    std::fprintf(stderr, "far behind: brightest background %.4f, brightest stripe %.4f\n",
                 static_cast<f64>(brightest), static_cast<f64>(background_high));
    CY_CHECK_LE(brightest, background_high + 1.0e-3F);
    // And the background IS blurred: its eight-pixel stripes, under a disc of more than eight
    // pixels' radius, keep a small part of their contrast.
    f32 low = 1.0F;
    f32 high = 0.0F;
    for (u32 x = kEdge + 32U; x < kEdge + 64U; ++x) {
        low = math::min(low, result.column(x));
        high = math::max(high, result.column(x));
    }
    std::fprintf(stderr, "far behind: stripe contrast %.4f of 0.125\n",
                 static_cast<f64>(high - low));
    CY_CHECK_LT(high - low, 0.25F * 0.125F);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("six aperture blades draw a far point as a hexagon") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Bench bench(fixture.device());
    CY_REQUIRE(bench.build().has_value());

    // One bright half-resolution texel on a black plane 8 m away.
    Scene scene;
    scene.fill(0, kSize, 0.0F, kFar);
    for (const u32 texel : {(128U * kSize) + 128U, (128U * kSize) + 129U, (129U * kSize) + 128U,
                            (129U * kSize) + 129U}) {
        scene.grey[texel] = 64.0F;
    }
    /// How far from the point the light reaches along x and along y: the farthest lit texel
    /// centre, over the whole footprint, so a texel the sparse taps missed does not shorten it.
    const auto extents = [](const Result& result) {
        f32 across = 0.0F;
        f32 down = 0.0F;
        for (u32 y = 0; y < kSize; ++y) {
            for (u32 x = 0; x < kSize; ++x) {
                if (result.at(x, y) > 0.02F) {
                    across = math::max(across, std::fabs((static_cast<f32>(x) + 0.5F) - 129.0F));
                    down = math::max(down, std::fabs((static_cast<f32>(y) + 0.5F) - 129.0F));
                }
            }
        }
        return Vec2{across, down};
    };
    // The shape is the aperture's, not the sampling's: enough rings for a tap a texel.
    DofSettings circular = bench_settings();
    circular.max_rings = rendering::depth_of_field::kMaxRings;
    Result round;
    CY_REQUIRE(bench.run(scene, circular, round).has_value());
    DofSettings bladed = circular;
    bladed.blades = 6;
    Result hexagon;
    CY_REQUIRE(bench.run(scene, bladed, hexagon).has_value());
    write_strip("depth-of-field-hexagon.png", hexagon);

    // Blades rotated 0: corners along x, edges' midpoints along y, cos(30 degrees) = 0.866 of the
    // way out. The circle reaches as far both ways.
    const Vec2 round_extent = extents(round);
    const Vec2 hexagon_extent = extents(hexagon);
    const f32 round_ratio = round_extent.y / round_extent.x;
    const f32 hexagon_ratio = hexagon_extent.y / hexagon_extent.x;
    std::fprintf(stderr, "aperture: edge over corner %.3f for the circle, %.3f for six blades\n",
                 static_cast<f64>(round_ratio), static_cast<f64>(hexagon_ratio));
    CY_CHECK_GT(round_ratio, 0.95F);
    CY_CHECK_LT(hexagon_ratio, 0.93F);
    CY_CHECK_GT(hexagon_ratio, 0.78F);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

// --- The frame --------------------------------------------------------------------------------

namespace {

/// What a frame case configures, and the hooks that do it.
struct FrameCase {
    FrameScene* scene = nullptr;
    DepthOfFieldPass* pass = nullptr;
    DofSettings settings{};
};

void configure(rendering::assembly::AssemblyDescription& description, void* /*user*/) noexcept {
    description.post.depth_of_field = true;
}

Status before_assemble(RenderGraph& /*graph*/, rendering::assembly::AssemblyView& /*view*/,
                       FrameSinks& sinks, void* user) noexcept {
    auto* frame = static_cast<FrameCase*>(user);
    frame->pass->set_settings(frame->settings);
    if (Status viewed = frame->pass->set_view(DofView{frame->scene->projection(), kWidth, kHeight});
        !viewed) {
        return viewed;
    }
    sinks.depth_of_field = frame->pass->stage();
    return ok();
}

/// The frame's lens: the scene's own field of view, focused a metre out, so everything the scene
/// holds is behind the focus plane — at f/1 on a 270-pixel image, a circle of about 3 pixels.
[[nodiscard]] DofSettings frame_settings(f32 f_number) noexcept {
    DofSettings settings;
    settings.lens.sensor_height_mm = 24.0F;
    settings.lens.focal_length_mm =
        rendering::depth_of_field::focal_length_for_field_of_view(0.9F, 24.0F);
    settings.lens.aperture = f_number;
    settings.lens.focus_distance = 1.0F;
    return settings;
}

[[nodiscard]] bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

}  // namespace

CY_TEST_CASE("a pinhole through the assembled frame is the frame without the stage") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // THE FRAME AS IT WAS: no depth of field in the post chain.
    Array<u32> without(allocator());
    {
        FrameScene scene(allocator());
        CY_REQUIRE(scene.build(fixture.device()).has_value());
        scene.set_read_back(true);
        rendering::assembly::AssemblyReport report;
        CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
        CY_CHECK_EQ(scene.assembly().frame().pass_of(FramePassKind::DepthOfField), kInvalidPass);
        CY_REQUIRE(without.append(scene.pixels()).has_value());
    }

    // f/infinity: every pass declared and recorded, and not one byte of the output moved.
    DepthOfFieldPass pass;
    CY_REQUIRE(
        pass.create(fixture.device(), DepthOfFieldPassDescription{kWidth, kHeight}).has_value());
    {
        FrameScene scene(allocator());
        FrameCase frame{&scene, &pass, frame_settings(std::numeric_limits<f32>::infinity())};
        FrameSceneHooks hooks;
        hooks.user = &frame;
        hooks.configure = &configure;
        hooks.before_assemble = &before_assemble;
        scene.set_hooks(hooks);
        CY_REQUIRE(scene.build(fixture.device()).has_value());
        scene.set_read_back(true);
        rendering::assembly::AssemblyReport report;
        pass.reset_report();
        CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
        CY_CHECK_NE(scene.assembly().frame().pass_of(FramePassKind::DepthOfField), kInvalidPass);
        CY_CHECK_EQ(pass.report().dispatches, 5U);
        CY_CHECK_EQ(pass.constants().lens[0], 0.0F);
        CY_REQUIRE_EQ(scene.pixels().size(), without.size());
        u32 exact = 0;
        for (usize texel = 0; texel < without.size(); ++texel) {
            exact += scene.pixels()[texel] == without[texel] ? 1U : 0U;
        }
        CY_CHECK_EQ(exact, static_cast<u32>(without.size()));
    }

    // And with a real aperture the same frame is defocused.
    {
        FrameScene scene(allocator());
        FrameCase frame{&scene, &pass, frame_settings(1.0F)};
        FrameSceneHooks hooks;
        hooks.user = &frame;
        hooks.configure = &configure;
        hooks.before_assemble = &before_assemble;
        scene.set_hooks(hooks);
        CY_REQUIRE(scene.build(fixture.device()).has_value());
        scene.set_read_back(true);
        rendering::assembly::AssemblyReport report;
        CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
        const u32 differing = scene.differing_texels(without.span());
        std::fprintf(stderr, "f/1 changed %u of %u texels\n", differing, kWidth * kHeight);
        CY_CHECK_GT(differing, 1000U);
    }
    pass.destroy();
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("with depth of field off the frame is the one drawn before the stage existed") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // `references/depth_of_field_absent.png` is render.bloom's `frame_scene_before_bloom.png`
    // (md5 b8a26f4c2f82f4939e3dedda3b72199b), copied unchanged: `render.pipeline`'s capture of this
    // scene from a build of the tree before bloom existed, and so before this module. The module
    // adds no shader to the frame and changes none, so the frame without the stage must still be
    // that frame. It is held to the golden rule, and byte for byte on the machine that wrote it.
    char path[1024];
    (void)std::snprintf(path, sizeof(path), "%s/references/depth_of_field_absent.png",
                        CY_DOF_TEST_DIR);
    FrameScene scene(allocator());
    CY_REQUIRE(scene.build(fixture.device()).has_value());
    scene.set_read_back(true);
    rendering::assembly::AssemblyReport report;
    CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
    CY_CHECK_EQ(scene.assembly().frame().pass_of(FramePassKind::DepthOfField), kInvalidPass);
    CY_CHECK_EQ(scene.assembly().resources().depth_of_field, kInvalidResource);

    render_test::Image candidate(allocator());
    CY_REQUIRE(render_test::adopt(candidate, scene.pixels(), kWidth, kHeight).has_value());
    if (updating_references()) {
        CY_CHECK(render_test::write_png(path, candidate).has_value());
        std::fprintf(stderr, "wrote %s — look at it, then commit it. This run FAILS on purpose.\n",
                     path);
        CY_TEST_FAIL_CHECK("references were regenerated; this mode never passes");
        return;
    }
    render_test::Image reference(allocator());
    CY_REQUIRE(render_test::read_png(path, reference).has_value());
    const render_test::Comparison compared = render_test::compare(reference, candidate);
    u32 exact = 0;
    const usize texels = compared.comparable ? candidate.texels.size() : 0;
    for (usize texel = 0; texel < texels; ++texel) {
        exact += candidate.texels[texel] == reference.texels[texel] ? 1U : 0U;
    }
    std::fprintf(stderr,
                 "depth of field off against the frame before it: %u of %zu texels "
                 "byte-identical, %u over tolerance (%u off an edge)\n",
                 exact, candidate.texels.size(), compared.differing, compared.differing_off_edge);
    CY_REQUIRE(compared.comparable);
    CY_CHECK_EQ(compared.differing_off_edge, 0U);
    CY_CHECK_LE(compared.differing, compared.edge_texels);
    // On the machine that wrote the reference, byte for byte.
    CY_CHECK_EQ(exact, static_cast<u32>(candidate.texels.size()));
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
