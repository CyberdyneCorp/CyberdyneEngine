// SPDX-License-Identifier: MIT
// Bloom on a device: what the chain DOES to an image, measured on the linear HDR it writes.
//
// `rendering-post-processing`'s Bloom requirement and its two scenarios, plus the three properties
// a bloom has to have before it is allowed near a frame:
//
//   a bright region glows onto its neighbours     the halo is there, falls off, and is centred on
//                                                 its source rather than on a flipped copy of it
//   no bloom is no change                         intensity zero returns the scene BIT FOR BIT, and
//                                                 the assembled frame with bloom at zero is the
//                                                 frame without bloom, texel for texel
//   bloom off is the frame before bloom           the assembled frame without bloom matches
//                                                 `references/frame_scene_before_bloom.png`, which
//                                                 a build from before bloom wrote
//   a dim scene is untouched                     nothing above the knee, nothing moves — exactly
//   energy is bounded                             the frame never gains energy, and the bloom gives
//                                                 back most of what the threshold took
//   firefly suppression (the Karis average)       one very bright texel scatters a third of what a
//                                                 block of the same energy does, and the same
//                                                 amount wherever it lands
//   anamorphic stretch and lens dirt              the two optional controls, each measured against
//                                                 the same frame without it
//
// THE CHAIN IS THE FRAME'S. Every case declares its passes with `declare_bloom_chain` — the
// function `ForwardFrame` calls — and records them with `BloomRenderer`, so what is measured here
// is what a frame runs. The source is uploaded as half floats and the composite read back as half
// floats; nothing in between is converted.
//
// Vulkan with validation and synchronisation validation on. Without a GPU the suite SKIPS LOUDLY.

#include "frame_scene.h"
#include "golden.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/pipeline/bloom_renderer.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace cy;
using namespace cy::pipeline_test;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

inline constexpr u32 kSize = 256;
inline constexpr u32 kTexels = kSize * kSize;

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
        description.application_name = "cy_test_render_bloom";
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
            f32 value = std::ldexp(static_cast<f32>(mantissa), -24);
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

/// A grey image: one luminance per texel, stored in all three channels.
struct Image {
    explicit Image(Allocator& memory) noexcept : texels(memory) {}
    Array<f32> texels;

    [[nodiscard]] Status make(f32 background) noexcept {
        if (Status sized = texels.resize(kTexels); !sized) {
            return sized;
        }
        for (f32& texel : texels) {
            texel = background;
        }
        return ok();
    }
    void fill(u32 x0, u32 y0, u32 width, u32 height, f32 value) noexcept {
        for (u32 y = y0; y < y0 + height; ++y) {
            for (u32 x = x0; x < x0 + width; ++x) {
                texels[(y * kSize) + x] = value;
            }
        }
    }
    [[nodiscard]] f32 at(u32 x, u32 y) const noexcept { return texels[(y * kSize) + x]; }
    [[nodiscard]] f64 sum() const noexcept {
        f64 total = 0.0;
        for (const f32 texel : texels) {
            total += static_cast<f64>(texel);
        }
        return total;
    }
};

/// The chain over one image, on the device, and the composite read back.
class Bench {
public:
    explicit Bench(rhi::Device& device) noexcept : device_(&device) {}
    ~Bench() {
        (void)device_->wait_idle();
        bloom_.shutdown();
        pipelines_.shutdown();
        if (!upload_.is_null()) {
            device_->destroy_buffer(upload_);
        }
        if (!readback_.is_null()) {
            device_->destroy_buffer(readback_);
        }
        if (!dirt_view_.is_null()) {
            device_->destroy_texture_view(dirt_view_);
        }
        if (!dirt_.is_null()) {
            device_->destroy_texture(dirt_);
        }
    }

    Bench(const Bench&) = delete;
    Bench& operator=(const Bench&) = delete;

    [[nodiscard]] Status build() noexcept {
        PipelineSetup setup;
        setup.color_format = rhi::Format::Rgba16Sfloat;
        setup.transparency = false;
        if (Status made = pipelines_.initialize(*device_, setup); !made) {
            return made;
        }
        if (Status made = bloom_.initialize(*device_, pipelines_); !made) {
            return made;
        }
        rhi::BufferDescription buffer;
        buffer.size = static_cast<u64>(kTexels) * 8U;
        buffer.name = "bloom test upload";
        buffer.usage = rhi::BufferUsage::TransferSource;
        buffer.memory = rhi::MemoryUse::Upload;
        Expected<rhi::BufferHandle, Error> made = device_->create_buffer(buffer);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        upload_ = *made;
        buffer.name = "bloom test readback";
        buffer.usage = rhi::BufferUsage::TransferDestination;
        buffer.memory = rhi::MemoryUse::Readback;
        made = device_->create_buffer(buffer);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        readback_ = *made;
        return ok();
    }

