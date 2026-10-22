// SPDX-License-Identifier: MIT
// map_render.cpp — the RTS session drawn on the RTS test map. See map_render.h.

#include "map_render.h"

#include <cy/animation/clip.h>
#include <cy/animation/skeleton.h>
#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/fog/fog_of_war.h>
#include <cy/rendering/assembly/frame_assembly.h>
#include <cy/rendering/fog_of_war/fog_of_war_pass.h>
#include <cy/rendering/fog_of_war/minimap.h>
#include <cy/rendering/gi_gpu/gpu_frame.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/lighting/soft_shadows.h>
#include <cy/rendering/material/standard.h>
#include <cy/rendering/occlusion/occlusion_pass.h>
#include <cy/rendering/pipeline/frame_recorder.h>
#include <cy/rendering/pipeline/material_textures.h>
#include <cy/rendering/skinning/frame_skinning.h>
#include <cy/rendering/skinning/skinned_scene.h>
#include <cy/servers/render/geometry/skin_dispatch.h>
#include <cy/servers/render/mesh.h>
#include <cy/servers/render/server.h>
#include <cy/ui/paint.h>
#include <cy/ui/render/ui_renderer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

#include "fog.h"
#include "golden.h"
#include "illumination.h"
#include "map_format.h"
#include "rts_host.h"

namespace sample::rts {
namespace {

namespace rendering = cy::rendering;
namespace pipeline = cy::rendering::pipeline;
namespace gi_gpu = cy::rendering::gi_gpu;
namespace fow = cy::rendering::fog_of_war;
namespace gfog = cy::gameplay::fog;
namespace rhi = cy::rhi;
using cy::Aabb;
using cy::f32;
using cy::f64;
using cy::i32;
using cy::Mat4;
using cy::Status;
using cy::u16;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;
using cy::Vec2;
using cy::Vec3;
using cy::Vec4;
using map::MapContent;
using map::MeshLibrary;
using map::MeshPart;

// --- The frame's constants
// ------------------------------------------------------------------------

/// The frame-texture slots the frame's own targets are bound at. The material textures take the
/// low slots the device's table hands out.
constexpr u32 kGiDiffuseSlot = 118;
constexpr u32 kGiSpecularSlot = 119;
constexpr u32 kOcclusionSlot = 120;
constexpr u32 kShadowSlot = 121;
constexpr u32 kShadowExtent = 2048;
constexpr f32 kNear = 0.25F;
constexpr f32 kFar = 400.0F;
/// The output the frame resolves into: what a swapchain image is, and what a PNG holds.
constexpr rhi::Format kOutputFormat = rhi::Format::Rgba8Unorm;
/// Exposure: the frame holds illuminance in physical units under a 22 000 lux sun.
constexpr f32 kExposureStops = -11.0F;
/// The stills, at the frames render.h writes its own.
constexpr u64 kStills[] = {150, 250, 350, 420};
constexpr u64 kWarmFrames = 10;
/// The game's step: what the fog fades and the company's clips advance on.
constexpr f32 kFrameSeconds = 1.0F / 60.0F;
/// Blue, whose fog the frame draws.
constexpr cy::gameplay::TeamId kBlue = 1;
/// Units the frame has room for: the game's workers, the company, the camp's raiders.
constexpr u32 kMaxWorkers = 8;
constexpr u32 kCompanySize = 16;
constexpr u32 kRaiders = 2;
constexpr u32 kFigures = kMaxWorkers + kCompanySize + kRaiders;
constexpr u32 kJoints = 8;
/// Where an unused figure or a hidden camp member waits: far under the map.
constexpr Vec3 kParked{16.0F, -200.0F, 16.0F};
/// The fog's grid over the whole map, a cell a metre; the level's own grid is the square at its
/// origin, and everything outside the level is ground blue has explored and does not see.
constexpr u32 kMapFogCells = 128;
/// Team colours, linear: neutral, blue, red, and the company's second group.
constexpr Vec3 kTeamColours[] = {Vec3{0.78F, 0.74F, 0.66F}, Vec3{0.10F, 0.24F, 0.70F},
                                 Vec3{0.70F, 0.10F, 0.07F}, Vec3{0.08F, 0.46F, 0.50F}};
constexpr u32 kTeams = 4;
/// The HUD's minimap fog: the atlas page game/Contract.swift names, its extent, and how often it is
/// composed again from blue's field (every sixth frame: a tenth of a second).
constexpr cy::u16 kMinimapFogPage = 2;
constexpr u32 kMinimapFogTexels = 64;
constexpr u64 kMinimapFogEvery = 6;
/// Tints a plant's variation byte is bucketed into, so a batch shares one.
constexpr u32 kTintBuckets = 3;
/// The square the foliage is batched in, metres.
constexpr f32 kBatchMetres = 32.0F;

[[nodiscard]] f64 since_ms(std::chrono::steady_clock::time_point start) noexcept {
    return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
        std::fprintf(stderr, "graphics validation error: %s\n", message != nullptr ? message : "");
    }
}

/// A rotation about +Y by `yaw` as map/map_meshes.cpp turns a point: +z goes to (sin, cos).
[[nodiscard]] Mat4 yaw_matrix(f32 yaw) noexcept {
    const f32 c = std::cos(yaw);
    const f32 s = std::sin(yaw);
    return Mat4::from_columns(Vec4{c, 0.0F, -s, 0.0F}, Vec4{0.0F, 1.0F, 0.0F, 0.0F},
                              Vec4{s, 0.0F, c, 0.0F}, Vec4{0.0F, 0.0F, 0.0F, 1.0F});
}

/// Tilts about the x and z axes: how a plant leans with the ground it stands on.
[[nodiscard]] Mat4 tilt_matrix(f32 about_x, f32 about_z) noexcept {
    const f32 cx = std::cos(about_x);
    const f32 sx = std::sin(about_x);
    const f32 cz = std::cos(about_z);
    const f32 sz = std::sin(about_z);
    const Mat4 x = Mat4::from_columns(Vec4{1.0F, 0.0F, 0.0F, 0.0F}, Vec4{0.0F, cx, sx, 0.0F},
                                      Vec4{0.0F, -sx, cx, 0.0F}, Vec4{0.0F, 0.0F, 0.0F, 1.0F});
    const Mat4 z = Mat4::from_columns(Vec4{cz, sz, 0.0F, 0.0F}, Vec4{-sz, cz, 0.0F, 0.0F},
                                      Vec4{0.0F, 0.0F, 1.0F, 0.0F}, Vec4{0.0F, 0.0F, 0.0F, 1.0F});
    return x * z;
}

[[nodiscard]] Mat4 placement_matrix(Vec3 position, f32 yaw, Vec3 scale) noexcept {
    return Mat4::from_translation(position) * yaw_matrix(yaw) * Mat4::from_scale(scale);
}

[[nodiscard]] Vec3 apply_point(const Mat4& m, Vec3 p) noexcept {
    const Vec4 r = m * Vec4{p.x, p.y, p.z, 1.0F};
    return Vec3{r.x, r.y, r.z};
}

[[nodiscard]] Vec3 apply_direction(const Mat4& m, Vec3 d) noexcept {
    const Vec4 r = m * Vec4{d.x, d.y, d.z, 0.0F};
    return cy::normalize(Vec3{r.x, r.y, r.z});
}

[[nodiscard]] Aabb transform_box(const Mat4& m, Vec3 low, Vec3 high) noexcept {
    Aabb out = Aabb::empty();
    for (u32 corner = 0; corner < 8U; ++corner) {
        const Vec3 p{(corner & 1U) != 0U ? high.x : low.x, (corner & 2U) != 0U ? high.y : low.y,
                     (corner & 4U) != 0U ? high.z : low.z};
        const Vec3 w = apply_point(m, p);
        out = Aabb::from_min_max(cy::cwise_min(out.min, w), cy::cwise_max(out.max, w));
    }
    return out;
}

/// Any unit vector perpendicular to `normal`: the frame encodes a tangent beside every normal,
/// and these materials sample no normal map.
[[nodiscard]] Vec3 any_tangent(Vec3 normal) noexcept {
    const Vec3 axis = std::fabs(normal.y) < 0.9F ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{1.0F, 0.0F, 0.0F};
    return cy::normalize(cy::cross(axis, normal));
}

// --- The geometry
// ---------------------------------------------------------------------------------

/// One drawable mesh in the frame's streams: where its vertices and its parts' indices are.
struct DrawMesh {
    i32 vertex_offset = 0;
    std::vector<MeshPart> parts;  ///< first_index absolute in the frame's index buffer
    Vec3 low{0.0F, 0.0F, 0.0F};
    Vec3 high{0.0F, 0.0F, 0.0F};
};

/// The frame's three streams and its indices, built on the host and uploaded once.
struct Streams {
    std::vector<Vec3> positions;
    std::vector<u16> normals;
    std::vector<Vec2> uvs;
    std::vector<u32> indices;

    void vertex(Vec3 position, Vec3 normal, Vec2 uv) {
        positions.push_back(position);
        u16 packed[4];
        pipeline::pack_normal_stream(normal, any_tangent(normal), packed);
        normals.insert(normals.end(), packed, packed + 4);
        uvs.push_back(uv);
    }

