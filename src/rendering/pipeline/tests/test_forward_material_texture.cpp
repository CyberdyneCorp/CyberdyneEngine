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

/// Whether a surface was shaded into this texel. The clear is a dark neutral, exactly as
/// `render.pipeline` reads it.
[[nodiscard]] bool is_shaded(u32 texel) noexcept {
    return (texel & 0xFFU) > 24U || ((texel >> 8U) & 0xFFU) > 24U || ((texel >> 16U) & 0xFFU) > 24U;
}

/// The texels a surface was actually shaded into, so a difference can be read against the part of
/// the frame that could have differed rather than against the background.
[[nodiscard]] u32 shaded_texels(Span<const u32> texels) noexcept {
    u32 shaded = 0;
    for (const u32 texel : texels) {
        if (is_shaded(texel)) {
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

// --- The mip chain, M11.c task 3.8 -------------------------------------------------------------

/// Big enough that every face of `FrameScene`'s cubes MINIFIES it: a face covers a few dozen texels
/// of a 480x270 frame and this is 256 texels across, so the gradient asks for level 2 or deeper.
constexpr u32 kMipExtent = 256;

/// A one-texel checker, black and white in every channel. It is the pattern with the most to lose
/// to aliasing: level 0 is nothing but its highest frequency, and every level above it averages to
/// the same mid grey. A frame that reads the chain shades a minified face grey; a frame that reads
/// only level 0 shades it with whatever moire the sample positions happen to land on.
void write_fine_checker(u8* pixels) noexcept {
    for (u32 y = 0; y < kMipExtent; ++y) {
        for (u32 x = 0; x < kMipExtent; ++x) {
            const u8 value = (((x + y) & 1U) != 0U) ? u8{255} : u8{0};
            const usize base = ((static_cast<usize>(y) * kMipExtent) + x) * 4U;
            pixels[base + 0] = value;
            pixels[base + 1] = value;
            pixels[base + 2] = value;
            pixels[base + 3] = 255U;
        }
    }
}

/// `mip_levels` levels of the fine checker, level 0 first and tightly packed, each level the box
/// filter of the one below it — what a cooker writes. With `mip_levels == 1` it is level 0 alone.
[[nodiscard]] Status build_checker_chain(Array<u8>& chain, u32 mip_levels) noexcept {
    const u64 bytes = render::texture_mip_chain_byte_size(render::TextureFormat::Rgba8Unorm,
                                                          kMipExtent, kMipExtent, mip_levels);
    if (Status sized = chain.resize(static_cast<usize>(bytes)); !sized) {
        return sized;
    }
    write_fine_checker(chain.data());
    usize previous = 0;
    usize offset = static_cast<usize>(kMipExtent) * kMipExtent * 4U;
    u32 width = kMipExtent;
    u32 height = kMipExtent;
    for (u32 level = 1; level < mip_levels; ++level) {
        downsample(chain.data() + previous, width, height, chain.data() + offset);
        previous = offset;
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
        offset += static_cast<usize>(width) * height * 4U;
    }
    return ok();
}

[[nodiscard]] u32 channel_distance(u32 a, u32 b) noexcept {
    u32 total = 0;
    for (u32 channel = 0; channel < 3; ++channel) {
        const auto left = static_cast<i32>((a >> (channel * 8U)) & 0xFFU);
        const auto right = static_cast<i32>((b >> (channel * 8U)) & 0xFFU);
        total += static_cast<u32>(left > right ? left - right : right - left);
    }
    return total;
}

/// How much a picture moves from one texel to its right-hand and lower neighbours, per channel,
/// over pairs that are both shaded — the energy of its highest frequency, which is where aliasing
/// lives. A minified one-texel checker sampled at level 0 is moire; sampled through its chain it is
/// a flat grey.
[[nodiscard]] f64 high_frequency(Span<const u32> texels) noexcept {
    u64 total = 0;
    u64 pairs = 0;
    for (u32 y = 0; y + 1 < kHeight; ++y) {
        for (u32 x = 0; x + 1 < kWidth; ++x) {
            const u32 here = texels[(static_cast<usize>(y) * kWidth) + x];
            const u32 right = texels[(static_cast<usize>(y) * kWidth) + x + 1];
            const u32 below = texels[(static_cast<usize>(y + 1) * kWidth) + x];
            if (!is_shaded(here)) {
                continue;
            }
            if (is_shaded(right)) {
                total += channel_distance(here, right);
                ++pairs;
            }
            if (is_shaded(below)) {
                total += channel_distance(here, below);
                ++pairs;
            }
        }
    }
    return pairs == 0 ? 0.0 : static_cast<f64>(total) / (static_cast<f64>(pairs) * 3.0);
}

/// The FIRST frame of a freshly built scene with every material's base colour pointed at `slot`.
///
/// Fresh on purpose. The frame is temporally antialiased, and a static camera under a jittered
/// projection is SUPERSAMPLING: every frame it accumulates moves an aliased level-0 picture towards
/// the same average the mip chain holds, so the longer one scene runs the less there is to tell the
/// two textures apart by — and the more one frame differs from the next on jitter alone (measured
/// on one scene rendered three times: 15.41% of texels moving at mean |delta| 2.617/255 between two
/// frames of the SAME texture, against 2.636/255 between the two textures). A first frame has no
/// history and the first jitter offset, so two of them with the same texture are the same picture.
[[nodiscard]] Status render_first_frame(rhi::Device& device, rhi::BindlessIndex slot,
                                        Span<const MaterialTextureSlot> resident,
                                        Array<u32>& out) noexcept {
    FrameScene scene(allocator());
    if (Status built = scene.build(device); !built) {
        return built;
    }
    scene.set_read_back(true);
    if (Status bound = scene.bind_material_texture(slot, resident); !bound) {
        return bound;
    }
    rendering::assembly::AssemblyReport report;
    if (Status rendered = scene.render(RecordMode::Callbacks, report); !rendered) {
        return rendered;
    }
    return copy_pixels(out, scene.pixels());
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

    textures.shutdown();
    server.shutdown();
}

// THE SHOT SAMPLES ONE MIP LEVEL, OR IT READS THE CHAIN. M11.c task 3.8.
//
// Measured before this case existed: the same frame with eight of the importer's nine cooked mip
// levels never uploaded was BYTE-IDENTICAL — mean |delta| 0.000/255, 0.00% of texels — because the
// sample was `cyMaterialSampleTextureLevel(..., 0.0)`, the explicit form, and the chain below level
// 0 was unreachable. `surfaceOf()` now takes the implicit form, `cyMaterialSampleTexture`, and
// nothing measured that it does: substituting the explicit form back leaves the case above green.
//
// So the case IS that measurement, repeated with the answer required to change. The same scene is
// rendered with the same level 0 twice over — once as a full cooked chain and once as a texture
// that HAS NO LEVEL BUT 0 — and the two frames must differ. A frame that samples at level 0 cannot
// tell them apart, whatever else it does right. Each frame is the first of a freshly built scene
// (`render_first_frame` says why), and the chain is rendered twice so that "differ" is read against
// what two renders of an identical scene move by, which is measured as nothing.
//
// Proven red two ways, both measured in build/m11c-mip on an RTX 5060, both restored and
// md5-verified. Substituting `cyMaterialSampleTextureLevel(albedoSlot, uv, 0.0)` into `surfaceOf()`
// and regenerating `frame_spirv.h`: the two frames become BYTE-IDENTICAL again, 0.00% at 0.000/255,
// and five assertions go red — while the case above stays green, which is why this one exists.
// Deleting `info.maxLod = desc.max_lod;` from the Vulkan sampler, the declared mutation: the same
// five, the same 0.00%.
CY_TEST_CASE("the forward path reads the cooked mip chain, not level 0 alone") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    rhi::Device& device = fixture.device();
    Allocator& gpu = system_allocator(MemoryDomain::Gpu);
    if (device.descriptor_model() != rhi::DescriptorModel::Bindless) {
        std::fprintf(stderr,
                     "this device is on the compatibility path and has no global texture table; "
                     "the forward path shades material constants there\n");
        return;
    }

    render::RenderServer server(gpu);
    render::RenderServerConfig config;
    config.debug_primitive_capacity = 16;
    config.debug_label_capacity = 4;
    CY_REQUIRE(server.configure(config).has_value());
    CY_REQUIRE(server.initialize().has_value());

    render::TextureRecord description;
    description.name = Name::intern("forward mip chain");
    description.format = render::TextureFormat::Rgba8Unorm;
    description.usage_class = render::TextureUsageClass::Data;
    description.width = kMipExtent;
    description.height = kMipExtent;
    description.mip_levels = 0;  // the server fills in the whole chain
    Expected<render::TextureHandle, Error> chain = server.create_texture(description);
    CY_REQUIRE(chain.has_value());
    description.name = Name::intern("forward mip level zero only");
    description.mip_levels = 1;
    Expected<render::TextureHandle, Error> level_zero = server.create_texture(description);
    CY_REQUIRE(level_zero.has_value());

    const render::TextureRecord* chain_record = server.texture(*chain);
    const render::TextureRecord* level_zero_record = server.texture(*level_zero);
    CY_REQUIRE(chain_record != nullptr);
    CY_REQUIRE(level_zero_record != nullptr);
    CY_CHECK_EQ(u32{chain_record->mip_levels}, 9U);
    CY_CHECK_EQ(u32{level_zero_record->mip_levels}, 1U);

    Array<u8> chain_pixels(gpu);
    Array<u8> level_zero_pixels(gpu);
    CY_REQUIRE(build_checker_chain(chain_pixels, chain_record->mip_levels).has_value());
    CY_REQUIRE(build_checker_chain(level_zero_pixels, 1).has_value());
    // THE SAME LEVEL 0, byte for byte: the only thing the two textures disagree about is whether
    // anything lies beneath it.
    CY_REQUIRE(level_zero_pixels.size() <= chain_pixels.size());
    CY_CHECK(std::memcmp(chain_pixels.data(), level_zero_pixels.data(), level_zero_pixels.size()) ==
             0);

    MaterialTextureTable textures;
    rhi::SamplerDescription sampler;
    sampler.name = "forward mip chain sampler";
    CY_REQUIRE(textures.initialize(device, gpu, sampler).has_value());
    const TextureUpload uploads[2] = {
        {*chain, Span<const u8>(chain_pixels.data(), chain_pixels.size())},
        {*level_zero, Span<const u8>(level_zero_pixels.data(), level_zero_pixels.size())},
    };
    CY_REQUIRE(textures.upload(server, Span<const TextureUpload>(uploads, 2)).has_value());
    const rhi::BindlessIndex chain_slot = textures.slot_of(*chain);
    const rhi::BindlessIndex level_zero_slot = textures.slot_of(*level_zero);
    CY_REQUIRE(chain_slot != rhi::kInvalidBindlessIndex);
    CY_REQUIRE(level_zero_slot != rhi::kInvalidBindlessIndex);
    MaterialTextureSlot resident[2];
    CY_CHECK_EQ(textures.slots(Span<MaterialTextureSlot>(resident, 2)), usize{2});
    const Span<const MaterialTextureSlot> slots(resident, 2);

    Array<u32> chain_first(allocator());
    Array<u32> chain_again(allocator());
    Array<u32> level_zero_only(allocator());
    CY_REQUIRE(render_first_frame(device, chain_slot, slots, chain_first).has_value());
    CY_REQUIRE(render_first_frame(device, chain_slot, slots, chain_again).has_value());
    CY_REQUIRE(render_first_frame(device, level_zero_slot, slots, level_zero_only).has_value());

    save("forward-mip-chain.png", chain_again.span());
    save("forward-mip-level-zero.png", level_zero_only.span());

    if (chain_first.empty() || chain_first.size() != chain_again.size() ||
        chain_again.size() != level_zero_only.size()) {
        return;
    }

    const u32 shaded = shaded_texels(chain_again.span());
    const Difference noise = compare(chain_first.span(), chain_again.span());
    const Difference mips = compare(chain_again.span(), level_zero_only.span());
    std::fprintf(stderr,
                 "forward mip chain: %u of %u texels shaded; frame to frame with the same chain "
                 "%.2f%% of texels differ at mean |delta| %.3f/255; full chain against level 0 "
                 "alone %.2f%% differ at mean |delta| %.3f/255\n",
                 shaded, kWidth * kHeight, noise.share_differing * 100.0, noise.mean_absolute,
                 mips.share_differing * 100.0, mips.mean_absolute);

    // THE CLAIM, read against the shaded part of the frame and against the frame's own
    // frame-to-frame movement. A sample taken at level 0 makes the two textures the same texture,
    // and `mips` falls to `noise` — which is the 0.00% this task began from.
    const f64 shaded_share = static_cast<f64>(shaded) / static_cast<f64>(kWidth * kHeight);
    CY_CHECK(shaded > 2000U);
    CY_CHECK(mips.share_differing > shaded_share * 0.05);
    CY_CHECK(mips.share_differing > noise.share_differing * 4.0);
    CY_CHECK(mips.mean_absolute > 1.0);
    CY_CHECK(mips.mean_absolute > noise.mean_absolute * 10.0);

    // AND THE DIFFERENCE IS ALIASING, in the direction the task names. Two frames can differ
    // because the chain is wrong — a level uploaded to the wrong offset, a chain that is not the
    // box filter of level 0 — and neither of those makes the level-0 picture the NOISIER one. A
    // minified one-texel checker read through its chain is grey; read at level 0 it is moire.
    const f64 chain_energy = high_frequency(chain_again.span());
    const f64 level_zero_energy = high_frequency(level_zero_only.span());
    std::fprintf(stderr,
                 "forward mip chain: neighbour-to-neighbour |delta| %.3f/255 through the chain, "
                 "%.3f/255 at level 0 alone\n",
                 chain_energy, level_zero_energy);
    CY_CHECK(level_zero_energy > chain_energy * 1.5);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);

    textures.shutdown();
    server.shutdown();
}
