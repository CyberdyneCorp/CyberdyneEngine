#include "stage.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/core/math/projection.h>
#include <cy/rendering/assembly/frame_assembly.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/lighting/lights.h>
#include <cy/rendering/material/standard.h>
#include <cy/rendering/pipeline/frame_recorder.h>
#include <cy/rendering/skinning/frame_skinning.h>
#include <cy/rendering/skinning/skinned_scene.h>

#if defined(CY_SAMPLE_CHARACTER_VULKAN)
#    include <cy/backends/rhi/vulkan/vulkan_backend.h>
#endif

#include "golden.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

namespace cy::sample::character {
namespace {

using namespace cy::rendering;
using namespace cy::rendering::pipeline;

/// The ground: a square of one-metre tiles, centred on the origin, large enough that the camera's
/// slow turn never shows its edge.
constexpr i32 kGroundHalfTiles = 10;
constexpr u32 kTiles = static_cast<u32>(kGroundHalfTiles * kGroundHalfTiles * 4);
/// The tile's thickness: a slab, so the shadow pass has a receiver with a top face at y = 0.
constexpr f32 kTileThickness = 0.05F;
/// The character's GPU scene slot: after every tile.
constexpr u32 kCharacterSlot = kTiles;
constexpr u32 kInstances = kTiles + 1U;
constexpr u32 kCubeVertices = 24;
constexpr u32 kCubeIndices = 36;
constexpr u32 kMaterials = 3;
/// The frame's set 0 texture-table slot the shadow map is bound at, and its size.
constexpr u32 kShadowSlot = 121;
constexpr u32 kShadowExtent = 2048;
/// The shadow volume around the character: what the follow camera can see of the ground.
constexpr f32 kShadowRadius = 5.0F;
/// The way the sun TRAVELS: high and from behind the camera's left shoulder, the key light the
/// sample has always had.
const Vec3 kSunTravel{0.45F, -0.75F, -0.48F};
/// Exposure in stops for a 22 000 lux sun: `FrameScene`'s, the frame's own physical units.
constexpr f32 kExposureStops = -11.4F;
inline constexpr rhi::Format kOutputFormat = rhi::Format::Rgba8Unorm;

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
    }
    std::fprintf(stderr, "vulkan validation %s: %s\n",
                 severity == rhi::ValidationSeverity::Error ? "error" : "warning",
                 message != nullptr ? message : "");
}

[[nodiscard]] Status upload_bytes(rhi::Device& device, rhi::BufferHandle buffer, const void* source,
                                  u64 bytes, u64 offset = 0) noexcept {
    auto* mapped = static_cast<u8*>(device.buffer_mapped_pointer(buffer));
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "a staged buffer is not mapped");
    }
    std::memcpy(mapped + offset, source, bytes);
    return ok();
}

[[nodiscard]] Expected<rhi::BufferHandle, Error> make_buffer(rhi::Device& device, const char* name,
                                                             u64 bytes, rhi::BufferUsage usage,
                                                             rhi::MemoryUse memory) noexcept {
    rhi::BufferDescription description;
    description.name = name;
    description.size = bytes;
    description.usage = usage;
    description.memory = memory;
    return device.create_buffer(description);
}

/// The frame's unit cube in its three streams: four vertices a face, so each face has its own
/// normal. The tiles are instances of it.
struct Cube {
    f32 positions[kCubeVertices * 3] = {};
    u16 normals[kCubeVertices * 4] = {};
    f32 uvs[kCubeVertices * 2] = {};
    u16 indices[kCubeIndices] = {};

