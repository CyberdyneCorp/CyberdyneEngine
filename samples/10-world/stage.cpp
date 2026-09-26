#include "stage.h"

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/pipeline.h>
#include <cy/backends/rhi/validation.h>
#include <cy/core/jobs/parallel.h>
#include <cy/core/math/projection.h>
#include <cy/rendering/assembly/capture_manifest.h>
#include <cy/rendering/assembly/frame_assembly.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/pipeline/frame_bindings.h>
#include <cy/rendering/pipeline/frame_pipelines.h>
#include <cy/rendering/pipeline/frame_recorder.h>
#include <cy/water/shading.h>

#include <cy_features.h>

#if defined(CY_RENDERER_VULKAN)
#    include <cy/backends/rhi/vulkan/vulkan_backend.h>
#endif
#if defined(CY_RENDERER_METAL)
#    include <cy/backends/rhi-metal/backend.h>
#endif

#include "golden.h"
#include "shaders/world_msl.h"
#include "shaders/world_spirv.h"
#include "shaders/world_visual_msl.h"
#include "shaders/world_visual_spirv.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

namespace cy::sample::world {
namespace {

using cy::rendering::PassContext;
using cy::rendering::ResourceId;
using rhi::Access;
using rhi::QueueKind;

// THE FRAME'S OWN VOCABULARY, brought in by name. M11.c task 3.1: before this rung
// `samples/10-world` linked `cy::rendering-graph`, `cy::rendering-sky` and `cy::rhi` and NOTHING
// ELSE under `src/rendering/`, so the largest picture this project publishes went through no
// assembled frame, no tone mapping and no post chain at all. These are what that sentence stops
// being true.
using cy::rendering::FramePassKind;
using cy::rendering::kInvalidResource;
using cy::rendering::assembly::AssemblyDescription;
using cy::rendering::assembly::AssemblyReport;
using cy::rendering::assembly::AssemblyView;
using cy::rendering::assembly::CaptureManifest;
using cy::rendering::assembly::CaptureProvenance;
using cy::rendering::assembly::CapturePurpose;
using cy::rendering::assembly::FrameAssembly;
using cy::rendering::assembly::FrameSinks;
using cy::rendering::pipeline::FrameBindings;
using cy::rendering::pipeline::FramePipelineKind;
using cy::rendering::pipeline::FramePipelines;
using cy::rendering::pipeline::GlobalsData;

/// The frame's HDR scene colour, which is what makes tone mapping a transformation rather than a
/// no-op: the world's own shading writes radiance here and the resolve is what turns it into
/// something a display can show.
constexpr rhi::Format kSceneFormat = rhi::Format::Rgba16Sfloat;
constexpr rhi::Format kOutputFormat = rhi::Format::Rgba8Unorm;

/// The sky dome's radius, in metres. Inside the far plane and outside everything else, so the dome
/// is depth-tested like any other geometry and no second pipeline state is needed for it.
constexpr f32 kSkyRadius = 9'000.0F;
constexpr f32 kFarPlane = 24'000.0F;
constexpr f32 kNearPlane = 1.0F;
/// The vertical field of view, in radians. Named because three places need the same number now:
/// the projection, the cull view and the cluster grid's slice mapping.
constexpr f32 kFieldOfView = 0.95F;

/// The most plants drawn in one frame, and the distance beyond which none is.
///
/// A CAP AND NOT A CULL: this artefact has no hierarchical culling, no impostor ladder and no
/// aggregate tier — `foliage` ships all three and `FoliageBudget::ladder()` prices them, and
/// binding them to a render budget is renderer work M10 did not do. So the proxies nearest the
/// camera are drawn, the rest are not, and the number actually drawn is reported every frame rather
/// than implied.
constexpr u32 kMaxDrawnPlants = 7'000;
constexpr f32 kPlantDrawMetres = 620.0F;

/// Triangles in one plant proxy: eight around the crown cone and six around the three-sided trunk.
constexpr u32 kCrownSides = 8;
constexpr u32 kTrunkSides = 3;
constexpr u32 kPlantVertices = (kCrownSides * 3) + (kTrunkSides * 2 * 3);
/// Plant proxies one job writes. Each is about fifty flops of trigonometry and a square root per
/// face; a few hundred of them amortise the task.
constexpr u64 kProxiesPerJob = 256;

[[nodiscard]] f64 now_millis() noexcept {
    const auto at = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<f64, std::milli>(at).count();
}

/// The push block, which is `WorldPush` in shaders/world.slang. Four explicit rows and four
/// float4s; see that file for why the matrix is not a `float4x4`.
struct WorldPush {
    f32 row0[4] = {1.0F, 0.0F, 0.0F, 0.0F};
    f32 row1[4] = {0.0F, 1.0F, 0.0F, 0.0F};
    f32 row2[4] = {0.0F, 0.0F, 1.0F, 0.0F};
    f32 row3[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    f32 light[4] = {0.0F, -1.0F, 0.0F, 0.0F};
    f32 eye[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    f32 sun[4] = {1.0F, 1.0F, 1.0F, 0.0F};
    f32 ambient[4] = {0.1F, 0.1F, 0.1F, 0.0F};
};

static_assert(sizeof(WorldPush) == 128,
              "the push block must fit the 128-byte portability limit exactly");

struct VisualPush {
    u32 terrain_count = 0;
    u32 sky_start = 0;
    u32 sky_count = 0;
    u32 water_start = 0;
    u32 water_count = 0;
    u32 foam_resolution = 128;
    u32 frame_index = 0;
    u32 cloud_seed = 0;
    f32 time_seconds = 0.0F;
    f32 delta_seconds = 0.0F;
    f32 wetness = 0.0F;
    f32 snow_depth = 0.0F;
    f32 cloud_coverage = 0.0F;
    f32 sun_height = 0.0F;
    f32 exposure = 1.0F;
    f32 unused_float = 0.0F;
    f32 field_origin[4] = {};
};

static_assert(sizeof(VisualPush) == 80);

struct VisualPassState {
    rhi::ComputePipelineHandle pipeline;
    rhi::PipelineLayoutHandle layout;
    rhi::DescriptorSetHandle descriptors;
    VisualPush push;
    u32 groups = 0;
    u32* dispatches = nullptr;
};

void record_visual(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<VisualPassState*>(user);
    context.commands->bind_compute_pipeline(state->pipeline);
    context.commands->bind_descriptor_sets(
        state->layout, 0, Span<const rhi::DescriptorSetHandle>(&state->descriptors, 1));
    context.commands->push_constants(
        state->layout, rhi::ShaderStage::Compute, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&state->push), sizeof(VisualPush)));
    context.commands->dispatch(state->groups, 1, 1);
    if (state->dispatches != nullptr) {
        ++*state->dispatches;
    }
}

void write_row(f32 (&out)[4], Vec4 row) noexcept {
    out[0] = row.x;
    out[1] = row.y;
    out[2] = row.z;
    out[3] = row.w;
}

void write_vec3(f32 (&out)[4], Vec3 value, f32 w) noexcept {
    out[0] = value.x;
    out[1] = value.y;
    out[2] = value.z;
    out[3] = w;
}

/// One run of the frame: a vertex range, an index range and the push block it is drawn with.
struct DrawRun {
    u32 first_index = 0;
    u32 index_count = 0;
    i32 vertex_offset = 0;
    bool dynamic = false;
    WorldPush push;
};

struct DrawState {
    const cy::rendering::GraphExecutor* executor = nullptr;
    rhi::GraphicsPipelineHandle pipeline;
    rhi::PipelineLayoutHandle layout;
    rhi::DescriptorSetHandle cloud_shadow;
    rhi::BufferHandle static_vertices;
    rhi::BufferHandle static_colours;
    rhi::BufferHandle static_indices;
    rhi::BufferHandle dynamic_vertices;
    rhi::BufferHandle dynamic_colours;
    rhi::BufferHandle dynamic_indices;
    ResourceId color = cy::rendering::kInvalidResource;
    ResourceId depth = cy::rendering::kInvalidResource;
    u32 width = 0;
    u32 height = 0;
    DrawRun runs[5];
    u32 run_count = 0;
};

struct ReadbackState {
    const cy::rendering::GraphExecutor* executor = nullptr;
    ResourceId color = cy::rendering::kInvalidResource;
    rhi::BufferHandle buffer;
    u32 width = 0;
    u32 height = 0;
};

void record_draw(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<DrawState*>(user);

    rhi::RenderAttachment color;
    color.view = state->executor->view(state->color);
    color.load = rhi::LoadOp::Clear;
    color.store = rhi::StoreOp::Store;
    // THE TARGET IS THE ASSEMBLED FRAME'S SCENE COLOUR NOW, not a texture this file created. It is
    // `Rgba16Sfloat` and linear, and the values written into it are radiance rather than display
    // values — which is what makes the post chain's exposure and tonemap stages do something.
    // Black, and NOT a stand-in sky: the dome covers every pixel the world does not, so a frame in
    // which black is visible is a frame in which the dome failed to draw — which is information a
    // plausible clear colour would have hidden.
    color.clear.color[0] = 0.0F;
    color.clear.color[1] = 0.0F;
    color.clear.color[2] = 0.0F;
    color.clear.color[3] = 1.0F;

    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, state->width, state->height};
    info.color_attachments = Span<const rhi::RenderAttachment>(&color, 1);
    info.depth_attachment.view = state->executor->view(state->depth);
    info.depth_attachment.load = rhi::LoadOp::Clear;
    info.depth_attachment.store = rhi::StoreOp::Store;
    // Reversed Z, so the clear is zero and a nearer fragment has a GREATER depth.
    info.depth_attachment.clear = rhi::reversed_z_depth_clear();

    context.commands->begin_rendering(info);
    context.commands->set_viewport(rhi::Viewport{0.0F, 0.0F, static_cast<f32>(state->width),
                                                 static_cast<f32>(state->height), 0.0F, 1.0F});
    context.commands->set_scissor(rhi::Rect2D{0, 0, state->width, state->height});
    context.commands->bind_graphics_pipeline(state->pipeline);
    context.commands->bind_descriptor_sets(
        state->layout, 0, Span<const rhi::DescriptorSetHandle>(&state->cloud_shadow, 1));

    const u64 offsets[2] = {0, 0};
    for (u32 index = 0; index < state->run_count; ++index) {
        const DrawRun& run = state->runs[index];
        if (run.index_count == 0) {
            continue;
        }
        const rhi::BufferHandle streams[2] = {
            run.dynamic ? state->dynamic_vertices : state->static_vertices,
            run.dynamic ? state->dynamic_colours : state->static_colours};
        context.commands->bind_vertex_buffers(0, Span<const rhi::BufferHandle>(streams, 2),
                                              Span<const u64>(offsets, 2));
        context.commands->bind_index_buffer(
            run.dynamic ? state->dynamic_indices : state->static_indices, 0, true);
        context.commands->push_constants(
            state->layout, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
            Span<const u8>(reinterpret_cast<const u8*>(&run.push), sizeof(WorldPush)));
        context.commands->draw_indexed(run.index_count, 1, run.first_index, run.vertex_offset, 0);
    }
    context.commands->end_rendering();
}

void record_readback(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<ReadbackState*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{state->width, state->height, 1};
    context.commands->copy_texture_to_buffer(state->executor->texture(state->color), state->buffer,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

/// What the tonemapping resolve reads. `cy::rendering-pipeline` owns the pipeline and the sets;
/// this records the one triangle that runs them.
struct ResolveState {
    const cy::rendering::GraphExecutor* executor = nullptr;
    FramePipelines* pipelines = nullptr;
    FrameBindings* bindings = nullptr;
    ResourceId scene = kInvalidResource;
    ResourceId output = kInvalidResource;
    u32 width = 0;
    u32 height = 0;
};

/// THE POST CHAIN, RECORDED. M11.c task 3.1.
///
/// `cy/fullscreen.slang`'s resolve — the engine's own, the same one `cy::rendering-pipeline` binds
/// for every other caller — divides the scene colour by the exposure the globals block carries and
/// runs the tonemap curve `rendering-post-processing` fixes at step 12. Before this rung the world
/// picture reached its PNG straight out of the rasteriser, so `exposure_stops` in the block below
/// is the first exposure this artefact has ever had.
void record_resolve(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<ResolveState*>(user);
    // The scene colour is a transient the graph realised a moment ago, so its view cannot be named
    // before this point — the same reason `FrameRecorder` writes the pass set here.
    if (Status bound = state->bindings->bind_scene_color(state->executor->view(state->scene));
        !bound) {
        return;
    }
    rhi::RenderAttachment color;
    color.view = state->executor->view(state->output);
    color.load = rhi::LoadOp::Clear;
    color.store = rhi::StoreOp::Store;
    color.clear.color[3] = 1.0F;

    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, state->width, state->height};
    info.color_attachments = Span<const rhi::RenderAttachment>(&color, 1);

    context.commands->begin_rendering(info);
    context.commands->set_viewport(rhi::Viewport{0.0F, 0.0F, static_cast<f32>(state->width),
                                                 static_cast<f32>(state->height), 0.0F, 1.0F});
    context.commands->set_scissor(rhi::Rect2D{0, 0, state->width, state->height});
    context.commands->bind_descriptor_sets(state->pipelines->layout(), 0, state->bindings->sets());
    context.commands->bind_graphics_pipeline(
        state->pipelines->pipeline(FramePipelineKind::Resolve));
    // One oversized triangle from `SV_VertexID`; the resolve pipeline has no vertex bindings.
    context.commands->draw(3, 1, 0, 0);
    context.commands->end_rendering();
}

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
    }
    std::fprintf(stderr, "RHI validation %s: %s\n",
                 severity == rhi::ValidationSeverity::Error ? "error" : "warning",
                 message != nullptr ? message : "");
}

