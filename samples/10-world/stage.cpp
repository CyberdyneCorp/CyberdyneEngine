#include "stage.h"

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/pipeline.h>
#include <cy/backends/rhi/validation.h>
#include <cy/core/math/projection.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/water/shading.h>

#if defined(CY_SAMPLE_WORLD_VULKAN)
#    include <cy/backends/rhi/vulkan/vulkan_backend.h>
#endif

#include "golden.h"
#include "shaders/world_spirv.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <new>

namespace cy::sample::world {
namespace {

using cy::rendering::PassContext;
using cy::rendering::ResourceId;
using rhi::Access;
using rhi::QueueKind;

/// The sky dome's radius, in metres. Inside the far plane and outside everything else, so the dome
/// is depth-tested like any other geometry and no second pipeline state is needed for it.
constexpr f32 kSkyRadius = 9'000.0F;
constexpr f32 kFarPlane = 24'000.0F;
constexpr f32 kNearPlane = 1.0F;

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

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
    }
    std::fprintf(stderr, "vulkan validation %s: %s\n",
                 severity == rhi::ValidationSeverity::Error ? "error" : "warning",
                 message != nullptr ? message : "");
}

[[nodiscard]] Status upload_bytes(rhi::Device& device, rhi::BufferHandle buffer, const void* source,
                                  u64 bytes) noexcept {
    if (bytes == 0) {
        return ok();
    }
    void* mapped = device.buffer_mapped_pointer(buffer);
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "a staged buffer is not mapped");
    }
    const auto* from = static_cast<const u8*>(source);
    auto* to = static_cast<u8*>(mapped);
    for (u64 index = 0; index < bytes; ++index) {
        to[index] = from[index];
    }
    return ok();
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
    Device() noexcept = default;

    Expected<rhi::Device*, Error> handle = fail(ErrorCode::Unavailable, "not created");
    rhi::BackendSelection selection{};
    u32 validation_errors = 0;

    rhi::ShaderModuleHandle vertex;
    rhi::ShaderModuleHandle fragment;
    rhi::PipelineLayoutHandle layout;
    rhi::GraphicsPipelineHandle pipeline;
    rhi::BufferHandle static_vertices;
    rhi::BufferHandle static_colours;
    rhi::BufferHandle static_indices;
    rhi::BufferHandle dynamic_vertices;
    rhi::BufferHandle dynamic_colours;
    rhi::BufferHandle dynamic_indices;
    rhi::BufferHandle readback;
};

Stage::Stage(Allocator& allocator) noexcept
    : allocator_(&allocator),
      dynamic_vertices_(allocator),
      dynamic_colours_(allocator),
      dynamic_indices_(allocator),
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

Status Stage::open(u32 width, u32 height) noexcept {
    width_ = width;
    height_ = height;
    device_ = new (std::nothrow) Device();
    if (device_ == nullptr) {
        return fail(ErrorCode::OutOfMemory, "the stage did not allocate");
    }
#if defined(CY_SAMPLE_WORLD_VULKAN)
    (void)rhi::vulkan::register_vulkan_backend();
#endif
    (void)rhi::null::register_null_backend();

    rhi::DeviceDescription description;
    description.application_name = "cy_sample_world";
    // VALIDATION ON, AND ITS ERRORS REPORTED AS A NUMBER rather than as a log line nobody reads.
    description.enable_validation = true;
    description.enable_synchronisation_validation = true;
    device_->handle = rhi::create_device(*allocator_, "vulkan", description, device_->selection);
    if (!device_->handle.has_value()) {
        return ok();
    }
    if (device_->handle.value()->capabilities().backend() != rhi::BackendKind::Vulkan) {
        return ok();
    }
    device_->handle.value()->set_validation_callback(&count_validation,
                                                     &device_->validation_errors);
    available_ = true;
    return create_pipeline();
}