    Cube() noexcept {
        static constexpr Vec3 kAxes[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                                          Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
        u32 vertex = 0;
        u32 index = 0;
        for (const Vec3 normal : kAxes) {
            const Vec3 tangent =
                std::fabs(normal.y) > 0.5F ? Vec3{1.0F, 0.0F, 0.0F} : Vec3{0.0F, 1.0F, 0.0F};
            const Vec3 bitangent = cross(normal, tangent);
            const auto first = static_cast<u16>(vertex);
            for (u32 corner = 0; corner < 4U; ++corner) {
                const f32 u = (corner == 1U || corner == 2U) ? 1.0F : -1.0F;
                const f32 v = (corner >= 2U) ? 1.0F : -1.0F;
                const Vec3 position =
                    (normal * 0.5F) + (tangent * (u * 0.5F)) + (bitangent * (v * 0.5F));
                positions[(vertex * 3U) + 0] = position.x;
                positions[(vertex * 3U) + 1] = position.y;
                positions[(vertex * 3U) + 2] = position.z;
                pack_normal_stream(normal, tangent, &normals[static_cast<size_t>(vertex) * 4U]);
                uvs[(vertex * 2U) + 0] = (u * 0.5F) + 0.5F;
                uvs[(vertex * 2U) + 1] = (v * 0.5F) + 0.5F;
                ++vertex;
            }
            for (const u32 step : {0U, 1U, 2U, 0U, 2U, 3U}) {
                indices[index++] = static_cast<u16>(first + step);
            }
        }
    }
};

[[nodiscard]] Mat4 shadow_to_clip(Vec3 centre) noexcept {
    const Vec3 travel = normalize(kSunTravel);
    const Vec3 eye = centre - (travel * (kShadowRadius * 3.0F));
    const Mat4 view = look_at(eye, centre, Vec3{0.0F, 1.0F, 0.0F});
    const Mat4 projection = orthographic_reversed_z(-kShadowRadius, kShadowRadius, -kShadowRadius,
                                                    kShadowRadius, 0.1F, kShadowRadius * 6.0F);
    return projection * view;
}

}  // namespace

/// Everything that needs a device, so that the header names none of it and a build without the
/// Vulkan backend still compiles this file.
struct Stage::Device {
    explicit Device(Allocator& allocator) noexcept
        : assembly(allocator),
          index(allocator),
          graph(allocator),
          program(allocator),
          skins(allocator),
          instances(allocator),
          centres(allocator),
          lights(allocator),
          frame_lights(allocator) {}

    Expected<rhi::Device*, Error> handle = fail(ErrorCode::Unavailable, "not created");
    rhi::BackendSelection selection{};
    u32 validation_errors = 0;

    FrameAssembly assembly;
    SpatialIndex index;
    RenderGraph graph;
    MaterialProgram program;
    FramePipelines pipelines;
    FrameBindings bindings;
    FrameRecorder recorder;
    skinning::SkinnedScene skins;
    skinning::SkinnedInstance character;
    skinning::SkinnedDrawMesh character_mesh;
    u32 character_vertices = 0;

    Array<InstanceTransform> instances;
    /// Each instance's world centre and scale: the tiles', then the character's.
    Array<Vec4> centres;
    Array<render::LightDescription> lights;
    Array<GpuLight> frame_lights;
    u32 material_offsets[4] = {0, 0, 0, 0};
    u32 material_slots[kMaterials] = {};
    DrawSurface surface;
    FrameResourceRead opaque_reads[1] = {};

    Cube cube;
    rhi::BufferHandle positions;
    rhi::BufferHandle normals;
    rhi::BufferHandle uvs;
    rhi::BufferHandle cube_indices;
    rhi::BufferHandle character_indices;
    rhi::BufferHandle readback;
    rhi::TextureHandle output;
    rhi::TextureHandle shadow_color;
    rhi::TextureHandle shadow_depth;
    rhi::TextureViewHandle shadow_view;

    Vec3 eye{0.0F, 0.0F, 0.0F};
    Vec3 shadow_centre{0.0F, 0.0F, 0.0F};
};