/// Bytes one job copies in a parallel upload. Large enough that a task is cheap next to it.
constexpr u64 kUploadBytesPerJob = u64{512} * 1024;

[[nodiscard]] Status upload_bytes(rhi::Device& device, rhi::BufferHandle buffer, const void* source,
                                  u64 bytes, jobs::JobSystem* jobs = nullptr) noexcept {
    if (bytes == 0) {
        return ok();
    }
    void* mapped = device.buffer_mapped_pointer(buffer);
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "a staged buffer is not mapped");
    }
    // A BLOCK COPY, NOT A BYTE LOOP. The dynamic stream is about eleven megabytes a frame (seven
    // thousand plant proxies of forty-two vertices, plus the sea and the sky), and GCC at -O2 did
    // not turn the byte-at-a-time loop this was into one: a sampling profile of the take put about
    // 2 ms of every frame there. Given workers, the copy is split into disjoint chunks across them,
    // because one thread does not saturate the write path into the mapped buffer.
    auto* to = static_cast<u8*>(mapped);
    const auto* from = static_cast<const u8*>(source);
    if (jobs == nullptr || bytes <= kUploadBytesPerJob) {
        std::memcpy(to, from, static_cast<usize>(bytes));
        return ok();
    }
    const u64 chunks = (bytes + kUploadBytesPerJob - 1) / kUploadBytesPerJob;
    auto body = [to, from, bytes](const jobs::TaskContext& /*task*/, u64 begin, u64 end) noexcept {
        const u64 first = begin * kUploadBytesPerJob;
        const u64 last = std::min(end * kUploadBytesPerJob, bytes);
        std::memcpy(to + first, from + first, static_cast<usize>(last - first));
    };
    return jobs::parallel_for(*jobs, chunks, 1, body, "world.upload");
}

/// How much of a proxy's height is trunk. A boulder has none at all: its cone starts at the ground
/// and is wide, which is what makes the generator's scatter read as rock rather than as a short
/// conifer.
[[nodiscard]] f32 trunk_fraction_of(PlantKind kind) noexcept {
    switch (kind) {
        case PlantKind::Tree:
            return 0.42F;
        case PlantKind::Bush:
            return 0.15F;
        default:
            return 0.0F;
    }
}

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    if (value < 0.0F) {
        return 0.0F;
    }
    return value > 1.0F ? 1.0F : value;
}

struct U32x3 {
    u32 x = 0;
    u32 y = 0;
    u32 z = 0;
};

[[nodiscard]] U32x3 hash_pcg3d(U32x3 value) noexcept {
    U32x3 result{(value.x * 1664525U) + 1013904223U, (value.y * 1664525U) + 1013904223U,
                 (value.z * 1664525U) + 1013904223U};
    result.x += result.y * result.z;
    result.y += result.z * result.x;
    result.z += result.x * result.y;
    result.x ^= result.x >> 16U;
    result.y ^= result.y >> 16U;
    result.z ^= result.z >> 16U;
    result.x += result.y * result.z;
    result.y += result.z * result.x;
    result.z += result.x * result.y;
    return result;
}

[[nodiscard]] f32 value_noise(Vec3 position) noexcept {
    const Vec3 cell{std::floor(position.x), std::floor(position.y), std::floor(position.z)};
    const Vec3 fraction = position - cell;
    const Vec3 weight{fraction.x * fraction.x * (3.0F - (2.0F * fraction.x)),
                      fraction.y * fraction.y * (3.0F - (2.0F * fraction.y)),
                      fraction.z * fraction.z * (3.0F - (2.0F * fraction.z))};
    const U32x3 base{static_cast<u32>(static_cast<i32>(cell.x) + 1024),
                     static_cast<u32>(static_cast<i32>(cell.y) + 1024),
                     static_cast<u32>(static_cast<i32>(cell.z) + 1024)};
    f32 result = 0.0F;
    for (u32 corner = 0; corner < 8U; ++corner) {
        const U32x3 offset{corner & 1U, (corner >> 1U) & 1U, (corner >> 2U) & 1U};
        const U32x3 hashed =
            hash_pcg3d(U32x3{base.x + offset.x, base.y + offset.y, base.z + offset.z});
        const f32 sample = static_cast<f32>(hashed.x >> 8U) * (1.0F / 16777216.0F);
        const f32 blend_x = offset.x != 0U ? weight.x : 1.0F - weight.x;
        const f32 blend_y = offset.y != 0U ? weight.y : 1.0F - weight.y;
        const f32 blend_z = offset.z != 0U ? weight.z : 1.0F - weight.z;
        result += sample * blend_x * blend_y * blend_z;
    }
    return result;
}

[[nodiscard]] f32 smoothstep(f32 low, f32 high, f32 value) noexcept {
    const f32 t = clamp01((value - low) / (high - low));
    return t * t * (3.0F - (2.0F * t));
}

[[nodiscard]] Vec3 blend(Vec3 from, Vec3 to, f32 amount) noexcept {
    return Vec3{from.x + ((to.x - from.x) * amount), from.y + ((to.y - from.y) * amount),
                from.z + ((to.z - from.z) * amount)};
}

[[nodiscard]] f32 cloud_fbm(Vec3 position, Vec3 seed_offset) noexcept {
    const f32 coarse = value_noise(position + seed_offset);
    const f32 detail = value_noise((position * 2.03F) + seed_offset + Vec3{17.0F, 31.0F, 47.0F});
    return (coarse + (detail * 0.5F)) / 1.5F;
}

[[nodiscard]] f32 cloud_density_reference(Vec3 position, const VisualPush& visual,
                                          Vec3 seed_offset) noexcept {
    const f32 height_fraction = clamp01((position.y - 900.0F) / 3400.0F);
    const f32 profile = smoothstep(0.0F, 0.18F, height_fraction) *
                        (1.0F - smoothstep(0.68F, 1.0F, height_fraction));
    const Vec3 advected =
        position - Vec3{visual.time_seconds * 9.0F, 0.0F, visual.time_seconds * 2.5F};
    const f32 base = cloud_fbm(advected / 5200.0F, seed_offset);
    const f32 threshold = 0.70F - (visual.cloud_coverage * 0.38F);
    f32 shape = clamp01((base - threshold) * 4.5F);
    const f32 erosion = value_noise((advected / 780.0F) + seed_offset + Vec3{71.0F, 11.0F, 29.0F});
    shape *= 0.62F + (erosion * 0.58F);
    return shape * profile;
}

[[nodiscard]] Vec3 compose_cloud_reference(Vec3 direction, const VisualPush& visual) noexcept {
    const f32 horizon = clamp01((direction.y * 0.5F) + 0.5F);
    const f32 daylight = clamp01((visual.sun_height * 3.0F) + 0.25F);
    Vec3 clear = blend(Vec3{0.002F, 0.005F, 0.018F}, Vec3{0.16F, 0.43F, 0.92F}, daylight);
    clear = clear * (0.16F + (0.84F * horizon));
    if (direction.y <= 0.025F) {
        return clear;
    }

    const f32 seed = static_cast<f32>(visual.cloud_seed & 255U);
    const Vec3 seed_offset{seed * 0.37F, seed * 0.19F, seed * 0.53F};
    const Vec3 origin{visual.field_origin[2], 0.0F, visual.field_origin[3]};
    const f32 start = 900.0F / direction.y;
    const f32 step_length = 3400.0F / (direction.y * 12.0F);
    f32 transmittance = 1.0F;
    Vec3 scattering{0.0F, 0.0F, 0.0F};
    for (u32 step = 0; step < 12U; ++step) {
        const f32 distance = start + (step_length * (static_cast<f32>(step) + 0.5F));
        const Vec3 position = origin + (direction * distance);
        const f32 density = cloud_density_reference(position, visual, seed_offset);
        const f32 extinction = 1.0F - std::exp(-density * 0.48F);
        const f32 height_fraction = clamp01((position.y - 900.0F) / 3400.0F);
        const Vec3 bright = Vec3{1.00F, 0.94F, 0.84F} * (0.30F + (daylight * 0.70F));
        const Vec3 source =
            blend(Vec3{0.14F, 0.17F, 0.22F}, bright, 0.25F + (height_fraction * 0.75F));
        scattering = scattering + (source * (transmittance * extinction));
        transmittance *= 1.0F - extinction;
        if (transmittance < 0.015F) {
            break;
        }
    }
    return (clear * transmittance) + scattering;
}

[[nodiscard]] Vec3 normalised(Vec3 value) noexcept {
    const f32 magnitude =
        std::sqrt((value.x * value.x) + (value.y * value.y) + (value.z * value.z));
    return magnitude > 1e-6F ? Vec3{value.x / magnitude, value.y / magnitude, value.z / magnitude}
                             : Vec3{0.0F, 1.0F, 0.0F};
}

[[nodiscard]] Vec3 face_normal(Vec3 a, Vec3 b, Vec3 c) noexcept {
    const Vec3 u{b.x - a.x, b.y - a.y, b.z - a.z};
    const Vec3 v{c.x - a.x, c.y - a.y, c.z - a.z};
    return normalised(
        Vec3{(u.y * v.z) - (u.z * v.y), (u.z * v.x) - (u.x * v.z), (u.x * v.y) - (u.y * v.x)});
}

}  // namespace

/// Everything that needs a device, so the header names none of it and a build without the Vulkan
/// backend still compiles this file.
struct Stage::Device {
    explicit Device(Allocator& allocator) noexcept : assembly(allocator) {}

    Expected<rhi::Device*, Error> handle = fail(ErrorCode::Unavailable, "not created");
    rhi::BackendSelection selection{};
    u32 validation_errors = 0;

    rhi::ShaderModuleHandle vertex;
    rhi::ShaderModuleHandle fragment;
    rhi::PipelineLayoutHandle layout;
    rhi::GraphicsPipelineHandle pipeline;
    rhi::ShaderModuleHandle visual_shaders[3];
    rhi::DescriptorSetLayoutHandle visual_set_layout;
    rhi::PipelineLayoutHandle visual_layout;
    rhi::ComputePipelineHandle visual_pipelines[3];
    rhi::DescriptorSetHandle visual_set;
    rhi::BufferHandle static_vertices;
    rhi::BufferHandle static_colours;
    rhi::BufferHandle static_indices;
    rhi::BufferHandle dynamic_vertices;
    rhi::BufferHandle dynamic_colours;
    rhi::BufferHandle dynamic_indices;
    rhi::BufferHandle foam_previous;
    rhi::BufferHandle foam_next;
    rhi::BufferHandle terrain_fields[4];
    rhi::BufferHandle readback;
    /// Set 0 of the lit pipeline: the cloud shadow field's image and where it sits. See
    /// `shaders/world.slang`, which reads both in the fragment stage.
    rhi::DescriptorSetLayoutHandle world_set_layout;
    rhi::DescriptorSetHandle world_set;
    rhi::BufferHandle cloud_shadow_field;
    rhi::BufferHandle cloud_shadow_placement;

    // --- THE ASSEMBLED FRAME. M11.c task 3.1. --------------------------------------------------
    //
    // `FrameAssembly` is the renderer's own frame: the post chain decides the feature set, the
    // temporal framework advances one jitter, the shadow cache spends its budget, the sky table
    // updates, and thirteen stages go into the graph in the specification's order. This program
    // supplies the record callback for the stage its geometry belongs in and `cy::rendering-
    // pipeline` supplies the tonemapping resolve; neither existed in this file before M11.c.
    FrameAssembly assembly;
    FramePipelines pipelines;
    FrameBindings bindings;
    /// The display-referred image the resolve writes and the readback copies. Imported into the
    /// frame every frame, because the resolve clears it.
    rhi::TextureHandle output;
    /// The sun, as the frame's light list. One directional light, which is what this world has.
    cy::render::LightDescription sun;
    VisualPush last_visual;
    bool frame_ready = false;
};

