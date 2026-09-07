#include <cy/rendering/virtual_geometry/visbuffer.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../shaders/vg_visbuffer_spirv.h"

namespace cy::rendering::vg {

namespace {

constexpr u32 kGroupSize = 64;

/// The pass order, and the index of each pipeline. `vgVisScan` and `vgVisPrepare` are one thread
/// each; the rest are one thread per pixel or one workgroup per visible cluster.
enum VisPass : u32 {
    kPassClear = 0,
    kPassPrepare,
    kPassRaster,
    kPassClassify,
    kPassScan,
    kPassScatter,
    kPassResolve,
    kPassCount,
};

enum VisBinding : u32 {
    kBindVisible = 0,
    kBindClusters,
    kBindGeometry,
    kBindInstances,
    kBindAssets,
    kBindPayload,
    kBindVisbuffer,
    kBindDepth,
    kBindBinCounts,
    kBindBinOffsets,
    kBindBinCursor,
    kBindBinPixels,
    kBindResolved,
    kBindCounters,
    kBindVisArgs,
    kVisBindingCount,
};

/// The push block, matching `VgVisPush` in the shader field for field.
struct VisPush {
    f32 row0[4];
    f32 row1[4];
    f32 row2[4];
    f32 row3[4];
    u32 width;
    u32 height;
    u32 material_count;
    u32 vertex_stride;
    u32 normal_offset;
    u32 uv_offset;
    f32 position_scale;
    f32 normal_scale;
    f32 uv_scale;
    u32 reserved;
};
static_assert(sizeof(VisPush) == 104, "VisPush must match VgVisPush in vg_visbuffer.slang");

constexpr u32 kNoOffset = 0xFFFFFFFFU;
constexpr u64 kVisArgsBytes = 6 * sizeof(u32);

/// The per-cluster geometry record the shader reads: where a cluster's vertices are in the
/// concatenated payload, and how many of each there are.
struct GpuClusterGeometry {
    u32 payload_offset = 0;
    u32 vertex_count = 0;
    u32 index_count = 0;
    u32 reserved = 0;
};
static_assert(sizeof(GpuClusterGeometry) == 16,
              "GpuClusterGeometry must match VgClusterGeometry in vg_visbuffer.slang");

/// Bytes one component of an attribute occupies. The same rounding `asset.cpp` applies, restated
/// here because the shader's decode is written against sixteen-bit components and the host has to
/// check that the asset it was handed actually uses them.
[[nodiscard]] u32 component_bytes(u32 bits) noexcept {
    if (bits <= 8) {
        return 1;
    }
    if (bits <= 16) {
        return 2;
    }
    return 4;
}

[[nodiscard]] Vec3 rotate(const Quat& q, Vec3 v) noexcept {
    return q * v;
}

}  // namespace

MaterialBins::MaterialBins(Allocator& allocator) noexcept
    : counts(allocator), offsets(allocator), pixels(allocator) {}

Span<const u32> MaterialBins::bin(u32 material) const noexcept {
    if (material + 1 >= offsets.size()) {
        return {};
    }
    const u32 first = offsets[material];
    const u32 last = offsets[material + 1];
    return {pixels.data() + first, last - first};
}

Status bin_by_material(Span<const VisibilitySample> visbuffer, Span<const VisibleCluster> visible,
                       u32 material_count, MaterialBins& out) noexcept {
    if (Status resized = out.counts.resize(material_count); !resized) {
        return resized;
    }
    if (Status resized = out.offsets.resize(material_count + 1U); !resized) {
        return resized;
    }
    for (u32 index = 0; index < material_count; ++index) {
        out.counts[index] = 0;
    }
    for (const VisibilitySample& sample : visbuffer) {
        if (!sample.covered() || sample.visible >= visible.size()) {
            continue;
        }
        const u32 material = visible[sample.visible].material;
        if (material < material_count) {
            ++out.counts[material];
        }
    }
    u32 running = 0;
    for (u32 index = 0; index < material_count; ++index) {
        out.offsets[index] = running;
        running += out.counts[index];
    }
    out.offsets[material_count] = running;

    if (Status resized = out.pixels.resize(running); !resized) {
        return resized;
    }
    Array<u32> cursor(out.pixels.allocator());
    if (Status resized = cursor.resize(material_count); !resized) {
        return resized;
    }
    for (u32 index = 0; index < material_count; ++index) {
        cursor[index] = out.offsets[index];
    }
    for (u32 pixel = 0; pixel < visbuffer.size(); ++pixel) {
        const VisibilitySample& sample = visbuffer[pixel];
        if (!sample.covered() || sample.visible >= visible.size()) {
            continue;
        }
        const u32 material = visible[sample.visible].material;
        if (material >= material_count) {
            continue;
        }
        out.pixels[cursor[material]] = pixel;
        ++cursor[material];
    }
    return ok();
}

Expected<SurfaceAttributes, Error> reconstruct_surface(const DecodedAsset& asset,
                                                       const GeometryInstance& instance,
                                                       const VisibleCluster& visible, u32 triangle,
                                                       const Mat4& world_to_clip, Vec2 pixel,
                                                       u32 width, u32 height,
                                                       Allocator& allocator) noexcept {
    if (visible.cluster >= asset.clusters.size()) {
        return fail(ErrorCode::OutOfRange, "reconstruct_surface: no such cluster");
    }
    Expected<DecodedCluster, Error> geometry = decode_cluster(asset, visible.cluster, allocator);
    if (!geometry) {
        return make_unexpected(geometry.error());
    }
    if ((triangle * 3U) + 2U >= geometry->indices.size()) {
        return fail(ErrorCode::OutOfRange, "reconstruct_surface: no such triangle");
    }

    Vec3 world[3];
    Vec2 screen[3];
    for (u32 corner = 0; corner < 3; ++corner) {
        const u32 slot = geometry->indices[(triangle * 3U) + corner];
        world[corner] = (rotate(instance.rotation, geometry->positions[slot]) * instance.scale) +
                        instance.translation;
        const Vec4 clip =
            world_to_clip * Vec4{world[corner].x, world[corner].y, world[corner].z, 1.0F};
        const f32 w = clip.w > 1.0e-6F ? clip.w : 1.0e-6F;
        screen[corner] = Vec2{(((clip.x / w) * 0.5F) + 0.5F) * static_cast<f32>(width),
                              (((clip.y / w) * 0.5F) + 0.5F) * static_cast<f32>(height)};
    }

    const f32 area = ((screen[1].x - screen[0].x) * (screen[2].y - screen[0].y)) -
                     ((screen[1].y - screen[0].y) * (screen[2].x - screen[0].x));
    const f32 inverse_area = std::fabs(area) > 1.0e-9F ? 1.0F / area : 0.0F;
    const f32 w0 = (((screen[1].x - pixel.x) * (screen[2].y - pixel.y)) -
                    ((screen[1].y - pixel.y) * (screen[2].x - pixel.x))) *
                   inverse_area;
    const f32 w1 = (((screen[2].x - pixel.x) * (screen[0].y - pixel.y)) -
                    ((screen[2].y - pixel.y) * (screen[0].x - pixel.x))) *
                   inverse_area;
    const f32 w2 = 1.0F - w0 - w1;

    const f32 weights[3] = {w0, w1, w2};
    SurfaceAttributes out;
    out.barycentric = Vec3{w0, w1, w2};
    out.material = visible.material;
    out.position = (world[0] * w0) + (world[1] * w1) + (world[2] * w2);
    if (!geometry->normals.empty()) {
        Vec3 blended{0.0F, 0.0F, 0.0F};
        for (u32 corner = 0; corner < 3; ++corner) {
            const u32 slot = geometry->indices[(triangle * 3U) + corner];
            const f32 weight = weights[corner];
            blended = blended + (geometry->normals[slot] * weight);
        }
        const Vec3 rotated = rotate(instance.rotation, blended);
        const f32 magnitude = length(rotated);
        out.normal = magnitude > 1.0e-9F ? rotated * (1.0F / magnitude) : Vec3{0.0F, 0.0F, 1.0F};
    }
    if (!geometry->uvs.empty()) {
        Vec2 blended{0.0F, 0.0F};
        for (u32 corner = 0; corner < 3; ++corner) {
            const u32 slot = geometry->indices[(triangle * 3U) + corner];
            const f32 weight = weights[corner];
            blended = blended + (geometry->uvs[slot] * weight);
        }
        out.uv = blended;
    }
    return out;
}

VisbufferReadback::VisbufferReadback(Allocator& allocator) noexcept
    : samples(allocator),
      bin_counts(allocator),
      bin_offsets(allocator),
      bin_pixels(allocator),
      resolved(allocator) {}

u32 VisbufferReadback::covered_pixels() const noexcept {
    u32 covered = 0;
    for (const VisibilitySample& sample : samples) {
        covered += sample.covered() ? 1U : 0U;
    }
    return covered;
}

VisbufferPass::VisbufferPass(Allocator& allocator, rhi::Device& device) noexcept
    : allocator_(allocator), device_(device), states_(allocator) {}

VisbufferPass::~VisbufferPass() {
    for (const rhi::ComputePipelineHandle pipeline : pipelines_) {
        device_.destroy_compute_pipeline(pipeline);
    }
    for (const rhi::ShaderModuleHandle module : modules_) {
        device_.destroy_shader_module(module);
    }
    device_.destroy_pipeline_layout(pipeline_layout_);
    device_.destroy_descriptor_set_layout(set_layout_);
    for (const rhi::BufferHandle buffer :
         {geometry_, payload_, visbuffer_, depth_, bin_counts_, bin_offsets_, bin_cursor_,
          bin_pixels_, resolved_, vis_args_, staging_, readback_}) {
        device_.destroy_buffer(buffer);
    }
}

Expected<rhi::BufferHandle, Error> VisbufferPass::make_buffer(const char* name, u64 bytes,
                                                              rhi::BufferUsage usage,
                                                              rhi::MemoryUse memory) noexcept {
    rhi::BufferDescription description;
    description.name = name;
    description.size = bytes > 0 ? bytes : 4;
    description.usage = usage;
    description.memory = memory;
    return device_.create_buffer(description);
}

Status VisbufferPass::upload(rhi::BufferHandle target, const void* data, u64 bytes) noexcept {
    // These two uploads happen once, at initialise, before any pass reads them, and they are
    // followed by a host wait — so unlike the traversal's per-frame staging they need no barrier
    // beyond the queue's own ordering against the graph submitted afterwards. The traversal's
    // `stage()` carries the argument for the case where that is NOT true.
    if (bytes == 0) {
        return ok();
    }
    auto* mapped = static_cast<u8*>(device_.buffer_mapped_pointer(staging_));
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "VisbufferPass::upload: the staging buffer is not mapped");
    }
    std::memcpy(mapped, data, bytes);
    Expected<rhi::CommandBufferHandle, Error> command =
        device_.acquire_command_buffer(rhi::QueueKind::Graphics, false);
    if (!command) {
        return make_unexpected(command.error());
    }
    if (Status begun = device_.begin_command_buffer(*command); !begun) {
        return begun;
    }
    rhi::BufferCopy region;
    region.size = bytes;
    device_.command_buffer(*command)->copy_buffer(staging_, target,
                                                  Span<const rhi::BufferCopy>(&region, 1));
    if (Status ended = device_.end_command_buffer(*command); !ended) {
        return ended;
    }
    rhi::SubmitInfo submit;
    submit.command_buffers = Span<const rhi::CommandBufferHandle>(&*command, 1);
    Expected<u64, Error> value = device_.submit(submit);
    if (!value) {
        return make_unexpected(value.error());
    }
    return device_.wait_timeline(rhi::QueueKind::Graphics, *value, 5'000'000'000ULL);
}