    /// Run the chain over `in` and write the composite into `out`, as luminance. `raw` keeps the
    /// half-float bits of the red channel for a bit-exact comparison.
    /// A lens dirt mask, black left of column `split` and white from it on, uploaded once and left
    /// in the sampled layout — the state a resident texture is in — then handed to the renderer.
    [[nodiscard]] Status make_dirt(u32 split) noexcept {
        rhi::TextureDescription texture;
        texture.name = "bloom test lens dirt";
        texture.format = rhi::Format::Rgba16Sfloat;
        texture.extent = rhi::Extent3D{kSize, kSize, 1};
        texture.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination;
        Expected<rhi::TextureHandle, Error> made = device_->create_texture(texture);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        dirt_ = *made;
        Image mask(allocator());
        if (Status built = mask.make(0.0F); !built) {
            return built;
        }
        mask.fill(split, 0, kSize - split, kSize, 1.0F);
        if (Status staged = stage(mask); !staged) {
            return staged;
        }
        Expected<u32, Error> began = device_->begin_frame();
        if (!began.has_value()) {
            return make_unexpected(began.error());
        }
        RenderGraph graph(allocator());
        TextureRequest request;
        request.name = "bloom test lens dirt";
        request.format = rhi::Format::Rgba16Sfloat;
        request.width = kSize;
        request.height = kSize;
        const ResourceId dirt = graph.import_texture(request, dirt_, rhi::ImageUse::Undefined);
        Transfer upload{this, dirt};
        graph.add_pass("bloom test dirt upload", rhi::QueueKind::Graphics)
            .write(dirt, rhi::Access::TransferWrite)
            .record(&record_upload, &upload);
        // The reader that leaves it sampled, as the beauty shot's texture residency pass does.
        graph.add_pass("bloom test dirt residency", rhi::QueueKind::Graphics)
            .read(dirt, rhi::Access::FragmentSampledRead)
            .side_effect();
        if (Status executed = execute(graph); !executed) {
            return executed;
        }
        rhi::TextureViewDescription view;
        view.name = "bloom test lens dirt";
        view.texture = dirt_;
        Expected<rhi::TextureViewHandle, Error> viewed = device_->create_texture_view(view);
        if (!viewed.has_value()) {
            return make_unexpected(viewed.error());
        }
        dirt_view_ = *viewed;
        bloom_.set_lens_dirt(dirt_view_);
        return ok();
    }

    [[nodiscard]] Status run(const Image& in, const BloomSettings& settings, Image& out,
                             Array<u16>* raw = nullptr) noexcept {
        if (Status staged = stage(in); !staged) {
            return staged;
        }
        Expected<u32, Error> began = device_->begin_frame();
        if (!began.has_value()) {
            return make_unexpected(began.error());
        }
        RenderGraph graph(allocator());
        TextureRequest request;
        request.name = "bloom test scene";
        request.format = rhi::Format::Rgba16Sfloat;
        request.width = kSize;
        request.height = kSize;
        const ResourceId scene = graph.create_texture(request);
        Transfer upload{this, scene};
        graph.add_pass("bloom test upload", rhi::QueueKind::Graphics)
            .write(scene, rhi::Access::TransferWrite)
            .record(&record_upload, &upload);

        bloom_.set_settings(settings);
        bloom_.reset_report();
        BloomChain chain;
        const FramePassCallback sink = bloom_.sink(chain);
        if (Status declared =
                declare_bloom_chain(graph, scene, kSize, kSize, rhi::Format::Rgba16Sfloat,
                                    settings.mip_count, sink, chain);
            !declared) {
            (void)device_->end_frame();
            return declared;
        }

        BufferRequest readback_request;
        readback_request.name = "bloom test readback";
        readback_request.size = static_cast<u64>(kTexels) * 8U;
        const ResourceId readback = graph.import_buffer(readback_request, readback_);
        Transfer copy{this, chain.output};
        graph.add_pass("bloom test copy", rhi::QueueKind::Graphics)
            .read(chain.output, rhi::Access::TransferRead)
            .write(readback, rhi::Access::TransferWrite)
            .record(&record_readback, &copy);
        graph.add_pass("bloom test host", rhi::QueueKind::Graphics)
            .read(readback, rhi::Access::HostRead)
            .side_effect();

        if (Status executed = execute(graph); !executed) {
            return executed;
        }
        steps_ = bloom_.report().steps;

        const auto* read = static_cast<const u16*>(device_->buffer_mapped_pointer(readback_));
        if (read == nullptr) {
            return fail(ErrorCode::Internal, "bloom test: the readback buffer is not mapped");
        }
        if (Status sized = out.texels.resize(kTexels); !sized) {
            return sized;
        }
        if (raw != nullptr) {
            if (Status sized = raw->resize(kTexels); !sized) {
                return sized;
            }
        }
        for (usize texel = 0; texel < kTexels; ++texel) {
            // Grey in, grey out: the three channels carry one value, and red is it.
            out.texels[texel] = from_half(read[texel * 4U]);
            if (raw != nullptr) {
                (*raw)[texel] = read[texel * 4U];
            }
        }
        return ok();
    }