namespace {

/// Every tile is the cube; the character is the skinning pass's output.
bool stage_geometry(const render::DrawItem& /*item*/, const GpuDrawInstance& instance, void* user,
                    DrawGeometry& out) noexcept {
    auto& device = *static_cast<Stage::Device*>(user);
    if (instance.instance_slot == kCharacterSlot) {
        return skinning::skinned_draw_geometry(device.skins, device.character,
                                               device.character_mesh, out);
    }
    out.indices = device.cube_indices;
    out.wide_indices = false;
    out.index_count = kCubeIndices;
    out.first_index = 0;
    out.vertex_offset = 0;
    return true;
}

/// The light and dark squares, and the character's own material.
Span<const DrawSurface> stage_surfaces(const VisibleInstance& instance, void* user) noexcept {
    auto& device = *static_cast<Stage::Device*>(user);
    DrawSurface& surface = device.surface;
    surface.pipeline = 0;
    surface.mesh = 1;
    surface.surface = 0;
    surface.blend = render::BlendMode::Opaque;
    if (instance.gpu_slot == kCharacterSlot) {
        surface.material = device.material_slots[2];
    } else {
        const auto tile = static_cast<i32>(instance.gpu_slot);
        const i32 x = tile % (kGroundHalfTiles * 2);
        const i32 z = tile / (kGroundHalfTiles * 2);
        surface.material = device.material_slots[(x + z) & 1];
    }
    return {&surface, 1};
}

struct Readback {
    ResourceId output = kInvalidResource;
    rhi::BufferHandle buffer;
    u32 width = 0;
    u32 height = 0;
};

void record_readback(const PassContext& context, void* user) noexcept {
    auto* readback = static_cast<Readback*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{readback->width, readback->height, 1};
    context.commands->copy_texture_to_buffer(context.executor->texture(readback->output),
                                             readback->buffer,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

}  // namespace

Stage::Stage(Allocator& allocator) noexcept : allocator_(&allocator), pixels_(allocator) {}

Stage::~Stage() {
    close();
}

const char* Stage::absence() const noexcept {
    if (device_ == nullptr) {
        return "the stage was never opened";
    }
    return device_->selection.reason != nullptr ? device_->selection.reason
                                                : "no reason was reported";
}

Status Stage::open(u32 width, u32 height) noexcept {
    width_ = width;
    height_ = height;
    device_ = new (std::nothrow) Device(*allocator_);
    if (device_ == nullptr) {
        return fail(ErrorCode::OutOfMemory, "the stage did not allocate");
    }
#if defined(CY_SAMPLE_CHARACTER_VULKAN)
    (void)rhi::vulkan::register_vulkan_backend();
#endif
    (void)rhi::null::register_null_backend();

    rhi::DeviceDescription description;
    description.application_name = "cy_sample_animated-character";
    // VALIDATION ON, AND ITS ERRORS REPORTED AS A NUMBER rather than as a log line nobody reads.
    // Synchronisation validation with it, because the claim about the barriers between the
    // skinning dispatch and the frame's passes is a synchronisation claim.
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
    return pixels_.resize(static_cast<usize>(width_) * height_);
}

Status Stage::create_frame() noexcept {
    rhi::Device& device = *device_->handle.value();
    AssemblyDescription description;
    description.width = width_;
    description.height = height_;
    description.near_plane = 0.05F;
    description.far_plane = 200.0F;
    description.clusters = ClusterGridConfig{32, 18, 32};
    description.material_capacity = kMaterials + 1U;
    description.max_draws = kInstances + 16U;
    description.max_instances = kInstances + 16U;
    description.gpu_culling = false;
    description.post.temporal_antialiasing = true;
    if (Status made = device_->assembly.initialize(description); !made) {
        return made;
    }
    if (Status attached = device_->assembly.attach_device(device); !attached) {
        return attached;
    }

    PipelineSetup setup;
    setup.color_format = description.color_format;
    setup.depth_format = description.depth_format;
    setup.output_format = kOutputFormat;
    setup.prepass_normal = true;
    setup.prepass_velocity = true;
    // THE SKINNED VARIANTS: the character's normal-tangent stream is the dispatch's own
    // `Rgba16Snorm` encoding.
    setup.skinned = true;
    if (Status made = device_->pipelines.initialize(device, setup); !made) {
        return made;
    }
    Expected<ClusterGrid, Error> grid = make_cluster_grid(
        description.clusters, width_, height_, description.near_plane, description.far_plane);
    if (!grid.has_value()) {
        return make_unexpected(grid.error());
    }
    const BindingCapacity capacity = BindingCapacity::for_grid(
        *grid, description.max_draws, description.max_instances, description.material_capacity, 16);
    if (Status made = device_->bindings.initialize(device, device_->pipelines, capacity); !made) {
        return made;
    }
    if (Status made = device_->recorder.initialize(device_->pipelines, device_->bindings); !made) {
        return made;
    }

    // The materials: two greys for the chequerboard, and the character's warm tone.
    if (Status described =
            describe_standard_material(device_->program, Name::intern("standard"),
                                       render::ShadingModel::Lit, render::BlendMode::Opaque);
        !described) {
        return described;
    }
    const StandardParameters ids;
    const ParameterId wanted[4] = {ids.base_color_factor, ids.roughness_factor, ids.metallic_factor,
                                   ids.emission_color};
    for (u32 index = 0; index < 4U; ++index) {
        const MaterialParameter* parameter = device_->program.find(wanted[index]);
        if (parameter == nullptr) {
            return fail(ErrorCode::NotFound, "the standard material lacks a parameter");
        }
        device_->material_offsets[index] = parameter->offset / 4U;
    }
    static constexpr f32 kColours[kMaterials][3] = {
        {0.20F, 0.21F, 0.24F}, {0.11F, 0.12F, 0.15F}, {0.78F, 0.60F, 0.42F}};
    MaterialTable& table = device_->assembly.materials();
    for (u32 material = 0; material < kMaterials; ++material) {
        Expected<u32, Error> allocated = table.allocate();
        if (!allocated.has_value()) {
            return make_unexpected(allocated.error());
        }
        device_->material_slots[material] = *allocated;
        if (Status defaults = apply_standard_defaults(device_->program, table, *allocated, ids);
            !defaults) {
            return defaults;
        }
        const Vec4 colour{kColours[material][0], kColours[material][1], kColours[material][2],
                          1.0F};
        if (Status set =
                table.set_color(device_->program, *allocated, ids.base_color_factor, colour);
            !set) {
            return set;
        }
        if (Status set = table.set_float(device_->program, *allocated, ids.roughness_factor,
                                         material == 2U ? 0.55F : 0.85F);
            !set) {
            return set;
        }
    }

    // The sun, from the key light's direction.
    if (Status sized = device_->lights.resize(1); !sized) {
        return sized;
    }
    render::LightDescription& sun = device_->lights[0];
    sun.kind = render::LightKind::Directional;
    sun.intensity = 22000.0F;
    sun.transform = Transform::from_translation(Vec3{0.0F, 12.0F, 0.0F});
    sun.transform.rotation = Quat::look_rotation(normalize(kSunTravel), Vec3{0.0F, 1.0F, 0.0F});
    sun.color[0] = 1.0F;
    sun.color[1] = 0.96F;
    sun.color[2] = 0.88F;
    sun.stable_id = 1;

    // The output, the read-back and the shadow map.
    rhi::TextureDescription output;
    output.name = "animated character output";
    output.format = kOutputFormat;
    output.extent = rhi::Extent3D{width_, height_, 1};
    output.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSource;
    Expected<rhi::TextureHandle, Error> made_output = device.create_texture(output);
    if (!made_output.has_value()) {
        return make_unexpected(made_output.error());
    }
    device_->output = *made_output;
    Expected<rhi::BufferHandle, Error> readback =
        make_buffer(device, "animated character readback", u64{width_} * height_ * sizeof(u32),
                    rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback);
    if (!readback.has_value()) {
        return make_unexpected(readback.error());
    }
    device_->readback = *readback;

    rhi::TextureDescription shadow;
    shadow.name = "animated character shadow map";
    shadow.format = rhi::Format::R32Sfloat;
    shadow.extent = rhi::Extent3D{kShadowExtent, kShadowExtent, 1};
    shadow.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
    Expected<rhi::TextureHandle, Error> color = device.create_texture(shadow);
    if (!color.has_value()) {
        return make_unexpected(color.error());
    }
    device_->shadow_color = *color;
    shadow.name = "animated character shadow depth";
    shadow.format = rhi::Format::D32Sfloat;
    shadow.usage = rhi::TextureUsage::DepthStencilAttachment;
    Expected<rhi::TextureHandle, Error> depth = device.create_texture(shadow);
    if (!depth.has_value()) {
        return make_unexpected(depth.error());
    }
    device_->shadow_depth = *depth;
    rhi::TextureViewDescription view;
    view.name = "animated character shadow map";
    view.texture = device_->shadow_color;
    Expected<rhi::TextureViewHandle, Error> made_view = device.create_texture_view(view);
    if (!made_view.has_value()) {
        return make_unexpected(made_view.error());
    }
    device_->shadow_view = *made_view;
    return ok();
}

Status Stage::create_scene(const Character& character) noexcept {
    rhi::Device& device = *device_->handle.value();
    const auto vertices = static_cast<u32>(character.positions.size());
    device_->character_vertices = vertices;

    // THE RIGID STREAMS: the cube, then — in the UV stream only — a zero coordinate per character
    // vertex, because a skinned draw reads its UVs from the frame's streams at its mesh's own
    // vertices and the character carries none.
    const Cube& cube = device_->cube;
    struct Request {
        const char* name;
        u64 bytes;
        rhi::BufferUsage usage;
        rhi::BufferHandle* out;
    };
    const u64 uv_bytes = (u64{kCubeVertices} + vertices) * sizeof(f32) * 2U;
    const Request requests[] = {
        {"animated character cube positions", sizeof(cube.positions), rhi::BufferUsage::Vertex,
         &device_->positions},
        {"animated character cube normals", sizeof(cube.normals), rhi::BufferUsage::Vertex,
         &device_->normals},
        {"animated character uvs", uv_bytes, rhi::BufferUsage::Vertex, &device_->uvs},
        {"animated character cube indices", sizeof(cube.indices), rhi::BufferUsage::Index,
         &device_->cube_indices},
        {"animated character indices", character.indices.size() * sizeof(u32),
         rhi::BufferUsage::Index, &device_->character_indices},
    };
    for (const Request& request : requests) {
        Expected<rhi::BufferHandle, Error> made =
            make_buffer(device, request.name, request.bytes, request.usage, rhi::MemoryUse::Upload);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        *request.out = *made;
    }
    std::memset(device.buffer_mapped_pointer(device_->uvs), 0, uv_bytes);
    const Status uploads[] = {
        upload_bytes(device, device_->positions, cube.positions, sizeof(cube.positions)),
        upload_bytes(device, device_->normals, cube.normals, sizeof(cube.normals)),
        upload_bytes(device, device_->uvs, cube.uvs, sizeof(cube.uvs)),
        upload_bytes(device, device_->cube_indices, cube.indices, sizeof(cube.indices)),
        upload_bytes(device, device_->character_indices, character.indices.data(),
                     character.indices.size() * sizeof(u32)),
    };
    for (const Status& uploaded : uploads) {
        if (!uploaded) {
            return uploaded;
        }
    }
    GeometrySource geometry;
    geometry.streams[kPositionStream] = device_->positions;
    geometry.streams[kNormalStream] = device_->normals;
    geometry.streams[kUvStream] = device_->uvs;
    geometry.geometry = &stage_geometry;
    geometry.user = device_;
    device_->recorder.set_geometry(geometry);

    // THE SKINNED SCENE: the character's bind pose once, one instance, and a pose buffer the size
    // of the pose world — one character, two halves of its skeleton.
    const u32 joints = character.skeleton.joint_count();
    skinning::SkinnedSceneDescription skins;
    skins.max_mesh_vertices = vertices;
    skins.max_instance_vertices = vertices;
    skins.max_pose_matrices = joints * 2U;
    skins.max_meshes = 1;
    skins.max_instances = 1;
    if (Status created = device_->skins.create(device, skins); !created) {
        return created;
    }
    skinning::SkinnedMeshDescription mesh;
    mesh.positions = character.positions.span();
    mesh.frames = character.frames.span();
    mesh.influences = character.influences.span();
    mesh.bone_count = joints;
    Expected<skinning::SkinnedMeshId, Error> added = device_->skins.add_mesh(mesh);
    if (!added.has_value()) {
        return make_unexpected(added.error());
    }
    Expected<skinning::SkinnedInstance, Error> instance = device_->skins.add_instance(*added);
    if (!instance.has_value()) {
        return make_unexpected(instance.error());
    }
    device_->character = *instance;
    device_->character_mesh.indices = device_->character_indices;
    device_->character_mesh.wide_indices = true;
    device_->character_mesh.index_count = static_cast<u32>(character.indices.size());
    device_->character_mesh.static_vertex_offset = static_cast<i32>(kCubeVertices);

    // THE SPATIAL INDEX: the tiles, and the character bounded by its bind pose and a metre and a
    // half every way, which holds every pose of the four clips — the death's 0.9 m fall included.
    if (Status sized = device_->instances.resize(kInstances); !sized) {
        return sized;
    }
    if (Status sized = device_->centres.resize(kInstances); !sized) {
        return sized;
    }
    for (u32 tile = 0; tile < kTiles; ++tile) {
        const auto x = static_cast<i32>(tile % (kGroundHalfTiles * 2)) - kGroundHalfTiles;
        const auto z = static_cast<i32>(tile / (kGroundHalfTiles * 2)) - kGroundHalfTiles;
        const Vec3 centre{static_cast<f32>(x) + 0.5F, -kTileThickness * 0.5F,
                          static_cast<f32>(z) + 0.5F};
        SpatialEntry entry;
        entry.bounds = Aabb::from_center_extents(centre, Vec3{0.5F, kTileThickness * 0.5F, 0.5F});
        entry.stable_id = 1000U + tile;
        entry.gpu_slot = tile;
        entry.radius = 0.71F;
        Expected<u32, Error> slot = device_->index.insert(entry);
        if (!slot.has_value()) {
            return make_unexpected(slot.error());
        }
        device_->centres[tile] = Vec4{centre.x, centre.y, centre.z, 1.0F};
        InstanceTransform& row = device_->instances[tile];
        row.row1[1] = kTileThickness;
    }
    Vec3 lowest = character.positions[0];
    Vec3 highest = character.positions[0];
    for (const Vec3& position : character.positions) {
        lowest = Vec3{math::min(lowest.x, position.x), math::min(lowest.y, position.y),
                      math::min(lowest.z, position.z)};
        highest = Vec3{math::max(highest.x, position.x), math::max(highest.y, position.y),
                       math::max(highest.z, position.z)};
    }
    SpatialEntry entry;
    entry.bounds =
        Aabb::from_min_max(lowest - Vec3{1.5F, 1.5F, 1.5F}, highest + Vec3{1.5F, 1.5F, 1.5F});
    entry.stable_id = 1;
    entry.gpu_slot = kCharacterSlot;
    entry.flags |= kSpatialSkinned;
    entry.radius = length(entry.bounds.max - entry.bounds.min) * 0.5F;
    Expected<u32, Error> slot = device_->index.insert(entry);
    if (!slot.has_value()) {
        return make_unexpected(slot.error());
    }
    device_->centres[kCharacterSlot] = Vec4{0.0F, 0.0F, 0.0F, 1.0F};
    return ok();
}

Status Stage::stage_character(const Character& character) noexcept {
    if (!available_) {
        return fail(ErrorCode::Unavailable, "no graphics device answered");
    }
    if (Status created = create_frame(); !created) {
        return created;
    }
    return create_scene(character);
}

Status Stage::shoot(animation::PoseWorld& poses, animation::PoseHandle handle, const Shot& shot,
                    u64 frame_index, const char* png_path, FrameReport& out) noexcept {
    if (!available_) {
        return fail(ErrorCode::Unavailable, "no graphics device answered");
    }
    rhi::Device& device = *device_->handle.value();
    Device& state = *device_;

    // THE POSE WORLD'S DIRTY RANGE, and nothing else. Read every frame, never cached:
    // `matrix_offset` moves on every publish, which is what the double buffering IS.
    if (Status uploaded =
            state.skins.upload_poses(poses.matrices(), poses.upload_offset(), poses.upload_size());
        !uploaded) {
        return uploaded;
    }
    poses.clear_upload_range();
    const u32 pose_offset = poses.matrix_offset(handle);
    if (Status posed = state.skins.set_pose(state.character, pose_offset); !posed) {
        return posed;
    }

    // THE CAMERA. The frame's rows are camera-RELATIVE, so every instance is placed about the eye
    // and the view the frame draws through is the rotation alone.
    state.eye = shot.eye;
    const Mat4 view = look_at(shot.eye, shot.target, Vec3{0.0F, 1.0F, 0.0F});
    const Mat4 relative_view =
        look_at(Vec3{0.0F, 0.0F, 0.0F}, shot.target - shot.eye, Vec3{0.0F, 1.0F, 0.0F});
    const Mat4 projection = perspective_reversed_z(
        shot.fov_y_radians, static_cast<f32>(width_) / static_cast<f32>(height_), 0.05F, 200.0F);
    for (u32 slot = 0; slot < kInstances; ++slot) {
        InstanceTransform& row = state.instances[slot];
        row.row0[3] = state.centres[slot].x - shot.eye.x;
        row.row1[3] = state.centres[slot].y - shot.eye.y;
        row.row2[3] = state.centres[slot].z - shot.eye.z;
    }
    state.shadow_centre = Vec3{shot.target.x, 0.0F, shot.target.z};

    Expected<u32, Error> began = device.begin_frame();
    if (!began.has_value()) {
        return make_unexpected(began.error());
    }
    const u32 ring = *began;

    state.graph.reset();
    // THE SKINNING PASS FIRST, so every frame stage that draws the character reads vertices the
    // graph has ordered after it.
    if (Status declared = state.skins.declare(state.graph, frame_index); !declared) {
        return declared;
    }

    assembly::AssemblyView frame_view;
    frame_view.fov_y_radians = shot.fov_y_radians;
    frame_view.view = view;
    frame_view.projection = projection;
    frame_view.cull.frustum = Frustum::from_view_projection(projection * view);
    frame_view.cull.camera_position = shot.eye;
    frame_view.cull.camera_forward = normalize(shot.target - shot.eye);
    frame_view.cull.fov_y_radians = shot.fov_y_radians;
    frame_view.lights = state.lights.span();
    frame_view.sun_direction = normalize(kSunTravel) * -1.0F;

    TextureRequest request;
    request.name = "animated character output";
    request.format = kOutputFormat;
    request.width = width_;
    request.height = height_;
    request.extra_usage = rhi::TextureUsage::TransferSource;
    frame_view.output = state.graph.import_texture(request, state.output, rhi::ImageUse::Undefined);
    request.name = "animated character shadow map";
    request.format = rhi::Format::R32Sfloat;
    request.width = kShadowExtent;
    request.height = kShadowExtent;
    request.extra_usage = rhi::TextureUsage{};
    frame_view.shadow_color =
        state.graph.import_texture(request, state.shadow_color, rhi::ImageUse::Undefined);
    request.name = "animated character shadow depth";
    request.format = rhi::Format::D32Sfloat;
    frame_view.shadow_depth =
        state.graph.import_texture(request, state.shadow_depth, rhi::ImageUse::Undefined);

    state.recorder.set_shadow_targets(frame_view.shadow_color, frame_view.shadow_depth,
                                      kShadowExtent);
    if (Status bound = state.recorder.bind(state.assembly); !bound) {
        return bound;
    }
    FrameSinks sinks = state.recorder.sinks();
    sinks.surfaces = &stage_surfaces;
    sinks.surfaces_user = &state;
    const Span<const ResourceId> skinned = state.skins.vertex_reads();
    for (const FramePassKind kind : {FramePassKind::DepthPrepass, FramePassKind::Shadow,
                                     FramePassKind::Opaque, FramePassKind::Transparent}) {
        sinks.passes[static_cast<usize>(kind)].vertex_reads = skinned;
    }
    state.opaque_reads[0] =
        FrameResourceRead{frame_view.shadow_color, rhi::Access::FragmentSampledRead};
    sinks.passes[static_cast<usize>(FramePassKind::Opaque)].reads =
        Span<const FrameResourceRead>(state.opaque_reads, 1);

    AssemblyReport report;
    if (Status assembled =
            state.assembly.assemble(state.index, frame_view, sinks, state.graph, report);
        !assembled) {
        return assembled;
    }

    GlobalsData globals;
    globals.exposure_stops = kExposureStops;
    FrameUpload upload =
        upload_for(state.assembly, report, projection * relative_view, relative_view,
                   state.instances.span(), globals, state.material_offsets);
    // THE DIRECTIONAL SHADOW, about the character.
    if (Status copied = state.frame_lights.resize(upload.lights.size()); !copied) {
        return copied;
    }
    u32 sun = static_cast<u32>(upload.lights.size());
    for (usize index = 0; index < upload.lights.size(); ++index) {
        state.frame_lights[index] = upload.lights[index];
        if (upload.lights[index].kind == kGpuLightDirectional) {
            sun = static_cast<u32>(index);
        }
    }
    upload.lights = state.frame_lights.span();
    const Mat4 to_clip = shadow_to_clip(state.shadow_centre) *
                         Mat4::from_translation(shot.eye);  // relative to the camera
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            upload.view.shadow_to_clip[(row * 4U) + column] = to_clip.at(row, column);
        }
    }
    upload.view.shadow_control[0] = kShadowSlot;
    upload.view.shadow_control[1] = sun;
    upload.view.shadow_control[2] = kShadowExtent;
    upload.view.shadow_control[3] = sun < upload.lights.size() ? 1U : 0U;
    const MaterialTextureSlot shadow_slot{kShadowSlot, state.shadow_view};
    if (Status set =
            state.bindings.set_material_textures(Span<const MaterialTextureSlot>(&shadow_slot, 1));
        !set) {
        return set;
    }
    if (Status uploaded = state.bindings.upload(ring, upload); !uploaded) {
        return uploaded;
    }

