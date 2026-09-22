// SPDX-License-Identifier: MIT
// THE ENGINE'S OWN FORWARD PATH PUTS A TEXTURE ON A MODEL. M11.c task 3.7's last mile.
//
// ================================================================================================
// WHAT WAS MISSING, IN THE WORDS THE TASK LEFT OPEN
// ================================================================================================
//
// > **WHAT IS NOT DONE: `cy/frame.slang` STILL SAMPLES NO TEXTURE.** `surfaceOf()` reads four
// > constants out of `cyMaterialWords`, and the engine's forward pipeline cannot bind this table as
// > it stands: `src/rendering/pipeline/`'s set 0 carries `cy/globals.slang`'s block at binding 0
// > and a pipeline binds ONE set per index, so a program that wants both needs a set 0 that
// > carries both.
//
// Both ends were already built and measured. `render.material_binding` renders a surface whose
// material samples a texture through the DEVICE's own global table — 100.00% of texels differing
// from the declared average at mean |delta| 82.167/255 — and `MaterialTextureTable` makes cooked
// pixels resident in that table. What no suite in the tree could say is whether the ENGINE'S frame
// — `FrameAssembly`'s passes, `FramePipelines`' pipelines, `cy/frame.slang`'s own fragment shader —
// samples anything at all. It did not: every object in every frame this engine assembled was one
// flat colour, and nothing went red.
//
// ================================================================================================
// THE PICTURE IS THE CLAIM, AND THE CONTROL IS A SUBSTITUTION
// ================================================================================================
//
// The same scene is rendered three times through the same code path, with one thing changed each
// time:
//
//   constants   every material's texture slot says "nothing here" — WHICH IS THE FRAME THIS
//               ENGINE PRODUCED BEFORE THIS TASK, produced by today's code rather than remembered.
//   textured    every material's base colour is multiplied by a pattern in the global table.
//   averaged    THE SAME FRAME with that texture replaced by its own DECLARED AVERAGE. Same
//               constants, same lights, same exposure, same geometry: a single different image in
//               one slot of one table.
//
// The substitution is what makes this falsifiable. An average-coloured frame is entirely plausible
// — it is what a binding that silently sampled nothing, a slot that always resolved to zero, or a
// set bound at the wrong index all produce — so "it looks textured" is not a claim a person can
// check and a difference is. Both differences below go to zero if the sampling is removed from
// `surfaceOf()`, and the second goes to zero on its own if the frame samples SOMETHING that is not
// the texture the material names.

#include "frame_scene.h"

#include "golden.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/material/material.h>
#include <cy/rendering/pipeline/material_textures.h>
#include <cy/servers/render/server.h>
#include <cy/test/test.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace cy;
using namespace cy::pipeline_test;

namespace {

using rendering::pipeline::MaterialTextureSlot;
using rendering::pipeline::MaterialTextureTable;
using rendering::pipeline::TextureUpload;

constexpr u32 kExtent = 64;
constexpr u32 kTexels = kExtent * kExtent;

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
    }
    std::fprintf(stderr, "graphics validation %s: %s\n",
                 severity == rhi::ValidationSeverity::Error ? "error" : "warning",
                 message != nullptr ? message : "");
}

/// A device for the case, built exactly as `render.pipeline`'s is — validation and synchronisation
/// validation on, because a frame that draws and trips the validator is not a frame that works.
class DeviceFixture {
public:
    DeviceFixture() noexcept : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_test_render_forward_material_texture";
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
                     "no Vulkan device on this machine; the backend selected was '%s' because %s\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)",
                     selection_.reason != nullptr ? selection_.reason : "(no reason given)");
    }

private:
    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    u32 errors_ = 0;
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

// --- The content -------------------------------------------------------------------------------

/// A pattern with structure at every scale — a coarse checker, a fine checker over it and a ramp —
/// so that no mip level of it is flat and no channel is constant. A texture that was nearly its own
/// average would make the control below pass for the wrong reason, which is the failure this
/// generator exists to avoid.
void write_level0(Array<u8>& pixels) noexcept {
    for (u32 y = 0; y < kExtent; ++y) {
        for (u32 x = 0; x < kExtent; ++x) {
            const bool coarse = (((x / 16U) + (y / 16U)) & 1U) != 0U;
            const bool fine = (((x / 4U) + (y / 4U)) & 1U) != 0U;
            const usize base = ((static_cast<usize>(y) * kExtent) + x) * 4U;
            pixels[base + 0] = static_cast<u8>(coarse ? 235U : 20U);
            pixels[base + 1] = static_cast<u8>(fine ? 210U : 35U);
            pixels[base + 2] = static_cast<u8>((x * 4U) & 0xFFU);
            pixels[base + 3] = 255U;
        }
    }
}