Stage::Stage(Allocator& allocator) noexcept
    : allocator_(&allocator),
      dynamic_vertices_(allocator),
      dynamic_colours_(allocator),
      dynamic_indices_(allocator),
      drawn_plants_(allocator),
      terrain_colours_(allocator),
      pixels_(allocator) {}

Stage::~Stage() {
    close();
}

const char* Stage::absence() const noexcept {
    if (device_ == nullptr) {
        return "the stage was never opened";
    }
    // EMPTY IS AS UNHELPFUL AS NULL, and it is the case that actually happens: a host whose Vulkan
    // loader finds no driver at all reports a selection with a reason that is present and blank,
    // and the message a reader then gets is "no graphics device answered ()". Measured by running
    // this program with `VK_DRIVER_FILES` pointed at a file that does not exist.
    const char* reason = device_->selection.reason;
    return (reason != nullptr && reason[0] != '\0') ? reason
                                                    : "no backend reported a reason, which on this "
                                                      "platform usually means the Vulkan loader "
                                                      "found no driver";
}

const char* Stage::backend_name() const noexcept {
    return device_ != nullptr ? device_->selection.selected : "none";
}

const char* Stage::device_name() const noexcept {
    return device_ != nullptr && device_->handle.has_value()
               ? device_->handle.value()->capabilities().device_name()
               : "none";
}

void Stage::set_jobs(jobs::JobSystem* jobs) noexcept {
    jobs_ = jobs;
    if (device_ != nullptr) {
        device_->assembly.set_jobs(jobs);
    }
}

Status Stage::open(u32 width, u32 height) noexcept {
    width_ = width;
    height_ = height;
    device_ = new (std::nothrow) Device(*allocator_);
    if (device_ == nullptr) {
        return fail(ErrorCode::OutOfMemory, "the stage did not allocate");
    }
#if defined(CY_RENDERER_VULKAN)
    (void)rhi::vulkan::register_vulkan_backend();
#endif
#if defined(CY_RENDERER_METAL)
    (void)rhi::metal::register_metal_backend();
#endif
    (void)rhi::null::register_null_backend();

    rhi::DeviceDescription description;
    description.application_name = "cy_sample_world";
    // VALIDATION ON, AND ITS ERRORS REPORTED AS A NUMBER rather than as a log line nobody reads.
    description.enable_validation = true;
    description.enable_synchronisation_validation = true;
#if defined(__APPLE__) && defined(CY_RENDERER_METAL)
    constexpr const char* backend = rhi::metal::kMetalBackendName;
#else
    constexpr const char* backend = "vulkan";
#endif
    device_->handle = rhi::create_device(*allocator_, backend, description, device_->selection);
    if (!device_->handle.has_value()) {
        return ok();
    }
    const rhi::BackendKind selected = device_->handle.value()->capabilities().backend();
    if (selected != rhi::BackendKind::Vulkan && selected != rhi::BackendKind::Metal) {
        return ok();
    }
    device_->handle.value()->set_validation_callback(&count_validation,
                                                     &device_->validation_errors);
    available_ = true;
    return create_pipeline();
}

Status Stage::create_pipeline() noexcept {
    rhi::Device& device = *device_->handle.value();

    const bool metal = device.capabilities().native_shader_format() == rhi::ShaderFormat::Msl;

    rhi::ShaderModuleDescription vertex;
    vertex.name = "world vertex";
    vertex.stage = rhi::ShaderStage::Vertex;
    if (metal) {
        vertex.entry_point = "worldVertex";
        vertex.native = Span<const u8>(reinterpret_cast<const u8*>(kWorldVertexMsl),
                                       sizeof(kWorldVertexMsl) - 1);
        vertex.native_format = rhi::ShaderFormat::Msl;
    } else {
        vertex.entry_point = "main";
        vertex.spirv = Span<const u32>(kWorldVertexSpirv, sizeof(kWorldVertexSpirv) / sizeof(u32));
    }
    Expected<rhi::ShaderModuleHandle, Error> vertex_module = device.create_shader_module(vertex);
    if (!vertex_module) {
        return make_unexpected(vertex_module.error());
    }
    device_->vertex = *vertex_module;

    rhi::ShaderModuleDescription fragment;
    fragment.name = "world fragment";
    fragment.stage = rhi::ShaderStage::Fragment;
    if (metal) {
        fragment.entry_point = "worldFragment";
        fragment.native = Span<const u8>(reinterpret_cast<const u8*>(kWorldFragmentMsl),
                                         sizeof(kWorldFragmentMsl) - 1);
        fragment.native_format = rhi::ShaderFormat::Msl;
    } else {
        fragment.entry_point = "main";
        fragment.spirv =
            Span<const u32>(kWorldFragmentSpirv, sizeof(kWorldFragmentSpirv) / sizeof(u32));
    }
    Expected<rhi::ShaderModuleHandle, Error> fragment_module =
        device.create_shader_module(fragment);
    if (!fragment_module) {
        return make_unexpected(fragment_module.error());
    }
    device_->fragment = *fragment_module;

    if (Status bound = create_cloud_shadow_binding(); !bound) {
        return bound;
    }
    const rhi::PushConstantRange range{rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
                                       sizeof(WorldPush)};
    rhi::PipelineLayoutDescription layout;
    layout.name = "world layout";
    layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(&device_->world_set_layout, 1);
    layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> layout_handle =
        device.create_pipeline_layout(layout);
    if (!layout_handle) {
        return make_unexpected(layout_handle.error());
    }
    device_->layout = *layout_handle;

    // TWO BINDINGS, and the split is the point: geometry that never moves is uploaded once, and the
    // colours the environment substrate produces are uploaded every frame. Interleaving them would
    // mean re-sending a hundred and fifty thousand terrain positions so that a snowfall can be
    // seen.
    const rhi::VertexBinding bindings[2] = {{0, sizeof(Vertex), rhi::VertexInputRate::PerVertex},
                                            {1, sizeof(Vec3), rhi::VertexInputRate::PerVertex}};
    const rhi::VertexAttribute attributes[3] = {{0, 0, rhi::Format::Rgb32Sfloat, 0},
                                                {1, 0, rhi::Format::Rgb32Sfloat, sizeof(Vec3)},
                                                {2, 1, rhi::Format::Rgb32Sfloat, 0}};

    // THE FRAME'S SCENE COLOUR AND NOT THE SWAPCHAIN'S FORMAT. M11.c task 3.1: this pipeline now
    // draws into `FrameResources::color`, which is linear `Rgba16Sfloat`, and the tonemapping
    // resolve is what produces the 8-bit image. A pipeline created for `Rgba8Unorm` against an
    // `Rgba16Sfloat` attachment is a dynamic-rendering format mismatch — a validation error at draw
    // time from a pipeline created long before.
    rhi::ColorAttachmentState color;
    color.format = kSceneFormat;

    rhi::GraphicsPipelineDescription pipeline;
    pipeline.name = "world";
    pipeline.layout = device_->layout;
    pipeline.vertex_shader = device_->vertex;
    pipeline.fragment_shader = device_->fragment;
    pipeline.vertex_bindings = Span<const rhi::VertexBinding>(bindings, 2);
    pipeline.vertex_attributes = Span<const rhi::VertexAttribute>(attributes, 3);
    pipeline.color_attachments = Span<const rhi::ColorAttachmentState>(&color, 1);
    // NO CULLING. The sky dome is seen from inside, the ocean patch's winding is the surface
    // generator's and a plant proxy's cone is closed at neither end; the fragment stage turns the
    // facet towards the eye instead, so nothing disappears for a reason a viewer cannot see.
    pipeline.rasterisation.cull_mode = rhi::CullMode::None;
    pipeline.depth_stencil.format = rhi::Format::D32Sfloat;
    pipeline.depth_stencil.depth_test_enable = true;
    pipeline.depth_stencil.depth_write_enable = true;
    Expected<rhi::GraphicsPipelineHandle, Error> created =
        device.create_graphics_pipeline(pipeline);
    if (!created) {
        return make_unexpected(created.error());
    }
    device_->pipeline = *created;

    rhi::BufferDescription readback;
    readback.name = "world colour readback";
    readback.size = static_cast<u64>(width_) * height_ * sizeof(u32);
    readback.usage = rhi::BufferUsage::TransferDestination;
    readback.memory = rhi::MemoryUse::Readback;
    Expected<rhi::BufferHandle, Error> buffer = device.create_buffer(readback);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->readback = *buffer;
    if (Status sized = pixels_.resize(static_cast<usize>(width_) * height_); !sized) {
        return sized;
    }
    return create_frame();
}

// ================================================================================================
// THE ASSEMBLED FRAME. M11.c task 3.1.
// ================================================================================================
//
// THE SENTENCE THIS METHOD EXISTS TO MAKE FALSE, from this rung's own ledger: "`samples/10-world`
// links neither `cy::rendering-assembly` nor the post, temporal, forward or shadow libraries, so
// the world picture never passes through tone mapping or anti-aliasing at all."
//
// WHAT IS CLAIMED, AND IT IS NARROWER THAN "THE WORLD GOES THROUGH THE RENDERER". The frame is the
// engine's: `FrameAssembly` decides the feature set from the post chain, advances the temporal
// framework, requests the sun's shadow pages, updates the sky table and declares the stages into
// the render graph, and `cy::rendering-pipeline`'s resolve tonemaps the result. The GEOMETRY is
// still this file's — the world is not in a mesh table, its vertices are not the render server's
// three streams, and its shading is still per-vertex colour computed on the processor. So this
// program draws ITS geometry INSIDE the engine's frame through `FrameSinks::passes`, which is
// exactly the seam `ForwardFrame` documents: "ForwardFrame knows the frame STRUCTURE and the caller
// knows how to draw".
//
// WHAT IS STILL ABSENT AND IS NOT CLAIMED: no anti-aliasing IN THIS PROGRAM. The engine's temporal
// resolve exists — `cy::rendering-pipeline`'s `FrameRecorder` records `FramePassKind::Temporal`
// with `cy/fullscreen.slang`'s `temporalResolve`, and `render.pipeline` measures it accumulating —
// but this program supplies its own sinks, draws its own geometry with no velocity output and no
// jitter, and runs no depth prepass to write motion vectors. Switching `temporal_antialiasing` on
// here would put a stage in the manifest that no pass of THIS frame ran, which is the exact
// dishonesty `capture_manifest.h` exists to detect. The chain this frame runs is the three
// unconditional stages, and the manifest says three.

Status Stage::create_frame() noexcept {
    rhi::Device& device = *device_->handle.value();

    AssemblyDescription description;
    description.width = width_;
    description.height = height_;
    description.near_plane = kNearPlane;
    description.far_plane = kFarPlane;
    description.clusters = cy::rendering::ClusterGridConfig{16, 8, 24};
    description.color_format = kSceneFormat;
    description.depth_format = rhi::Format::D32Sfloat;
    description.material_capacity = 4;
    description.max_draws = 16;
    description.max_instances = 16;
    // No instances reach the cull — see the header note — so the device dispatch would be a
    // dispatch over nothing. The CPU path is the same answer at no cost.
    description.gpu_culling = false;
    // NO DEPTH PREPASS, and the reason is a validation hazard rather than a preference: this
    // program's geometry is not in the frame's draw list — it draws its own buffers in the opaque
    // stage — so a declared prepass would record nothing, and the opaque pass's `LoadOp::Clear` on
    // a depth target the graph had barriered for a READER is a write-after-write the
    // synchronisation validator reports three times a frame. With the prepass off, `ForwardFrame`
    // declares the opaque pass as the depth WRITER, which is what this frame actually is.
    description.depth_prepass = false;
    // THE LOWEST SKY TABLE, DELIBERATELY. This world composes its own sky on the processor, dome
    // vertex by dome vertex, and the assembly's table is read only for the frame's ambient term.
    // A higher quality here would be a second atmosphere integration per frame for a number one
    // shader reads.
    description.sky = cy::rendering::sky::SkyTableQuality::Low;
    // PINNED, because a capture has to be reproducible: `m10:world-still` requires two runs at one
    // seed to draw the byte-identical still, and `temporal-rendering`'s determinism requirement is
    // what makes that a property of the engine rather than of the machine.
    description.pin_jitter = true;
    if (Status made = device_->assembly.initialize(description); !made) {
        return made;
    }
    if (Status attached = device_->assembly.attach_device(device); !attached) {
        return attached;
    }

    cy::rendering::pipeline::PipelineSetup setup;
    setup.color_format = kSceneFormat;
    setup.depth_format = rhi::Format::D32Sfloat;
    setup.output_format = kOutputFormat;
    // Neither is recorded by this program: its geometry is one opaque pass of its own, and the
    // layer's transparent pipeline would be a pipeline state created for a pass nothing binds.
    setup.transparency = false;
    if (Status made = device_->pipelines.initialize(device, setup); !made) {
        return made;
    }

    Expected<cy::rendering::ClusterGrid, Error> grid = cy::rendering::make_cluster_grid(
        description.clusters, width_, height_, kNearPlane, kFarPlane);
    if (!grid.has_value()) {
        return make_unexpected(grid.error());
    }
    const cy::rendering::pipeline::BindingCapacity capacity =
        cy::rendering::pipeline::BindingCapacity::for_grid(*grid, description.max_draws,
                                                           description.max_instances,
                                                           description.material_capacity, 4);
    if (Status made = device_->bindings.initialize(device, device_->pipelines, capacity); !made) {
        return made;
    }

    rhi::TextureDescription output;
    output.name = "world output";
    output.format = kOutputFormat;
    output.extent = rhi::Extent3D{width_, height_, 1};
    output.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSource;
    Expected<rhi::TextureHandle, Error> created = device.create_texture(output);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    device_->output = *created;
    device_->frame_ready = true;
    return ok();
}