Status Stage::create_pipeline() noexcept {
    rhi::Device& device = *device_->handle.value();

    rhi::ShaderModuleDescription vertex;
    vertex.name = "world vertex";
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.entry_point = "main";
    vertex.spirv = Span<const u32>(kWorldVertexSpirv, sizeof(kWorldVertexSpirv) / sizeof(u32));
    Expected<rhi::ShaderModuleHandle, Error> vertex_module = device.create_shader_module(vertex);
    if (!vertex_module) {
        return make_unexpected(vertex_module.error());
    }
    device_->vertex = *vertex_module;

    rhi::ShaderModuleDescription fragment;
    fragment.name = "world fragment";
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.entry_point = "main";
    fragment.spirv =
        Span<const u32>(kWorldFragmentSpirv, sizeof(kWorldFragmentSpirv) / sizeof(u32));
    Expected<rhi::ShaderModuleHandle, Error> fragment_module =
        device.create_shader_module(fragment);
    if (!fragment_module) {
        return make_unexpected(fragment_module.error());
    }
    device_->fragment = *fragment_module;

    const rhi::PushConstantRange range{rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
                                       sizeof(WorldPush)};
    rhi::PipelineLayoutDescription layout;
    layout.name = "world layout";
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

    rhi::ColorAttachmentState color;
    color.format = rhi::Format::Rgba8Unorm;

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
    return pixels_.resize(static_cast<usize>(width_) * height_);
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
    description.usage = rhi::BufferUsage::Vertex;
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
    const u64 index_capacity = static_cast<u64>(world.sky_indices().size()) + (water_vertices * 6) +
                               star_vertices + (static_cast<u64>(kMaxDrawnPlants) * kPlantVertices);

    description.name = "world dynamic geometry";
    description.size = capacity * sizeof(Vertex);
    description.usage = rhi::BufferUsage::Vertex;
    buffer = device.create_buffer(description);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->dynamic_vertices = *buffer;

    description.name = "world dynamic colours";
    description.size = capacity * sizeof(Vec3);
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

    if (Status reserved = dynamic_vertices_.reserve(capacity); !reserved) {
        return reserved;
    }
    if (Status reserved = dynamic_colours_.reserve(capacity); !reserved) {
        return reserved;
    }
    return dynamic_indices_.reserve(index_capacity);
}