Status VisbufferPass::initialise(const GpuScene& scene, Span<const DecodedAsset* const> assets,
                                 Span<const u8> payload, Span<const u32> payload_offsets,
                                 const VisbufferOptions& options) noexcept {
    if (initialised_) {
        return fail(ErrorCode::AlreadyExists, "VisbufferPass::initialise: already initialised");
    }
    if (assets.empty() || assets.size() != payload_offsets.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "VisbufferPass::initialise: one payload offset per asset is required");
    }
    options_ = options;

    // THE SHADER DECODES SIXTEEN-BIT COMPONENTS, so an asset encoded any other way is refused here
    // rather than decoded wrongly. Every asset in one pass must share an encoding for the same
    // reason: the stride and the attribute offsets are push constants, not per-cluster fields.
    const VertexEncoding& encoding = assets[0]->encoding;
    for (const DecodedAsset* asset : assets) {
        if (asset == nullptr) {
            return fail(ErrorCode::InvalidArgument, "VisbufferPass::initialise: a null asset");
        }
        if (component_bytes(asset->encoding.position_bits) != 2 ||
            (asset->encoding.has_normals && component_bytes(asset->encoding.normal_bits) != 2) ||
            (asset->encoding.has_uvs && component_bytes(asset->encoding.uv_bits) != 2)) {
            return fail(
                ErrorCode::Unsupported,
                "VisbufferPass::initialise: the resolve decodes sixteen-bit components, and "
                "this asset was cooked with a different precision");
        }
        if (asset->encoding.position_bits != encoding.position_bits ||
            asset->encoding.has_normals != encoding.has_normals ||
            asset->encoding.has_uvs != encoding.has_uvs) {
            return fail(ErrorCode::Unsupported,
                        "VisbufferPass::initialise: every asset in one pass must share a vertex "
                        "encoding, because the stride is a push constant rather than a field");
        }
    }
    vertex_stride_ = encoding.vertex_bytes();
    // The attribute offsets within a vertex: the position occupies the first six bytes, the
    // normal the next four when there is one, and the UV whatever follows.
    normal_offset_ = encoding.has_normals ? 6U : kNoOffset;
    uv_offset_ = kNoOffset;
    if (encoding.has_uvs) {
        uv_offset_ = encoding.has_normals ? 10U : 6U;
    }
    // The DECLARED precision, not the storage width. See the comment on `positionScale` in the
    // shader for what dividing by the wrong one produces.
    auto scale = [](u32 bits) noexcept { return 1.0F / static_cast<f32>((1ULL << bits) - 1ULL); };
    position_scale_ = scale(encoding.position_bits);
    normal_scale_ = encoding.has_normals ? scale(encoding.normal_bits) : 1.0F;
    uv_scale_ = encoding.has_uvs ? scale(encoding.uv_bits) : 1.0F;

    // The per-cluster geometry table, in the same order the scene's cluster buffer is in.
    Array<GpuClusterGeometry> geometry(allocator_);
    for (u32 index = 0; index < assets.size(); ++index) {
        const DecodedAsset& asset = *assets[index];
        u32 offset = payload_offsets[index];
        for (const PageDescription& page : asset.pages) {
            u32 running = offset + page.byte_offset;
            for (u32 slot = 0; slot < page.cluster_count; ++slot) {
                const Cluster& cluster = asset.clusters[page.first_cluster + slot];
                GpuClusterGeometry record;
                record.payload_offset = running;
                record.vertex_count = cluster.vertex_count;
                record.index_count = cluster.index_count;
                if (Status pushed = geometry.push_back(record); !pushed) {
                    return pushed;
                }
                running += encoded_cluster_bytes(cluster.vertex_count, cluster.index_count,
                                                 asset.encoding) -
                           kClusterMetadataBytes;
            }
        }
    }
    if (geometry.size() != scene.clusters.size()) {
        return fail(
            ErrorCode::InvalidArgument,
            "VisbufferPass::initialise: the assets do not account for the scene's clusters");
    }

    const u64 pixels = static_cast<u64>(options.width) * options.height;
    const u64 geometry_bytes = geometry.size() * sizeof(GpuClusterGeometry);
    const u64 readback_bytes = (pixels * sizeof(VisibilitySample)) +
                               (static_cast<u64>(options.material_count) * sizeof(u32)) +
                               (static_cast<u64>(options.material_count + 1U) * sizeof(u32)) +
                               (pixels * sizeof(u32)) + (pixels * sizeof(f32) * 4);
    const rhi::BufferUsage storage =
        rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDestination;
    const u64 staging_bytes = payload.size() > geometry_bytes ? payload.size() : geometry_bytes;

    struct Plan {
        rhi::BufferHandle* target;
        const char* name;
        u64 bytes;
        rhi::BufferUsage usage;
        rhi::MemoryUse memory;
    };
    const Plan plan[] = {
        {&geometry_, "vg.vis.geometry", geometry_bytes, storage, rhi::MemoryUse::DeviceLocal},
        {&payload_, "vg.vis.payload", payload.size(), storage, rhi::MemoryUse::DeviceLocal},
        {&visbuffer_, "vg.vis.visbuffer", pixels * sizeof(VisibilitySample),
         storage | rhi::BufferUsage::TransferSource, rhi::MemoryUse::DeviceLocal},
        {&depth_, "vg.vis.depth", pixels * sizeof(u32), storage, rhi::MemoryUse::DeviceLocal},
        {&bin_counts_, "vg.vis.bin-counts", static_cast<u64>(options.material_count) * sizeof(u32),
         storage | rhi::BufferUsage::TransferSource, rhi::MemoryUse::DeviceLocal},
        {&bin_offsets_, "vg.vis.bin-offsets",
         static_cast<u64>(options.material_count + 1U) * sizeof(u32),
         storage | rhi::BufferUsage::TransferSource, rhi::MemoryUse::DeviceLocal},
        {&bin_cursor_, "vg.vis.bin-cursor", static_cast<u64>(options.material_count) * sizeof(u32),
         storage, rhi::MemoryUse::DeviceLocal},
        {&bin_pixels_, "vg.vis.bin-pixels", pixels * sizeof(u32),
         storage | rhi::BufferUsage::TransferSource, rhi::MemoryUse::DeviceLocal},
        {&resolved_, "vg.vis.resolved", pixels * sizeof(f32) * 4,
         storage | rhi::BufferUsage::TransferSource, rhi::MemoryUse::DeviceLocal},
        {&vis_args_, "vg.vis.args", kVisArgsBytes, storage | rhi::BufferUsage::Indirect,
         rhi::MemoryUse::DeviceLocal},
        {&staging_, "vg.vis.staging", staging_bytes, rhi::BufferUsage::TransferSource,
         rhi::MemoryUse::Upload},
        {&readback_, "vg.vis.readback", readback_bytes, rhi::BufferUsage::TransferDestination,
         rhi::MemoryUse::Readback},
    };
    for (const Plan& entry : plan) {
        Expected<rhi::BufferHandle, Error> buffer =
            make_buffer(entry.name, entry.bytes, entry.usage, entry.memory);
        if (!buffer) {
            return make_unexpected(buffer.error());
        }
        *entry.target = *buffer;
    }

    rhi::DescriptorBinding bindings[kVisBindingCount];
    for (u32 index = 0; index < kVisBindingCount; ++index) {
        bindings[index] = rhi::DescriptorBinding{};
        bindings[index].binding = index;
        bindings[index].kind = rhi::DescriptorKind::StorageBuffer;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription layout_description;
    layout_description.name = "vg.visbuffer";
    layout_description.bindings = Span<const rhi::DescriptorBinding>(bindings, kVisBindingCount);
    Expected<rhi::DescriptorSetLayoutHandle, Error> set_layout =
        device_.create_descriptor_set_layout(layout_description);
    if (!set_layout) {
        return make_unexpected(set_layout.error());
    }
    set_layout_ = *set_layout;

    rhi::PushConstantRange push;
    push.stages = rhi::ShaderStage::Compute;
    push.size = sizeof(VisPush);
    rhi::PipelineLayoutDescription pipeline_layout_description;
    pipeline_layout_description.name = "vg.visbuffer";
    pipeline_layout_description.set_layouts =
        Span<const rhi::DescriptorSetLayoutHandle>(&set_layout_, 1);
    pipeline_layout_description.push_constants = Span<const rhi::PushConstantRange>(&push, 1);
    Expected<rhi::PipelineLayoutHandle, Error> pipeline_layout =
        device_.create_pipeline_layout(pipeline_layout_description);
    if (!pipeline_layout) {
        return make_unexpected(pipeline_layout.error());
    }
    pipeline_layout_ = *pipeline_layout;

    Expected<rhi::DescriptorSetHandle, Error> descriptors =
        device_.allocate_descriptor_set(set_layout_, false);
    if (!descriptors) {
        return make_unexpected(descriptors.error());
    }
    descriptors_ = *descriptors;

    const struct {
        const char* name;
        Span<const u32> spirv;
    } modules[kPassCount] = {
        {"vg.vis.clear", Span<const u32>(kVgVisClearSpirv)},
        {"vg.vis.prepare", Span<const u32>(kVgVisPrepareSpirv)},
        {"vg.vis.raster", Span<const u32>(kVgVisRasterSpirv)},
        {"vg.vis.classify", Span<const u32>(kVgVisClassifySpirv)},
        {"vg.vis.scan", Span<const u32>(kVgVisScanSpirv)},
        {"vg.vis.scatter", Span<const u32>(kVgVisScatterSpirv)},
        {"vg.vis.resolve", Span<const u32>(kVgVisResolveSpirv)},
    };
    for (u32 index = 0; index < kPassCount; ++index) {
        rhi::ShaderModuleDescription module_description;
        module_description.name = modules[index].name;
        module_description.stage = rhi::ShaderStage::Compute;
        module_description.spirv = modules[index].spirv;
        Expected<rhi::ShaderModuleHandle, Error> module =
            device_.create_shader_module(module_description);
        if (!module) {
            return make_unexpected(module.error());
        }
        modules_[index] = *module;
        rhi::ComputePipelineDescription description;
        description.name = modules[index].name;
        description.layout = pipeline_layout_;
        description.shader = *module;
        Expected<rhi::ComputePipelineHandle, Error> pipeline =
            device_.create_compute_pipeline(description);
        if (!pipeline) {
            return make_unexpected(pipeline.error());
        }
        pipelines_[index] = *pipeline;
    }

    if (Status uploaded = upload(geometry_, geometry.data(), geometry_bytes); !uploaded) {
        return uploaded;
    }
    if (Status uploaded = upload(payload_, payload.data(), payload.size()); !uploaded) {
        return uploaded;
    }
    initialised_ = true;
    return ok();
}