    /// Every mesh of `library`, each a `DrawMesh` in `out`, in the library's order.
    void add_library(const MeshLibrary& library, std::vector<DrawMesh>& out) {
        const auto vertex_base = static_cast<i32>(positions.size());
        const auto index_base = static_cast<u32>(indices.size());
        for (usize index = 0; index < library.positions.size(); ++index) {
            vertex(library.positions[index], library.normals[index], library.uvs[index]);
        }
        indices.insert(indices.end(), library.indices.begin(), library.indices.end());
        for (const map::MeshRange& range : library.meshes) {
            DrawMesh mesh;
            mesh.vertex_offset = vertex_base + static_cast<i32>(range.first_vertex);
            for (u32 part = range.first_part; part < range.first_part + range.part_count; ++part) {
                MeshPart placed = library.parts[part];
                placed.first_index += index_base;
                mesh.parts.push_back(placed);
            }
            mesh.low = range.min;
            mesh.high = range.max;
            out.push_back(std::move(mesh));
        }
    }
};

/// The foliage, merged: every plant of a batch square, a species and a tint, transformed into one
/// mesh about the square's centre. One draw a material of it rather than one a plant.
struct FoliageBatch {
    Vec3 origin;
    u32 species = 0;
    u32 bucket = 0;
    DrawMesh mesh;
};

void build_foliage(const MapContent& content, Streams& streams, std::vector<FoliageBatch>& out) {
    const map::FoliageData& foliage = content.foliage;
    const MeshLibrary& library = content.props;
    const f32 size = content.layout.size;
    const auto squares = static_cast<u32>(std::ceil(size / kBatchMetres));
    for (u32 square = 0; square < squares * squares; ++square) {
        const f32 min_x =
            content.layout.origin_x + (static_cast<f32>(square % squares) * kBatchMetres);
        const f32 min_z =
            content.layout.origin_z + (static_cast<f32>(square / squares) * kBatchMetres);
        for (u32 species = 0; species < foliage.species.size(); ++species) {
            const map::MeshRange& range = library.meshes[foliage.species[species].mesh];
            for (u32 bucket = 0; bucket < kTintBuckets; ++bucket) {
                FoliageBatch batch;
                batch.origin =
                    Vec3{min_x + (kBatchMetres * 0.5F), 0.0F, min_z + (kBatchMetres * 0.5F)};
                batch.species = species;
                batch.bucket = bucket;
                batch.mesh.vertex_offset = static_cast<i32>(streams.positions.size());
                batch.mesh.low = Vec3{1e9F, 1e9F, 1e9F};
                batch.mesh.high = Vec3{-1e9F, -1e9F, -1e9F};
                // One run of indices a part (material), every plant's triangles appended to it.
                std::vector<std::vector<u32>> part_indices(range.part_count);
                u32 local = 0;
                for (const map::FoliagePlant& plant : foliage.plants) {
                    if (plant.species != species ||
                        (static_cast<u32>(plant.variation) % kTintBuckets) != bucket ||
                        plant.position.x < min_x || plant.position.x >= min_x + kBatchMetres ||
                        plant.position.z < min_z || plant.position.z >= min_z + kBatchMetres) {
                        continue;
                    }
                    const Vec3 at{plant.position.x - batch.origin.x, plant.position.y,
                                  plant.position.z - batch.origin.z};
                    const Mat4 place =
                        Mat4::from_translation(at) * tilt_matrix(plant.tilt_x, plant.tilt_z) *
                        yaw_matrix(plant.yaw) *
                        Mat4::from_scale(Vec3{plant.scale, plant.scale, plant.scale});
                    for (u32 vertex = 0; vertex < range.vertex_count; ++vertex) {
                        const u32 from = range.first_vertex + vertex;
                        const Vec3 p = apply_point(place, library.positions[from]);
                        streams.vertex(p, apply_direction(place, library.normals[from]),
                                       library.uvs[from]);
                        batch.mesh.low = cy::cwise_min(batch.mesh.low, p);
                        batch.mesh.high = cy::cwise_max(batch.mesh.high, p);
                    }
                    for (u32 part = 0; part < range.part_count; ++part) {
                        const MeshPart& source = library.parts[range.first_part + part];
                        for (u32 index = 0; index < source.index_count; ++index) {
                            part_indices[part].push_back(
                                local + library.indices[source.first_index + index]);
                        }
                    }
                    local += range.vertex_count;
                }
                if (local == 0) {
                    streams.positions.resize(static_cast<usize>(batch.mesh.vertex_offset));
                    streams.normals.resize(static_cast<usize>(batch.mesh.vertex_offset) * 4U);
                    streams.uvs.resize(static_cast<usize>(batch.mesh.vertex_offset));
                    continue;
                }
                for (u32 part = 0; part < range.part_count; ++part) {
                    MeshPart placed;
                    placed.first_index = static_cast<u32>(streams.indices.size());
                    placed.index_count = static_cast<u32>(part_indices[part].size());
                    placed.material = library.parts[range.first_part + part].material;
                    streams.indices.insert(streams.indices.end(), part_indices[part].begin(),
                                           part_indices[part].end());
                    batch.mesh.parts.push_back(placed);
                }
                out.push_back(std::move(batch));
            }
        }
    }
}

// --- The figures' poses
// -----------------------------------------------------------------------------

/// The worker rig's clips sampled here, for the figures the game does not animate: the company,
/// whose units walk while the session moves them, and the camp's raiders, who stand.
class FigurePoser {
public:
    explicit FigurePoser(cy::Allocator& allocator) noexcept : allocator_(&allocator) {}

    [[nodiscard]] Status bind(const cy::animation::AnimationRig& rig) noexcept {
        rig_ = &rig;
        const cy::Span<const cy::animation::Clip* const> clips = rig.clips();
        for (const cy::animation::Clip* clip : clips) {
            if (clip == nullptr) {
                continue;
            }
            if (clip->name() == cy::Name::intern("worker_idle")) {
                idle_ = clip;
            } else if (clip->name() == cy::Name::intern("worker_walk")) {
                walk_ = clip;
            }
        }
        if (idle_ == nullptr || walk_ == nullptr) {
            return cy::fail(cy::ErrorCode::NotFound, "the worker rig has no idle or walk clip");
        }
        const u16 joints = rig.skeleton().joint_count();
        local_.assign(joints, cy::Transform{});
        model_.assign(joints, cy::Transform{});
        return cy::ok();
    }

    /// The skinning matrices of `walking` or idle at `time`, into `out` (`kJoints` of them).
    void pose(bool walking, f32 time, Mat4* out) noexcept {
        const cy::animation::Skeleton& skeleton = rig_->skeleton();
        const cy::animation::JointMask& mask = skeleton.retained(0);
        skeleton.reference_pose(cy::Span<cy::Transform>(local_.data(), local_.size()));
        cy::animation::ClipCursor cursor(*allocator_);
        cy::animation::SampleStats stats;
        const cy::animation::Clip* clip = walking ? walk_ : idle_;
        (void)clip->sample(time, mask, cursor,
                           cy::Span<cy::Transform>(local_.data(), local_.size()), stats);
        skeleton.to_model(cy::Span<const cy::Transform>(local_.data(), local_.size()), mask,
                          cy::Span<cy::Transform>(model_.data(), model_.size()));
        skeleton.to_skinning(cy::Span<const cy::Transform>(model_.data(), model_.size()), mask,
                             cy::Span<Mat4>(out, kJoints));
    }

private:
    cy::Allocator* allocator_;
    const cy::animation::AnimationRig* rig_ = nullptr;
    const cy::animation::Clip* idle_ = nullptr;
    const cy::animation::Clip* walk_ = nullptr;
    std::vector<cy::Transform> local_;
    std::vector<cy::Transform> model_;
};

/// A figure's heading, eased toward where it walks.
struct Heading {
    Vec3 last = kParked;
    f32 yaw = 0.0F;
    bool placed = false;

    /// Follow `at`; `wanted` overrides the walked direction when the source states one.
    void follow(Vec3 at, const f32* wanted) noexcept {
        f32 target = yaw;
        if (wanted != nullptr) {
            target = *wanted;
        } else if (placed) {
            const f32 dx = at.x - last.x;
            const f32 dz = at.z - last.z;
            if ((dx * dx) + (dz * dz) > 1e-6F) {
                target = std::atan2(dx, dz);
            }
        }
        f32 delta = target - yaw;
        while (delta > std::numbers::pi_v<f32>) {
            delta -= 2.0F * std::numbers::pi_v<f32>;
        }
        while (delta < -std::numbers::pi_v<f32>) {
            delta += 2.0F * std::numbers::pi_v<f32>;
        }
        yaw = placed ? yaw + (delta * 0.2F) : target;
        last = at;
        placed = true;
    }
};

}  // namespace

// --- The state
// ------------------------------------------------------------------------------------

struct MapRenderer::State {
    explicit State(cy::Allocator& memory) noexcept
        : allocator(&memory),
          assembly(memory),
          index(memory),
          graph(memory),
          program(memory),
          server(cy::system_allocator(cy::MemoryDomain::Gpu)),
          skins(memory),
          poser(memory),
          ui(memory),
          ui_buffer(memory),
          lights(memory) {}

    /// One placed instance: a mesh, its world placement, its tint and how it draws.
    struct Instance {
        u32 mesh = 0;
        Mat4 world = Mat4::identity();
        Vec3 tint{1.0F, 1.0F, 1.0F};
        u32 team = 0;
        u32 index_slot = 0;
        /// A figure's skinned instance; invalid for a rigid mesh.
        rendering::skinning::SkinnedInstance skinned;
        std::vector<rendering::DrawSurface> surfaces;
    };

    cy::Allocator* allocator;
    MapRenderOptions options;
    rhi::Device* device = nullptr;
    rhi::BackendSelection selection{};
    u32 validation_errors = 0;
    RtsHost* host = nullptr;

    rendering::assembly::FrameAssembly assembly;
    rendering::SpatialIndex index;
    rendering::RenderGraph graph;
    rendering::MaterialProgram program;
    pipeline::FramePipelines pipelines;
    pipeline::FrameBindings bindings;
    pipeline::FrameRecorder recorder;
    cy::render::RenderServer server;
    pipeline::MaterialTextureTable textures;
    rendering::skinning::SkinnedScene skins;
    FigurePoser poser;
    rendering::occlusion::AmbientOcclusionPass occlusion;
    gi_gpu::GpuGiFrame gi;
    fow::FogOfWarPass fog;
    std::unique_ptr<fow::VisibilityField> field;
    /// Blue's field over the level alone, faded as the frame's is: what the minimap's fog shows.
    std::unique_ptr<fow::VisibilityField> level_field;
    std::vector<u32> minimap_fog;
    std::vector<u8> level_states;
    std::vector<u8> map_states;
    cy::ui::render::UiRenderer ui;
    cy::ui::PrimitiveBuffer ui_buffer;
    u32 ui_revision = 0xFFFF'FFFFU;