void downsample(const u8* source, u32 source_width, u32 source_height, u8* out) noexcept {
    const u32 width = source_width > 1 ? source_width / 2 : 1;
    const u32 height = source_height > 1 ? source_height / 2 : 1;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            for (u32 channel = 0; channel < 4; ++channel) {
                u32 sum = 0;
                for (u32 dy = 0; dy < 2; ++dy) {
                    for (u32 dx = 0; dx < 2; ++dx) {
                        const u32 sx = ((x * 2) + dx) % source_width;
                        const u32 sy = ((y * 2) + dy) % source_height;
                        sum +=
                            source[(((static_cast<usize>(sy) * source_width) + sx) * 4) + channel];
                    }
                }
                out[(((static_cast<usize>(y) * width) + x) * 4) + channel] =
                    static_cast<u8>((sum + 2) / 4);
            }
        }
    }
}

/// The whole cooked chain, level 0 first and tightly packed — which is what
/// `MaterialTextureTable::upload` refuses to take any other size of. `flatten_to_average` writes
/// the DECLARED AVERAGE into every level instead, which is the control texture.
[[nodiscard]] Status build_chain(Array<u8>& chain, u32 mip_levels, bool flatten_to_average,
                                 u8 average[4]) noexcept {
    const u64 bytes = render::texture_mip_chain_byte_size(render::TextureFormat::Rgba8Unorm,
                                                          kExtent, kExtent, mip_levels);
    if (Status sized = chain.resize(static_cast<usize>(bytes)); !sized) {
        return sized;
    }
    Array<u8> level0(system_allocator(MemoryDomain::Assets));
    if (Status sized = level0.resize(static_cast<usize>(kTexels) * 4U); !sized) {
        return sized;
    }
    write_level0(level0);

    u64 totals[4] = {0, 0, 0, 0};
    for (u32 texel = 0; texel < kTexels; ++texel) {
        for (u32 channel = 0; channel < 4; ++channel) {
            totals[channel] += level0[(static_cast<usize>(texel) * 4U) + channel];
        }
    }
    for (u32 channel = 0; channel < 4; ++channel) {
        average[channel] = static_cast<u8>(totals[channel] / kTexels);
    }

    if (flatten_to_average) {
        for (usize index = 0; index < chain.size(); index += 4) {
            for (u32 channel = 0; channel < 4; ++channel) {
                chain[index + channel] = average[channel];
            }
        }
        return ok();
    }

    std::memcpy(chain.data(), level0.data(), level0.size());
    usize previous = 0;
    usize offset = level0.size();
    u32 width = kExtent;
    u32 height = kExtent;
    for (u32 level = 1; level < mip_levels; ++level) {
        downsample(chain.data() + previous, width, height, chain.data() + offset);
        previous = offset;
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
        offset += static_cast<usize>(width) * height * 4U;
    }
    return ok();
}

// --- Reading the pictures ----------------------------------------------------------------------

/// How two frames differ: the share of texels that are not identical, and the mean absolute
/// difference over the three colour channels. `render.material_binding`'s own two numbers, computed
/// the same way, so the device half of this rung and the frame half are comparable.
struct Difference {
    f64 share_differing = 0.0;
    f64 mean_absolute = 0.0;
};

[[nodiscard]] Difference compare(Span<const u32> a, Span<const u32> b) noexcept {
    Difference difference;
    if (a.size() != b.size() || a.empty()) {
        return difference;
    }
    u64 differing = 0;
    u64 total_absolute = 0;
    for (usize index = 0; index < a.size(); ++index) {
        if (a[index] != b[index]) {
            ++differing;
        }
        for (u32 channel = 0; channel < 3; ++channel) {
            const auto left = static_cast<i32>((a[index] >> (channel * 8U)) & 0xFFU);
            const auto right = static_cast<i32>((b[index] >> (channel * 8U)) & 0xFFU);
            total_absolute += static_cast<u64>(left > right ? left - right : right - left);
        }
    }
    difference.share_differing = static_cast<f64>(differing) / static_cast<f64>(a.size());
    difference.mean_absolute =
        static_cast<f64>(total_absolute) / (static_cast<f64>(a.size()) * 3.0);
    return difference;
}

/// The texels a surface was actually shaded into, so a difference can be read against the part of
/// the frame that could have differed rather than against the background. The clear is a dark
/// neutral, exactly as `render.pipeline` reads it.
[[nodiscard]] u32 shaded_texels(Span<const u32> texels) noexcept {
    u32 shaded = 0;
    for (const u32 texel : texels) {
        if ((texel & 0xFFU) > 24U || ((texel >> 8U) & 0xFFU) > 24U ||
            ((texel >> 16U) & 0xFFU) > 24U) {
            ++shaded;
        }
    }
    return shaded;
}

