// A material program samples a texture the engine uploaded, through the device's OWN global table.
// M11.c task 3.7.
//
// ================================================================================================
// WHAT WAS WRONG, AND WHY THIS SUITE IS A DEVICE SUITE
// ================================================================================================
//
// M11.c's spike measured the bind junction ABSENT and said what that meant:
//
// > The engine's own global bindless table is WRITE-ONLY. `VulkanDevice::create_bindless_table`
// > builds `bindless_layout_` and `bindless_set_`; `bind_texture_globally` writes descriptors into
// > it [...] It is never put in a pipeline layout and `bindless_set_` is never bound in a command
// > buffer, so every `BindlessIndex` the device hands out names a descriptor no shader can reach.
//
// Every part of that is answerable ONLY on a device. A test that checked the handles were non-null
// would pass on a table whose descriptors the driver never sees; a test that checked the pipeline
// layout was created would pass on a table bound at the wrong set. The one question that cannot be
// faked is whether a shader's sample came back with the texture's own texels, and the only thing
// that can answer it is a GPU.
//
// ================================================================================================
// AND IT IS A DIFFERENCE, NOT A PICTURE
// ================================================================================================
//
// The same surface is drawn twice: once sampling the pattern, once sampling a texture that is
// nothing but that pattern's DECLARED AVERAGE. That control is the spike's own — "Textures bound
// against the declared average: mean |delta| 34.072/255, 65.38% of texels" — and it exists because
// the average frame is an entirely plausible picture. A binding that silently sampled nothing, a
// slot index that always resolved to zero, a table bound at the wrong set with
// `partially_bound` hiding it: each of those produces two frames that AGREE, and agreement is what
// this suite fails on.
//
// The third case is the strongest and the cheapest: the frame that samples the pattern is compared
// TEXEL FOR TEXEL against the bytes the test uploaded. The probe draws the texture at its own size
// with its whole [0,1] square across the target, so every pixel centre lands on a texel centre and
// a correct sample is exact. That is a claim about which texture arrived, not merely that one did.

#include <cy/test/test.h>

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/pipeline/material_textures.h>
#include <cy/servers/render/server.h>

#include "device.h"
#include "shaders/material_probe_spirv.h"

#include <cstdio>