    // The geometry.
    std::vector<DrawMesh> meshes;
    std::vector<Instance> instances;
    std::vector<pipeline::InstanceTransform> rows;
    rhi::BufferHandle positions;
    rhi::BufferHandle normals;
    rhi::BufferHandle uvs;
    rhi::BufferHandle indices;
    rhi::BufferHandle readback;
    rhi::TextureHandle output;
    rhi::TextureHandle shadow_color;
    rhi::TextureHandle shadow_depth;
    rhi::TextureViewHandle shadow_view;

    // The materials: the map's, then the team colour's variants.
    u32 material_offsets[4] = {};
    u32 material_texture_offsets[4] = {pipeline::kNoMaterialTexture, pipeline::kNoMaterialTexture,
                                       pipeline::kNoMaterialTexture, pipeline::kNoMaterialTexture};
    std::vector<u32> material_slots;
    std::vector<bool> material_transparent;
    u32 team_material = map::kNoMaterial;
    u32 team_slots[kTeams] = {};
    std::vector<pipeline::MaterialTextureSlot> resident;
    cy::Array<cy::render::LightDescription> lights;

    // The figures: workers, the company, the raiders, in that order; then the watchtower and the
    // crate, rigid.
    u32 first_figure = 0;
    u32 watchtower = 0;
    u32 crate = 0;
    u32 worker_mesh_index_base = 0;
    i32 worker_uv_base = 0;
    std::vector<map::MeshPart> worker_parts;
    Heading headings[kFigures];
    std::vector<Mat4> poses;
    f32 clock = 0.0F;

    // The view.
    Mat4 view = Mat4::identity();
    Mat4 relative_view = Mat4::identity();
    Mat4 projection = Mat4::identity();
    Vec3 eye{0.0F, 0.0F, 0.0F};
    Vec3 forward{0.0F, 0.0F, -1.0F};
    Vec3 focus{16.0F, 0.0F, 16.0F};
    f32 fov = 0.9F;
    Mat4 shadow_to_clip = Mat4::identity();
    f32 shadow_radius = 40.0F;
    rendering::FrameResourceRead opaque_reads[1];
    rendering::ResourceId shadow_resource = rendering::kInvalidResource;
    f32 map_origin_x = 0.0F;
    f32 map_origin_z = 0.0F;
    f32 company_x = 0.0F;
    f32 company_z = 0.0F;
    Vec3 sun_travel{-0.28F, -0.82F, -0.50F};
    f32 sun_lux = 22000.0F;
    const map::TerrainData* terrain = nullptr;
    map::TerrainData terrain_copy;

    // What the report says.
    MapRenderReport report;
    f64 last_record_ms = 0.0;
    f64 last_wait_ms = 0.0;
    std::vector<u32> pixels;
    u64 frame = 0;

    // --- Building ------------------------------------------------------------------------------

    [[nodiscard]] Status create_device() noexcept {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_sample_rts-api map";
        description.enable_validation = options.validation;
        cy::Allocator& gpu = cy::system_allocator(cy::MemoryDomain::Gpu);
        cy::Expected<rhi::Device*, cy::Error> made =
            rhi::create_device(gpu, "vulkan", description, selection);
        if (!made || made.value()->capabilities().backend() != rhi::BackendKind::Vulkan) {
            if (made) {
                rhi::destroy_device(gpu, made.value());
            }
            return cy::fail(cy::ErrorCode::Unavailable,
                            "no requested graphics device on this machine");
        }
        device = made.value();
        device->set_validation_callback(&count_validation, &validation_errors);
        return cy::ok();
    }

    [[nodiscard]] Status create_frame(u32 material_count) noexcept {
        rendering::assembly::AssemblyDescription description;
        description.width = options.width;
        description.height = options.height;
        description.near_plane = kNear;
        description.far_plane = kFar;
        description.clusters = rendering::ClusterGridConfig{32, 18, 32};
        description.material_capacity = material_count;
        description.max_draws = 8192;
        description.max_instances = 1024;
        description.gpu_culling = false;
        description.post.temporal_antialiasing = true;
        description.post.ambient_occlusion = options.ambient_occlusion;
        description.global_illumination = options.gi;
        description.fog_of_war = options.fog;
        if (Status made = assembly.initialize(description); !made) {
            return made;
        }
        if (Status attached = assembly.attach_device(*device); !attached) {
            return attached;
        }
        pipeline::PipelineSetup setup;
        setup.color_format = description.color_format;
        setup.depth_format = description.depth_format;
        setup.output_format = kOutputFormat;
        setup.prepass_normal = true;
        setup.prepass_velocity = true;
        setup.skinned = true;
        if (Status made = pipelines.initialize(*device, setup); !made) {
            return made;
        }
        cy::Expected<rendering::ClusterGrid, cy::Error> grid =
            rendering::make_cluster_grid(description.clusters, options.width, options.height,
                                         description.near_plane, description.far_plane);
        if (!grid) {
            return cy::make_unexpected(grid.error());
        }
        const pipeline::BindingCapacity capacity = pipeline::BindingCapacity::for_grid(
            *grid, description.max_draws, description.max_instances, description.material_capacity,
            16);
        if (Status made = bindings.initialize(*device, pipelines, capacity); !made) {
            return made;
        }
        return recorder.initialize(pipelines, bindings);
    }

    [[nodiscard]] Status upload_buffer(const char* name, const void* data, u64 bytes,
                                       rhi::BufferUsage usage, rhi::BufferHandle& out) noexcept {
        rhi::BufferDescription description;
        description.name = name;
        description.size = std::max<u64>(bytes, 16U);
        description.usage = usage;
        description.memory = rhi::MemoryUse::Upload;
        cy::Expected<rhi::BufferHandle, cy::Error> made = device->create_buffer(description);
        if (!made) {
            return cy::make_unexpected(made.error());
        }
        out = *made;
        void* mapped = device->buffer_mapped_pointer(out);
        if (mapped == nullptr) {
            return cy::fail(cy::ErrorCode::Internal, "a map buffer is not mapped");
        }
        std::memcpy(mapped, data, bytes);
        return cy::ok();
    }

    /// The textures: the baked albedo and the content's, resident in the device's table.
    [[nodiscard]] Status create_textures(const MapContent& content,
                                         std::vector<u32>& slot_of_texture) noexcept {
        cy::render::RenderServerConfig config;
        config.debug_primitive_capacity = 16;
        config.debug_label_capacity = 4;
        if (Status configured = server.configure(config); !configured) {
            return configured;
        }
        if (Status initialized = server.initialize(); !initialized) {
            return initialized;
        }
        rhi::SamplerDescription sampler;
        sampler.name = "map material sampler";
        sampler.max_anisotropy = 8.0F;
        if (Status made = textures.initialize(*device, *allocator, sampler); !made) {
            return made;
        }
        std::vector<const map::TextureData*> sources;
        for (const map::TextureData& texture : content.textures) {
            sources.push_back(&texture);
        }
        sources.push_back(&content.albedo);
        std::vector<pipeline::TextureUpload> uploads;
        std::vector<cy::render::TextureHandle> handles;
        for (const map::TextureData* texture : sources) {
            cy::render::TextureRecord record;
            record.name = cy::Name::intern("map texture");
            record.format = texture->srgb ? cy::render::TextureFormat::Rgba8Srgb
                                          : cy::render::TextureFormat::Rgba8Unorm;
            record.usage_class = cy::render::TextureUsageClass::Color;
            record.width = texture->width;
            record.height = texture->height;
            record.mip_levels = static_cast<u8>(texture->mips);
            cy::Expected<cy::render::TextureHandle, cy::Error> handle =
                server.create_texture(record);
            if (!handle) {
                return cy::make_unexpected(handle.error());
            }
            handles.push_back(*handle);
            uploads.push_back(pipeline::TextureUpload{
                *handle, cy::Span<const u8>(texture->pixels.data(), texture->pixels.size())});
        }
        if (Status uploaded = textures.upload(
                server, cy::Span<const pipeline::TextureUpload>(uploads.data(), uploads.size()));
            !uploaded) {
            return uploaded;
        }
        for (const cy::render::TextureHandle handle : handles) {
            slot_of_texture.push_back(textures.slot_of(handle));
        }
        resident.resize(textures.resident());
        (void)textures.slots(
            cy::Span<pipeline::MaterialTextureSlot>(resident.data(), resident.size()));
        return cy::ok();
    }

    [[nodiscard]] Status set_material(u32 slot, Vec3 colour, f32 roughness, f32 metallic,
                                      Vec3 emission, u32 texture) noexcept {
        const rendering::StandardParameters ids;
        rendering::MaterialTable& table = assembly.materials();
        if (Status defaults = rendering::apply_standard_defaults(program, table, slot, ids);
            !defaults) {
            return defaults;
        }
        if (Status set = table.set_color(program, slot, ids.base_color_factor,
                                         Vec4{colour.x, colour.y, colour.z, 1.0F});
            !set) {
            return set;
        }
        if (Status set = table.set_float(program, slot, ids.roughness_factor, roughness); !set) {
            return set;
        }
        if (Status set = table.set_float(program, slot, ids.metallic_factor, metallic); !set) {
            return set;
        }
        if (Status set = table.set_color(program, slot, ids.emission_color,
                                         Vec4{emission.x, emission.y, emission.z, 1.0F});
            !set) {
            return set;
        }
        return table.set_texture(program, slot, ids.base_color_texture, texture);
    }