    [[nodiscard]] u32 steps() const noexcept { return steps_; }

private:
    /// Write `image` into the staging buffer as grey half floats.
    [[nodiscard]] Status stage(const Image& image) noexcept {
        auto* staged = static_cast<u16*>(device_->buffer_mapped_pointer(upload_));
        if (staged == nullptr) {
            return fail(ErrorCode::Internal, "bloom test: the upload buffer is not mapped");
        }
        for (u32 texel = 0; texel < kTexels; ++texel) {
            const u16 half = to_half(image.texels[texel]);
            staged[(texel * 4U) + 0] = half;
            staged[(texel * 4U) + 1] = half;
            staged[(texel * 4U) + 2] = half;
            staged[(texel * 4U) + 3] = to_half(1.0F);
        }
        return ok();
    }

    /// Compile, run and wait for `graph`, then close the device frame the caller opened.
    [[nodiscard]] Status execute(RenderGraph& graph) noexcept {
        Status frame = graph.status();
        if (frame) {
            GraphExecutor executor(allocator(), *device_);
            const Expected<ExecutionResult, Error> result =
                executor.execute(graph, CompileOptions{}, ExecuteOptions{});
            if (result.has_value()) {
                frame = device_->wait_idle();
            } else {
                frame = make_unexpected(result.error());
            }
            executor.release();
        }
        if (Status ended = device_->end_frame(); !ended && frame) {
            frame = ended;
        }
        return frame;
    }

    struct Transfer {
        Bench* bench = nullptr;
        ResourceId texture = kInvalidResource;
    };

    static void record_upload(const PassContext& context, void* user) noexcept {
        const auto* transfer = static_cast<const Transfer*>(user);
        rhi::BufferTextureCopy region;
        region.texture_extent = rhi::Extent3D{kSize, kSize, 1};
        context.commands->copy_buffer_to_texture(transfer->bench->upload_,
                                                 context.executor->texture(transfer->texture),
                                                 Span<const rhi::BufferTextureCopy>(&region, 1));
    }

    static void record_readback(const PassContext& context, void* user) noexcept {
        const auto* transfer = static_cast<const Transfer*>(user);
        rhi::BufferTextureCopy region;
        region.texture_extent = rhi::Extent3D{kSize, kSize, 1};
        context.commands->copy_texture_to_buffer(context.executor->texture(transfer->texture),
                                                 transfer->bench->readback_,
                                                 Span<const rhi::BufferTextureCopy>(&region, 1));
    }

    rhi::Device* device_ = nullptr;
    FramePipelines pipelines_;
    BloomRenderer bloom_;
    rhi::BufferHandle upload_;
    rhi::BufferHandle readback_;
    rhi::TextureHandle dirt_;
    rhi::TextureViewHandle dirt_view_;
    u32 steps_ = 0;
};

/// The settings every case starts from: a threshold of one scene unit, a visible intensity.
[[nodiscard]] BloomSettings test_settings() noexcept {
    BloomSettings settings;
    settings.threshold = 1.0F;
    settings.knee = 0.5F;
    settings.intensity = 0.5F;
    settings.scatter = 0.7F;
    settings.mip_count = 6;
    return settings;
}

