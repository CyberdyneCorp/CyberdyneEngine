// SPDX-License-Identifier: MIT
#include "water_surface.h"

#include <cy/backends/rhi/pipeline.h>
#include <cy/water/shading.h>

#include "shaders/water_msl.h"
#include "shaders/water_spirv.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace cy::sample::world {
namespace {

/// How steeply the mirrored picture's depth falls with height above the water. See
/// `mirrored_rows()`.
constexpr f32 kMirrorDepthSlope = 0.25F;

/// Binding 0's stride: stage.h's `Vertex`, a position and a normal. Restated rather than included,
/// so that the render suite can build this file without the world the stage draws.
constexpr u32 kVertexStride = 24;

/// Enough trains for any model the ocean builds: eight bands of at most a few dozen each.
constexpr u32 kMaxResolvedTrains = 256;

/// The share of a surface's curvature a train carries: amplitude times wavenumber squared.
[[nodiscard]] f32 curvature_of(const water::WaveTrain& train) noexcept {
    const f32 k = (2.0F * std::numbers::pi_v<f32>) / train.wavelength;
    return train.amplitude * k * k;
}

void write_row(f32 (&out)[4], Vec4 row) noexcept {
    out[0] = row.x;
    out[1] = row.y;
    out[2] = row.z;
    out[3] = row.w;
}

[[nodiscard]] Expected<rhi::ShaderModuleHandle, Error> make_module(rhi::Device& device,
                                                                   rhi::ShaderStage stage,
                                                                   bool vertex) noexcept {
    rhi::ShaderModuleDescription description;
    description.name = vertex ? "water vertex" : "water fragment";
    description.stage = stage;
    if (device.capabilities().native_shader_format() == rhi::ShaderFormat::Msl) {
        const char* source = vertex ? kWaterVertexMsl : kWaterFragmentMsl;
        const usize bytes = vertex ? sizeof(kWaterVertexMsl) - 1 : sizeof(kWaterFragmentMsl) - 1;
        description.entry_point = vertex ? "waterVertex" : "waterFragment";
        description.native = Span<const u8>(reinterpret_cast<const u8*>(source), bytes);
        description.native_format = rhi::ShaderFormat::Msl;
    } else {
        description.entry_point = "main";
        description.spirv =
            vertex ? Span<const u32>(kWaterVertexSpirv, std::size(kWaterVertexSpirv))
                   : Span<const u32>(kWaterFragmentSpirv, std::size(kWaterFragmentSpirv));
    }
    return device.create_shader_module(description);
}

[[nodiscard]] Expected<rhi::TextureHandle, Error> make_target(rhi::Device& device, const char* name,
                                                              rhi::Format format,
                                                              rhi::TextureUsage attachment,
                                                              u32 width, u32 height) noexcept {
    rhi::TextureDescription description;
    description.name = name;
    description.format = format;
    description.extent = rhi::Extent3D{width, height, 1};
    description.usage = attachment | rhi::TextureUsage::Sampled;
    return device.create_texture(description);
}

}  // namespace

u32 select_caustic_trains(const water::DisplacementModel& model,
                          Span<water::WaveTrain> out) noexcept {
    water::WaveTrain resolved[kMaxResolvedTrains];
    const u32 count = water::resolve_trains(model, water::BandSelection::All,
                                            Span<water::WaveTrain>(resolved, kMaxResolvedTrains));
    // Strongest first, and of two equally strong the one the model lists first, so a frame is
    // reproducible. A selection rather than a sort: sixteen passes over a few dozen trains.
    const u32 kept = std::min(count, static_cast<u32>(out.size()));
    bool taken[kMaxResolvedTrains] = {};
    for (u32 slot = 0; slot < kept; ++slot) {
        u32 best = count;
        for (u32 index = 0; index < count; ++index) {
            if (!taken[index] &&
                (best == count || curvature_of(resolved[index]) > curvature_of(resolved[best]))) {
                best = index;
            }
        }
        taken[best] = true;
        out[slot] = resolved[best];
    }
    return kept;
}