void VisbufferPass::dispatch(const PassContext& context, const PassState& state) noexcept {
    rhi::CommandBuffer& commands = *context.commands;
    commands.bind_compute_pipeline(pipelines_[state.pass]);
    commands.bind_descriptor_sets(pipeline_layout_, 0,
                                  Span<const rhi::DescriptorSetHandle>(&descriptors_, 1));
    commands.push_constants(pipeline_layout_, rhi::ShaderStage::Compute, 0,
                            Span<const u8>(push_, sizeof(VisPush)));
    if (state.pass == kPassRaster) {
        commands.dispatch_indirect(vis_args_, 0);
        return;
    }
    if (state.pass == kPassResolve) {
        commands.dispatch(state.groups, 1, 1);
        return;
    }
    commands.dispatch(state.groups, 1, 1);
}

void VisbufferPass::record_pass(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<PassState*>(user);
    state->self->dispatch(context, *state);
}

Status VisbufferPass::record(RenderGraph& graph, const GpuTraversal& traversal,
                             const Mat4& world_to_clip) noexcept {
    if (!initialised_) {
        return fail(ErrorCode::Unavailable, "VisbufferPass::record: initialise() has not run");
    }
    VisPush push{};
    // THE ROWS, TAKEN OUT OF A COLUMN-MAJOR Mat4 EXPLICITLY. `cy::Mat4` stores four COLUMNS and
    // multiplies a column vector on the left, so row `r` is the `r`th component of each column.
    // Writing the columns into the shader's rows instead would transpose the projection, which
    // renders a plausible-looking picture of the wrong thing.
    f32* rows[4] = {push.row0, push.row1, push.row2, push.row3};
    for (u32 column = 0; column < 4; ++column) {
        const Vec4& source = world_to_clip.columns[column];
        rows[0][column] = source.x;
        rows[1][column] = source.y;
        rows[2][column] = source.z;
        rows[3][column] = source.w;
    }
    push.width = options_.width;
    push.height = options_.height;
    push.material_count = options_.material_count;
    push.vertex_stride = vertex_stride_;
    push.normal_offset = normal_offset_;
    push.uv_offset = uv_offset_;
    push.position_scale = position_scale_;
    push.normal_scale = normal_scale_;
    push.uv_scale = uv_scale_;
    std::memcpy(push_, &push, sizeof(push));

    // The descriptor set is rewritten each frame because the traversal's buffers are the ones it
    // reads, and a caller may hand a different traversal to the next frame.
    const rhi::BufferHandle bound[kVisBindingCount] = {traversal.visible_buffer(),
                                                       traversal.cluster_buffer(),
                                                       geometry_,
                                                       traversal.instance_buffer(),
                                                       traversal.asset_buffer(),
                                                       payload_,
                                                       visbuffer_,
                                                       depth_,
                                                       bin_counts_,
                                                       bin_offsets_,
                                                       bin_cursor_,
                                                       bin_pixels_,
                                                       resolved_,
                                                       traversal.counter_buffer(),
                                                       vis_args_};
    rhi::DescriptorWrite writes[kVisBindingCount];
    for (u32 index = 0; index < kVisBindingCount; ++index) {
        writes[index] = rhi::DescriptorWrite{};
        writes[index].binding = index;
        writes[index].kind = rhi::DescriptorKind::StorageBuffer;
        writes[index].buffer = bound[index];
    }
    if (Status updated = device_.update_descriptor_set(
            descriptors_, Span<const rhi::DescriptorWrite>(writes, kVisBindingCount));
        !updated) {
        return updated;
    }

    auto import = [&graph](rhi::BufferHandle handle, const char* name, u64 bytes,
                           rhi::BufferUsage usage) noexcept {
        BufferRequest request;
        request.name = name;
        request.size = bytes;
        request.extra_usage = usage;
        return graph.import_buffer(request, handle);
    };
    const rhi::BufferUsage storage = rhi::BufferUsage::Storage;
    const u64 pixels = static_cast<u64>(options_.width) * options_.height;
    const ResourceId visbuffer =
        import(visbuffer_, "vg.vis.visbuffer", pixels * sizeof(VisibilitySample), storage);
    const ResourceId depth = import(depth_, "vg.vis.depth", pixels * sizeof(u32), storage);
    const ResourceId bin_counts =
        import(bin_counts_, "vg.vis.bin-counts",
               static_cast<u64>(options_.material_count) * sizeof(u32), storage);
    const ResourceId bin_offsets =
        import(bin_offsets_, "vg.vis.bin-offsets",
               static_cast<u64>(options_.material_count + 1U) * sizeof(u32), storage);
    const ResourceId bin_cursor =
        import(bin_cursor_, "vg.vis.bin-cursor",
               static_cast<u64>(options_.material_count) * sizeof(u32), storage);
    const ResourceId bin_pixels =
        import(bin_pixels_, "vg.vis.bin-pixels", pixels * sizeof(u32), storage);
    const ResourceId resolved =
        import(resolved_, "vg.vis.resolved", pixels * sizeof(f32) * 4, storage);
    const ResourceId args =
        import(vis_args_, "vg.vis.args", kVisArgsBytes, storage | rhi::BufferUsage::Indirect);
    const ResourceId visible =
        import(traversal.visible_buffer(), "vg.visible",
               static_cast<u64>(traversal.visible_capacity()) * sizeof(GpuVisibleCluster), storage);

    states_.clear();
    if (Status reserved = states_.reserve(kPassCount); !reserved) {
        return reserved;
    }
    const u32 pixel_groups = static_cast<u32>((pixels + kGroupSize - 1U) / kGroupSize);

    if (Status pushed = states_.push_back(PassState{this, kPassClear, pixel_groups}); !pushed) {
        return pushed;
    }
    graph.add_pass("vg.vis.clear", rhi::QueueKind::Graphics)
        .write(visbuffer, rhi::Access::ComputeStorageWrite)
        .write(depth, rhi::Access::ComputeStorageWrite)
        .write(bin_counts, rhi::Access::ComputeStorageWrite)
        .write(bin_offsets, rhi::Access::ComputeStorageWrite)
        .write(bin_cursor, rhi::Access::ComputeStorageWrite)
        .write(bin_pixels, rhi::Access::ComputeStorageWrite)
        .write(resolved, rhi::Access::ComputeStorageWrite)
        .record(&record_pass, &states_.back());

    if (Status pushed = states_.push_back(PassState{this, kPassPrepare, 1}); !pushed) {
        return pushed;
    }
    graph.add_pass("vg.vis.prepare", rhi::QueueKind::Graphics)
        .write(args, rhi::Access::ComputeStorageWrite)
        .record(&record_pass, &states_.back());

    if (Status pushed = states_.push_back(PassState{this, kPassRaster, 0}); !pushed) {
        return pushed;
    }
    graph.add_pass("vg.vis.raster", rhi::QueueKind::Graphics)
        .read(args, rhi::Access::IndirectCommandRead)
        .read(visible, rhi::Access::ComputeStorageRead)
        .use(depth, rhi::Access::ComputeStorageReadWrite)
        .write(visbuffer, rhi::Access::ComputeStorageWrite)
        .record(&record_pass, &states_.back());

    if (Status pushed = states_.push_back(PassState{this, kPassClassify, pixel_groups}); !pushed) {
        return pushed;
    }
    graph.add_pass("vg.vis.classify", rhi::QueueKind::Graphics)
        .read(visbuffer, rhi::Access::ComputeStorageRead)
        .use(bin_counts, rhi::Access::ComputeStorageReadWrite)
        .record(&record_pass, &states_.back());

    if (Status pushed = states_.push_back(PassState{this, kPassScan, 1}); !pushed) {
        return pushed;
    }
    graph.add_pass("vg.vis.scan", rhi::QueueKind::Graphics)
        .read(bin_counts, rhi::Access::ComputeStorageRead)
        .write(bin_offsets, rhi::Access::ComputeStorageWrite)
        .write(args, rhi::Access::ComputeStorageWrite)
        .record(&record_pass, &states_.back());

    if (Status pushed = states_.push_back(PassState{this, kPassScatter, pixel_groups}); !pushed) {
        return pushed;
    }
    graph.add_pass("vg.vis.scatter", rhi::QueueKind::Graphics)
        .read(visbuffer, rhi::Access::ComputeStorageRead)
        .read(bin_offsets, rhi::Access::ComputeStorageRead)
        .use(bin_cursor, rhi::Access::ComputeStorageReadWrite)
        .write(bin_pixels, rhi::Access::ComputeStorageWrite)
        .record(&record_pass, &states_.back());

    if (Status pushed = states_.push_back(PassState{this, kPassResolve, pixel_groups}); !pushed) {
        return pushed;
    }
    graph.add_pass("vg.vis.resolve", rhi::QueueKind::Graphics)
        .read(args, rhi::Access::IndirectCommandRead)
        .read(visbuffer, rhi::Access::ComputeStorageRead)
        .read(bin_pixels, rhi::Access::ComputeStorageRead)
        .read(bin_offsets, rhi::Access::ComputeStorageRead)
        .write(resolved, rhi::Access::ComputeStorageWrite)
        .record(&record_pass, &states_.back());

    return graph.status();
}