    [[nodiscard]] Status create_materials(const MapContent& content,
                                          const std::vector<u32>& slot_of_texture) noexcept {
        if (Status described = rendering::describe_standard_material(
                program, cy::Name::intern("standard"), cy::render::ShadingModel::Lit,
                cy::render::BlendMode::Opaque);
            !described) {
            return described;
        }
        const rendering::StandardParameters ids;
        const rendering::ParameterId wanted[4] = {ids.base_color_factor, ids.roughness_factor,
                                                  ids.metallic_factor, ids.emission_color};
        for (u32 word = 0; word < 4U; ++word) {
            const rendering::MaterialParameter* parameter = program.find(wanted[word]);
            if (parameter == nullptr) {
                return cy::fail(cy::ErrorCode::NotFound,
                                "the standard material lacks a parameter the frame reads");
            }
            material_offsets[word] = parameter->offset / 4U;
        }
        const rendering::MaterialParameter* texture = program.find(ids.base_color_texture);
        if (texture == nullptr) {
            return cy::fail(cy::ErrorCode::NotFound, "the standard material has no texture slot");
        }
        material_texture_offsets[0] = texture->offset / 4U;

        rendering::MaterialTable& table = assembly.materials();
        const map::MapLayout& layout = content.layout;
        const auto texture_slot = [&](u32 which) -> u32 {
            if (which == map::kNoTexture) {
                return pipeline::kNoMaterialTexture;
            }
            if (which == map::kAlbedoTexture) {
                return slot_of_texture.back();
            }
            return which < slot_of_texture.size() - 1U ? slot_of_texture[which]
                                                       : pipeline::kNoMaterialTexture;
        };
        for (const map::MapMaterial& material : layout.materials) {
            cy::Expected<u32, cy::Error> slot = table.allocate();
            if (!slot) {
                return cy::make_unexpected(slot.error());
            }
            material_slots.push_back(*slot);
            material_transparent.push_back(material.blend == map::Blend::Transparent);
            if (Status set =
                    set_material(*slot, material.base_colour, material.roughness, material.metallic,
                                 material.emission, texture_slot(material.texture));
                !set) {
                return set;
            }
        }
        team_material = layout.material("team");
        for (u32 team = 0; team < kTeams; ++team) {
            cy::Expected<u32, cy::Error> slot = table.allocate();
            if (!slot) {
                return cy::make_unexpected(slot.error());
            }
            team_slots[team] = *slot;
            const map::MapMaterial* base =
                team_material == map::kNoMaterial ? nullptr : &layout.materials[team_material];
            if (Status set = set_material(*slot, kTeamColours[team],
                                          base == nullptr ? 0.8F : base->roughness, 0.0F,
                                          Vec3{0.0F, 0.0F, 0.0F}, pipeline::kNoMaterialTexture);
                !set) {
                return set;
            }
        }
        return cy::ok();
    }

    /// The instance's surfaces: one a part, its material the map's or its team's colour.
    void fill_surfaces(Instance& instance, const std::vector<map::MeshPart>& parts) {
        instance.surfaces.clear();
        for (usize part = 0; part < parts.size(); ++part) {
            rendering::DrawSurface surface;
            const u32 material = parts[part].material;
            surface.material =
                material == team_material ? team_slots[instance.team] : material_slots[material];
            surface.mesh = instance.mesh;
            surface.surface = static_cast<u32>(part);
            surface.blend = material_transparent[material] ? cy::render::BlendMode::Translucent
                                                           : cy::render::BlendMode::Opaque;
            instance.surfaces.push_back(surface);
        }
    }

    [[nodiscard]] Status add_instance(u32 mesh, const Mat4& world, Vec3 tint, u32 team) noexcept {
        Instance instance;
        instance.mesh = mesh;
        instance.world = world;
        instance.tint = tint;
        instance.team = team;
        fill_surfaces(instance, meshes[mesh].parts);
        rendering::SpatialEntry entry;
        entry.bounds = transform_box(world, meshes[mesh].low, meshes[mesh].high);
        entry.stable_id = 5000U + instances.size();
        entry.gpu_slot = static_cast<u32>(instances.size());
        entry.radius = cy::length(entry.bounds.max - entry.bounds.min) * 0.5F;
        cy::Expected<u32, cy::Error> slot = index.insert(entry);
        if (!slot) {
            return cy::make_unexpected(slot.error());
        }
        instance.index_slot = *slot;
        instances.push_back(std::move(instance));
        return cy::ok();
    }

    [[nodiscard]] Status create_geometry(const MapContent& content) noexcept {
        Streams streams;
        // The ground, then the kit: mesh 0 is the terrain and the kit follows in its own order.
        streams.add_library(content.ground, meshes);
        const auto kit = static_cast<u32>(meshes.size());
        streams.add_library(content.props, meshes);
        std::vector<FoliageBatch> batches;
        build_foliage(content, streams, batches);

        // THE WORKER: its UVs in the frame's stream — the skinning pass leaves them alone — and
        // its indices in the frame's index buffer; its positions and frames are the skinning
        // pass's own.
        const map::SkinnedMesh& worker = content.worker;
        worker_uv_base = static_cast<i32>(streams.positions.size());
        for (usize vertex = 0; vertex < worker.positions.size(); ++vertex) {
            streams.vertex(worker.positions[vertex], worker.normals[vertex], worker.uvs[vertex]);
        }
        worker_mesh_index_base = static_cast<u32>(streams.indices.size());
        streams.indices.insert(streams.indices.end(), worker.indices.begin(), worker.indices.end());
        for (const map::MeshPart& part : worker.parts) {
            map::MeshPart placed = part;
            placed.first_index += worker_mesh_index_base;
            worker_parts.push_back(placed);
        }

        if (Status made = upload_buffer("map positions", streams.positions.data(),
                                        streams.positions.size() * sizeof(Vec3),
                                        rhi::BufferUsage::Vertex, positions);
            !made) {
            return made;
        }
        if (Status made = upload_buffer("map normals", streams.normals.data(),
                                        streams.normals.size() * sizeof(u16),
                                        rhi::BufferUsage::Vertex, normals);
            !made) {
            return made;
        }
        if (Status made =
                upload_buffer("map uvs", streams.uvs.data(), streams.uvs.size() * sizeof(Vec2),
                              rhi::BufferUsage::Vertex, uvs);
            !made) {
            return made;
        }
        if (Status made = upload_buffer("map indices", streams.indices.data(),
                                        streams.indices.size() * sizeof(u32),
                                        rhi::BufferUsage::Index, indices);
            !made) {
            return made;
        }
        report.triangles = static_cast<u32>(streams.indices.size() / 3U);
        report.plants = static_cast<u32>(content.foliage.plants.size());

        // THE SKINNED WORKER, once, and a figure of it for every unit the frame has room for.
        rendering::skinning::SkinnedSceneDescription description;
        description.max_mesh_vertices = static_cast<u32>(worker.positions.size());
        description.max_instance_vertices = static_cast<u32>(worker.positions.size()) * kFigures;
        description.max_pose_matrices = kJoints * kFigures * 2U;
        description.max_meshes = 1;
        description.max_instances = kFigures;
        if (Status made = skins.create(*device, description); !made) {
            return made;
        }
        std::vector<cy::render::PackedNormalTangent> frames;
        std::vector<cy::render::geometry::GpuSkinInfluence> influences;
        for (usize vertex = 0; vertex < worker.positions.size(); ++vertex) {
            frames.push_back(cy::render::pack_normal_tangent(
                worker.normals[vertex], any_tangent(worker.normals[vertex]), 1.0F));
            const u8 bones[4] = {worker.joints[vertex], 0, 0, 0};
            const u8 weights[4] = {255, 0, 0, 0};
            influences.push_back(cy::render::geometry::skin_influence(bones, weights));
        }
        rendering::skinning::SkinnedMeshDescription mesh;
        mesh.positions = cy::Span<const Vec3>(worker.positions.data(), worker.positions.size());
        mesh.frames = cy::Span<const cy::render::PackedNormalTangent>(frames.data(), frames.size());
        mesh.influences = cy::Span<const cy::render::geometry::GpuSkinInfluence>(influences.data(),
                                                                                 influences.size());
        mesh.bone_count = worker.joint_count;
        cy::Expected<rendering::skinning::SkinnedMeshId, cy::Error> added = skins.add_mesh(mesh);
        if (!added) {
            return cy::make_unexpected(added.error());
        }

        // THE INSTANCES: the terrain, the placements, the foliage, the figures, the camp's tower
        // and the crate.
        if (Status placed = add_instance(0, Mat4::identity(), Vec3{1.0F, 1.0F, 1.0F}, 0); !placed) {
            return placed;
        }
        for (const map::Placement& placement : content.layout.placements) {
            const Mat4 world = placement_matrix(placement.position, placement.yaw, placement.scale);
            if (Status placed = add_instance(kit + placement.mesh, world, placement.tint,
                                             std::min<u32>(placement.team, kTeams - 1U));
                !placed) {
                return placed;
            }
        }
        for (FoliageBatch& batch : batches) {
            const map::FoliageSpecies& species = content.foliage.species[batch.species];
            const f32 t = (static_cast<f32>(batch.bucket) + 0.5F) / static_cast<f32>(kTintBuckets);
            const Vec3 tint = species.tint_a + ((species.tint_b - species.tint_a) * t);
            meshes.push_back(std::move(batch.mesh));
            if (Status placed = add_instance(static_cast<u32>(meshes.size() - 1U),
                                             Mat4::from_translation(batch.origin), tint, 0);
                !placed) {
                return placed;
            }
        }
        // A figure's mesh: the worker's parts, which the geometry lookup draws from the skinning
        // pass's output, and a generous box — a posed figure stays inside two metres.
        DrawMesh figure;
        figure.parts = worker_parts;
        figure.low = Vec3{-0.8F, 0.0F, -0.8F};
        figure.high = Vec3{0.8F, 2.2F, 0.8F};
        meshes.push_back(figure);
        const auto figure_mesh = static_cast<u32>(meshes.size() - 1U);
        first_figure = static_cast<u32>(instances.size());
        for (u32 which = 0; which < kFigures; ++which) {
            u32 team = 1;
            if (which >= kMaxWorkers + kCompanySize) {
                team = 2;
            } else if (which >= kMaxWorkers + (kCompanySize / 2U)) {
                team = 3;
            }
            if (Status placed = add_instance(figure_mesh, Mat4::from_translation(kParked),
                                             Vec3{1.0F, 1.0F, 1.0F}, team);
                !placed) {
                return placed;
            }
            cy::Expected<rendering::skinning::SkinnedInstance, cy::Error> skinned =
                skins.add_instance(*added);
            if (!skinned) {
                return cy::make_unexpected(skinned.error());
            }
            instances.back().skinned = *skinned;
        }
        const u32 tower_mesh = kit + content.props.find("watchtower");
        const u32 crate_mesh = kit + content.props.find("crate");
        if (tower_mesh < kit || crate_mesh < kit) {
            return cy::fail(cy::ErrorCode::NotFound, "the map's kit has no watchtower or crate");
        }
        watchtower = static_cast<u32>(instances.size());
        if (Status placed = add_instance(tower_mesh, Mat4::from_translation(kParked),
                                         Vec3{1.0F, 1.0F, 1.0F}, 2);
            !placed) {
            return placed;
        }
        crate = static_cast<u32>(instances.size());
        if (Status placed = add_instance(crate_mesh, Mat4::from_translation(kParked),
                                         Vec3{1.0F, 1.0F, 1.0F}, 0);
            !placed) {
            return placed;
        }
        rows.resize(instances.size());
        poses.assign(static_cast<usize>(kFigures) * kJoints, Mat4::identity());
        report.instances = static_cast<u32>(instances.size());

        pipeline::GeometrySource source;
        source.streams[pipeline::kPositionStream] = positions;
        source.streams[pipeline::kNormalStream] = normals;
        source.streams[pipeline::kUvStream] = uvs;
        source.geometry = &State::geometry;
        source.user = this;
        recorder.set_geometry(source);
        return cy::ok();
    }