namespace cy::render_test {
namespace {

using rendering::PassContext;
using rendering::ResourceId;
using rendering::pipeline::MaterialTextureTable;
using rendering::pipeline::TextureUpload;
using rhi::Access;
using rhi::QueueKind;

constexpr u32 kExtent = 64;
constexpr u32 kTexels = kExtent * kExtent;

/// One vertex of the probe's triangle: a clip-space position and the texture coordinate at it.
///
/// THREE VERTICES COVERING THE TARGET, and v = 0 is paired with clip +1 because the engine's
/// viewport has a NEGATIVE height — `VulkanCommandBuffer::set_viewport` flips it so that clip +Y is
/// up — and a texture's first row is its top one. Pairing them the other way round samples the
/// texture upside down, which is a perfectly plausible picture and would pass every difference this
/// suite measures.
struct ProbeVertex {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 u = 0.0F;
    f32 v = 0.0F;
};

constexpr ProbeVertex kProbeTriangle[3] = {
    {-1.0F, 1.0F, 0.0F, 0.0F},   // top left of the visible square
    {3.0F, 1.0F, 2.0F, 0.0F},    // off to the right, so one triangle covers it
    {-1.0F, -3.0F, 0.0F, 2.0F},  // and off below
};

/// `material_probe.slang`'s push block: which slot of the global table this draw reads.
struct ProbePush {
    u32 slot = 0;
    u32 pad0 = 0;
    u32 pad1 = 0;
    u32 pad2 = 0;
};

// --- The content ------------------------------------------------------------------------------

/// A pattern with structure at every scale: a coarse checker, a fine checker on top of it, and a
/// diagonal ramp, so no downsample of it is flat and no channel is constant. A texture that was
/// nearly its own average would make the control below pass for the wrong reason.
void write_level0(Array<u8>& pixels) noexcept {
    for (u32 y = 0; y < kExtent; ++y) {
        for (u32 x = 0; x < kExtent; ++x) {
            const bool coarse = (((x / 16U) + (y / 16U)) & 1U) != 0U;
            const bool fine = (((x / 4U) + (y / 4U)) & 1U) != 0U;
            const usize base = ((static_cast<usize>(y) * kExtent) + x) * 4U;
            pixels[base + 0] = static_cast<u8>(coarse ? 230U : 25U);
            pixels[base + 1] = static_cast<u8>(fine ? 200U : 40U);
            pixels[base + 2] = static_cast<u8>((x * 4U) & 0xFFU);
            pixels[base + 3] = 255U;
        }
    }
}

/// Box-filter the level above into `out`, which is the chain the cooker would have produced.
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

/// A full mip chain for a 64x64 RGBA8 texture, level 0 first, tightly packed — exactly what
/// `texture_mip_chain_byte_size` says it costs, because `MaterialTextureTable::upload` refuses a
/// payload that is any other size.
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

// --- The frame --------------------------------------------------------------------------------

struct PassState {
    rendering::GraphExecutor* executor = nullptr;
    rhi::GraphicsPipelineHandle pipeline;
    rhi::PipelineLayoutHandle layout;
    rhi::DescriptorSetHandle table;
    rhi::BufferHandle vertices;
    rhi::BufferHandle readback;
    ResourceId color = rendering::kInvalidResource;
    ProbePush push{};
};

void record_draw(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<PassState*>(user);
    rhi::RenderAttachment color;
    color.view = state->executor->view(state->color);
    color.load = rhi::LoadOp::Clear;
    color.store = rhi::StoreOp::Store;
    color.clear.color[3] = 1.0F;

    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, kExtent, kExtent};
    info.color_attachments = Span<const rhi::RenderAttachment>(&color, 1);

    context.commands->begin_rendering(info);
    context.commands->set_viewport(rhi::Viewport{0.0F, 0.0F, static_cast<f32>(kExtent),
                                                 static_cast<f32>(kExtent), 0.0F, 1.0F});
    context.commands->set_scissor(rhi::Rect2D{0, 0, kExtent, kExtent});
    context.commands->bind_graphics_pipeline(state->pipeline);
    // THE LINE THE WHOLE SUITE IS ABOUT. `state->table` is `rhi::Device::global_texture_table()` —
    // the device's own set, allocated in `create_bindless_table()` — bound at
    // `rhi::kGlobalTableSet`, which is where `cy/material.slang` declares the array the fragment
    // stage indexes. Before M11.c this call did not exist anywhere in the tree.
    context.commands->bind_descriptor_sets(state->layout, rhi::kGlobalTableSet,
                                           Span<const rhi::DescriptorSetHandle>(&state->table, 1));
    context.commands->push_constants(
        state->layout, rhi::ShaderStage::Fragment, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&state->push), sizeof(ProbePush)));
    const u64 offset = 0;
    context.commands->bind_vertex_buffers(0, Span<const rhi::BufferHandle>(&state->vertices, 1),
                                          Span<const u64>(&offset, 1));
    context.commands->draw(3, 1, 0, 0);
    context.commands->end_rendering();
}