Status VisbufferPass::read_back(VisbufferReadback& out) const noexcept {
    Expected<rhi::CommandBufferHandle, Error> command =
        device_.acquire_command_buffer(rhi::QueueKind::Graphics, false);
    if (!command) {
        return make_unexpected(command.error());
    }
    if (Status begun = device_.begin_command_buffer(*command); !begun) {
        return begun;
    }
    const u64 pixels = static_cast<u64>(options_.width) * options_.height;
    const u64 sample_bytes = pixels * sizeof(VisibilitySample);
    const u64 count_bytes = static_cast<u64>(options_.material_count) * sizeof(u32);
    const u64 offset_bytes = static_cast<u64>(options_.material_count + 1U) * sizeof(u32);
    const u64 pixel_bytes = pixels * sizeof(u32);
    const u64 resolved_bytes = pixels * sizeof(f32) * 4;

    rhi::CommandBuffer& commands = *device_.command_buffer(*command);
    struct Copy {
        rhi::BufferHandle source;
        u64 bytes = 0;
    };
    const Copy copies[] = {{visbuffer_, sample_bytes},
                           {bin_counts_, count_bytes},
                           {bin_offsets_, offset_bytes},
                           {bin_pixels_, pixel_bytes},
                           {resolved_, resolved_bytes}};
    u64 destination = 0;
    for (const auto& copy : copies) {
        rhi::BufferCopy region;
        region.destination_offset = destination;
        region.size = copy.bytes;
        commands.copy_buffer(copy.source, readback_, Span<const rhi::BufferCopy>(&region, 1));
        destination += copy.bytes;
    }
    if (Status ended = device_.end_command_buffer(*command); !ended) {
        return ended;
    }
    rhi::SubmitInfo submit;
    submit.command_buffers = Span<const rhi::CommandBufferHandle>(&*command, 1);
    Expected<u64, Error> value = device_.submit(submit);
    if (!value) {
        return make_unexpected(value.error());
    }
    if (Status waited = device_.wait_timeline(rhi::QueueKind::Graphics, *value, 5'000'000'000ULL);
        !waited) {
        return waited;
    }

    const auto* bytes = static_cast<const u8*>(device_.buffer_mapped_pointer(readback_));
    if (bytes == nullptr) {
        return fail(ErrorCode::Internal, "VisbufferPass::read_back: the buffer is not mapped");
    }
    if (Status resized = out.samples.resize(pixels); !resized) {
        return resized;
    }
    std::memcpy(out.samples.data(), bytes, sample_bytes);
    u64 cursor = sample_bytes;
    if (Status resized = out.bin_counts.resize(options_.material_count); !resized) {
        return resized;
    }
    std::memcpy(out.bin_counts.data(), bytes + cursor, count_bytes);
    cursor += count_bytes;
    if (Status resized = out.bin_offsets.resize(options_.material_count + 1U); !resized) {
        return resized;
    }
    std::memcpy(out.bin_offsets.data(), bytes + cursor, offset_bytes);
    cursor += offset_bytes;
    if (Status resized = out.bin_pixels.resize(pixels); !resized) {
        return resized;
    }
    std::memcpy(out.bin_pixels.data(), bytes + cursor, pixel_bytes);
    cursor += pixel_bytes;
    if (Status resized = out.resolved.resize(pixels); !resized) {
        return resized;
    }
    std::memcpy(out.resolved.data(), bytes + cursor, resolved_bytes);
    return ok();
}

}  // namespace cy::rendering::vg