    [[nodiscard]] Status create_targets() noexcept {
        rhi::BufferDescription host_copy;
        host_copy.name = "map readback";
        host_copy.size = u64{options.width} * options.height * 4U;
        host_copy.usage = rhi::BufferUsage::TransferDestination;
        host_copy.memory = rhi::MemoryUse::Readback;
        cy::Expected<rhi::BufferHandle, cy::Error> buffer = device->create_buffer(host_copy);
        if (!buffer) {
            return cy::make_unexpected(buffer.error());
        }
        readback = *buffer;
        rhi::TextureDescription out;
        out.name = "map output";
        out.format = kOutputFormat;
        out.extent = rhi::Extent3D{options.width, options.height, 1};
        out.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSource;
        cy::Expected<rhi::TextureHandle, cy::Error> made = device->create_texture(out);
        if (!made) {
            return cy::make_unexpected(made.error());
        }
        output = *made;
        if (!options.shadows) {
            return cy::ok();
        }
        rhi::TextureDescription colour;
        colour.name = "map shadow map";
        colour.format = rhi::Format::R32Sfloat;
        colour.extent = rhi::Extent3D{kShadowExtent, kShadowExtent, 1};
        colour.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
        cy::Expected<rhi::TextureHandle, cy::Error> map_colour = device->create_texture(colour);
        if (!map_colour) {
            return cy::make_unexpected(map_colour.error());
        }
        shadow_color = *map_colour;
        rhi::TextureDescription depth;
        depth.name = "map shadow depth";
        depth.format = rhi::Format::D32Sfloat;
        depth.extent = rhi::Extent3D{kShadowExtent, kShadowExtent, 1};
        depth.usage = rhi::TextureUsage::DepthStencilAttachment;
        cy::Expected<rhi::TextureHandle, cy::Error> map_depth = device->create_texture(depth);
        if (!map_depth) {
            return cy::make_unexpected(map_depth.error());
        }
        shadow_depth = *map_depth;
        rhi::TextureViewDescription view_description;
        view_description.texture = shadow_color;
        cy::Expected<rhi::TextureViewHandle, cy::Error> made_view =
            device->create_texture_view(view_description);
        if (!made_view) {
            return cy::make_unexpected(made_view.error());
        }
        shadow_view = *made_view;
        return cy::ok();
    }

    // --- The frame's lookups ---------------------------------------------------------------------

    static cy::Span<const rendering::DrawSurface> surfaces(
        const rendering::VisibleInstance& visible, void* user) noexcept {
        auto* self = static_cast<State*>(user);
        if (visible.gpu_slot >= self->instances.size()) {
            return {};
        }
        const Instance& instance = self->instances[visible.gpu_slot];
        return {instance.surfaces.data(), instance.surfaces.size()};
    }

    static bool geometry(const cy::render::DrawItem& item, const rendering::GpuDrawInstance& draw,
                         void* user, pipeline::DrawGeometry& out) noexcept {
        auto* self = static_cast<State*>(user);
        if (draw.instance_slot >= self->instances.size()) {
            return false;
        }
        const Instance& instance = self->instances[draw.instance_slot];
        const DrawMesh& mesh = self->meshes[instance.mesh];
        if (item.surface >= mesh.parts.size()) {
            return false;
        }
        const map::MeshPart& part = mesh.parts[item.surface];
        if (instance.skinned.valid()) {
            rendering::skinning::SkinnedDrawMesh skinned;
            skinned.indices = self->indices;
            skinned.wide_indices = true;
            skinned.index_count = part.index_count;
            skinned.first_index = part.first_index;
            skinned.static_vertex_offset = self->worker_uv_base;
            return rendering::skinning::skinned_draw_geometry(self->skins, instance.skinned,
                                                              skinned, out);
        }
        out.indices = self->indices;
        out.wide_indices = true;
        out.index_count = part.index_count;
        out.first_index = part.first_index;
        out.vertex_offset = mesh.vertex_offset;
        return true;
    }

    // --- One frame -------------------------------------------------------------------------------

    void set_camera(RtsHost& from) noexcept {
        const f32 aspect = static_cast<f32>(options.width) / static_cast<f32>(options.height);
        if (options.view == MapView::Game) {
            eye = from.camera_position();
            forward = cy::normalize(from.camera_forward());
            fov = from.camera_fov();
            focus = from.camera_focus();
        } else {
            // A slow orbit over the level and the company's field, looking down as an RTS does,
            // steep enough that the map's edge never comes into the frame.
            const f32 t = static_cast<f32>(frame) / 420.0F;
            const f32 angle = 0.15F + (0.8F * t);
            focus = Vec3{16.0F - (2.0F * t), 0.0F, 14.0F - (34.0F * t)};
            const Vec3 offset{std::sin(angle) * 26.0F, 46.0F, std::cos(angle) * 26.0F};
            eye = focus + offset;
            forward = cy::normalize(focus - eye);
            fov = 0.85F;
        }
        view = cy::look_at(eye, eye + forward, Vec3{0.0F, 1.0F, 0.0F});
        relative_view = cy::look_at(Vec3{0.0F, 0.0F, 0.0F}, forward, Vec3{0.0F, 1.0F, 0.0F});
        projection = cy::perspective_reversed_z(fov, aspect, kNear, kFar);

        // THE SUN'S MAP follows the ground the camera frames: an orthographic box over it, a
        // camera-relative transform as the frame's rows are.
        shadow_radius = options.view == MapView::Game ? 40.0F : 72.0F;
        const Vec3 travel = cy::normalize(sun_travel);
        const Vec3 centre = focus - eye;
        const Vec3 light_eye = centre - (travel * (shadow_radius * 2.5F));
        const Mat4 light_view = cy::look_at(light_eye, centre, Vec3{0.0F, 1.0F, 0.0F});
        const Mat4 light_projection =
            cy::orthographic_reversed_z(-shadow_radius, shadow_radius, -shadow_radius,
                                        shadow_radius, 0.5F, shadow_radius * 5.0F);
        shadow_to_clip = light_projection * light_view;
    }

    void place_rows() noexcept {
        for (usize slot = 0; slot < instances.size(); ++slot) {
            const Instance& instance = instances[slot];
            pipeline::InstanceTransform& row = rows[slot];
            for (u32 column = 0; column < 4U; ++column) {
                row.row0[column] = instance.world.at(0, column);
                row.row1[column] = instance.world.at(1, column);
                row.row2[column] = instance.world.at(2, column);
            }
            row.row0[3] -= eye.x;
            row.row1[3] -= eye.y;
            row.row2[3] -= eye.z;
            row.tint[0] = instance.tint.x;
            row.tint[1] = instance.tint.y;
            row.tint[2] = instance.tint.z;
            row.tint[3] = 1.0F;
        }
    }

    [[nodiscard]] Status move(u32 slot, const Mat4& world) noexcept {
        Instance& instance = instances[slot];
        instance.world = world;
        const DrawMesh& mesh = meshes[instance.mesh];
        return index.update(instance.index_slot, transform_box(world, mesh.low, mesh.high));
    }

    /// The ground a figure stands on: the level's floor inside it, the cooked terrain outside.
    [[nodiscard]] f32 ground(f32 x, f32 z) const noexcept {
        return terrain == nullptr ? 0.0F : terrain->height_at(x, z);
    }