// ================================================================================================
// THE PER-FRAME STREAMS
// ================================================================================================

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
    water_first_index_ = static_cast<u32>(dynamic_indices_.size());
    const water::WaterOptics optics = water::clear_sea_optics();
    const Span<const Vec3> positions = ocean.positions();
    const Span<const Vec3> normals = ocean.normals();
    const Span<const f32> breaking = ocean.breaking();
    for (usize index = 0; index < positions.size(); ++index) {
        Vertex vertex;
        vertex.position = Vec3{static_cast<f32>(ocean.origin().x - middle.x) + positions[index].x,
                               static_cast<f32>(ocean.origin().y) + positions[index].y,
                               static_cast<f32>(ocean.origin().z - middle.z) + positions[index].z};
        vertex.normal = normals[index];
        if (Status pushed = dynamic_vertices_.push_back(vertex); !pushed) {
            return pushed;
        }
        // THE WATER'S COLOUR IS THE BODY'S OWN OPTICS, not a painted blue. `water_column_colour()`
        // is src/water/'s Beer-Lambert absorption plus its in-scatter term over a nominal column,
        // which is what makes deep water darken AND shift hue with nothing authored — and the foam
        // is the surface's own breaking indicator, which comes from the Jacobian of the Gerstner
        // map rather than from a threshold on height.
        const Vec3 deep = water::water_column_colour(optics, 14.0F, Vec3{0.02F, 0.05F, 0.07F});
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

    // --- THE FOLIAGE, one proxy per plant near the camera.
    foliage_first_index_ = static_cast<u32>(dynamic_indices_.size());
    out.plants_drawn = 0;
    for (const PlantDraw& plant : world.plants()) {
        if (out.plants_drawn >= kMaxDrawnPlants) {
            break;
        }
        const Vec3 base{plant.position.x - static_cast<f32>(middle.x), plant.position.y,
                        plant.position.z - static_cast<f32>(middle.z)};
        const f32 dx = base.x - relative_eye.x;
        const f32 dz = base.z - relative_eye.z;
        if ((dx * dx) + (dz * dz) > kPlantDrawMetres * kPlantDrawMetres) {
            continue;
        }
        const u32 first = static_cast<u32>(dynamic_vertices_.size());
        const f32 trunk_fraction = trunk_fraction_of(plant.kind);
        const f32 trunk_height = plant.height * trunk_fraction;
        const f32 trunk_radius = plant.radius * (plant.kind == PlantKind::Tree ? 0.16F : 0.25F);
        const Vec3 apex{base.x + plant.crown_offset.x, base.y + plant.height,
                        base.z + plant.crown_offset.z};
        const Vec3 crown_base{base.x + (plant.crown_offset.x * 0.25F), base.y + trunk_height,
                              base.z + (plant.crown_offset.z * 0.25F)};

        // The crown: a cone whose apex carries the full wind displacement and whose skirt carries a
        // quarter of it, which is the shear a trunk-and-branch response produces along the plant.
        for (u32 side = 0; side < kCrownSides; ++side) {
            const f32 a = plant.yaw + (6.28318F * static_cast<f32>(side) / kCrownSides);
            const f32 b = plant.yaw + (6.28318F * static_cast<f32>(side + 1) / kCrownSides);
            const Vec3 left{crown_base.x + (std::cos(a) * plant.radius), crown_base.y,
                            crown_base.z + (std::sin(a) * plant.radius)};
            const Vec3 right{crown_base.x + (std::cos(b) * plant.radius), crown_base.y,
                             crown_base.z + (std::sin(b) * plant.radius)};
            const Vec3 normal = face_normal(apex, left, right);
            const Vec3 corners[3] = {apex, left, right};
            for (const Vec3& corner : corners) {
                Vertex vertex;
                vertex.position = corner;
                vertex.normal = normal;
                if (Status pushed = dynamic_vertices_.push_back(vertex); !pushed) {
                    return pushed;
                }
                if (Status pushed = dynamic_colours_.push_back(plant.crown_colour); !pushed) {
                    return pushed;
                }
            }
        }
        // The trunk: a three-sided prism, leaning by a quarter of the crown's displacement.
        for (u32 side = 0; side < kTrunkSides; ++side) {
            const f32 a = plant.yaw + (6.28318F * static_cast<f32>(side) / kTrunkSides);
            const f32 b = plant.yaw + (6.28318F * static_cast<f32>(side + 1) / kTrunkSides);
            const Vec3 low_a{base.x + (std::cos(a) * trunk_radius), base.y,
                             base.z + (std::sin(a) * trunk_radius)};
            const Vec3 low_b{base.x + (std::cos(b) * trunk_radius), base.y,
                             base.z + (std::sin(b) * trunk_radius)};
            const Vec3 high_a{low_a.x + (plant.crown_offset.x * 0.25F), base.y + trunk_height,
                              low_a.z + (plant.crown_offset.z * 0.25F)};
            const Vec3 high_b{low_b.x + (plant.crown_offset.x * 0.25F), base.y + trunk_height,
                              low_b.z + (plant.crown_offset.z * 0.25F)};
            const Vec3 normal = face_normal(low_a, high_a, low_b);
            const Vec3 corners[6] = {low_a, high_a, low_b, low_b, high_a, high_b};
            for (const Vec3& corner : corners) {
                Vertex vertex;
                vertex.position = corner;
                vertex.normal = normal;
                if (Status pushed = dynamic_vertices_.push_back(vertex); !pushed) {
                    return pushed;
                }
                if (Status pushed = dynamic_colours_.push_back(plant.trunk_colour); !pushed) {
                    return pushed;
                }
            }
        }
        for (u32 vertex = 0; vertex < kPlantVertices; ++vertex) {
            if (Status pushed = dynamic_indices_.push_back(first + vertex); !pushed) {
                return pushed;
            }
        }
        ++out.plants_drawn;
    }
    foliage_index_count_ = static_cast<u32>(dynamic_indices_.size()) - foliage_first_index_;
    out.foliage_triangles = foliage_index_count_ / 3;

    // --- The terrain's colours, which are the only part of it that changes.
    usize cursor = 0;
    for (const TerrainPatch& patch : world.terrain_patches()) {
        for (const Vec3& colour : patch.colours.span()) {
            terrain_colours_[cursor] = colour;
            ++cursor;
        }
    }
    out.terrain_triangles = terrain_indices_ / 3;
    return ok();
}