Expected<WaterParams, Error> build_water_params(const water::WaterOptics& optics,
                                                const WaterLook& look, const WaterView& view,
                                                Span<const water::WaveTrain> trains) noexcept {
    const Expected<Mat4, Error> inverse = cy::inverse(view.world_to_clip);
    if (!inverse) {
        return make_unexpected(inverse.error());
    }
    WaterParams params;
    for (usize row = 0; row < 4; ++row) {
        write_row(params.inverse[row], inverse->row(row));
    }
    const auto height = static_cast<f32>(view.height);
    params.viewport[0] = static_cast<f32>(view.width);
    params.viewport[1] = height;
    params.viewport[2] = look.refraction_offset * height;
    params.viewport[3] = look.reflection_offset * height;

    // THE BODY'S OWN OPTICS, through src/water/'s own functions. Extinction is what
    // `water_transmittance()` exponentiates; the in-scatter of an opaque column is what
    // `water_in_scatter()` tends to, and the shader's `(1 - T) * inScatter` is that function at the
    // column's own thickness. The Fresnel reflectance is the closure's.
    const Vec3 extinction = optics.absorption + optics.scattering;
    const Vec3 opaque = water::water_in_scatter(optics, 1.0e6F);
    const water::WaterSurfaceClosure closure =
        water::build_closure(optics, Vec3{0.0F, 1.0F, 0.0F}, 0.0F, 0.0F);
    params.extinction[0] = extinction.x;
    params.extinction[1] = extinction.y;
    params.extinction[2] = extinction.z;
    params.extinction[3] = look.foam_band_metres;
    params.in_scatter[0] = opaque.x;
    params.in_scatter[1] = opaque.y;
    params.in_scatter[2] = opaque.z;
    params.in_scatter[3] = closure.f0;
    params.surface[0] = view.level;
    params.surface[1] = view.seconds;
    params.surface[2] = look.caustic_strength;
    params.surface[3] = 1.0F - (1.0F / optics.refractive_index);

    const u32 count = std::min(static_cast<u32>(trains.size()), kCausticTrains);
    params.caustic[0] = static_cast<f32>(count);
    params.caustic[1] = look.caustic_max_focus;
    constexpr f64 kTurn = 2.0 * std::numbers::pi;
    for (u32 index = 0; index < count; ++index) {
        const water::WaveTrain& train = trains[index];
        const f32 k = static_cast<f32>(kTurn) / train.wavelength;
        const f64 omega = std::sqrt(static_cast<f64>(water::kGravity) * static_cast<f64>(k));
        params.trains[index][0] = train.dir_x;
        params.trains[index][1] = train.dir_z;
        params.trains[index][2] = k;
        params.trains[index][3] = train.amplitude;
        // `water::evaluate_trains()`'s phase, k (d . x) - omega t + phase, with the world-relative
        // origin and the clock folded in here in f64 and reduced to one turn, so the device only
        // ever adds a small number to k (d . x_relative).
        const f64 along = (view.centre.x * static_cast<f64>(train.dir_x)) +
                          (view.centre.z * static_cast<f64>(train.dir_z));
        const f64 phase = (along * static_cast<f64>(k)) - (view.water_time * omega) +
                          static_cast<f64>(train.phase);
        params.phases[index] = static_cast<f32>(phase - (kTurn * std::floor(phase / kTurn)));
    }
    return params;
}

void mirrored_rows(const Mat4& world_to_clip, f32 level, Vec4 (&rows)[4]) noexcept {
    for (usize index = 0; index < 4; ++index) {
        // The reflection y -> 2 level - y, applied before the row: the y column changes sign and
        // its old contribution at the plane moves into the translation.
        const Vec4 row = world_to_clip.row(index);
        rows[index] = Vec4{row.x, -row.y, row.z, row.w + (2.0F * level * row.y)};
    }
    // The near plane onto the water: depth = 1 - k (y - level) / w, where y is the UNMIRRORED
    // height the vertex arrives with.
    rows[2] = Vec4{rows[3].x, rows[3].y - kMirrorDepthSlope, rows[3].z,
                   rows[3].w + (kMirrorDepthSlope * level)};
}

// --- The device half ---------------------------------------------------------------------------

Status WaterSurface::create(rhi::Device& device, rhi::DescriptorSetLayoutHandle cloud_shadow_layout,
                            rhi::Format scene_format, u32 width, u32 height) noexcept {
    width_ = width;
    height_ = height;
    scene_format_ = scene_format;
    cloud_shadow_layout_ = cloud_shadow_layout;
    if (Status made = create_pipeline(device, scene_format); !made) {
        return made;
    }
    return create_targets(device, scene_format);
}