/// The cloud shadow placement, `WorldCloudShadow::placement` in shaders/world.slang.
struct CloudShadowPlacement {
    f32 origin_x = 0.0F;
    f32 origin_z = 0.0F;
    f32 enabled = 0.0F;
    f32 unused = 0.0F;
};

static_assert(sizeof(CloudShadowPlacement) == 16, "one float4, as the shader declares it");

/// Words in the placeholder image bound before the first frame: a field image's header and
/// nothing else. It is never read — the placement says "off" until a frame uploads a real one.
constexpr u64 kPlaceholderCloudShadowWords = 16;

Status Stage::create_cloud_shadow_binding() noexcept {
    rhi::Device& device = *device_->handle.value();
    rhi::DescriptorBinding bindings[2] = {};
    for (u32 index = 0; index < 2; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = rhi::DescriptorKind::StorageBuffer;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Fragment;
    }
    rhi::DescriptorSetLayoutDescription set_description;
    set_description.name = "world cloud shadow";
    set_description.bindings = Span<const rhi::DescriptorBinding>(bindings, 2);
    auto set_layout = device.create_descriptor_set_layout(set_description);
    if (!set_layout) {
        return make_unexpected(set_layout.error());
    }
    device_->world_set_layout = *set_layout;

    rhi::BufferDescription description;
    description.name = "world cloud shadow field";
    description.size = kPlaceholderCloudShadowWords * sizeof(u32);
    description.usage = rhi::BufferUsage::Storage;
    description.memory = rhi::MemoryUse::Upload;
    auto field = device.create_buffer(description);
    if (!field) {
        return make_unexpected(field.error());
    }
    device_->cloud_shadow_field = *field;
    cloud_shadow_bytes_ = description.size;
    std::memset(device.buffer_mapped_pointer(*field), 0, static_cast<usize>(description.size));

    description.name = "world cloud shadow placement";
    description.size = sizeof(CloudShadowPlacement);
    auto placement = device.create_buffer(description);
    if (!placement) {
        return make_unexpected(placement.error());
    }
    device_->cloud_shadow_placement = *placement;
    const CloudShadowPlacement off;
    std::memcpy(device.buffer_mapped_pointer(*placement), &off, sizeof(off));

    auto set = device.allocate_descriptor_set(device_->world_set_layout, false);
    if (!set) {
        return make_unexpected(set.error());
    }
    device_->world_set = *set;
    rhi::DescriptorWrite writes[2] = {};
    writes[0].binding = 0;
    writes[0].kind = rhi::DescriptorKind::StorageBuffer;
    writes[0].buffer = device_->cloud_shadow_field;
    writes[1].binding = 1;
    writes[1].kind = rhi::DescriptorKind::StorageBuffer;
    writes[1].buffer = device_->cloud_shadow_placement;
    return device.update_descriptor_set(device_->world_set,
                                        Span<const rhi::DescriptorWrite>(writes, 2));
}

Status Stage::upload_cloud_shadow(const World& world) noexcept {
    rhi::Device& device = *device_->handle.value();
    CloudShadowPlacement placement;
    if (world.cloud_shadows()) {
        auto image = world.cloud_shadow_image();
        if (!image) {
            return make_unexpected(image.error());
        }
        const u64 bytes = image->words.size() * sizeof(u32);
        if (bytes > cloud_shadow_bytes_) {
            // The image grows when the field's written area gains a tile. Every previous frame is
            // idle before `shoot()` uploads, so the buffer is replaced here and the set moved to
            // it before this frame is recorded — the terrain field images' own arrangement.
            rhi::BufferDescription description;
            description.name = "world cloud shadow field";
            description.size = bytes;
            description.usage = rhi::BufferUsage::Storage;
            description.memory = rhi::MemoryUse::Upload;
            auto grown = device.create_buffer(description);
            if (!grown) {
                return make_unexpected(grown.error());
            }
            rhi::DescriptorWrite write;
            write.binding = 0;
            write.kind = rhi::DescriptorKind::StorageBuffer;
            write.buffer = *grown;
            if (Status updated = device.update_descriptor_set(
                    device_->world_set, Span<const rhi::DescriptorWrite>(&write, 1));
                !updated) {
                device.destroy_buffer(*grown);
                return updated;
            }
            device.destroy_buffer(device_->cloud_shadow_field);
            device_->cloud_shadow_field = *grown;
            cloud_shadow_bytes_ = bytes;
        }
        if (Status uploaded =
                upload_bytes(device, device_->cloud_shadow_field, image->words.data(), bytes);
            !uploaded) {
            return uploaded;
        }
        // The f64 subtraction `environment::image_local()` makes, once: a fragment's world-relative
        // position plus this is its position relative to the image's origin corner.
        placement.origin_x = static_cast<f32>(world.centre().x - image->origin_x);
        placement.origin_z = static_cast<f32>(world.centre().z - image->origin_z);
        placement.enabled = 1.0F;
    }
    return upload_bytes(device, device_->cloud_shadow_placement, &placement, sizeof(placement));
}