[[nodiscard]] Status copy_pixels(Array<u32>& out, Span<const u32> pixels) noexcept {
    if (Status sized = out.resize(pixels.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < pixels.size(); ++index) {
        out[index] = pixels[index];
    }
    return ok();
}

/// Diagnostic frames, written WHERE THE RUN OWNS rather than into the current directory.
///
/// A bare filename here lands in whatever directory the suite was invoked from, which for a person
/// running `just test-render` by hand is the repository root. That is not a cosmetic problem: it
/// left two tracked PNGs in the root of this repository, and it later BLOCKED A FALSIFIABILITY
/// PROOF — `falsify --mutate-the-tree` refuses to run in a dirty tree, because a mutation applied
/// to it "could not be told from what is already there". A test that cannot be run without dirtying
/// the tree is a test that cannot be used as evidence.
///
/// So the destination is CY_TEST_ARTEFACT_DIR when the harness sets it, else the build tree this
/// binary was configured into, and never the caller's working directory.
void save(const char* name, Span<const u32> texels) noexcept {
    render_test::Image image(allocator());
    if (!render_test::adopt(image, texels, kWidth, kHeight).has_value()) {
        return;
    }
    const char* directory = std::getenv("CY_TEST_ARTEFACT_DIR");
    if (directory == nullptr || *directory == '\0') {
        directory = CY_TEST_BINARY_DIR;
    }
    char path[1024];
    const int written = std::snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (written <= 0 || static_cast<usize>(written) >= sizeof(path)) {
        return;
    }
    if (render_test::write_png(path, image).has_value()) {
        std::fprintf(stderr, "wrote %s (%ux%u)\n", path, kWidth, kHeight);
    }
}

}  // namespace