Status WaterSurface::create_pipeline(rhi::Device& device, rhi::Format scene_format) noexcept {
    auto vertex = make_module(device, rhi::ShaderStage::Vertex, true);
    if (!vertex) {
        return make_unexpected(vertex.error());
    }
    vertex_ = *vertex;
    auto fragment = make_module(device, rhi::ShaderStage::Fragment, false);
    if (!fragment) {
        return make_unexpected(fragment.error());
    }
    fragment_ = *fragment;

    // Set 1, `WaterSurface` in water.slang: three sampled pictures and the parameter block.
    rhi::DescriptorBinding bindings[4] = {};
    for (u32 index = 0; index < 4; ++index) {
        bindings[index].binding = index;
        bindings[index].kind =
            index < 3 ? rhi::DescriptorKind::SampledTexture : rhi::DescriptorKind::StorageBuffer;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Fragment;
    }
    rhi::DescriptorSetLayoutDescription set_description;
    set_description.name = "world water surface";
    set_description.bindings = Span<const rhi::DescriptorBinding>(bindings, 4);
    auto set_layout = device.create_descriptor_set_layout(set_description);
    if (!set_layout) {
        return make_unexpected(set_layout.error());
    }
    set_layout_ = *set_layout;

    const rhi::DescriptorSetLayoutHandle sets[2] = {cloud_shadow_layout_, set_layout_};
    const rhi::PushConstantRange range{rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
                                       128};
    rhi::PipelineLayoutDescription layout;
    layout.name = "world water layout";
    layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(sets, 2);
    layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    auto layout_handle = device.create_pipeline_layout(layout);
    if (!layout_handle) {
        return make_unexpected(layout_handle.error());
    }
    layout_ = *layout_handle;

    // The world pipeline's vertex streams and states, restated: the water run is the same vertices
    // it always was, depth-tested and depth-written in the same opaque pass.
    const rhi::VertexBinding streams[2] = {{0, kVertexStride, rhi::VertexInputRate::PerVertex},
                                           {1, sizeof(Vec3), rhi::VertexInputRate::PerVertex}};
    const rhi::VertexAttribute attributes[3] = {{0, 0, rhi::Format::Rgb32Sfloat, 0},
                                                {1, 0, rhi::Format::Rgb32Sfloat, sizeof(Vec3)},
                                                {2, 1, rhi::Format::Rgb32Sfloat, 0}};
    rhi::ColorAttachmentState color;
    color.format = scene_format;
    rhi::GraphicsPipelineDescription pipeline;
    pipeline.name = "world water";
    pipeline.layout = layout_;
    pipeline.vertex_shader = vertex_;
    pipeline.fragment_shader = fragment_;
    pipeline.vertex_bindings = Span<const rhi::VertexBinding>(streams, 2);
    pipeline.vertex_attributes = Span<const rhi::VertexAttribute>(attributes, 3);
    pipeline.color_attachments = Span<const rhi::ColorAttachmentState>(&color, 1);
    pipeline.rasterisation.cull_mode = rhi::CullMode::None;
    pipeline.depth_stencil.format = rhi::Format::D32Sfloat;
    pipeline.depth_stencil.depth_test_enable = true;
    pipeline.depth_stencil.depth_write_enable = true;
    auto created = device.create_graphics_pipeline(pipeline);
    if (!created) {
        return make_unexpected(created.error());
    }
    pipeline_ = *created;

    rhi::BufferDescription buffer;
    buffer.name = "world water parameters";
    buffer.size = sizeof(WaterParams);
    buffer.usage = rhi::BufferUsage::Storage;
    buffer.memory = rhi::MemoryUse::Upload;
    auto params = device.create_buffer(buffer);
    if (!params) {
        return make_unexpected(params.error());
    }
    params_ = *params;
    if (Status uploaded = upload(device, WaterParams{}); !uploaded) {
        return uploaded;
    }

    auto set = device.allocate_descriptor_set(set_layout_, false);
    if (!set) {
        return make_unexpected(set.error());
    }
    set_ = *set;
    rhi::DescriptorWrite write;
    write.binding = 3;
    write.kind = rhi::DescriptorKind::StorageBuffer;
    write.buffer = params_;
    return device.update_descriptor_set(set_, Span<const rhi::DescriptorWrite>(&write, 1));
}