void record_readback(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<PassState*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{kExtent, kExtent, 1};
    context.commands->copy_texture_to_buffer(state->executor->texture(state->color),
                                             state->readback,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

/// Everything the probe needs beyond the device: the pipeline whose set 0 IS the device's table.
class ProbePipeline {
public:
    [[nodiscard]] Status create(rhi::Device& device) noexcept {
        device_ = &device;
        rhi::ShaderModuleDescription vertex;
        vertex.name = "material probe vertex";
        vertex.stage = rhi::ShaderStage::Vertex;
        vertex.spirv = Span<const u32>(kMaterialProbeVertexSpirv,
                                       sizeof(kMaterialProbeVertexSpirv) / sizeof(u32));
        Expected<rhi::ShaderModuleHandle, Error> vertex_module =
            device.create_shader_module(vertex);
        if (!vertex_module) {
            return make_unexpected(vertex_module.error());
        }
        vertex_ = *vertex_module;

        rhi::ShaderModuleDescription fragment;
        fragment.name = "material probe fragment";
        fragment.stage = rhi::ShaderStage::Fragment;
        fragment.spirv = Span<const u32>(kMaterialProbeFragmentSpirv,
                                         sizeof(kMaterialProbeFragmentSpirv) / sizeof(u32));
        Expected<rhi::ShaderModuleHandle, Error> fragment_module =
            device.create_shader_module(fragment);
        if (!fragment_module) {
            return make_unexpected(fragment_module.error());
        }
        fragment_ = *fragment_module;

        // ONE SET, AND IT IS NOT THIS TEST'S. The probe declares no descriptor of its own — its
        // only other input is a push constant — so if the sample comes back with the texture's
        // texels in it, it came through the device's table and through nothing else.
        const rhi::DescriptorSetLayoutHandle sets[1] = {device.global_texture_table_layout()};
        const rhi::PushConstantRange range{rhi::ShaderStage::Fragment, 0, sizeof(ProbePush)};
        rhi::PipelineLayoutDescription layout;
        layout.name = "material probe layout";
        layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(sets, 1);
        layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
        Expected<rhi::PipelineLayoutHandle, Error> layout_handle =
            device.create_pipeline_layout(layout);
        if (!layout_handle) {
            return make_unexpected(layout_handle.error());
        }
        layout_ = *layout_handle;

        rhi::ColorAttachmentState color;
        color.format = rhi::Format::Rgba8Unorm;
        const rhi::VertexBinding binding{0, sizeof(ProbeVertex), rhi::VertexInputRate::PerVertex};
        const rhi::VertexAttribute attributes[2] = {
            {0, 0, rhi::Format::Rg32Sfloat, 0},
            {1, 0, rhi::Format::Rg32Sfloat, sizeof(f32) * 2},
        };
        rhi::GraphicsPipelineDescription pipeline;
        pipeline.name = "material probe";
        pipeline.layout = layout_;
        pipeline.vertex_shader = vertex_;
        pipeline.fragment_shader = fragment_;
        pipeline.vertex_bindings = Span<const rhi::VertexBinding>(&binding, 1);
        pipeline.vertex_attributes = Span<const rhi::VertexAttribute>(attributes, 2);
        pipeline.color_attachments = Span<const rhi::ColorAttachmentState>(&color, 1);
        pipeline.rasterisation.cull_mode = rhi::CullMode::None;
        pipeline.depth_stencil.depth_test_enable = false;
        pipeline.depth_stencil.depth_write_enable = false;
        Expected<rhi::GraphicsPipelineHandle, Error> handle =
            device.create_graphics_pipeline(pipeline);
        if (!handle) {
            return make_unexpected(handle.error());
        }
        pipeline_ = *handle;

        rhi::BufferDescription vertices;
        vertices.name = "material probe vertices";
        vertices.size = sizeof(kProbeTriangle);
        vertices.usage = rhi::BufferUsage::Vertex;
        vertices.memory = rhi::MemoryUse::Upload;
        Expected<rhi::BufferHandle, Error> vertex_buffer = device.create_buffer(vertices);
        if (!vertex_buffer) {
            return make_unexpected(vertex_buffer.error());
        }
        vertices_ = *vertex_buffer;
        auto* mapped = static_cast<ProbeVertex*>(device.buffer_mapped_pointer(vertices_));
        if (mapped == nullptr) {
            return fail(ErrorCode::Internal, "the probe vertex buffer is not mapped");
        }
        for (u32 index = 0; index < 3; ++index) {
            mapped[index] = kProbeTriangle[index];
        }

        rhi::BufferDescription readback;
        readback.name = "material probe readback";
        readback.size = static_cast<u64>(kTexels) * sizeof(u32);
        readback.usage = rhi::BufferUsage::TransferDestination;
        readback.memory = rhi::MemoryUse::Readback;
        Expected<rhi::BufferHandle, Error> buffer = device.create_buffer(readback);
        if (!buffer) {
            return make_unexpected(buffer.error());
        }
        readback_ = *buffer;
        return ok();
    }

    ~ProbePipeline() {
        if (device_ == nullptr) {
            return;
        }
        (void)device_->wait_idle();
        device_->destroy_buffer(readback_);
        device_->destroy_buffer(vertices_);
        device_->destroy_graphics_pipeline(pipeline_);
        device_->destroy_pipeline_layout(layout_);
        device_->destroy_shader_module(fragment_);
        device_->destroy_shader_module(vertex_);
    }

    ProbePipeline() noexcept = default;
    ProbePipeline(const ProbePipeline&) = delete;
    ProbePipeline& operator=(const ProbePipeline&) = delete;

    /// Draw the quad sampling `slot` and copy the target back. `out` is Rgba8Unorm texels, row
    /// major from the top left.
    [[nodiscard]] Status render(Allocator& allocator, rhi::DescriptorSetHandle table, u32 slot,
                                Array<u32>& out) noexcept {
        rhi::Device& device = *device_;
        if (Expected<u32, Error> began = device.begin_frame(); !began) {
            return make_unexpected(began.error());
        }
        rendering::RenderGraph graph(allocator);
        rendering::GraphExecutor executor(allocator, device);

        rendering::TextureRequest color_request;
        color_request.name = "material probe colour";
        color_request.format = rhi::Format::Rgba8Unorm;
        color_request.width = kExtent;
        color_request.height = kExtent;
        const ResourceId color = graph.create_texture(color_request);

        rendering::BufferRequest readback_request;
        readback_request.name = "material probe readback";
        readback_request.size = static_cast<u64>(kTexels) * sizeof(u32);
        readback_request.extra_usage = rhi::BufferUsage::TransferDestination;
        const ResourceId color_out = graph.import_buffer(readback_request, readback_);

        PassState state;
        state.executor = &executor;
        state.pipeline = pipeline_;
        state.layout = layout_;
        state.table = table;
        state.vertices = vertices_;
        state.readback = readback_;
        state.color = color;
        state.push.slot = slot;

        graph.add_pass("material probe draw", QueueKind::Graphics)
            .write(color, Access::ColorAttachmentWrite)
            .record(&record_draw, &state);
        graph.add_pass("material probe readback", QueueKind::Graphics)
            .read(color, Access::TransferRead)
            .write(color_out, Access::TransferWrite)
            .record(&record_readback, &state);
        graph.add_pass("material probe host", QueueKind::Graphics)
            .read(color_out, Access::HostRead)
            .side_effect();
        if (Status declared = graph.status(); !declared) {
            return declared;
        }
        Expected<rendering::ExecutionResult, Error> result =
            executor.execute(graph, rendering::CompileOptions{}, rendering::ExecuteOptions{});
        if (!result) {
            return make_unexpected(result.error());
        }
        if (Status idle = device.wait_idle(); !idle) {
            return idle;
        }
        if (Status ended = device.end_frame(); !ended) {
            return ended;
        }
        if (Status sized = out.resize(kTexels); !sized) {
            return sized;
        }
        const auto* texels = static_cast<const u32*>(device.buffer_mapped_pointer(readback_));
        if (texels == nullptr) {
            return fail(ErrorCode::Internal, "the probe readback buffer is not mapped");
        }
        for (u32 index = 0; index < kTexels; ++index) {
            out[index] = texels[index];
        }
        executor.release();
        return ok();
    }

private:
    rhi::Device* device_ = nullptr;
    rhi::ShaderModuleHandle vertex_;
    rhi::ShaderModuleHandle fragment_;
    rhi::PipelineLayoutHandle layout_;
    rhi::GraphicsPipelineHandle pipeline_;
    rhi::BufferHandle readback_;
    rhi::BufferHandle vertices_;
};

/// How two frames differ: the share of texels that are not identical, and the mean absolute
/// difference over every channel. The spike's own two numbers.
struct Difference {
    f64 share_differing = 0.0;
    f64 mean_absolute = 0.0;
};

[[nodiscard]] Difference compare(const Array<u32>& a, const Array<u32>& b) noexcept {
    u64 differing = 0;
    u64 total_absolute = 0;
    for (u32 index = 0; index < kTexels; ++index) {
        if (a[index] != b[index]) {
            ++differing;
        }
        for (u32 channel = 0; channel < 4; ++channel) {
            const auto left = static_cast<i32>((a[index] >> (channel * 8U)) & 0xFFU);
            const auto right = static_cast<i32>((b[index] >> (channel * 8U)) & 0xFFU);
            total_absolute += static_cast<u64>(left > right ? left - right : right - left);
        }
    }
    Difference difference;
    difference.share_differing = static_cast<f64>(differing) / kTexels;
    // Over the three colour channels: alpha is 255 in both and averaging it in would dilute every
    // number by a quarter for no information.
    difference.mean_absolute = static_cast<f64>(total_absolute) / (kTexels * 3.0);
    return difference;
}

}  // namespace

TEST_SUITE("render.material_binding") {
    TEST_CASE("a material program samples a texture through the device's global table") {
        DeviceFixture fixture("vulkan", "cy_test_render_material_binding");
        if (!fixture.is(rhi::BackendKind::Vulkan)) {
            fixture.report_skip();
            return;
        }
        rhi::Device& device = fixture.device();
        Allocator& allocator = system_allocator(MemoryDomain::Gpu);

        // The table exists and is reachable. Both were true of `bindless_set_` before M11.c only in
        // the sense that the Vulkan objects existed: there were no handles, so nothing above the
        // backend could name either one.
        REQUIRE(device.descriptor_model() == rhi::DescriptorModel::Bindless);
        REQUIRE_FALSE(device.global_texture_table_layout().is_null());
        REQUIRE_FALSE(device.global_texture_table().is_null());

        render::RenderServer server(allocator);
        render::RenderServerConfig config;
        config.debug_primitive_capacity = 16;
        config.debug_label_capacity = 4;
        REQUIRE(server.configure(config));
        REQUIRE(server.initialize());

        render::TextureRecord description;
        description.name = Name::intern("material probe pattern");
        description.format = render::TextureFormat::Rgba8Unorm;
        description.usage_class = render::TextureUsageClass::Data;
        description.width = kExtent;
        description.height = kExtent;
        description.mip_levels = 0;  // the server fills in the whole chain
        Expected<render::TextureHandle, Error> pattern = server.create_texture(description);
        REQUIRE(pattern.has_value());
        description.name = Name::intern("material probe average");
        Expected<render::TextureHandle, Error> flat = server.create_texture(description);
        REQUIRE(flat.has_value());

        const render::TextureRecord* pattern_record = server.texture(*pattern);
        REQUIRE(pattern_record != nullptr);
        const u32 mip_levels = pattern_record->mip_levels;
        CHECK(mip_levels == 7);

        Array<u8> pattern_pixels(allocator);
        Array<u8> flat_pixels(allocator);
        u8 average[4] = {0, 0, 0, 0};
        u8 same_average[4] = {0, 0, 0, 0};
        REQUIRE(build_chain(pattern_pixels, mip_levels, true, average));
        REQUIRE(build_chain(flat_pixels, mip_levels, true, same_average));

        // --- Upload, which is the path that did not exist under src/ -------------------------
        MaterialTextureTable table;
        rhi::SamplerDescription sampler;
        sampler.name = "material probe sampler";
        REQUIRE(table.initialize(device, allocator, sampler));

        const TextureUpload uploads[2] = {
            {*pattern, Span<const u8>(pattern_pixels.data(), pattern_pixels.size())},
            {*flat, Span<const u8>(flat_pixels.data(), flat_pixels.size())},
        };
        REQUIRE(table.upload(server, Span<const TextureUpload>(uploads, 2)));
        CHECK(table.resident() == 2);
        CHECK(table.resident_bytes() == pattern_record->bytes * 2);

        const rhi::BindlessIndex pattern_slot = table.slot_of(*pattern);
        const rhi::BindlessIndex flat_slot = table.slot_of(*flat);
        REQUIRE(pattern_slot != rhi::kInvalidBindlessIndex);
        REQUIRE(flat_slot != rhi::kInvalidBindlessIndex);
        REQUIRE(pattern_slot != flat_slot);
        // The table the module reports is the DEVICE's, not one of its own.
        CHECK(table.set() == device.global_texture_table());
        CHECK(table.layout() == device.global_texture_table_layout());

        // --- Two frames -----------------------------------------------------------------------
        ProbePipeline probe;
        REQUIRE(probe.create(device));
        Array<u32> textured(allocator);
        Array<u32> averaged(allocator);
        REQUIRE(probe.render(allocator, device.global_texture_table(), pattern_slot, textured));
        REQUIRE(probe.render(allocator, device.global_texture_table(), flat_slot, averaged));

        // `REQUIRE` does not unwind in this build — doctest is compiled with exceptions off — so a
        // render that failed leaves an empty readback that the comparison below would walk off the
        // end of. Stop here instead: the failure is already reported.
        if (textured.size() != kTexels || averaged.size() != kTexels) {
            return;
        }

        const Difference difference = compare(textured, averaged);
        std::fprintf(stderr,
                     "material binding: %.2f%% of texels differ, mean |delta| %.3f/255 "
                     "against the declared average\n",
                     difference.share_differing * 100.0, difference.mean_absolute);

        // THE CONTROL. The spike measured 65.38% and 34.072/255 for this comparison on a real
        // material; the thresholds are well under both, because what must never pass is two frames
        // that AGREE — which is what an unbound table, a slot that always resolves to zero, or a
        // set bound at the wrong index all produce.
        CHECK(difference.share_differing > 0.5);
        CHECK(difference.mean_absolute > 8.0);

        // AND IT IS THIS TEXTURE. Every pixel centre lands on a texel centre, so a correct sample
        // is the uploaded byte exactly. A frame that sampled SOMETHING — the wrong slot, a stale
        // descriptor, another texture in the table — passes the difference above and fails here.
        u32 exact = 0;
        for (u32 index = 0; index < kTexels; ++index) {
            const u32 texel = textured[index];
            const usize base = static_cast<usize>(index) * 4U;
            if (((texel >> 0U) & 0xFFU) == pattern_pixels[base + 0] &&
                ((texel >> 8U) & 0xFFU) == pattern_pixels[base + 1] &&
                ((texel >> 16U) & 0xFFU) == pattern_pixels[base + 2]) {
                ++exact;
            }
        }
        std::fprintf(stderr, "material binding: %u of %u texels are the uploaded byte exactly\n",
                     exact, kTexels);
        CHECK(exact == kTexels);

        // And the average frame is the average, which is what makes it a control rather than a
        // second unknown.
        for (u32 index = 0; index < kTexels; index += 137U) {
            CHECK(((averaged[index] >> 0U) & 0xFFU) == average[0]);
            CHECK(((averaged[index] >> 8U) & 0xFFU) == average[1]);
            CHECK(((averaged[index] >> 16U) & 0xFFU) == average[2]);
        }

        // A frame drawn with validation errors is not a frame that works: M3's recycled-descriptor
        // defect reached an artefact while the sample still printed "exit 0 (clean)".
        CHECK(fixture.validation_errors() == 0);

        table.shutdown();
        server.shutdown();
    }
}

}  // namespace cy::render_test