CY_TEST_CASE("the forward path samples a material texture, and the substitution proves it") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    rhi::Device& device = fixture.device();
    Allocator& gpu = system_allocator(MemoryDomain::Gpu);

    // The table has to be reachable before anything else means anything. On the compatibility path
    // there is none, and a frame there shades its constants — which is a different claim this suite
    // is not written for, so it says so rather than pretending.
    if (device.descriptor_model() != rhi::DescriptorModel::Bindless) {
        std::fprintf(stderr,
                     "this device is on the compatibility path and has no global texture table; "
                     "the forward path shades material constants there\n");
        return;
    }

    // --- Two textures: a pattern, and that pattern's declared average ----------------------------
    render::RenderServer server(gpu);
    render::RenderServerConfig config;
    config.debug_primitive_capacity = 16;
    config.debug_label_capacity = 4;
    CY_REQUIRE(server.configure(config).has_value());
    CY_REQUIRE(server.initialize().has_value());

    render::TextureRecord description;
    description.name = Name::intern("forward material pattern");
    description.format = render::TextureFormat::Rgba8Unorm;
    description.usage_class = render::TextureUsageClass::Data;
    description.width = kExtent;
    description.height = kExtent;
    description.mip_levels = 0;  // the server fills in the whole chain
    Expected<render::TextureHandle, Error> pattern = server.create_texture(description);
    CY_REQUIRE(pattern.has_value());
    description.name = Name::intern("forward material average");
    Expected<render::TextureHandle, Error> flat = server.create_texture(description);
    CY_REQUIRE(flat.has_value());

    const render::TextureRecord* record = server.texture(*pattern);
    CY_REQUIRE(record != nullptr);

    Array<u8> pattern_pixels(gpu);
    Array<u8> flat_pixels(gpu);
    u8 average[4] = {0, 0, 0, 0};
    u8 same_average[4] = {0, 0, 0, 0};
    CY_REQUIRE(build_chain(pattern_pixels, record->mip_levels, false, average).has_value());
    CY_REQUIRE(build_chain(flat_pixels, record->mip_levels, true, same_average).has_value());

    MaterialTextureTable textures;
    rhi::SamplerDescription sampler;
    sampler.name = "forward material texture sampler";
    CY_REQUIRE(textures.initialize(device, gpu, sampler).has_value());

    const TextureUpload uploads[2] = {
        {*pattern, Span<const u8>(pattern_pixels.data(), pattern_pixels.size())},
        {*flat, Span<const u8>(flat_pixels.data(), flat_pixels.size())},
    };
    CY_REQUIRE(textures.upload(server, Span<const TextureUpload>(uploads, 2)).has_value());
    CY_CHECK_EQ(textures.resident(), usize{2});

    const rhi::BindlessIndex pattern_slot = textures.slot_of(*pattern);
    const rhi::BindlessIndex flat_slot = textures.slot_of(*flat);
    CY_REQUIRE(pattern_slot != rhi::kInvalidBindlessIndex);
    CY_REQUIRE(flat_slot != rhi::kInvalidBindlessIndex);
    CY_REQUIRE(pattern_slot != flat_slot);

    MaterialTextureSlot resident[2];
    CY_CHECK_EQ(textures.slots(Span<MaterialTextureSlot>(resident, 2)), usize{2});

    // --- The frame, three times ------------------------------------------------------------------
    FrameScene scene(allocator());
    const Status built = scene.build(device);
    if (!built) {
        std::fprintf(stderr, "forward material build failed: %s\n", built.error().message);
    }
    CY_REQUIRE(built.has_value());
    scene.set_read_back(true);

    // THE FRAME THIS ENGINE PRODUCED BEFORE TASK 3.7 — every texture slot unbound, every surface
    // shaded from four constants — rendered by today's code so that it is a measurement rather than
    // a memory.
    rendering::assembly::AssemblyReport report;
    CY_CHECK_EQ(scene.material_texture_offset(), rendering::pipeline::kNoMaterialTexture);
    CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
    Array<u32> constants(allocator());
    CY_REQUIRE(copy_pixels(constants, scene.pixels()).has_value());
    const u32 shaded = shaded_texels(constants.span());

    CY_REQUIRE(scene.bind_material_texture(pattern_slot, {resident, 2}).has_value());
    CY_CHECK(scene.material_texture_offset() != rendering::pipeline::kNoMaterialTexture);
    CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
    Array<u32> textured(allocator());
    CY_REQUIRE(copy_pixels(textured, scene.pixels()).has_value());

    // THE SUBSTITUTION. One slot index changes; nothing else in the frame does.
    CY_REQUIRE(scene.bind_material_texture(flat_slot, {resident, 2}).has_value());
    CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
    Array<u32> averaged(allocator());
    CY_REQUIRE(copy_pixels(averaged, scene.pixels()).has_value());

    save("forward-material-constants.png", constants.span());
    save("forward-material-textured.png", textured.span());
    save("forward-material-averaged.png", averaged.span());

    // A render that failed leaves an empty readback behind, and doctest is compiled with exceptions
    // off in this build so `CY_REQUIRE` does not unwind. Stop before reading off the end; the
    // failure is already reported.
    if (textured.size() != averaged.size() || textured.size() != constants.size() ||
        textured.empty()) {
        return;
    }

    const Difference substitution = compare(textured.span(), averaged.span());
    const Difference against_constants = compare(textured.span(), constants.span());
    std::fprintf(stderr,
                 "forward material texture: %u of %u texels shaded; against the declared average "
                 "%.2f%% of texels differ at mean |delta| %.3f/255; against the constant-shaded "
                 "frame %.2f%% differ at mean |delta| %.3f/255\n",
                 shaded, kWidth * kHeight, substitution.share_differing * 100.0,
                 substitution.mean_absolute, against_constants.share_differing * 100.0,
                 against_constants.mean_absolute);

    // THE CLAIM. The thresholds are read against the SHADED part of the frame rather than against
    // the whole of it: most of a 480x270 frame is background that no material touches, and a
    // threshold written against the frame would be a threshold written against how big the cubes
    // are. A twentieth of the shaded texels differing is far under what the scene produces and far
    // over what any of the failure modes produce, every one of which produces zero.
    const f64 shaded_share = static_cast<f64>(shaded) / static_cast<f64>(kWidth * kHeight);
    CY_CHECK(shaded > 2000U);
    CY_CHECK(substitution.share_differing > shaded_share * 0.05);
    CY_CHECK(substitution.mean_absolute > 1.0);

    // AND IT IS THE TEXTURE THAT MADE THE DIFFERENCE, not the act of declaring a texture slot. The
    // averaged frame is a textured frame too — it samples a real image out of the same table
    // through the same set — so a frame that sampled its albedo from somewhere else entirely would
    // pass the comparison above and fail this one.
    CY_CHECK(against_constants.share_differing > shaded_share * 0.05);
    CY_CHECK(against_constants.mean_absolute > 1.0);

    // A frame that renders and trips the validator is not a frame that works: M3's recycled
    // descriptor reached an artefact while the sample still printed "exit 0 (clean)".
    CY_CHECK_EQ(fixture.validation_errors(), 0U);

    scene.release();
    textures.shutdown();
    server.shutdown();
}