Status Stage::upload_dynamic() noexcept {
    rhi::Device& device = *device_->handle.value();
    if (Status uploaded = upload_bytes(device, device_->static_colours, terrain_colours_.data(),
                                       terrain_colours_.size() * sizeof(Vec3));
        !uploaded) {
        return uploaded;
    }
    if (Status uploaded = upload_bytes(device, device_->dynamic_vertices, dynamic_vertices_.data(),
                                       dynamic_vertices_.size() * sizeof(Vertex));
        !uploaded) {
        return uploaded;
    }
    if (Status uploaded = upload_bytes(device, device_->dynamic_colours, dynamic_colours_.data(),
                                       dynamic_colours_.size() * sizeof(Vec3));
        !uploaded) {
        return uploaded;
    }
    return upload_bytes(device, device_->dynamic_indices, dynamic_indices_.data(),
                        dynamic_indices_.size() * sizeof(u32));
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
    if (Status uploaded = upload_dynamic(); !uploaded) {
        return uploaded;
    }
    out.build_ms = now_millis() - mark;

    mark = now_millis();
    if (Expected<u32, Error> began = device.begin_frame(); !began) {
        return make_unexpected(began.error());
    }

    cy::rendering::RenderGraph graph(*allocator_);
    cy::rendering::GraphExecutor executor(*allocator_, device);

    cy::rendering::TextureRequest color_request;
    color_request.name = "world colour";
    color_request.format = rhi::Format::Rgba8Unorm;
    color_request.width = width_;
    color_request.height = height_;
    const ResourceId color = graph.create_texture(color_request);

    cy::rendering::TextureRequest depth_request;
    depth_request.name = "world depth";
    depth_request.format = rhi::Format::D32Sfloat;
    depth_request.width = width_;
    depth_request.height = height_;
    const ResourceId depth = graph.create_texture(depth_request);

    cy::rendering::BufferRequest readback_request;
    readback_request.name = "world colour readback";
    readback_request.size = static_cast<u64>(width_) * height_ * sizeof(u32);
    readback_request.extra_usage = rhi::BufferUsage::TransferDestination;
    const ResourceId color_out = graph.import_buffer(readback_request, device_->readback);

    const WorldVec3d middle = world.centre();
    const Vec3 relative_eye{static_cast<f32>(eye.x - middle.x), static_cast<f32>(eye.y),
                            static_cast<f32>(eye.z - middle.z)};
    const Vec3 relative_target{static_cast<f32>(target.x - middle.x), static_cast<f32>(target.y),
                               static_cast<f32>(target.z - middle.z)};
    const Mat4 projection = perspective_reversed_z(
        0.95F, static_cast<f32>(width_) / static_cast<f32>(height_), kNearPlane, kFarPlane);
    const Mat4 world_to_clip = projection * look_at(relative_eye, relative_target);

    WorldPush view;
    for (usize row = 0; row < 4; ++row) {
        const Vec4 values = world_to_clip.row(row);
        switch (row) {
            case 0:
                write_row(view.row0, values);
                break;
            case 1:
                write_row(view.row1, values);
                break;
            case 2:
                write_row(view.row2, values);
                break;
            default:
                write_row(view.row3, values);
                break;
        }
    }
    const Lighting& lighting = world.lighting();
    write_vec3(view.light, lighting.sun_travel, 0.0F);
    write_vec3(view.eye, relative_eye, 0.0F);
    write_vec3(view.sun, lighting.sun_colour, 0.0F);
    write_vec3(view.ambient, lighting.ambient, 0.0F);

    DrawState state;
    state.executor = &executor;
    state.pipeline = device_->pipeline;
    state.layout = device_->layout;
    state.static_vertices = device_->static_vertices;
    state.static_colours = device_->static_colours;
    state.static_indices = device_->static_indices;
    state.dynamic_vertices = device_->dynamic_vertices;
    state.dynamic_colours = device_->dynamic_colours;
    state.dynamic_indices = device_->dynamic_indices;
    state.color = color;
    state.depth = depth;
    state.width = width_;
    state.height = height_;

    // The sky, emissive, first — so that a frame in which it failed to draw shows black rather than
    // a plausible background this file invented.
    state.runs[0].first_index = sky_first_index_;
    state.runs[0].index_count = sky_index_count_;
    state.runs[0].dynamic = true;
    state.runs[0].push = view;
    state.runs[0].push.eye[3] = 1.0F;

    state.runs[1].first_index = 0;
    state.runs[1].index_count = terrain_indices_;
    state.runs[1].dynamic = false;
    state.runs[1].push = view;

    state.runs[2].first_index = water_first_index_;
    state.runs[2].index_count = water_index_count_;
    state.runs[2].dynamic = true;
    state.runs[2].push = view;
    // The specular lobe is water's alone. See the shader.
    state.runs[2].push.sun[3] = 24.0F;

    state.runs[3].first_index = star_first_index_;
    state.runs[3].index_count = star_index_count_;
    state.runs[3].dynamic = true;
    state.runs[3].push = view;
    state.runs[3].push.eye[3] = 1.0F;

    state.runs[4].first_index = foliage_first_index_;
    state.runs[4].index_count = foliage_index_count_;
    state.runs[4].dynamic = true;
    state.runs[4].push = view;
    state.run_count = 5;

    ReadbackState readback;
    readback.executor = &executor;
    readback.color = color;
    readback.buffer = device_->readback;
    readback.width = width_;
    readback.height = height_;

    graph.add_pass("world draw", QueueKind::Graphics)
        .write(color, Access::ColorAttachmentWrite)
        .write(depth, Access::DepthStencilAttachmentWrite)
        .record(&record_draw, &state);
    graph.add_pass("world readback", QueueKind::Graphics)
        .read(color, Access::TransferRead)
        .write(color_out, Access::TransferWrite)
        .record(&record_readback, &readback);
    graph.add_pass("world host", QueueKind::Graphics)
        .read(color_out, Access::HostRead)
        .side_effect();
    if (Status declared = graph.status(); !declared) {
        return declared;
    }

    Status frame = ok();
    if (Expected<cy::rendering::ExecutionResult, Error> executed = executor.execute(
            graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{});
        !executed) {
        frame = make_unexpected(executed.error());
    }
    if (frame) {
        frame = device.wait_idle();
    }
    out.submit_ms = now_millis() - mark;
    if (frame && png_path != nullptr) {
        frame = write_png(png_path);
    }
    executor.release();
    if (Status ended = device.end_frame(); !ended && frame) {
        frame = ended;
    }
    out.validation_errors = device_->validation_errors;
    return frame;
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
        device.destroy_buffer(device_->readback);
        rhi::destroy_device(*allocator_, device_->handle.value());
    }
    delete device_;
    device_ = nullptr;
    available_ = false;
}

}  // namespace cy::sample::world