    /// Every figure where its source put it, and its pose for this frame.
    [[nodiscard]] Status place_figures(RtsHost& from) noexcept {
        clock += kFrameSeconds;
        for (u32 which = 0; which < kFigures; ++which) {
            Mat4* pose = poses.data() + (static_cast<usize>(which) * kJoints);
            Vec3 at = kParked;
            const f32* heading = nullptr;
            f32 stated = 0.0F;
            if (which < kMaxWorkers) {
                if (which < from.unit_count()) {
                    const CyEntity unit = from.unit(which);
                    at = from.unit_position(unit);
                    const cy::Span<const Mat4> animated = from.unit_pose(unit);
                    if (animated.size() >= kJoints) {
                        std::copy(animated.begin(), animated.begin() + kJoints, pose);
                    } else {
                        poser.pose(false, clock, pose);
                    }
                } else {
                    poser.pose(false, clock, pose);
                }
            } else if (which < kMaxWorkers + kCompanySize) {
                cy::game_backend::LockstepUnitState unit;
                if (from.company_unit(which - kMaxWorkers, unit)) {
                    const auto one = static_cast<f64>(cy::detmath::Fixed::kOneRaw);
                    const f32 x =
                        static_cast<f32>(static_cast<f64>(unit.position.x.raw) / one) + company_x;
                    const f32 z =
                        static_cast<f32>(static_cast<f64>(unit.position.y.raw) / one) + company_z;
                    at = Vec3{x, ground(x, z), z};
                    const f64 turn = static_cast<f64>(unit.heading.raw) / 4294967296.0;
                    const f32 h = static_cast<f32>(turn * 2.0 * std::numbers::pi);
                    stated = std::atan2(std::cos(h), std::sin(h));
                    heading = unit.moving ? &stated : nullptr;
                    poser.pose(unit.moving, clock + (0.13F * static_cast<f32>(which)), pose);
                } else {
                    poser.pose(false, clock, pose);
                }
            } else {
                poser.pose(false, clock + static_cast<f32>(which), pose);
            }
            if (which >= kMaxWorkers + kCompanySize) {
                at = camp_position(from, which - (kMaxWorkers + kCompanySize));
                stated = std::atan2(16.0F - at.x, 16.0F - at.z);
                heading = &stated;
            }
            headings[which].follow(at, heading);
            if (Status moved = move(first_figure + which,
                                    Mat4::from_translation(at) * yaw_matrix(headings[which].yaw));
                !moved) {
                return moved;
            }
        }
        // THE POSES, uploaded whole: twenty-six figures of eight joints is a few kilobytes.
        if (Status uploaded = skins.upload_poses(cy::Span<const Mat4>(poses.data(), poses.size()),
                                                 0, static_cast<u32>(poses.size()));
            !uploaded) {
            return uploaded;
        }
        for (u32 which = 0; which < kFigures; ++which) {
            if (Status set =
                    skins.set_pose(instances[first_figure + which].skinned, which * kJoints);
                !set) {
                return set;
            }
        }
        return cy::ok();
    }

    /// Where camp member `index` is drawn by blue's sight: where it stands, where blue last saw it,
    /// or nowhere.
    [[nodiscard]] Vec3 camp_position(const RtsHost& from, u32 member) const noexcept {
        if (!options.fog) {
            return Vec3{kCamp[member].at.x, ground(kCamp[member].at.x, kCamp[member].at.z),
                        kCamp[member].at.z};
        }
        const gfog::FogOfWar& grid = from.fog().grid();
        switch (grid.sight(kBlue, from.camp(member))) {
            case gfog::UnitSight::Visible:
                return Vec3{kCamp[member].at.x, 0.0F, kCamp[member].at.z};
            case gfog::UnitSight::Ghost: {
                const gfog::FogGhost* ghost = grid.ghost(kBlue, from.camp(member));
                return Vec3{static_cast<f32>(static_cast<f64>(ghost->x_raw) / 4294967296.0), 0.0F,
                            static_cast<f32>(static_cast<f64>(ghost->z_raw) / 4294967296.0)};
            }
            case gfog::UnitSight::Hidden:
                break;
        }
        return kParked;
    }

    /// Blue's field over the map: the level's grid at its square, explored ground round it.
    [[nodiscard]] Status fog_field(const RtsHost& from) noexcept {
        const gfog::FogOfWar& grid = from.fog().grid();
        if (Status written = grid.write_states(
                kBlue, cy::Span<cy::u8>(level_states.data(), level_states.size()));
            !written) {
            return written;
        }
        std::fill(map_states.begin(), map_states.end(), fow::kCellExplored);
        const auto offset_x = static_cast<u32>(-map_origin_x);
        const auto offset_z = static_cast<u32>(-map_origin_z);
        for (u32 z = 0; z < kFogCells; ++z) {
            for (u32 x = 0; x < kFogCells; ++x) {
                map_states[((z + offset_z) * kMapFogCells) + x + offset_x] =
                    level_states[(z * kFogCells) + x];
            }
        }
        if (Status set =
                field->set_states(cy::Span<const u8>(map_states.data(), map_states.size()));
            !set) {
            return set;
        }
        if (Status set = level_field->set_states(
                cy::Span<const u8>(level_states.data(), level_states.size()));
            !set) {
            return set;
        }
        if (frame == 0) {
            field->snap();
            level_field->snap();
        } else {
            field->advance(kFrameSeconds);
            level_field->advance(kFrameSeconds);
        }
        return cy::ok();
    }

    /// The minimap's fog: blue's field over the level, composed into the atlas page the game's HUD
    /// shows on its minimap (ABI 1.10), as `render.rts_api_hud` composes it.
    [[nodiscard]] Status minimap_page() noexcept {
        if (!options.fog || level_field == nullptr || frame % kMinimapFogEvery != 0U) {
            return cy::ok();
        }
        const fow::FogGridPlacement placement{0.0F, 0.0F, 1.0F};
        const fow::MinimapRegion region{0.0F, static_cast<f32>(kFogCells), 0.0F,
                                        static_cast<f32>(kFogCells)};
        if (Status composed = fow::compose_minimap_fog(
                level_field->texels(), kFogCells, kFogCells, placement, region, kMinimapFogTexels,
                kMinimapFogTexels, fow::MinimapFogStyle{},
                cy::Span<u32>(minimap_fog.data(), minimap_fog.size()));
            !composed) {
            return composed;
        }
        return ui.upload_atlas(kMinimapFogPage, rhi::Format::Rgba8Unorm, kMinimapFogTexels,
                               kMinimapFogTexels,
                               cy::Span<const u8>(reinterpret_cast<const u8*>(minimap_fog.data()),
                                                  minimap_fog.size() * sizeof(u32)));
    }

    // --- The frame's hooks, in the order the frame runs them -------------------------------------

    [[nodiscard]] Status before_assemble(rendering::assembly::AssemblyView& assembly_view,
                                         rendering::assembly::FrameSinks& sinks) noexcept {
        if (Status declared = skins.declare(graph, frame); !declared) {
            return declared;
        }
        const cy::Span<const rendering::ResourceId> reads = skins.vertex_reads();
        for (const rendering::FramePassKind kind :
             {rendering::FramePassKind::DepthPrepass, rendering::FramePassKind::Shadow,
              rendering::FramePassKind::Opaque, rendering::FramePassKind::Transparent}) {
            sinks.passes[static_cast<usize>(kind)].vertex_reads = reads;
        }
        if (options.shadows) {
            rendering::TextureRequest request;
            request.name = "map shadow map";
            request.format = rhi::Format::R32Sfloat;
            request.width = kShadowExtent;
            request.height = kShadowExtent;
            assembly_view.shadow_color =
                graph.import_texture(request, shadow_color, rhi::ImageUse::Undefined);
            request.name = "map shadow depth";
            request.format = rhi::Format::D32Sfloat;
            assembly_view.shadow_depth =
                graph.import_texture(request, shadow_depth, rhi::ImageUse::Undefined);
            recorder.set_shadow_targets(assembly_view.shadow_color, assembly_view.shadow_depth,
                                        kShadowExtent);
            const auto shadow = static_cast<usize>(rendering::FramePassKind::Shadow);
            sinks.passes[shadow] = recorder.sinks().passes[shadow];
            sinks.passes[shadow].vertex_reads = reads;
            opaque_reads[0] = rendering::FrameResourceRead{assembly_view.shadow_color,
                                                           rhi::Access::FragmentSampledRead};
            sinks.passes[static_cast<usize>(rendering::FramePassKind::Opaque)].reads =
                cy::Span<const rendering::FrameResourceRead>(opaque_reads, 1);
        }
        if (options.ambient_occlusion) {
            rendering::occlusion::GtaoView gtao;
            gtao.projection = projection;
            gtao.relative_to_view = relative_view;
            gtao.width = options.width;
            gtao.height = options.height;
            if (Status set = occlusion.set_view(gtao); !set) {
                return set;
            }
            assembly_view.ambient_occlusion = occlusion.import_target(graph);
            sinks.ambient_occlusion = occlusion.stage();
        }
        if (options.fog) {
            fow::FogOfWarFrame frame_fog;
            frame_fog.view.projection = projection;
            frame_fog.view.relative_view = relative_view;
            frame_fog.view.eye = eye;
            frame_fog.placement = fow::FogGridPlacement{map_origin_x, map_origin_z, 1.0F};
            // Ground blue has explored and does not see now, the map round the level included,
            // dimmed and greyed less than the engine's default so the map still reads as a place.
            frame_fog.style.explored_brightness = 0.72F;
            frame_fog.style.explored_saturation = 0.6F;
            // Never explored: a dark slate rather than a void, in the frame's scene-referred units
            // (a dark slate once the frame is exposed and tonemapped).
            frame_fog.style.unexplored_colour = Vec3{300.0F, 340.0F, 400.0F};
            frame_fog.field = field.get();
            if (Status set = fog.set_frame(frame_fog); !set) {
                return set;
            }
            sinks.fog_of_war = fog.stage();
        }
        if (options.hud && ui.ready()) {
            sinks.ui = ui.stage();
        }
        if (!options.gi) {
            return cy::ok();
        }
        gi_gpu::GpuGiFrameView gi_view;
        gi_view.eye = eye;
        gi_view.world_to_clip = projection * view;
        gi_view.projection = projection;
        gi_view.relative_to_view = relative_view;
        gi_gpu::GpuGiFrameTargets targets;
        if (Status prepared = gi.prepare(graph, gi_view, host->illumination().frame(), targets);
            !prepared) {
            return prepared;
        }
        assembly_view.screen_space_gi = targets.diffuse;
        assembly_view.screen_space_gi_specular = targets.specular;
        sinks.screen_space_gi = gi.stage();
        return cy::ok();
    }