Status WaterSurface::create_targets(rhi::Device& device, rhi::Format scene_format) noexcept {
    const struct {
        rhi::TextureHandle* handle;
        const char* name;
        rhi::Format format;
        rhi::TextureUsage attachment;
    } targets[4] = {
        {&refraction_, "world water refraction", scene_format, rhi::TextureUsage::ColorAttachment},
        {&refraction_depth_, "world water refraction depth", rhi::Format::D32Sfloat,
         rhi::TextureUsage::DepthStencilAttachment},
        {&reflection_, "world water reflection", scene_format, rhi::TextureUsage::ColorAttachment},
        {&reflection_depth_, "world water reflection depth", rhi::Format::D32Sfloat,
         rhi::TextureUsage::DepthStencilAttachment},
    };
    for (const auto& target : targets) {
        auto made =
            make_target(device, target.name, target.format, target.attachment, width_, height_);
        if (!made) {
            return make_unexpected(made.error());
        }
        *target.handle = *made;
    }
    return ok();
}

void WaterSurface::destroy(rhi::Device& device) noexcept {
    for (rhi::TextureHandle* texture :
         {&refraction_, &refraction_depth_, &reflection_, &reflection_depth_}) {
        if (!texture->is_null()) {
            device.destroy_texture(*texture);
            *texture = rhi::TextureHandle{};
        }
    }
    if (!params_.is_null()) {
        device.destroy_buffer(params_);
        params_ = rhi::BufferHandle{};
    }
    if (!pipeline_.is_null()) {
        device.destroy_graphics_pipeline(pipeline_);
        pipeline_ = rhi::GraphicsPipelineHandle{};
    }
    if (!layout_.is_null()) {
        device.destroy_pipeline_layout(layout_);
        layout_ = rhi::PipelineLayoutHandle{};
    }
    if (!set_layout_.is_null()) {
        device.destroy_descriptor_set_layout(set_layout_);
        set_layout_ = rhi::DescriptorSetLayoutHandle{};
    }
    for (rhi::ShaderModuleHandle* module : {&vertex_, &fragment_}) {
        if (!module->is_null()) {
            device.destroy_shader_module(*module);
            *module = rhi::ShaderModuleHandle{};
        }
    }
}

Status WaterSurface::upload(rhi::Device& device, const WaterParams& params) const noexcept {
    void* mapped = device.buffer_mapped_pointer(params_);
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "the water parameter buffer is not mapped");
    }
    std::memcpy(mapped, &params, sizeof(params));
    return ok();
}

WaterTargets WaterSurface::import(rendering::RenderGraph& graph) const noexcept {
    const auto import_one = [&graph, this](const char* name, rhi::TextureHandle handle,
                                           rhi::Format format) noexcept {
        rendering::TextureRequest request;
        request.name = name;
        request.format = format;
        request.width = width_;
        request.height = height_;
        request.extra_usage = rhi::TextureUsage::Sampled;
        return graph.import_texture(request, handle, rhi::ImageUse::Undefined);
    };
    WaterTargets targets;
    targets.refraction = import_one("world water refraction", refraction_, scene_format_);
    targets.refraction_depth =
        import_one("world water refraction depth", refraction_depth_, rhi::Format::D32Sfloat);
    targets.reflection = import_one("world water reflection", reflection_, scene_format_);
    targets.reflection_depth =
        import_one("world water reflection depth", reflection_depth_, rhi::Format::D32Sfloat);
    return targets;
}

Status WaterSurface::bind(rhi::Device& device, const rendering::GraphExecutor& executor,
                          const WaterTargets& targets) const noexcept {
    const rendering::ResourceId sampled[3] = {targets.refraction, targets.refraction_depth,
                                              targets.reflection};
    rhi::DescriptorWrite writes[3] = {};
    for (u32 index = 0; index < 3; ++index) {
        writes[index].binding = index;
        writes[index].kind = rhi::DescriptorKind::SampledTexture;
        writes[index].texture_view = executor.view(sampled[index]);
        writes[index].use = rhi::ImageUse::SampledRead;
        if (writes[index].texture_view.is_null()) {
            return fail(ErrorCode::Internal, "a water target has no view in this frame");
        }
    }
    return device.update_descriptor_set(set_, Span<const rhi::DescriptorWrite>(writes, 3));
}

}  // namespace cy::sample::world