Status Stage::create_visual_pipelines() noexcept {
    rhi::Device& device = *device_->handle.value();
    rhi::DescriptorBinding bindings[10] = {};
    for (u32 index = 0; index < 10; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = rhi::DescriptorKind::StorageBuffer;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription set_description;
    set_description.name = "world visual producers";
    set_description.bindings = Span<const rhi::DescriptorBinding>(bindings, 10);
    auto set_layout = device.create_descriptor_set_layout(set_description);
    if (!set_layout) {
        return make_unexpected(set_layout.error());
    }
    device_->visual_set_layout = *set_layout;

    const rhi::PushConstantRange range{rhi::ShaderStage::Compute, 0, sizeof(VisualPush)};
    rhi::PipelineLayoutDescription layout_description;
    layout_description.name = "world visual layout";
    layout_description.set_layouts =
        Span<const rhi::DescriptorSetLayoutHandle>(&device_->visual_set_layout, 1);
    layout_description.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    auto layout = device.create_pipeline_layout(layout_description);
    if (!layout) {
        return make_unexpected(layout.error());
    }
    device_->visual_layout = *layout;

    struct Request {
        const char* name;
        const char* entry;
        const u32* spirv;
        usize spirv_words;
        const char* msl;
        usize msl_bytes;
    };
    const Request requests[3] = {
        {"world terrain visual", "shadeTerrain", kWorldShadeTerrainSpirv,
         sizeof(kWorldShadeTerrainSpirv) / sizeof(u32), kWorldShadeTerrainMsl,
         sizeof(kWorldShadeTerrainMsl) - 1},
        {"world cloud visual", "shadeClouds", kWorldShadeCloudsSpirv,
         sizeof(kWorldShadeCloudsSpirv) / sizeof(u32), kWorldShadeCloudsMsl,
         sizeof(kWorldShadeCloudsMsl) - 1},
        {"world foam visual", "evolveFoam", kWorldEvolveFoamSpirv,
         sizeof(kWorldEvolveFoamSpirv) / sizeof(u32), kWorldEvolveFoamMsl,
         sizeof(kWorldEvolveFoamMsl) - 1},
    };
    const bool metal = device.capabilities().native_shader_format() == rhi::ShaderFormat::Msl;
    for (u32 index = 0; index < 3; ++index) {
        rhi::ShaderModuleDescription shader;
        shader.name = requests[index].name;
        shader.stage = rhi::ShaderStage::Compute;
        if (metal) {
            shader.entry_point = requests[index].entry;
            shader.native = Span<const u8>(reinterpret_cast<const u8*>(requests[index].msl),
                                           requests[index].msl_bytes);
            shader.native_format = rhi::ShaderFormat::Msl;
        } else {
            shader.entry_point = "main";
            shader.spirv = Span<const u32>(requests[index].spirv, requests[index].spirv_words);
        }
        auto module = device.create_shader_module(shader);
        if (!module) {
            return make_unexpected(module.error());
        }
        device_->visual_shaders[index] = *module;
        rhi::ComputePipelineDescription pipeline;
        pipeline.name = requests[index].name;
        pipeline.layout = device_->visual_layout;
        pipeline.shader = *module;
        pipeline.workgroup_size[0] = 64;
        auto created = device.create_compute_pipeline(pipeline);
        if (!created) {
            return make_unexpected(created.error());
        }
        device_->visual_pipelines[index] = *created;
    }

    auto descriptor_set = device.allocate_descriptor_set(device_->visual_set_layout, false);
    if (!descriptor_set) {
        return make_unexpected(descriptor_set.error());
    }
    device_->visual_set = *descriptor_set;
    const rhi::BufferHandle resources[10] = {
        device_->static_vertices,   device_->static_colours,    device_->dynamic_vertices,
        device_->dynamic_colours,   device_->foam_previous,     device_->foam_next,
        device_->terrain_fields[0], device_->terrain_fields[1], device_->terrain_fields[2],
        device_->terrain_fields[3]};
    rhi::DescriptorWrite writes[10] = {};
    for (u32 index = 0; index < 10; ++index) {
        writes[index].binding = index;
        writes[index].kind = rhi::DescriptorKind::StorageBuffer;
        writes[index].buffer = resources[index];
    }
    return device.update_descriptor_set(device_->visual_set,
                                        Span<const rhi::DescriptorWrite>(writes, 10));
}

Status Stage::stage_world(const World& world) noexcept {
    if (!available_) {
        return fail(ErrorCode::Unavailable, "no graphics device answered");
    }
    rhi::Device& device = *device_->handle.value();
    const WorldVec3d middle = world.centre();

    Array<Vertex> vertices(*allocator_);
    Array<u32> indices(*allocator_);
    for (const TerrainPatch& patch : world.terrain_patches()) {
        const u32 base = static_cast<u32>(vertices.size());
        for (usize index = 0; index < patch.mesh.positions.size(); ++index) {
            const Vec3& local = patch.mesh.positions[index];
            Vertex vertex;
            // WORLD-RELATIVE, not absolute: the mesh's positions are relative to its tile's corner
            // and the corner is absolute, so the two are composed in f64 and the result is offset
            // by the world's centre before it becomes an f32.
            vertex.position = Vec3{
                static_cast<f32>(patch.origin.x + static_cast<f64>(local.x) - middle.x), local.y,
                static_cast<f32>(patch.origin.z + static_cast<f64>(local.z) - middle.z)};
            vertex.normal = patch.mesh.normals[index];
            if (Status pushed = vertices.push_back(vertex); !pushed) {
                return pushed;
            }
        }
        for (const u32 index : patch.mesh.indices.span()) {
            if (Status pushed = indices.push_back(base + index); !pushed) {
                return pushed;
            }
        }
    }
    terrain_vertices_ = static_cast<u32>(vertices.size());
    terrain_indices_ = static_cast<u32>(indices.size());
    if (Status sized = terrain_colours_.resize(vertices.size()); !sized) {
        return sized;
    }

    rhi::BufferDescription description;
    description.name = "world terrain geometry";
    description.size = vertices.size() * sizeof(Vertex);
    description.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::Storage;
    description.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> buffer = device.create_buffer(description);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->static_vertices = *buffer;
    if (Status uploaded =
            upload_bytes(device, device_->static_vertices, vertices.data(), description.size);
        !uploaded) {
        return uploaded;
    }

    description.name = "world terrain colours";
    description.size = vertices.size() * sizeof(Vec3);
    description.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::Storage;
    buffer = device.create_buffer(description);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->static_colours = *buffer;

    description.name = "world terrain indices";
    description.size = indices.size() * sizeof(u32);
    description.usage = rhi::BufferUsage::Index;
    buffer = device.create_buffer(description);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->static_indices = *buffer;
    if (Status uploaded =
            upload_bytes(device, device_->static_indices, indices.data(), description.size);
        !uploaded) {
        return uploaded;
    }

    // The dynamic half is sized ONCE, for the worst frame: the whole sky dome, the whole ocean
    // patch and the plant cap. A buffer that grew would mean a device allocation inside the frame
    // loop, which is the thing every other artefact in this repository avoids.
    const u64 sky_vertices = world.sky_dome().size();
    const u64 water_vertices = 8'192;
    const u64 star_vertices = static_cast<u64>(world.stars().size() + 4'096) * 3;
    const u64 capacity = sky_vertices + water_vertices + star_vertices +
                         (static_cast<u64>(kMaxDrawnPlants) * kPlantVertices);
    dynamic_capacity_ = static_cast<u32>(capacity);
    sky_vertices_ = static_cast<u32>(sky_vertices);
    const u64 index_capacity = static_cast<u64>(world.sky_indices().size()) + (water_vertices * 6) +
                               star_vertices + (static_cast<u64>(kMaxDrawnPlants) * kPlantVertices);

    description.name = "world dynamic geometry";
    description.size = capacity * sizeof(Vertex);
    description.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::Storage;
    buffer = device.create_buffer(description);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->dynamic_vertices = *buffer;

    description.name = "world dynamic colours";
    description.size = capacity * sizeof(Vec3);
    description.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::Storage;
    buffer = device.create_buffer(description);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->dynamic_colours = *buffer;

    description.name = "world dynamic indices";
    description.size = index_capacity * sizeof(u32);
    description.usage = rhi::BufferUsage::Index;
    buffer = device.create_buffer(description);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->dynamic_indices = *buffer;

    constexpr u64 foam_cells = u64{128} * u64{128};
    description.name = "world foam previous";
    description.size = foam_cells * sizeof(f32);
    description.usage = rhi::BufferUsage::Storage;
    description.memory = rhi::MemoryUse::Upload;
    buffer = device.create_buffer(description);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->foam_previous = *buffer;
    std::memset(device.buffer_mapped_pointer(*buffer), 0, static_cast<usize>(description.size));
    description.name = "world foam next";
    buffer = device.create_buffer(description);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->foam_next = *buffer;
    std::memset(device.buffer_mapped_pointer(*buffer), 0, static_cast<usize>(description.size));

    for (u32 index = 0; index < 4; ++index) {
        auto image = world.terrain_field_image(index);
        if (!image) {
            return make_unexpected(image.error());
        }
        description.name = "world terrain field image";
        description.size = image->words.size() * sizeof(u32);
        description.usage = rhi::BufferUsage::Storage;
        description.memory = rhi::MemoryUse::Upload;
        buffer = device.create_buffer(description);
        if (!buffer) {
            return make_unexpected(buffer.error());
        }
        device_->terrain_fields[index] = *buffer;
        field_image_bytes_[index] = description.size;
        if (Status uploaded = upload_bytes(device, *buffer, image->words.data(), description.size);
            !uploaded) {
            return uploaded;
        }
    }

    // THE DYNAMIC HALF IS FAULTED HERE, NOT BY THE FIRST FRAME OF THE TAKE. Mapping a buffer
    // commits no page; the first write to each one does. The warm-up frame `open_stage()` draws
    // before the take writes almost none of these — the world has not advanced yet, so it has no
    // plants and no sea — which left the first frame of every take paying to fault in about eleven
    // megabytes of proxy streams: 2 to 3 ms more `stage_build` on frame 0 than on any other frame,
    // in every one of ten measured takes. Touching every page once, here, is what the warm-up
    // comment in `main.cpp` already promised.
    if (Status reserved = dynamic_vertices_.reserve(sky_vertices + water_vertices + star_vertices);
        !reserved) {
        return reserved;
    }
    if (Status reserved = dynamic_colours_.reserve(sky_vertices + water_vertices + star_vertices);
        !reserved) {
        return reserved;
    }
    if (Status reserved = dynamic_indices_.reserve(index_capacity); !reserved) {
        return reserved;
    }
    if (Status reserved = drawn_plants_.reserve(kMaxDrawnPlants); !reserved) {
        return reserved;
    }
    dynamic_index_capacity_ = static_cast<u32>(index_capacity);
    const rhi::BufferHandle dynamic[3] = {device_->dynamic_vertices, device_->dynamic_colours,
                                          device_->dynamic_indices};
    const u64 dynamic_bytes[3] = {capacity * sizeof(Vertex), capacity * sizeof(Vec3),
                                  index_capacity * sizeof(u32)};
    for (u32 index = 0; index < 3; ++index) {
        void* mapped = device.buffer_mapped_pointer(dynamic[index]);
        if (mapped == nullptr) {
            return fail(ErrorCode::Internal, "a dynamic buffer is not mapped");
        }
        std::memset(mapped, 0, static_cast<usize>(dynamic_bytes[index]));
    }
    return create_visual_pipelines();
}

// ================================================================================================
// THE PER-FRAME STREAMS
// ================================================================================================

namespace {

/// One plant's proxy — an eight-sided crown cone over a three-sided trunk prism — written into
/// `kPlantVertices` consecutive slots of each stream. `first` is the slot's index in the dynamic
/// vertex buffer, which the identity index run points at.
///
/// A FUNCTION OF ITS OWN PLANT ONLY, so the proxies are written in parallel: each writes its own
/// block and nothing else, and the streams hold the same bits whichever worker wrote which plant.
void write_plant_proxy(const PlantDraw& plant, const Vec3& base, u32 first, Vertex* vertices,
                       Vec3* colours, u32* indices) noexcept {
    const f32 trunk_fraction = trunk_fraction_of(plant.kind);
    const f32 trunk_height = plant.height * trunk_fraction;
    const f32 trunk_radius = plant.radius * (plant.kind == PlantKind::Tree ? 0.16F : 0.25F);
    const Vec3 apex{base.x + plant.crown_offset.x, base.y + plant.height,
                    base.z + plant.crown_offset.z};
    const Vec3 crown_base{base.x + (plant.crown_offset.x * 0.25F), base.y + trunk_height,
                          base.z + (plant.crown_offset.z * 0.25F)};

    u32 written = 0;

    // Each rim angle is shared by the two faces either side of it, so its cosine and sine are
    // taken once. Side `s` spans angles `s` and `s + 1`, written with the same expression the
    // faces used to evaluate twice each, so the proxy's vertices are unchanged bit for bit.
    f32 crown_cos[kCrownSides + 1];
    f32 crown_sin[kCrownSides + 1];
    for (u32 side = 0; side <= kCrownSides; ++side) {
        const f32 angle = plant.yaw + (6.28318F * static_cast<f32>(side) / kCrownSides);
        crown_cos[side] = std::cos(angle);
        crown_sin[side] = std::sin(angle);
    }
    // The crown: a cone whose apex carries the full wind displacement and whose skirt carries a
    // quarter of it, which is the shear a trunk-and-branch response produces along the plant.
    for (u32 side = 0; side < kCrownSides; ++side) {
        const Vec3 left{crown_base.x + (crown_cos[side] * plant.radius), crown_base.y,
                        crown_base.z + (crown_sin[side] * plant.radius)};
        const Vec3 right{crown_base.x + (crown_cos[side + 1] * plant.radius), crown_base.y,
                         crown_base.z + (crown_sin[side + 1] * plant.radius)};
        const Vec3 normal = face_normal(apex, left, right);
        const Vec3 corners[3] = {apex, left, right};
        for (const Vec3& corner : corners) {
            vertices[written] = Vertex{corner, normal};
            colours[written] = plant.crown_colour;
            ++written;
        }
    }
    // The trunk: a three-sided prism, leaning by a quarter of the crown's displacement.
    f32 trunk_cos[kTrunkSides + 1];
    f32 trunk_sin[kTrunkSides + 1];
    for (u32 side = 0; side <= kTrunkSides; ++side) {
        const f32 angle = plant.yaw + (6.28318F * static_cast<f32>(side) / kTrunkSides);
        trunk_cos[side] = std::cos(angle);
        trunk_sin[side] = std::sin(angle);
    }
    for (u32 side = 0; side < kTrunkSides; ++side) {
        const Vec3 low_a{base.x + (trunk_cos[side] * trunk_radius), base.y,
                         base.z + (trunk_sin[side] * trunk_radius)};
        const Vec3 low_b{base.x + (trunk_cos[side + 1] * trunk_radius), base.y,
                         base.z + (trunk_sin[side + 1] * trunk_radius)};
        const Vec3 high_a{low_a.x + (plant.crown_offset.x * 0.25F), base.y + trunk_height,
                          low_a.z + (plant.crown_offset.z * 0.25F)};
        const Vec3 high_b{low_b.x + (plant.crown_offset.x * 0.25F), base.y + trunk_height,
                          low_b.z + (plant.crown_offset.z * 0.25F)};
        const Vec3 normal = face_normal(low_a, high_a, low_b);
        const Vec3 corners[6] = {low_a, high_a, low_b, low_b, high_a, high_b};
        for (const Vec3& corner : corners) {
            vertices[written] = Vertex{corner, normal};
            colours[written] = plant.trunk_colour;
            ++written;
        }
    }
    for (u32 vertex = 0; vertex < kPlantVertices; ++vertex) {
        indices[vertex] = first + vertex;
    }
}

/// What the parallel proxy write shares, read-only, between its partitions.
struct ProxyWrite {
    Span<const PlantDraw> plants;
    Span<const u32> chosen;
    f32 middle_x = 0.0F;
    f32 middle_z = 0.0F;
    u32 first_vertex = 0;
    Vertex* vertices = nullptr;
    Vec3* colours = nullptr;
    u32* indices = nullptr;
};

void write_proxies(const ProxyWrite& write, u64 begin, u64 end) noexcept {
    for (u64 slot = begin; slot < end; ++slot) {
        const PlantDraw& plant = write.plants[write.chosen[slot]];
        const Vec3 base{plant.position.x - write.middle_x, plant.position.y,
                        plant.position.z - write.middle_z};
        const usize offset = static_cast<usize>(slot) * kPlantVertices;
        write_plant_proxy(plant, base, write.first_vertex + static_cast<u32>(offset),
                          write.vertices + offset, write.colours + offset, write.indices + offset);
    }
}

}  // namespace

Status Stage::build_dynamic(const World& world, const WorldVec3d& eye, StageReport& out) noexcept {
    dynamic_vertices_.clear();
    dynamic_colours_.clear();
    dynamic_indices_.clear();
    const WorldVec3d middle = world.centre();
    const Vec3 relative_eye{static_cast<f32>(eye.x - middle.x), static_cast<f32>(eye.y),
                            static_cast<f32>(eye.z - middle.z)};
    const f32 exposure = world.lighting().exposure;

    // --- THE SKY, as the dome `World::shade_sky()` filled.
    sky_first_index_ = 0;
    for (const SkyVertex& sky : world.sky_dome()) {
        Vertex vertex;
        vertex.position = Vec3{relative_eye.x + (sky.direction.x * kSkyRadius),
                               relative_eye.y + (sky.direction.y * kSkyRadius),
                               relative_eye.z + (sky.direction.z * kSkyRadius)};
        vertex.normal = sky.direction;
        if (Status pushed = dynamic_vertices_.push_back(vertex); !pushed) {
            return pushed;
        }
        // The exposure divide happens HERE and not in the shader, because the shader's emissive
        // path has to receive a number it can tone map and the radiance `compose_sky()` answers is
        // in nits. The frame's own mean sky lands at a quarter of white, which leaves two stops of
        // headroom for the sun's disc and the brightest cloud and keeps the sky brighter than the
        // ground it lights — which is true of every sky and is the first thing a wrong exposure
        // breaks.
        const f32 scale = 1.0F / (exposure * 4.0F);
        if (Status pushed = dynamic_colours_.push_back(
                Vec3{sky.radiance.x * scale, sky.radiance.y * scale, sky.radiance.z * scale});
            !pushed) {
            return pushed;
        }
    }
    for (const u32 index : world.sky_indices()) {
        if (Status pushed = dynamic_indices_.push_back(index); !pushed) {
            return pushed;
        }
    }
    sky_index_count_ = static_cast<u32>(dynamic_indices_.size());
    out.sky_triangles = sky_index_count_ / 3;

    // --- THE STARS, as the point lights they are. Drawn on the dome's own radius so the depth
    // test puts them behind everything, and emissive with the sky.
    star_first_index_ = static_cast<u32>(dynamic_indices_.size());
    out.stars_drawn = 0;
    for (const StarDraw& star : world.stars()) {
        if (star.radiance.x + star.radiance.y + star.radiance.z <= 0.004F) {
            continue;
        }
        // A screen-facing triangle at the star's own direction. Two axes perpendicular to the
        // direction, sized by the angular half-size the magnitude relation gave it.
        const Vec3 up =
            std::abs(star.direction.y) > 0.9F ? Vec3{1.0F, 0.0F, 0.0F} : Vec3{0.0F, 1.0F, 0.0F};
        const Vec3 right = normalised(Vec3{(up.y * star.direction.z) - (up.z * star.direction.y),
                                           (up.z * star.direction.x) - (up.x * star.direction.z),
                                           (up.x * star.direction.y) - (up.y * star.direction.x)});
        const Vec3 over =
            normalised(Vec3{(star.direction.y * right.z) - (star.direction.z * right.y),
                            (star.direction.z * right.x) - (star.direction.x * right.z),
                            (star.direction.x * right.y) - (star.direction.y * right.x)});
        const f32 radius = star.angular_size * kSkyRadius * 0.995F;
        const Vec3 centre{relative_eye.x + (star.direction.x * kSkyRadius * 0.99F),
                          relative_eye.y + (star.direction.y * kSkyRadius * 0.99F),
                          relative_eye.z + (star.direction.z * kSkyRadius * 0.99F)};
        const u32 first = static_cast<u32>(dynamic_vertices_.size());
        for (u32 corner = 0; corner < 3; ++corner) {
            const f32 angle = 2.0944F * static_cast<f32>(corner);
            const f32 cosine = std::cos(angle) * radius * 2.0F;
            const f32 sine = std::sin(angle) * radius * 2.0F;
            Vertex vertex;
            vertex.position = Vec3{centre.x + (right.x * cosine) + (over.x * sine),
                                   centre.y + (right.y * cosine) + (over.y * sine),
                                   centre.z + (right.z * cosine) + (over.z * sine)};
            vertex.normal = star.direction;
            if (Status pushed = dynamic_vertices_.push_back(vertex); !pushed) {
                return pushed;
            }
            if (Status pushed = dynamic_colours_.push_back(star.radiance); !pushed) {
                return pushed;
            }
            if (Status pushed = dynamic_indices_.push_back(first + corner); !pushed) {
                return pushed;
            }
        }
        ++out.stars_drawn;
    }
    star_index_count_ = static_cast<u32>(dynamic_indices_.size()) - star_first_index_;

    // --- THE OCEAN, as `water::OceanSurface::build()` generated it this frame.
    const water::OceanSurface& ocean = world.ocean();
    const u32 water_base = static_cast<u32>(dynamic_vertices_.size());
    water_first_vertex_ = water_base;
    water_vertices_ = static_cast<u32>(ocean.positions().size());
    water_first_index_ = static_cast<u32>(dynamic_indices_.size());
    const water::WaterOptics optics = water::clear_sea_optics();
    const Span<const Vec3> positions = ocean.positions();
    const Span<const Vec3> normals = ocean.normals();
    const Span<const f32> breaking = ocean.breaking();
    // THE WATER'S COLOUR IS THE BODY'S OWN OPTICS, not a painted blue. `water_column_colour()` is
    // src/water/'s Beer-Lambert absorption plus its in-scatter term over a nominal column, which is
    // what makes deep water darken AND shift hue with nothing authored. The column is the same for
    // every vertex, so it is evaluated once per frame rather than once per vertex.
    const Vec3 deep = water::water_column_colour(optics, 14.0F, Vec3{0.02F, 0.05F, 0.07F});
    for (usize index = 0; index < positions.size(); ++index) {
        Vertex vertex;
        vertex.position = Vec3{static_cast<f32>(ocean.origin().x - middle.x) + positions[index].x,
                               static_cast<f32>(ocean.origin().y) + positions[index].y,
                               static_cast<f32>(ocean.origin().z - middle.z) + positions[index].z};
        vertex.normal = normals[index];
        if (Status pushed = dynamic_vertices_.push_back(vertex); !pushed) {
            return pushed;
        }
        // The foam is the surface's own breaking indicator, which comes from the Jacobian of the
        // Gerstner map rather than from a threshold on height.
        const f32 foam = breaking.empty() ? 0.0F : breaking[index];
        const f32 white = clamp01(foam);
        if (Status pushed = dynamic_colours_.push_back(Vec3{deep.x + ((0.85F - deep.x) * white),
                                                            deep.y + ((0.88F - deep.y) * white),
                                                            deep.z + ((0.92F - deep.z) * white)});
            !pushed) {
            return pushed;
        }
    }
    for (const u32 index : ocean.indices()) {
        if (Status pushed = dynamic_indices_.push_back(water_base + index); !pushed) {
            return pushed;
        }
    }
    water_index_count_ = static_cast<u32>(dynamic_indices_.size()) - water_first_index_;
    out.water_triangles = water_index_count_ / 3;

    // --- THE FOLIAGE, one proxy per plant near the camera. Chosen in order first — the nearest
    // `kMaxDrawnPlants` inside the draw distance, exactly as a single loop chose them — and then
    // written, each proxy into its own fixed block, across the world's workers when it has them.
    //
    // THE PROXIES ARE WRITTEN STRAIGHT INTO THE MAPPED DYNAMIC BUFFERS, after the sky, stars and
    // sea that `upload_dynamic()` copies in front of them. They are about eleven megabytes a frame
    // and nothing on the processor reads them back, so staging them in an array first cost a
    // zero-fill, a write and a copy of all eleven for nothing. The previous frame is idle before
    // `shoot()` builds this one, so no draw is still reading what is overwritten here.
    foliage_first_index_ = static_cast<u32>(dynamic_indices_.size());
    drawn_plants_.clear();
    const Span<const PlantDraw> plants = world.plants();
    for (u32 index = 0; index < plants.size() && drawn_plants_.size() < kMaxDrawnPlants; ++index) {
        const PlantDraw& plant = plants[index];
        const f32 dx = (plant.position.x - static_cast<f32>(middle.x)) - relative_eye.x;
        const f32 dz = (plant.position.z - static_cast<f32>(middle.z)) - relative_eye.z;
        if ((dx * dx) + (dz * dz) > kPlantDrawMetres * kPlantDrawMetres) {
            continue;
        }
        if (Status pushed = drawn_plants_.push_back(index); !pushed) {
            return pushed;
        }
    }
    out.plants_drawn = static_cast<u32>(drawn_plants_.size());
    const usize first_vertex = dynamic_vertices_.size();
    const usize first_index = dynamic_indices_.size();
    const usize proxy_slots = drawn_plants_.size() * kPlantVertices;
    if (first_vertex + proxy_slots > dynamic_capacity_ ||
        first_index + proxy_slots > dynamic_index_capacity_) {
        return fail(ErrorCode::OutOfRange, "the frame's proxies exceed the dynamic buffers");
    }
    rhi::Device& device = *device_->handle.value();
    auto* vertices = static_cast<Vertex*>(device.buffer_mapped_pointer(device_->dynamic_vertices));
    auto* colours = static_cast<Vec3*>(device.buffer_mapped_pointer(device_->dynamic_colours));
    auto* indices = static_cast<u32*>(device.buffer_mapped_pointer(device_->dynamic_indices));
    if (vertices == nullptr || colours == nullptr || indices == nullptr) {
        return fail(ErrorCode::Internal, "a dynamic buffer is not mapped");
    }
    ProxyWrite write;
    write.plants = plants;
    write.chosen = drawn_plants_.span();
    write.middle_x = static_cast<f32>(middle.x);
    write.middle_z = static_cast<f32>(middle.z);
    write.first_vertex = static_cast<u32>(first_vertex);
    write.vertices = vertices + first_vertex;
    write.colours = colours + first_vertex;
    write.indices = indices + first_index;
    if (jobs_ == nullptr) {
        write_proxies(write, 0, drawn_plants_.size());
    } else {
        auto body = [&write](const jobs::TaskContext& /*task*/, u64 begin, u64 end) noexcept {
            write_proxies(write, begin, end);
        };
        if (Status ran = jobs::parallel_for(*jobs_, drawn_plants_.size(), kProxiesPerJob, body,
                                            "world.proxies");
            !ran) {
            return ran;
        }
    }
    foliage_index_count_ = static_cast<u32>(proxy_slots);
    out.foliage_triangles = foliage_index_count_ / 3;

    out.terrain_triangles = terrain_indices_ / 3;
    return ok();
}

Status Stage::upload_dynamic(const World& world, f32& field_origin_x,
                             f32& field_origin_z) noexcept {
    rhi::Device& device = *device_->handle.value();
    f64 origin_x = 0.0;
    f64 origin_z = 0.0;
    for (u32 index = 0; index < 4; ++index) {
        auto image = world.terrain_field_image(index);
        if (!image) {
            return make_unexpected(image.error());
        }
        const u64 bytes = image->words.size() * sizeof(u32);
        if (bytes > field_image_bytes_[index]) {
            // Gameplay fields can publish another tile after the stage is created. Every previous
            // frame is idle before this function is entered, so grow the upload buffer here and
            // move the persistent descriptor to it before the new frame is recorded.
            rhi::BufferDescription description;
            description.name = "world terrain field image";
            description.size = bytes;
            description.usage = rhi::BufferUsage::Storage;
            description.memory = rhi::MemoryUse::Upload;
            auto grown = device.create_buffer(description);
            if (!grown) {
                return make_unexpected(grown.error());
            }
            if (Status uploaded = upload_bytes(device, *grown, image->words.data(), bytes);
                !uploaded) {
                device.destroy_buffer(*grown);
                return uploaded;
            }
            rhi::DescriptorWrite write;
            write.binding = 6U + index;
            write.kind = rhi::DescriptorKind::StorageBuffer;
            write.buffer = *grown;
            if (Status updated = device.update_descriptor_set(
                    device_->visual_set, Span<const rhi::DescriptorWrite>(&write, 1));
                !updated) {
                device.destroy_buffer(*grown);
                return updated;
            }
            device.destroy_buffer(device_->terrain_fields[index]);
            device_->terrain_fields[index] = *grown;
            field_image_bytes_[index] = bytes;
        } else if (Status uploaded = upload_bytes(device, device_->terrain_fields[index],
                                                  image->words.data(), bytes);
                   !uploaded) {
            return uploaded;
        }
        if (index == 0) {
            origin_x = image->origin_x;
            origin_z = image->origin_z;
        } else if (image->origin_x != origin_x || image->origin_z != origin_z) {
            return fail(ErrorCode::InvalidArgument,
                        "terrain field images do not share one local origin");
        }
    }
    field_origin_x = static_cast<f32>(world.centre().x - origin_x);
    field_origin_z = static_cast<f32>(world.centre().z - origin_z);
    if (Status uploaded = upload_bytes(device, device_->dynamic_vertices, dynamic_vertices_.data(),
                                       dynamic_vertices_.size() * sizeof(Vertex), jobs_);
        !uploaded) {
        return uploaded;
    }
    if (Status uploaded = upload_bytes(device, device_->dynamic_colours, dynamic_colours_.data(),
                                       dynamic_colours_.size() * sizeof(Vec3), jobs_);
        !uploaded) {
        return uploaded;
    }
    return upload_bytes(device, device_->dynamic_indices, dynamic_indices_.data(),
                        dynamic_indices_.size() * sizeof(u32), jobs_);
}

Status Stage::shoot(const World& world, const WorldVec3d& eye, const WorldVec3d& target,
                    const char* png_path, StageReport& out) noexcept {
    if (!available_) {
        return fail(ErrorCode::Unavailable, "no graphics device answered");
    }
    rhi::Device& device = *device_->handle.value();

    f64 mark = now_millis();
    if (Status built = build_dynamic(world, eye, out); !built) {
        return built;
    }
    f32 field_origin_x = 0.0F;
    f32 field_origin_z = 0.0F;
    if (Status uploaded = upload_dynamic(world, field_origin_x, field_origin_z); !uploaded) {
        return uploaded;
    }
    if (Status uploaded = upload_cloud_shadow(world); !uploaded) {
        return uploaded;
    }
    out.build_ms = now_millis() - mark;

    mark = now_millis();
    const Expected<u32, Error> began = device.begin_frame();
    if (!began) {
        return make_unexpected(began.error());
    }
    const u32 slot = *began;

    cy::rendering::RenderGraph graph(*allocator_);

    const auto import_buffer = [&graph, &device](const char* name, rhi::BufferHandle handle,
                                                 rhi::BufferUsage usage) noexcept {
        cy::rendering::BufferRequest request;
        request.name = name;
        const rhi::BufferDescription* description = device.buffer_description(handle);
        request.size = description != nullptr ? description->size : 0;
        request.extra_usage = usage;
        return graph.import_buffer(request, handle);
    };
    const ResourceId terrain_vertices =
        import_buffer("world terrain visual input", device_->static_vertices,
                      rhi::BufferUsage::Storage | rhi::BufferUsage::Vertex);
    const ResourceId terrain_colours =
        import_buffer("world terrain visual output", device_->static_colours,
                      rhi::BufferUsage::Storage | rhi::BufferUsage::Vertex);
    const ResourceId dynamic_vertices =
        import_buffer("world dynamic visual input", device_->dynamic_vertices,
                      rhi::BufferUsage::Storage | rhi::BufferUsage::Vertex);
    const ResourceId dynamic_colours =
        import_buffer("world dynamic visual output", device_->dynamic_colours,
                      rhi::BufferUsage::Storage | rhi::BufferUsage::Vertex);
    const ResourceId foam_previous =
        import_buffer("world foam previous", device_->foam_previous, rhi::BufferUsage::Storage);
    const ResourceId foam_next =
        import_buffer("world foam next", device_->foam_next, rhi::BufferUsage::Storage);
    ResourceId terrain_fields[4];
    for (u32 index = 0; index < 4; ++index) {
        terrain_fields[index] = import_buffer("world terrain field", device_->terrain_fields[index],
                                              rhi::BufferUsage::Storage);
    }

    VisualPush visual;
    visual.terrain_count = terrain_vertices_;
    visual.sky_count = sky_vertices_;
    visual.water_start = water_first_vertex_;
    visual.water_count = water_vertices_;
    visual.frame_index = visual_frame_index_++;
    visual.time_seconds = static_cast<f32>(world.state().seconds);
    visual.delta_seconds = 1.0F / 60.0F;
    visual.wetness = world.state().wetness;
    visual.snow_depth = world.state().snow_depth_metres;
    visual.cloud_coverage = world.state().cloud_coverage;
    visual.cloud_seed = static_cast<u32>(world.options().seed ^ (world.options().seed >> 32U));
    visual.sun_height =
        std::sin(world.state().sun_elevation_degrees * (std::numbers::pi_v<f32> / 180.0F));
    visual.exposure = world.lighting().exposure;
    visual.field_origin[0] = field_origin_x;
    visual.field_origin[1] = field_origin_z;
    visual.field_origin[2] = static_cast<f32>(eye.x);
    visual.field_origin[3] = static_cast<f32>(eye.z);
    device_->last_visual = visual;

    VisualPassState terrain_visual{
        device_->visual_pipelines[0],    device_->visual_layout, device_->visual_set, visual,
        (terrain_vertices_ + 63U) / 64U, &out.terrain_dispatches};
    VisualPassState cloud_visual{device_->visual_pipelines[1], device_->visual_layout,
                                 device_->visual_set,          visual,
                                 (sky_vertices_ + 63U) / 64U,  &out.cloud_dispatches};
    const u32 foam_work = water_vertices_ > (128U * 128U) ? water_vertices_ : (128U * 128U);
    VisualPassState foam_visual{device_->visual_pipelines[2], device_->visual_layout,
                                device_->visual_set,          visual,
                                (foam_work + 63U) / 64U,      &out.foam_dispatches};
    graph.add_pass("world terrain substrate", QueueKind::Graphics)
        .read(terrain_vertices, Access::ComputeStorageRead)
        .read(terrain_fields[0], Access::ComputeStorageRead)
        .read(terrain_fields[1], Access::ComputeStorageRead)
        .read(terrain_fields[2], Access::ComputeStorageRead)
        .read(terrain_fields[3], Access::ComputeStorageRead)
        .write(terrain_colours, Access::ComputeStorageWrite)
        .record(&record_visual, &terrain_visual);
    graph.add_pass("world visual clouds", QueueKind::Graphics)
        .read(dynamic_vertices, Access::ComputeStorageRead)
        .write(dynamic_colours, Access::ComputeStorageWrite)
        .record(&record_visual, &cloud_visual);
    graph.add_pass("world visual foam", QueueKind::Graphics)
        .read(dynamic_vertices, Access::ComputeStorageRead)
        .read(foam_previous, Access::ComputeStorageRead)
        .write(foam_next, Access::ComputeStorageWrite)
        .write(dynamic_colours, Access::ComputeStorageWrite)
        .record(&record_visual, &foam_visual);

    const WorldVec3d middle = world.centre();
    const Vec3 relative_eye{static_cast<f32>(eye.x - middle.x), static_cast<f32>(eye.y),
                            static_cast<f32>(eye.z - middle.z)};
    const Vec3 relative_target{static_cast<f32>(target.x - middle.x), static_cast<f32>(target.y),
                               static_cast<f32>(target.z - middle.z)};
    const f32 aspect = static_cast<f32>(width_) / static_cast<f32>(height_);
    const Mat4 projection = perspective_reversed_z(kFieldOfView, aspect, kNearPlane, kFarPlane);
    const Mat4 camera = look_at(relative_eye, relative_target);
    const Mat4 world_to_clip = projection * camera;

    const Lighting& lighting = world.lighting();

    // --- The view the assembly is handed -------------------------------------------------------
    //
    // ONE DIRECTIONAL LIGHT, AND IT IS THE WORLD'S OWN SUN rather than a light invented for the
    // frame. `lighting.sun_travel` points FROM the surface TO the sun, which is what both the
    // assembly's sky table and this file's shader mean by it, so the frame's shadow request, its
    // ambient term and the picture's own shading cannot disagree about where the sun is.
    device_->sun.kind = cy::render::LightKind::Directional;
    device_->sun.transform = cy::Transform::identity();
    device_->sun.transform.rotation =
        cy::Quat::from_to(Vec3{0.0F, 0.0F, -1.0F}, -normalised(lighting.sun_travel));
    device_->sun.intensity = 100'000.0F;
    device_->sun.color[0] = lighting.sun_colour.x;
    device_->sun.color[1] = lighting.sun_colour.y;
    device_->sun.color[2] = lighting.sun_colour.z;
    device_->sun.casts_shadow = true;
    device_->sun.stable_id = 1;

    AssemblyView view;
    view.fov_y_radians = kFieldOfView;
    view.projection = projection;
    view.view = camera;
    view.cull.frustum = cy::Frustum::from_view_projection(world_to_clip);
    view.cull.camera_position = relative_eye;
    view.cull.camera_forward = normalised(relative_target - relative_eye);
    view.cull.fov_y_radians = kFieldOfView;
    view.lights = Span<const cy::render::LightDescription>(&device_->sun, 1);
    view.sun_direction = normalised(lighting.sun_travel);

    // IMPORTED WITH `Undefined`, and that is the truth rather than a shortcut: the resolve clears
    // the image, so last frame's contents are discarded and the graph derives the transition from
    // there.
    cy::rendering::TextureRequest output_request;
    output_request.name = "world output";
    output_request.format = kOutputFormat;
    output_request.width = width_;
    output_request.height = height_;
    output_request.extra_usage = rhi::TextureUsage::TransferSource;
    view.output = graph.import_texture(output_request, device_->output, rhi::ImageUse::Undefined);

    // --- What this program records into the frame ----------------------------------------------

    WorldPush push;
    for (usize row = 0; row < 4; ++row) {
        const Vec4 values = world_to_clip.row(row);
        switch (row) {
            case 0:
                write_row(push.row0, values);
                break;
            case 1:
                write_row(push.row1, values);
                break;
            case 2:
                write_row(push.row2, values);
                break;
            default:
                write_row(push.row3, values);
                break;
        }
    }
    write_vec3(push.light, lighting.sun_travel, 0.0F);
    write_vec3(push.eye, relative_eye, 0.0F);
    // UNDER CLOUD SHADOWS THE SUN IS PLACED BEFORE THE CLOUDS, because the field attenuates it per
    // fragment; `sun_colour` already carries the cloud over the viewer and would shade every
    // surface under that cloud twice. Off, the picture is exactly the one before cloud shadows.
    write_vec3(push.sun, world.cloud_shadows() ? lighting.clear_sun_colour : lighting.sun_colour,
               0.0F);
    write_vec3(push.ambient, lighting.ambient, 0.0F);

    DrawState state;
    state.pipeline = device_->pipeline;
    state.layout = device_->layout;
    state.cloud_shadow = device_->world_set;
    state.static_vertices = device_->static_vertices;
    state.static_colours = device_->static_colours;
    state.static_indices = device_->static_indices;
    state.dynamic_vertices = device_->dynamic_vertices;
    state.dynamic_colours = device_->dynamic_colours;
    state.dynamic_indices = device_->dynamic_indices;
    state.width = width_;
    state.height = height_;

    // The sky, emissive, first — so that a frame in which it failed to draw shows black rather than
    // a plausible background this file invented.
    state.runs[0].first_index = sky_first_index_;
    state.runs[0].index_count = sky_index_count_;
    state.runs[0].dynamic = true;
    state.runs[0].push = push;
    state.runs[0].push.eye[3] = 1.0F;

    state.runs[1].first_index = 0;
    state.runs[1].index_count = terrain_indices_;
    state.runs[1].dynamic = false;
    state.runs[1].push = push;

    state.runs[2].first_index = water_first_index_;
    state.runs[2].index_count = water_index_count_;
    state.runs[2].dynamic = true;
    state.runs[2].push = push;
    // The specular lobe is water's alone. See the shader.
    state.runs[2].push.sun[3] = 24.0F;

    state.runs[3].first_index = star_first_index_;
    state.runs[3].index_count = star_index_count_;
    state.runs[3].dynamic = true;
    state.runs[3].push = push;
    state.runs[3].push.eye[3] = 1.0F;

    state.runs[4].first_index = foliage_first_index_;
    state.runs[4].index_count = foliage_index_count_;
    state.runs[4].dynamic = true;
    state.runs[4].push = push;
    state.run_count = 5;

    ResolveState resolve;
    resolve.pipelines = &device_->pipelines;
    resolve.bindings = &device_->bindings;
    resolve.width = width_;
    resolve.height = height_;

    // THE TWO CALLBACKS, AND WHICH STAGES THEY ARE ON. The world's geometry records in the frame's
    // OPAQUE stage and the tonemapping resolve in its POST-PROCESS stage — the same two seams
    // `FrameSinks` leaves for every other caller. Every other stage the frame declares runs with no
    // callback, which `ForwardFrame` calls "a legitimate frame": this program has no transparent
    // layer, no screen-space effects and no interface.
    FrameSinks sinks;
    const ResourceId visual_inputs[4] = {terrain_vertices, terrain_colours, dynamic_vertices,
                                         dynamic_colours};
    sinks.passes[static_cast<usize>(FramePassKind::Opaque)] = cy::rendering::FramePassCallback{
        &record_draw, &state, Span<const ResourceId>(visual_inputs, 4)};
    sinks.passes[static_cast<usize>(FramePassKind::PostProcess)] =
        cy::rendering::FramePassCallback{&record_resolve, &resolve};

    cy::rendering::SpatialIndex index(*allocator_);
    AssemblyReport report;
    if (Status assembled = device_->assembly.assemble(index, view, sinks, graph, report);
        !assembled) {
        (void)device.end_frame();
        return assembled;
    }
    out.frame_passes = report.passes_declared;
    out.post_stages = report.post_stages;

    // The resources the callbacks draw into are the FRAME's, and they are named only after
    // `assemble` has declared them.
    const cy::rendering::FrameResources& resources = device_->assembly.resources();
    state.color = resources.color;
    state.depth = resources.depth;
    resolve.scene = resources.color;
    resolve.output = resources.output;

    // --- The uploads the resolve reads ---------------------------------------------------------
    //
    // THE EXPOSURE IS CONTENT. M11.c task 3.3: `samples/10-world/frame.cypost` carries it, this
    // file reads it, and the resolve divides by it before it tonemaps. A number typed here would
    // be a grade nobody could change without a compiler.
    GlobalsData globals;
    globals.exposure_stops = exposure_stops_;
    const u32 material_offsets[4] = {0, 0, 0, 0};
    const cy::rendering::pipeline::FrameUpload upload = cy::rendering::pipeline::upload_for(
        device_->assembly, report, world_to_clip, camera,
        Span<const cy::rendering::pipeline::InstanceTransform>(), globals, material_offsets);
    if (Status uploaded = device_->bindings.upload(slot, upload); !uploaded) {
        (void)device.end_frame();
        return uploaded;
    }

    // --- The read-back, declared as a write so the graph cannot cull it -------------------------

    ReadbackState readback;
    readback.color = resources.output;
    readback.buffer = device_->readback;
    readback.width = width_;
    readback.height = height_;

    cy::rendering::BufferRequest readback_request;
    readback_request.name = "world colour readback";
    readback_request.size = static_cast<u64>(width_) * height_ * sizeof(u32);
    readback_request.extra_usage = rhi::BufferUsage::TransferDestination;
    const ResourceId color_out = graph.import_buffer(readback_request, device_->readback);
    graph.add_pass("world readback", QueueKind::Graphics)
        .read(readback.color, Access::TransferRead)
        .write(color_out, Access::TransferWrite)
        .record(&record_readback, &readback);
    graph.add_pass("world host", QueueKind::Graphics)
        .read(color_out, Access::HostRead)
        .side_effect();
    if (Status declared = graph.status(); !declared) {
        (void)device.end_frame();
        return declared;
    }

    Status frame = ok();
    {
        cy::rendering::GraphExecutor executor(*allocator_, device);
        state.executor = &executor;
        resolve.executor = &executor;
        readback.executor = &executor;
        frame = device_->assembly.execute(executor, graph, report);
        if (frame) {
            frame = device.wait_idle();
        }
        out.submit_ms = now_millis() - mark;
        if (frame && png_path != nullptr) {
            frame = write_png(png_path);
        }
        executor.release();
    }
    if (Status ended = device.end_frame(); !ended && frame) {
        frame = ended;
    }
    if (frame) {
        std::swap(device_->foam_previous, device_->foam_next);
        rhi::DescriptorWrite foam_writes[2] = {};
        foam_writes[0].binding = 4;
        foam_writes[0].kind = rhi::DescriptorKind::StorageBuffer;
        foam_writes[0].buffer = device_->foam_previous;
        foam_writes[1].binding = 5;
        foam_writes[1].kind = rhi::DescriptorKind::StorageBuffer;
        foam_writes[1].buffer = device_->foam_next;
        if (Status updated = device.update_descriptor_set(
                device_->visual_set, Span<const rhi::DescriptorWrite>(foam_writes, 2));
            !updated) {
            frame = updated;
        }
    }

    // --- WHAT THE FRAME SAYS IT DID ------------------------------------------------------------
    //
    // The manifest is built from `AssemblyReport` and from nothing this file believes. It is a
    // PUBLICATION capture because the artefact is published: `capture_manifest` refuses one whose
    // frame never executed, and would refuse one taken while a budget arbiter was free to degrade
    // it — this program runs no arbiter at all, which is what `arbiter_pinned` says here.
    if (frame) {
        CaptureProvenance provenance;
        provenance.title = "samples/10-world";
        provenance.purpose = CapturePurpose::Publication;
        provenance.arbiter_pinned = true;
        provenance.ev100 = -exposure_stops_;
        provenance.quality = cy::rendering::post_quality_preset(cy::rendering::QualityLevel::High);
        const Expected<CaptureManifest, Error> manifest = cy::rendering::assembly::capture_manifest(
            device_->assembly.description(), report, provenance);
        if (!manifest.has_value()) {
            return make_unexpected(manifest.error());
        }
        out.manifest = *manifest;
        out.manifest_valid = true;
    }

    out.validation_errors = device_->validation_errors;
    return frame;
}

Status Stage::verify_terrain_agreement(const World& world, f32& worst_error,
                                       u32& compared) const noexcept {
    worst_error = 0.0F;
    compared = 0;
    if (!available_ || device_ == nullptr || !device_->handle.has_value()) {
        return fail(ErrorCode::Unavailable, "terrain agreement needs a graphics device");
    }
    const auto* device_colours = static_cast<const Vec3*>(
        device_->handle.value()->buffer_mapped_pointer(device_->static_colours));
    if (device_colours == nullptr) {
        return fail(ErrorCode::Internal,
                    "terrain agreement could not map the device colour stream");
    }
    constexpr f32 kTolerance = 0.0008F;
    u32 cursor = 0;
    for (const TerrainPatch& patch : world.terrain_patches()) {
        if (patch.colours.size() != patch.mesh.positions.size()) {
            return fail(ErrorCode::InvalidArgument,
                        "terrain reference did not shade every staged vertex");
        }
        for (const Vec3& reference : patch.colours.span()) {
            if (cursor >= terrain_vertices_) {
                return fail(ErrorCode::OutOfRange,
                            "terrain reference contains more vertices than the device stream");
            }
            const Vec3 actual = device_colours[cursor++];
            const f32 errors[3] = {std::abs(actual.x - reference.x),
                                   std::abs(actual.y - reference.y),
                                   std::abs(actual.z - reference.z)};
            for (const f32 error : errors) {
                worst_error = error > worst_error ? error : worst_error;
            }
        }
    }
    compared = cursor;
    if (compared == 0 || compared != terrain_vertices_) {
        return fail(ErrorCode::InvalidArgument,
                    "terrain agreement did not compare the complete device stream");
    }
    if (!std::isfinite(worst_error) || worst_error > kTolerance) {
        std::fprintf(stderr,
                     "terrain agreement: %u vertices, worst channel error %.8f (tolerance %.8f)\n",
                     compared, static_cast<double>(worst_error), static_cast<double>(kTolerance));
        return fail(ErrorCode::InvalidArgument,
                    "terrain device shading exceeds the CPU-reference tolerance");
    }
    return ok();
}

Status Stage::verify_cloud_agreement(const World&, f32& worst_error, u32& compared) const noexcept {
    worst_error = 0.0F;
    compared = 0;
    if (!available_ || device_ == nullptr || !device_->handle.has_value()) {
        return fail(ErrorCode::Unavailable, "cloud agreement needs a graphics device");
    }
    const auto* device_colours = static_cast<const Vec3*>(
        device_->handle.value()->buffer_mapped_pointer(device_->dynamic_colours));
    if (device_colours == nullptr || dynamic_vertices_.size() < sky_vertices_) {
        return fail(ErrorCode::Internal, "cloud agreement could not read the complete sky stream");
    }
    constexpr f32 kTolerance = 0.002F;
    for (u32 index = 0; index < sky_vertices_; ++index) {
        const Vec3 direction = normalised(dynamic_vertices_[index].normal);
        const Vec3 reference = compose_cloud_reference(direction, device_->last_visual);
        const Vec3 actual = device_colours[index];
        const f32 errors[3] = {std::abs(actual.x - reference.x), std::abs(actual.y - reference.y),
                               std::abs(actual.z - reference.z)};
        for (const f32 error : errors) {
            worst_error = error > worst_error ? error : worst_error;
        }
    }
    compared = sky_vertices_;
    if (compared == 0 || !std::isfinite(worst_error) || worst_error > kTolerance) {
        return fail(ErrorCode::InvalidArgument,
                    "cloud device shading exceeds the CPU-reference tolerance");
    }
    return ok();
}

Status Stage::verify_foam_output(u32& active_cells) const noexcept {
    active_cells = 0;
    if (!available_ || device_ == nullptr || !device_->handle.has_value()) {
        return fail(ErrorCode::Unavailable, "foam validation needs a graphics device");
    }
    const auto* foam = static_cast<const f32*>(
        device_->handle.value()->buffer_mapped_pointer(device_->foam_previous));
    if (foam == nullptr) {
        return fail(ErrorCode::Internal, "foam validation could not map the output buffer");
    }
    constexpr u32 kCells = 128U * 128U;
    for (u32 index = 0; index < kCells; ++index) {
        if (!std::isfinite(foam[index])) {
            return fail(ErrorCode::InvalidArgument,
                        "foam device output contains a non-finite cell");
        }
        active_cells += foam[index] > 0.0F ? 1U : 0U;
    }
    if (active_cells == 0) {
        return fail(ErrorCode::InvalidArgument, "foam device output contains no active cell");
    }
    return ok();
}

Status Stage::read_grade(const char* path) noexcept {
    // THE GRADE IS A COMMITTED FILE AND NOT A DEFAULT IN THIS FUNCTION. A missing file is an error:
    // a shot that silently fell back to zero stops is an ungraded shot photographed as a graded
    // one, which is the same class of claim `capture_manifest` refuses one level up.
    std::FILE* file = std::fopen(path, "r");
    if (file == nullptr) {
        return fail(ErrorCode::NotFound, "the committed grade could not be opened");
    }
    bool saw_exposure = false;
    char line[256];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        char key[64] = {};
        int consumed = 0;
        if (std::sscanf(line, "%63[a-z_-] = %n", key, &consumed) != 1 || consumed == 0) {
            continue;
        }
        // `strtod` RATHER THAN `%lf`: `sscanf` reports no conversion error, so a grade file with
        // `exposure-stops = kittens` in it would be read as 0 and the shot would be published at a
        // stop nobody chose.
        char* end = nullptr;
        const double value = std::strtod(line + consumed, &end);
        if (end == line + consumed) {
            continue;
        }
        if (std::strcmp(key, "exposure-stops") == 0) {
            exposure_stops_ = static_cast<f32>(value);
            saw_exposure = true;
        } else if (std::strcmp(key, "contrast") == 0) {
            grade_contrast_ = static_cast<f32>(value);
        } else if (std::strcmp(key, "saturation") == 0) {
            grade_saturation_ = static_cast<f32>(value);
        }
    }
    (void)std::fclose(file);
    if (!saw_exposure) {
        return fail(ErrorCode::InvalidArgument,
                    "the grade file names no `exposure-stops`, so the shot has no exposure");
    }
    return ok();
}

Status Stage::write_manifest(const StageReport& report, const char* path) noexcept {
    if (!report.manifest_valid) {
        return fail(ErrorCode::InvalidArgument,
                    "no frame executed, so there is no stage list to publish");
    }
    char text[4096] = {};
    const Expected<usize, Error> written =
        cy::rendering::assembly::write_capture_manifest(report.manifest, text, sizeof(text));
    if (!written.has_value()) {
        return make_unexpected(written.error());
    }
    std::FILE* file = std::fopen(path, "w");
    if (file == nullptr) {
        return fail(ErrorCode::Io, "the manifest could not be written");
    }
    const usize wrote = std::fwrite(text, 1, *written, file);
    (void)std::fclose(file);
    return wrote == *written ? ok() : fail(ErrorCode::Io, "the manifest was written short");
}

Status Stage::write_png(const char* path) noexcept {
    rhi::Device& device = *device_->handle.value();
    const auto* mapped = static_cast<const u32*>(device.buffer_mapped_pointer(device_->readback));
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "the colour readback buffer is not mapped");
    }
    for (usize texel = 0; texel < pixels_.size(); ++texel) {
        pixels_[texel] = mapped[texel];
    }
    render_test::Image image(*allocator_);
    if (Status adopted = render_test::adopt(image, pixels_.span(), width_, height_); !adopted) {
        return adopted;
    }
    return render_test::write_png(path, image);
}