    [[nodiscard]] Status before_upload(pipeline::FrameUpload& upload) noexcept {
        std::vector<pipeline::MaterialTextureSlot> slots = resident;
        if (options.ambient_occlusion) {
            rendering::occlusion::GtaoView gtao;
            gtao.projection = projection;
            gtao.relative_to_view = relative_view;
            gtao.width = options.width;
            gtao.height = options.height;
            gtao.jitter = Vec2{upload.view.temporal_jitter[0], upload.view.temporal_jitter[1]};
            if (Status set = occlusion.set_view(gtao); !set) {
                return set;
            }
            slots.push_back(pipeline::MaterialTextureSlot{kOcclusionSlot, occlusion.target_view()});
            rendering::occlusion::write_occlusion_control(
                kOcclusionSlot, occlusion.settings().shared, upload.view.occlusion_control);
        }
        if (options.shadows) {
            u32 sun = static_cast<u32>(upload.lights.size());
            for (usize light = 0; light < upload.lights.size(); ++light) {
                if (upload.lights[light].kind == rendering::kGpuLightDirectional) {
                    sun = static_cast<u32>(light);
                }
            }
            for (u32 row = 0; row < 4U; ++row) {
                for (u32 column = 0; column < 4U; ++column) {
                    upload.view.shadow_to_clip[(row * 4U) + column] =
                        shadow_to_clip.at(row, column);
                }
            }
            upload.view.shadow_control[0] = kShadowSlot;
            upload.view.shadow_control[1] = sun;
            upload.view.shadow_control[2] = kShadowExtent;
            upload.view.shadow_control[3] = 1U;
            rendering::SoftShadowSettings soft;
            soft.quality = rendering::ShadowQuality::High;
            soft.max_radius_texels = 8.0F;
            rendering::DirectionalShadowFootprint footprint;
            footprint.depth_range_metres = (shadow_radius * 5.0F) - 0.5F;
            footprint.width_metres = shadow_radius * 2.0F;
            footprint.extent = kShadowExtent;
            rendering::write_soft_shadow_words(
                pipeline::kSoftShadowPcss, pipeline::kNoMaterialTexture,
                rendering::make_pcss_shape(soft, footprint), upload.view.soft_shadow_control,
                upload.view.soft_shadow_shape);
            slots.push_back(pipeline::MaterialTextureSlot{kShadowSlot, shadow_view});
        }
        if (options.gi) {
            gi.set_jitter(Vec2{upload.view.temporal_jitter[0], upload.view.temporal_jitter[1]});
            slots.push_back(pipeline::MaterialTextureSlot{kGiDiffuseSlot, gi.diffuse_view()});
            slots.push_back(pipeline::MaterialTextureSlot{kGiSpecularSlot, gi.specular_view()});
            if (gi.composites()) {
                gi.write_control(kGiDiffuseSlot, kGiSpecularSlot, upload.view.gi_control);
            }
        }
        return bindings.set_material_textures(
            cy::Span<const pipeline::MaterialTextureSlot>(slots.data(), slots.size()));
    }

    struct Capture {
        rendering::ResourceId output = rendering::kInvalidResource;
        rhi::BufferHandle buffer;
        u32 width = 0;
        u32 height = 0;
    };
    Capture capture;

    static void record_capture(const rendering::PassContext& context, void* user) noexcept {
        auto* copy = static_cast<Capture*>(user);
        rhi::BufferTextureCopy region;
        region.texture_extent = rhi::Extent3D{copy->width, copy->height, 1};
        context.commands->copy_texture_to_buffer(
            context.executor->texture(copy->output), copy->buffer,
            cy::Span<const rhi::BufferTextureCopy>(&region, 1));
    }

    [[nodiscard]] Status render(bool read_back, rendering::assembly::AssemblyReport& out) noexcept {
        cy::Expected<u32, cy::Error> began = device->begin_frame();
        if (!began) {
            return cy::make_unexpected(began.error());
        }
        const u32 slot = *began;
        graph.reset();
        rendering::assembly::AssemblyView frame_view;
        frame_view.fov_y_radians = fov;
        frame_view.view = view;
        frame_view.projection = projection;
        // THE CULL LOOKS WIDER THAN THE PICTURE, so a tree just off the bottom of the screen still
        // casts its shadow into it: the shadow pass draws the opaque list the cull produced.
        const f32 aspect = static_cast<f32>(options.width) / static_cast<f32>(options.height);
        const Mat4 wide =
            cy::perspective_reversed_z(std::min(fov * 1.6F, 2.6F), aspect, kNear, kFar);
        frame_view.cull.frustum = cy::Frustum::from_view_projection(wide * view);
        frame_view.cull.camera_position = eye;
        frame_view.cull.camera_forward = forward;
        frame_view.cull.fov_y_radians = fov;
        frame_view.lights = lights.span();
        frame_view.sun_direction = cy::normalize(sun_travel * -1.0F);
        rendering::TextureRequest request;
        request.name = "map output";
        request.format = kOutputFormat;
        request.width = options.width;
        request.height = options.height;
        request.extra_usage = rhi::TextureUsage::TransferSource;
        frame_view.output = graph.import_texture(request, output, rhi::ImageUse::Undefined);

        if (Status bound = recorder.bind(assembly); !bound) {
            return bound;
        }
        rendering::assembly::FrameSinks sinks = recorder.sinks();
        sinks.surfaces = &State::surfaces;
        sinks.surfaces_user = this;
        if (Status hooked = before_assemble(frame_view, sinks); !hooked) {
            return hooked;
        }
        if (Status assembled = assembly.assemble(index, frame_view, sinks, graph, out);
            !assembled) {
            return assembled;
        }
        pipeline::GlobalsData globals;
        globals.exposure_stops = kExposureStops;
        pipeline::FrameUpload upload = pipeline::upload_for(
            assembly, out, projection * relative_view, relative_view,
            cy::Span<const pipeline::InstanceTransform>(rows.data(), rows.size()), globals,
            material_offsets);
        for (u32 word = 0; word < 4U; ++word) {
            upload.view.material_textures[word] = material_texture_offsets[word];
        }
        if (Status hooked = before_upload(upload); !hooked) {
            return hooked;
        }
        if (Status uploaded = bindings.upload(slot, upload); !uploaded) {
            return uploaded;
        }
        capture = Capture{assembly.resources().output, readback, options.width, options.height};
        if (read_back && capture.output != rendering::kInvalidResource) {
            rendering::BufferRequest capture_request;
            capture_request.name = "map capture";
            capture_request.size = u64{options.width} * options.height * 4U;
            capture_request.extra_usage = rhi::BufferUsage::TransferDestination;
            const rendering::ResourceId destination =
                graph.import_buffer(capture_request, readback);
            graph.add_pass("map capture", rhi::QueueKind::Graphics)
                .read(capture.output, rhi::Access::TransferRead)
                .write(destination, rhi::Access::TransferWrite)
                .record(&State::record_capture, &capture);
            graph.add_pass("map capture host", rhi::QueueKind::Graphics)
                .read(destination, rhi::Access::HostRead)
                .side_effect();
        }
        rendering::GraphExecutor executor(*allocator, *device);
        const auto submitted = std::chrono::steady_clock::now();
        Status executed = assembly.execute(executor, graph, out);
        const auto recorded = std::chrono::steady_clock::now();
        if (executed) {
            executed = device->wait_idle();
        }
        last_record_ms = std::chrono::duration<f64, std::milli>(recorded - submitted).count();
        last_wait_ms = since_ms(recorded);
        if (executed && read_back) {
            const auto* mapped = static_cast<const u32*>(device->buffer_mapped_pointer(readback));
            if (mapped == nullptr) {
                executed = cy::fail(cy::ErrorCode::Internal, "the map readback is not mapped");
            } else {
                pixels.assign(mapped,
                              mapped + (static_cast<usize>(options.width) * options.height));
            }
        }
        executor.release();
        if (Status ended = device->end_frame(); !ended && executed) {
            return ended;
        }
        return executed;
    }

    [[nodiscard]] Status write_png(const std::string& path) noexcept {
        cy::render_test::Image image(cy::system_allocator(cy::MemoryDomain::Renderer));
        if (Status adopted =
                cy::render_test::adopt(image, cy::Span<const u32>(pixels.data(), pixels.size()),
                                       options.width, options.height);
            !adopted) {
            return adopted;
        }
        return cy::render_test::write_png(path.c_str(), image);
    }

    [[nodiscard]] Status draw_hud(RtsHost& from) noexcept {
        cy::ui::ElementStore* store = from.ui_store();
        if (!options.hud || store == nullptr) {
            ui.clear();
            return cy::ok();
        }
        cy::ui::TextPainter& text = from.ui_text();
        if (text.atlas_revision() != ui_revision) {
            const u32 extent = text.atlas_extent();
            if (Status uploaded =
                    ui.upload_atlas(1, rhi::Format::R8Unorm, extent, extent, text.atlas_pixels());
                !uploaded) {
                return uploaded;
            }
            ui_revision = text.atlas_revision();
        }
        if (Status page = minimap_page(); !page) {
            return page;
        }
        const Vec2 viewport = RtsHost::ui_viewport();
        cy::ui::FlattenReport flattened{};
        if (Status flat = cy::ui::flatten(*store, cy::ui::Rect{0.0F, 0.0F, viewport.x, viewport.y},
                                          ui_buffer, flattened, &text);
            !flat) {
            return flat;
        }
        return ui.submit(ui_buffer, static_cast<f32>(options.width) / viewport.x);
    }
};

// --- The GI's boxes
// -------------------------------------------------------------------------------

std::vector<FieldBox> map_gi_boxes(const MapContent& content) {
    std::vector<FieldBox> boxes;
    for (const map::Placement& placement : content.layout.placements) {
        if ((placement.kind != map::PlacementKind::Building &&
             placement.kind != map::PlacementKind::Resource) ||
            placement.tag == "Barracks" || placement.tag == "Beacon") {
            continue;
        }
        const map::MeshRange& mesh = content.props.meshes[placement.mesh];
        const Aabb bounds =
            transform_box(placement_matrix(placement.position, placement.yaw, placement.scale),
                          mesh.min, mesh.max);
        FieldBox box;
        box.material = "map";
        box.centre = (bounds.min + bounds.max) * 0.5F;
        box.half = (bounds.max - bounds.min) * 0.5F;
        const u32 material =
            mesh.part_count > 0 ? content.props.parts[mesh.first_part].material : map::kNoMaterial;
        box.albedo = material < content.layout.materials.size()
                         ? content.layout.materials[material].base_colour * 0.8F
                         : Vec3{0.5F, 0.5F, 0.5F};
        box.card_spacing = 0.5F;
        boxes.push_back(box);
    }
    return boxes;
}