/// The energy the prefilter takes out of `image`: the part of each texel above the soft knee.
[[nodiscard]] f64 scattered(const Image& image, const BloomSettings& settings) noexcept {
    f64 total = 0.0;
    for (const f32 texel : image.texels) {
        total += static_cast<f64>(bloom_prefilter(Vec3{texel, texel, texel}, settings).x);
    }
    return total;
}

/// How much energy came BACK as bloom: the output, minus the input with its scattered part
/// removed. The whole of what was scattered for a lossless blur.
[[nodiscard]] f64 returned(const Image& in, const Image& out, const BloomSettings& settings) {
    return out.sum() -
           (in.sum() - (static_cast<f64>(settings.intensity) * scattered(in, settings)));
}

// The source block, placed off centre in both axes so a halo that came out mirrored would be
// found in the wrong corner.
inline constexpr u32 kBlockX = 72;
inline constexpr u32 kBlockY = 96;
inline constexpr u32 kBlockSize = 8;
inline constexpr f32 kBlockValue = 50.0F;
/// Exact in a half float, like every value these cases upload, so the input the device saw and the
/// input the sums below read are the same numbers.
inline constexpr f32 kBackground = 0.125F;

}  // namespace

CY_TEST_CASE("a bright region glows onto its neighbours, falling off and centred on its source") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Bench bench(fixture.device());
    CY_REQUIRE(bench.build().has_value());

    Image in(allocator());
    CY_REQUIRE(in.make(kBackground).has_value());
    in.fill(kBlockX, kBlockY, kBlockSize, kBlockSize, kBlockValue);
    Image out(allocator());
    const BloomSettings settings = test_settings();
    CY_REQUIRE(bench.run(in, settings, out).has_value());
    CY_CHECK_EQ(bench.steps(), 12U);

    // Along the row through the block's centre, to the right of it: brighter than the background
    // with bloom, and dimmer the further out.
    const u32 row = kBlockY + (kBlockSize / 2U);
    const u32 edge = kBlockX + kBlockSize;
    const f32 near = out.at(edge + 4U, row) - kBackground;
    const f32 middle = out.at(edge + 12U, row) - kBackground;
    const f32 far = out.at(edge + 40U, row) - kBackground;
    std::fprintf(stderr, "glow above background at 4, 12 and 40 texels: %.4f %.4f %.4f\n",
                 static_cast<f64>(near), static_cast<f64>(middle), static_cast<f64>(far));
    CY_CHECK_GT(near, 0.5F);
    CY_CHECK_GT(middle, 0.05F);
    CY_CHECK_GT(near, middle);
    CY_CHECK_GT(middle, far);
    CY_CHECK_GE(far, 0.0F);

    // CENTRED ON ITS SOURCE, in both axes: the halo 12 texels left of the block matches the one
    // 12 texels right of it, and above matches below. A chain whose passes disagreed about which
    // way is up would put the glow around the block's mirror image instead.
    const u32 column = kBlockX + (kBlockSize / 2U);
    const f32 left = out.at(kBlockX - 12U, row) - kBackground;
    const f32 above = out.at(column, kBlockY - 12U) - kBackground;
    const f32 below = out.at(column, kBlockY + kBlockSize + 12U) - kBackground;
    CY_CHECK_LT(std::fabs(left - middle), 0.25F * middle);
    CY_CHECK_LT(std::fabs(above - below), 0.25F * middle);
    CY_CHECK_LT(std::fabs(above - middle), 0.25F * middle);
    // And no second peak at the mirrored position, which is where a flipped chain would put it.
    const f32 mirrored = out.at(kSize - 1U - column, kSize - 1U - row) - kBackground;
    CY_CHECK_LT(mirrored, 0.1F * near);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("no bloom is no change: an intensity of zero returns the scene bit for bit") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Bench bench(fixture.device());
    CY_REQUIRE(bench.build().has_value());

    Image in(allocator());
    CY_REQUIRE(in.make(kBackground).has_value());
    in.fill(kBlockX, kBlockY, kBlockSize, kBlockSize, kBlockValue);
    BloomSettings settings = test_settings();
    settings.intensity = 0.0F;
    Image out(allocator());
    Array<u16> bits(allocator());
    CY_REQUIRE(bench.run(in, settings, out, &bits).has_value());

    u32 differing = 0;
    for (u32 texel = 0; texel < kTexels; ++texel) {
        differing += bits[texel] != to_half(in.texels[texel]) ? 1U : 0U;
    }
    CY_CHECK_EQ(differing, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("a dim scene below the threshold comes out of the chain exactly as it went in") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Bench bench(fixture.device());
    CY_REQUIRE(bench.build().has_value());

    // A checkerboard of 8-texel squares at 0.125 and 0.4375 — contrast everywhere, and every texel
    // below the knee's lower edge at threshold 1 and knee 0.5.
    Image in(allocator());
    CY_REQUIRE(in.make(kBackground).has_value());
    for (u32 y = 0; y < kSize; y += 8U) {
        for (u32 x = 0; x < kSize; x += 8U) {
            if (((x / 8U) + (y / 8U)) % 2U == 0U) {
                in.fill(x, y, 8, 8, 0.4375F);
            }
        }
    }
    Image out(allocator());
    Array<u16> bits(allocator());
    CY_REQUIRE(bench.run(in, test_settings(), out, &bits).has_value());

    u32 differing = 0;
    for (u32 texel = 0; texel < kTexels; ++texel) {
        differing += bits[texel] != to_half(in.texels[texel]) ? 1U : 0U;
    }
    CY_CHECK_EQ(differing, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("bloom moves energy and never adds it: the frame is bounded by what it started with") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Bench bench(fixture.device());
    CY_REQUIRE(bench.build().has_value());

    Image in(allocator());
    CY_REQUIRE(in.make(kBackground).has_value());
    in.fill(kBlockX, kBlockY, kBlockSize, kBlockSize, kBlockValue);
    in.fill(170, 40, 4, 12, 20.0F);
    BloomSettings settings = test_settings();
    const f64 taken = static_cast<f64>(settings.intensity) * scattered(in, settings);

    const bool karis_modes[] = {true, false};
    for (const bool karis : karis_modes) {
        settings.firefly_suppression = karis;
        Image out(allocator());
        CY_REQUIRE(bench.run(in, settings, out).has_value());
        const f64 before = in.sum();
        const f64 after = out.sum();
        const f64 back = returned(in, out, settings);
        std::fprintf(stderr,
                     "karis %s: energy in %.2f, out %.2f; scattered %.2f, returned %.2f (%.1f%%)\n",
                     karis ? "on" : "off", before, after, taken, back, 100.0 * back / taken);
        // NEVER BRIGHTER. Half-float storage rounds each texel by up to a part in two thousand,
        // which is the whole of the allowance.
        CY_CHECK_LE(after, before * 1.0005);
        // AND NOT A SINK: most of what the threshold took comes back as the halo. The Karis
        // average gives up some of a hard edge's energy by design; the plain average keeps it.
        CY_CHECK_GT(back, taken * (karis ? 0.6 : 0.9));
        CY_CHECK_LT(back, taken * 1.02);
    }
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the Karis average stops one very bright texel producing a bloom star") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Bench bench(fixture.device());
    CY_REQUIRE(bench.build().has_value());
    BloomSettings settings = test_settings();

    // The same energy twice: an 8x8 block, and all of it in one texel.
    Image block(allocator());
    CY_REQUIRE(block.make(kBackground).has_value());
    block.fill(kBlockX, kBlockY, kBlockSize, kBlockSize, kBlockValue);
    const f32 firefly = kBlockValue * static_cast<f32>(kBlockSize * kBlockSize);

    f64 firefly_back[2] = {};
    f64 block_back = 0.0;
    Image out(allocator());
    CY_REQUIRE(bench.run(block, settings, out).has_value());
    block_back = returned(block, out, settings);
    for (u32 shift = 0; shift < 2U; ++shift) {
        Image single(allocator());
        CY_REQUIRE(single.make(kBackground).has_value());
        single.fill(kBlockX + shift, kBlockY, 1, 1, firefly);
        CY_REQUIRE(bench.run(single, settings, out).has_value());
        firefly_back[shift] = returned(single, out, settings);
    }
    settings.firefly_suppression = false;
    Image single(allocator());
    CY_REQUIRE(single.make(kBackground).has_value());
    single.fill(kBlockX, kBlockY, 1, 1, firefly);
    CY_REQUIRE(bench.run(single, settings, out).has_value());
    const f64 unsuppressed = returned(single, out, settings);

    std::fprintf(stderr,
                 "halo energy: block %.2f, firefly %.2f and %.2f one texel over, firefly without "
                 "the Karis average %.2f\n",
                 block_back, firefly_back[0], firefly_back[1], unsuppressed);
    // Suppressed: a firefly scatters a fraction of what a surface of the same energy does, and of
    // what it would scatter unweighted. Measured on the reference device at about a third and a
    // quarter. Not less, and that is the filter rather than a defect: a texel beside the 13-tap
    // footprint's centre falls in all five of its boxes, so for that one target texel the boxes
    // weigh the same and the average cannot tell the outlier from the surface.
    CY_CHECK_LT(firefly_back[0], 0.45 * block_back);
    CY_CHECK_LT(firefly_back[0], 0.35 * unsuppressed);
    // Stable: moving it by one texel — which is what a firefly does between frames — leaves its
    // halo's energy where it was, so it does not flicker.
    const f64 larger = firefly_back[0] > firefly_back[1] ? firefly_back[0] : firefly_back[1];
    const f64 smaller = firefly_back[0] > firefly_back[1] ? firefly_back[1] : firefly_back[0];
    CY_CHECK_GT(smaller, 0.9 * larger);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("an anamorphic stretch widens the halo horizontally and leaves it vertically") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Bench bench(fixture.device());
    CY_REQUIRE(bench.build().has_value());
    Image in(allocator());
    CY_REQUIRE(in.make(kBackground).has_value());
    in.fill(kBlockX, kBlockY, kBlockSize, kBlockSize, kBlockValue);
    const u32 row = kBlockY + (kBlockSize / 2U);
    const u32 column = kBlockX + (kBlockSize / 2U);

    BloomSettings settings = test_settings();
    settings.anamorphic = 3.0F;
    Image out(allocator());
    CY_REQUIRE(bench.run(in, settings, out).has_value());
    const f32 across = out.at(kBlockX + kBlockSize + 24U, row) - kBackground;
    const f32 down = out.at(column, kBlockY + kBlockSize + 24U) - kBackground;
    std::fprintf(stderr, "anamorphic 3: glow 24 texels across %.4f, down %.4f\n",
                 static_cast<f64>(across), static_cast<f64>(down));
    CY_CHECK_GT(across, 1.3F * down);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("lens dirt brightens the bloom where the mask is, and is absent at zero") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Bench bench(fixture.device());
    CY_REQUIRE(bench.build().has_value());
    const u32 column = kBlockX + (kBlockSize / 2U);
    // Dirty to the right of the block's centre, clean to its left.
    CY_REQUIRE(bench.make_dirt(column).has_value());
    Image in(allocator());
    CY_REQUIRE(in.make(kBackground).has_value());
    in.fill(kBlockX, kBlockY, kBlockSize, kBlockSize, kBlockValue);
    const u32 row = kBlockY + (kBlockSize / 2U);

    BloomSettings settings = test_settings();
    Image clean(allocator());
    CY_REQUIRE(bench.run(in, settings, clean).has_value());
    settings.lens_dirt_intensity = 3.0F;
    Image dirty(allocator());
    CY_REQUIRE(bench.run(in, settings, dirty).has_value());

    const f32 clean_left = clean.at(kBlockX - 12U, row) - kBackground;
    const f32 clean_right = clean.at(kBlockX + kBlockSize + 12U, row) - kBackground;
    const f32 left = dirty.at(kBlockX - 12U, row) - kBackground;
    const f32 right = dirty.at(kBlockX + kBlockSize + 12U, row) - kBackground;
    std::fprintf(stderr, "lens dirt 3: glow on the clean side %.4f, on the dirty side %.4f\n",
                 static_cast<f64>(left), static_cast<f64>(right));
    // Four times the bloom over the dirt: 1 + 3 x white. The clean side is the clean lens's.
    CY_CHECK_GT(right, 3.0F * left);
    CY_CHECK_LT(std::fabs(left - clean_left), 1e-3F + (0.01F * clean_left));
    // At intensity zero the mask, although bound, changes nothing: the halo is symmetric.
    CY_CHECK_LT(std::fabs(clean_right - clean_left), 0.25F * clean_right);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("with bloom off the assembled frame is the frame drawn before bloom existed") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // `references/frame_scene_before_bloom.png` is `render.pipeline`'s capture of this scene from a
    // build of the tree before bloom was added — the file that suite wrote, copied unchanged. The
    // off path is the one every existing frame takes, so it is held to the golden rule rather than
    // to the other bloom cases' own baseline.
    render_test::Image reference(allocator());
    char path[1024];
    (void)std::snprintf(path, sizeof(path), "%s/references/frame_scene_before_bloom.png",
                        CY_PIPELINE_TEST_DIR);
    CY_REQUIRE(render_test::read_png(path, reference).has_value());

    FrameScene scene(allocator());
    CY_REQUIRE(scene.build(fixture.device()).has_value());
    scene.set_read_back(true);
    rendering::assembly::AssemblyReport report;
    CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
    CY_CHECK_EQ(scene.assembly().frame().pass_of(FramePassKind::Bloom), kInvalidPass);
    CY_CHECK_EQ(scene.assembly().frame().bloom().levels, 0U);

    render_test::Image candidate(allocator());
    CY_REQUIRE(render_test::adopt(candidate, scene.pixels(), kWidth, kHeight).has_value());
    const render_test::Comparison compared = render_test::compare(reference, candidate);
    u32 exact = 0;
    const usize texels = compared.comparable ? candidate.texels.size() : 0;
    for (usize texel = 0; texel < texels; ++texel) {
        exact += candidate.texels[texel] == reference.texels[texel] ? 1U : 0U;
    }
    std::fprintf(stderr,
                 "bloom off against the frame before bloom: %u of %zu texels byte-identical, %u "
                 "over tolerance (%u off an edge)\n",
                 exact, candidate.texels.size(), compared.differing, compared.differing_off_edge);
    CY_REQUIRE(compared.comparable);
    // The golden rule, so a conformant driver other than the one that wrote the file passes. On
    // the machine that wrote it, every texel is byte-identical — the line above says so.
    CY_CHECK_EQ(compared.differing_off_edge, 0U);
    CY_CHECK_LE(compared.differing, compared.edge_texels);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE(
    "the assembled frame records bloom, and at intensity zero it is the frame without it") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // THE FRAME AS IT WAS: no bloom in the post chain.
    Array<u32> without(allocator());
    {
        FrameScene scene(allocator());
        CY_REQUIRE(scene.build(fixture.device()).has_value());
        scene.set_read_back(true);
        rendering::assembly::AssemblyReport report;
        CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
        CY_CHECK_EQ(scene.assembly().frame().pass_of(FramePassKind::Bloom), kInvalidPass);
        CY_REQUIRE(without.append(scene.pixels()).has_value());
    }

    // Bloom in the chain at intensity zero: every pass declared and recorded, and not one texel of
    // the 8-bit output moved.
    BloomSettings settings;
    settings.threshold = bloom_threshold_for_exposure(-11.4F, 0.0F);
    settings.intensity = 0.0F;
    {
        FrameScene scene(allocator());
        CY_REQUIRE(scene.build(fixture.device(), &settings).has_value());
        scene.set_read_back(true);
        rendering::assembly::AssemblyReport report;
        CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
        CY_CHECK_EQ(scene.bloom().report().steps, scene.assembly().frame().bloom().step_count);
        CY_CHECK_GT(scene.bloom().report().steps, 0U);
        CY_CHECK_EQ(scene.differing_texels(without.span()), 0U);
        CY_CHECK_EQ(scene.pixels().size(), without.size());
        u32 exact = 0;
        for (usize texel = 0; texel < without.size(); ++texel) {
            exact += scene.pixels()[texel] == without[texel] ? 1U : 0U;
        }
        CY_CHECK_EQ(exact, static_cast<u32>(without.size()));
    }

    // And at a real intensity, the lit cubes' highlights bloom into the frame.
    settings.intensity = 0.5F;
    {
        FrameScene scene(allocator());
        CY_REQUIRE(scene.build(fixture.device(), &settings).has_value());
        scene.set_read_back(true);
        rendering::assembly::AssemblyReport report;
        CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
        const u32 differing = scene.differing_texels(without.span());
        std::fprintf(stderr, "bloom at 0.5 changed %u of %u texels\n", differing, kWidth * kHeight);
        CY_CHECK_GT(differing, 500U);
    }
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