    Readback readback{state.assembly.resources().output, state.readback, width_, height_};
    BufferRequest capture;
    capture.name = "animated character capture";
    capture.size = u64{width_} * height_ * sizeof(u32);
    capture.extra_usage = rhi::BufferUsage::TransferDestination;
    const ResourceId destination = state.graph.import_buffer(capture, state.readback);
    state.graph.add_pass("animated character capture", rhi::QueueKind::Graphics)
        .read(readback.output, rhi::Access::TransferRead)
        .write(destination, rhi::Access::TransferWrite)
        .record(&record_readback, &readback);
    state.graph.add_pass("animated character host", rhi::QueueKind::Graphics)
        .read(destination, rhi::Access::HostRead)
        .side_effect();

    GraphExecutor executor(*allocator_, device);
    Status frame = state.assembly.execute(executor, state.graph, report);
    if (frame) {
        frame = device.wait_idle();
    }
    if (frame && png_path != nullptr) {
        frame = write_png(png_path);
    }
    executor.release();
    if (Status ended = device.end_frame(); !ended && frame) {
        frame = ended;
    }

    skinning::SkinnedOutput skinned_output;
    out.validation_errors = state.validation_errors;
    out.vertex_offset =
        state.skins.output(state.character, skinned_output) ? skinned_output.current_vertex : 0U;
    out.pose_offset = pose_offset;
    out.uploaded_matrices = state.skins.stats().uploaded_matrices;
    out.skinned_draws = state.recorder.report().skinned_draws;
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
        device_->skins.destroy();
        device_->bindings.shutdown();
        device_->pipelines.shutdown();
        if (!device_->shadow_view.is_null()) {
            device.destroy_texture_view(device_->shadow_view);
        }
        for (const rhi::TextureHandle texture :
             {device_->output, device_->shadow_color, device_->shadow_depth}) {
            if (!texture.is_null()) {
                device.destroy_texture(texture);
            }
        }
        for (const rhi::BufferHandle buffer :
             {device_->positions, device_->normals, device_->uvs, device_->cube_indices,
              device_->character_indices, device_->readback}) {
            if (!buffer.is_null()) {
                device.destroy_buffer(buffer);
            }
        }
    }
    // THE FRAME BEFORE THE DEVICE: the assembly, the recorder and the skinned scene release what
    // they still hold on the device in their destructors, so they go first.
    rhi::Device* handle = device_->handle.has_value() ? device_->handle.value() : nullptr;
    delete device_;
    device_ = nullptr;
    if (handle != nullptr) {
        rhi::destroy_device(*allocator_, handle);
    }
    available_ = false;
}

}  // namespace cy::sample::character