// --- The renderer
// ----------------------------------------------------------------------------------

MapRenderer::MapRenderer(cy::Allocator& allocator) noexcept : allocator_(&allocator) {}

MapRenderer::~MapRenderer() {
    if (state_ == nullptr) {
        return;
    }
    // THE DEVICE GOES LAST: everything that made a resource through it releases it first.
    rhi::Device* device = state_->device;
    if (device != nullptr) {
        (void)device->wait_idle();
        State& state = *state_;
        state.ui.destroy();
        state.gi.destroy();
        state.fog.destroy();
        state.occlusion.destroy();
        state.skins.destroy();
        state.textures.shutdown();
        state.bindings.shutdown();
        state.pipelines.shutdown();
        if (!state.shadow_view.is_null()) {
            device->destroy_texture_view(state.shadow_view);
        }
        for (rhi::TextureHandle* texture :
             {&state.output, &state.shadow_color, &state.shadow_depth}) {
            if (!texture->is_null()) {
                device->destroy_texture(*texture);
            }
        }
        for (rhi::BufferHandle* buffer :
             {&state.positions, &state.normals, &state.uvs, &state.indices, &state.readback}) {
            if (!buffer->is_null()) {
                device->destroy_buffer(*buffer);
            }
        }
    }
    state_.reset();
    if (device != nullptr) {
        rhi::destroy_device(cy::system_allocator(cy::MemoryDomain::Gpu), device);
    }
}

cy::Status MapRenderer::start(RtsHost& host, const MapContent& content,
                              const MapRenderOptions& options) noexcept {
    state_ = std::make_unique<State>(*allocator_);
    State& state = *state_;
    state.options = options;
    state.host = &host;
    state.terrain_copy = content.terrain;
    state.terrain = &state.terrain_copy;
    state.map_origin_x = content.layout.origin_x;
    state.map_origin_z = content.layout.origin_z;
    state.company_x = content.layout.company_x;
    state.company_z = content.layout.company_z;
    state.sun_travel = content.layout.sun_travel;
    state.sun_lux = content.layout.sun_lux;
    if (Status made = state.create_device(); !made) {
        return made;
    }
    const auto materials = static_cast<u32>(content.layout.materials.size()) + kTeams;
    if (Status made = state.create_frame(materials); !made) {
        return made;
    }
    std::vector<u32> slot_of_texture;
    if (Status made = state.create_textures(content, slot_of_texture); !made) {
        return made;
    }
    if (Status made = state.create_materials(content, slot_of_texture); !made) {
        return made;
    }
    if (Status bound = state.poser.bind(host.worker_rig()); !bound) {
        return bound;
    }
    if (Status made = state.create_geometry(content); !made) {
        return made;
    }
    if (Status made = state.create_targets(); !made) {
        return made;
    }
    if (Status sized = state.lights.resize(1); !sized) {
        return sized;
    }
    state.lights[0].kind = cy::render::LightKind::Directional;
    state.lights[0].intensity = state.sun_lux;
    state.lights[0].transform.rotation =
        cy::Quat::look_rotation(cy::normalize(state.sun_travel), Vec3{0.0F, 1.0F, 0.0F});
    state.lights[0].color[0] = 1.0F;
    state.lights[0].color[1] = 0.96F;
    state.lights[0].color[2] = 0.88F;
    state.lights[0].stable_id = 1;

    if (options.ambient_occlusion) {
        rendering::occlusion::AmbientOcclusionPassDescription description;
        description.width = options.width;
        description.height = options.height;
        if (Status made = state.occlusion.create(*allocator_, *state.device, description); !made) {
            return made;
        }
        rendering::occlusion::GtaoSettings settings;
        settings.shared.radius = 1.6F;
        state.occlusion.set_settings(settings);
    }
    if (options.fog) {
        state.field = std::make_unique<fow::VisibilityField>(*allocator_);
        if (Status made = state.field->create(kMapFogCells, kMapFogCells); !made) {
            return made;
        }
        state.level_field = std::make_unique<fow::VisibilityField>(*allocator_);
        if (Status made = state.level_field->create(kFogCells, kFogCells); !made) {
            return made;
        }
        state.minimap_fog.assign(static_cast<usize>(kMinimapFogTexels) * kMinimapFogTexels, 0U);
        state.level_states.assign(static_cast<usize>(kFogCells) * kFogCells, 0U);
        state.map_states.assign(static_cast<usize>(kMapFogCells) * kMapFogCells, 0U);
        if (Status made =
                state.fog.create(*allocator_, *state.device,
                                 fow::FogOfWarPassDescription{options.width, options.height,
                                                              kMapFogCells, kMapFogCells, false});
            !made) {
            return made;
        }
    }
    if (options.hud) {
        cy::ui::render::UiRendererDescription description;
        description.width = options.width;
        description.height = options.height;
        description.output_format = kOutputFormat;
        if (Status made = state.ui.create(*state.device, description); !made) {
            return made;
        }
    }
    if (!options.gi) {
        return cy::ok();
    }
    if (!gi_gpu::GpuGiFrame::supported(*state.device)) {
        return cy::fail(cy::ErrorCode::Unsupported, "the device cannot run the GI");
    }
    gi_gpu::GpuGiFrameDescription gi;
    gi.width = options.width;
    gi.height = options.height;
    return state.gi.create(*allocator_, *state.device, host.illumination().system(), gi);
}

cy::Status MapRenderer::frame(RtsHost& host) noexcept {
    State& state = *state_;
    state.frame = host.frames();
    const auto started = std::chrono::steady_clock::now();
    state.set_camera(host);
    if (Status placed = state.place_figures(host); !placed) {
        return placed;
    }
    const Vec3 crate = host.crate_position();
    if (Status moved = state.move(state.crate, Mat4::from_translation(crate)); !moved) {
        return moved;
    }
    const Vec3 tower = state.camp_position(host, 2);
    if (Status moved = state.move(state.watchtower, Mat4::from_translation(tower)); !moved) {
        return moved;
    }
    if (state.options.fog) {
        if (Status fogged = state.fog_field(host); !fogged) {
            return fogged;
        }
    }
    if (Status hud = state.draw_hud(host); !hud) {
        return hud;
    }
    state.place_rows();
    const bool still = std::ranges::find(kStills, state.frame) != std::end(kStills);
    const bool video = !state.options.video_directory.empty() &&
                       state.frame % std::max<u32>(state.options.video_every, 1U) == 0U;
    rendering::assembly::AssemblyReport assembled{};
    if (Status rendered = state.render(still || video, assembled); !rendered) {
        return rendered;
    }
    const f64 elapsed = since_ms(started);
    MapRenderReport& report = state.report;
    report.frames = state.frame;
    report.draws = state.recorder.report().draws();
    report.skinned_draws = state.recorder.report().skinned_draws;
    if (state.frame > kWarmFrames && !still && !video) {
        report.timed += 1;
        report.frame_ms += elapsed;
        report.worst_ms = std::max(report.worst_ms, elapsed);
        report.game_ms += host.last_frame_ms();
        report.record_ms += state.last_record_ms;
        report.device_wait_ms += state.last_wait_ms;
        const gi_gpu::GpuGiCost cost = state.gi.last_cost();
        if (state.options.gi && cost.any()) {
            report.gi_device_ms += cost.total_ms();
        }
        const f64 fog_ms = state.options.fog ? state.fog.last_device_ms() : -1.0;
        report.fog_device_ms += fog_ms > 0.0 ? fog_ms : 0.0;
    }
    if (video) {
        char name[64];
        (void)std::snprintf(name, sizeof(name), "/frame-%04llu.png",
                            static_cast<unsigned long long>(state.frame));
        if (Status written = state.write_png(state.options.video_directory + name); !written) {
            return written;
        }
    }
    if (still) {
        char name[64];
        (void)std::snprintf(name, sizeof(name), "/rts-map-%03llu.png",
                            static_cast<unsigned long long>(state.frame));
        if (Status written = state.write_png(state.options.directory + name); !written) {
            return written;
        }
        std::fprintf(stderr, "wrote %s%s\n", state.options.directory.c_str(), name);
        report.stills += 1;
    }
    report.validation_errors = state.validation_errors;
    return cy::ok();
}

const MapRenderReport& MapRenderer::report() const noexcept {
    static const MapRenderReport kNone{};
    return state_ == nullptr ? kNone : state_->report;
}

void MapRenderer::print_report() const noexcept {
    if (state_ == nullptr) {
        return;
    }
    const MapRenderReport& report = state_->report;
    const f64 timed = static_cast<f64>(std::max<u64>(report.timed, 1));
    std::printf(
        "rts map render width=%u height=%u frames=%llu timed=%llu frame_ms=%.3f worst_ms=%.3f "
        "game_ms=%.3f record_ms=%.3f device_wait_ms=%.3f gi_device_ms=%.3f fog_device_ms=%.4f "
        "draws=%u skinned_draws=%u "
        "instances=%u triangles=%u plants=%u stills=%u validation_errors=%u gi=%d fog=%d hud=%d "
        "ao=%d shadows=%d\n",
        state_->options.width, state_->options.height,
        static_cast<unsigned long long>(report.frames),
        static_cast<unsigned long long>(report.timed), report.frame_ms / timed, report.worst_ms,
        report.game_ms / timed, report.record_ms / timed, report.device_wait_ms / timed,
        report.gi_device_ms / timed, report.fog_device_ms / timed, report.draws,
        report.skinned_draws, report.instances, report.triangles, report.plants, report.stills,
        report.validation_errors, state_->options.gi ? 1 : 0, state_->options.fog ? 1 : 0,
        state_->options.hud ? 1 : 0, state_->options.ambient_occlusion ? 1 : 0,
        state_->options.shadows ? 1 : 0);
}

}  // namespace sample::rts