void Stage::close() noexcept {
    if (device_ == nullptr) {
        return;
    }
    if (device_->handle.has_value()) {
        rhi::Device& device = *device_->handle.value();
        (void)device.wait_idle();
        // THE FRAME'S OWN OBJECTS FIRST, and before the device is destroyed: both hold device
        // handles and both state the same contract every device-owning object in this tree does.
        device_->bindings.shutdown();
        device_->pipelines.shutdown();
        if (!device_->output.is_null()) {
            device.destroy_texture(device_->output);
            device_->output = rhi::TextureHandle{};
        }
        for (auto& pipeline : device_->visual_pipelines) {
            if (!pipeline.is_null()) {
                device.destroy_compute_pipeline(pipeline);
            }
        }
        if (!device_->visual_layout.is_null()) {
            device.destroy_pipeline_layout(device_->visual_layout);
        }
        if (!device_->visual_set_layout.is_null()) {
            device.destroy_descriptor_set_layout(device_->visual_set_layout);
        }
        for (auto& shader : device_->visual_shaders) {
            if (!shader.is_null()) {
                device.destroy_shader_module(shader);
            }
        }
        device.destroy_graphics_pipeline(device_->pipeline);
        device.destroy_pipeline_layout(device_->layout);
        device.destroy_shader_module(device_->vertex);
        device.destroy_shader_module(device_->fragment);
        device.destroy_buffer(device_->static_vertices);
        device.destroy_buffer(device_->static_colours);
        device.destroy_buffer(device_->static_indices);
        device.destroy_buffer(device_->dynamic_vertices);
        device.destroy_buffer(device_->dynamic_colours);
        device.destroy_buffer(device_->dynamic_indices);
        device.destroy_buffer(device_->foam_previous);
        device.destroy_buffer(device_->foam_next);
        for (auto& field : device_->terrain_fields) {
            device.destroy_buffer(field);
        }
        device.destroy_buffer(device_->readback);
        device.destroy_buffer(device_->cloud_shadow_field);
        device.destroy_buffer(device_->cloud_shadow_placement);
        if (!device_->world_set_layout.is_null()) {
            device.destroy_descriptor_set_layout(device_->world_set_layout);
        }
        rhi::destroy_device(*allocator_, device_->handle.value());
    }
    delete device_;
    device_ = nullptr;
    available_ = false;
}

}  // namespace cy::sample::world
