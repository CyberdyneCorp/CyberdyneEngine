// SPDX-License-Identifier: MIT
// AERIAL PERSPECTIVE IN THE WORLD FRAME, DRAWN BY THE WORLD'S OWN SHADERS ON A DEVICE.
//
// `atmosphere-sky-and-clouds` — "Aerial perspective": "Distance attenuation SHALL be produced by
// the atmosphere model — scattering and transmittance over distance — and applied to opaque
// shading", "Aerial perspective SHALL be correct at large scale, so that terrain kilometres away is
// attenuated consistently with the sky above it", and the scenario "WHEN distant terrain is
// rendered THEN its attenuation SHALL come from the atmosphere, consistent with the sky".
//
// The frame that answers it is `samples/10-world`'s: its one lit fragment path draws the terrain,
// the foliage and the water, and that path reads `sky::AerialPerspectiveTable` through
// `cy/aerial_perspective.slang`, while the dome's clear sky is `sky::IncrementalSkyView` over the
// same atmosphere tables. This suite draws with THAT pipeline's committed SPIR-V —
// `samples/10-world/shaders/world_spirv.h` — a dome coloured the way `Stage::build_dynamic` colours
// it and flat walls at known distances, lit, and reads the pixels back.
//
// ================================================================================================
// FIVE CLAIMS
// ================================================================================================
//
//   * THE DEVICE APPLIES THE TABLE THE PROCESSOR BUILT. Every wall texel is `lit * T + S` with T
//     and S from `AerialPerspectiveTable::sample_at()` at that texel's own surface point, within
//     1% after the shader's tone map.
//   * A NEAR SURFACE KEEPS ITS OWN COLOUR, A FAR ONE TAKES THE SKY'S. A wall six metres away is
//     within 0.1% of the same wall with aerial perspective off; along the horizon, walls at 2, 30,
//     300 and 800 km approach the sky drawn in the same direction monotonically.
//   * AT THE HORIZON THE GEOMETRY MEETS THE SKY BESIDE IT. The last wall texel of every column
//     against the dome texel directly above it, in display values; with aerial perspective off the
//     same comparison is far apart, so the check can fail.
//   * CHANGE THE ATMOSPHERE AND BOTH MOVE TOGETHER. More aerosol and a lower sun each move the sky
//     and the distant wall by a visible amount, and at each the two still meet.
//   * OFF IS THE FRAME BEFORE, AND THE SKY IS NEVER HAZED. With the table's `enabled` word zero,
//     every texel is bit-identical to the frame drawn by the world's shaders before this change
//     (`world_before_aerial_perspective_spirv.h`, pinned at 1fe6446); and the emissive dome is
//     bit-identical with the table on and off.
//
// ================================================================================================
// THE MUTATIONS THIS SUITE WAS PROVED AGAINST
// ================================================================================================
//
// Each applied to the shader, the SPIR-V regenerated, the suite run and seen red, the file restored
// and md5-verified: `openspec/changes/add-aerial-perspective/evidence/falsification.txt`.

#include <cy/test/test.h>

#include <cy/backends/rhi/access.h>
#include <cy/core/math/math.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/sky/tables.h>

#include "10-world/shaders/world_spirv.h"
#include "device.h"
#include "world_before_aerial_perspective_spirv.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <iterator>

namespace {

using cy::f32;
using cy::u16;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;
using cy::Vec3;
using cy::Vec4;
namespace rhi = cy::rhi;
namespace sky = cy::rendering::sky;

[[nodiscard]] cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

// --- The camera ----------------------------------------------------------------------------------

constexpr u32 kWidth = 160;
constexpr u32 kHeight = 96;
constexpr u32 kTexels = kWidth * kHeight;
/// Metres above sea level. The world sample's orbit flies at about this height.
constexpr f32 kEyeHeight = 100.0F;
/// Half the vertical field of view's tangent: 54 degrees top to bottom, the sample's 0.95 radians.
constexpr f32 kTanHalfY = 0.5147F;
constexpr f32 kTanHalfX = kTanHalfY * (static_cast<f32>(kWidth) / static_cast<f32>(kHeight));
constexpr f32 kNear = 1.0F;
/// The dome, behind every wall at every angle in the frame, as the sample's dome is behind its
/// terrain: a sphere nearer than a wall 800 km out would cover the frame's corners.
constexpr f32 kDomeRadius = 3'000'000.0F;
/// Where the aerial perspective volume ends. Beyond every wall.
constexpr f32 kAerialFar = 1'000'000.0F;
constexpr f32 kAlbedo = 0.3F;
constexpr f32 kAmbient = 0.1F;
constexpr f32 kSun = 0.8F;

/// The view the table and the pixels share: straight along -Z, level.
[[nodiscard]] sky::AerialPerspectiveTable::View camera_view() noexcept {
    sky::AerialPerspectiveTable::View view;
    view.forward = Vec3{0.0F, 0.0F, -1.0F};
    view.right = Vec3{1.0F, 0.0F, 0.0F};
    view.up = Vec3{0.0F, 1.0F, 0.0F};
    view.tan_half_fov_x = kTanHalfX;
    view.tan_half_fov_y = kTanHalfY;
    return view;
}

/// The unit ray through a texel's centre. Row zero is the TOP of the picture: the Vulkan backend
/// flips the viewport so that clip y points up.
[[nodiscard]] Vec3 ray_of(u32 texel) noexcept {
    const u32 column = texel % kWidth;
    const u32 row = texel / kWidth;
    const f32 ndc_x =
        (((static_cast<f32>(column) + 0.5F) / static_cast<f32>(kWidth)) * 2.0F) - 1.0F;
    const f32 ndc_y = 1.0F - (((static_cast<f32>(row) + 0.5F) / static_cast<f32>(kHeight)) * 2.0F);
    return cy::normalize(Vec3{ndc_x * kTanHalfX, ndc_y * kTanHalfY, -1.0F});
}

[[nodiscard]] f32 elevation_degrees(Vec3 direction) noexcept {
    return std::asin(cy::math::clamp(direction.y, -1.0F, 1.0F)) * cy::math::kRadToDeg;
}

// --- The atmosphere ------------------------------------------------------------------------------

/// One atmosphere and one sun, integrated the way `Stage::update_aerial_perspective` integrates
/// them: the tables, the clear sky the dome is coloured from, and the aerial perspective volume.
struct Air {
    sky::Atmosphere atmosphere = sky::earth_atmosphere();
    Vec3 sun{0.0F, 1.0F, 0.0F};
    sky::AtmosphereTables tables;
    sky::IncrementalSkyView sky_view;
    sky::AerialPerspectiveTable aerial;

    Air() noexcept : tables(allocator()), sky_view(allocator()), aerial(allocator()) {}

    [[nodiscard]] cy::Status build(const sky::Atmosphere& chosen, Vec3 sun_direction) noexcept {
        atmosphere = chosen;
        sun = cy::normalize(sun_direction);
        if (cy::Status configured = tables.configure(sky::SkyTableQuality::Medium); !configured) {
            return configured;
        }
        if (auto built = tables.build(atmosphere); !built) {
            return cy::make_unexpected(built.error());
        }
        const Vec3 eye = sky::ground_position(atmosphere, kEyeHeight);
        if (cy::Status configured = sky_view.configure(sky::SkyTableQuality::Medium); !configured) {
            return configured;
        }
        if (auto updated = sky_view.update_aerial(atmosphere, tables, eye, sun, 0); !updated) {
            return cy::make_unexpected(updated.error());
        }
        cy::rendering::FroxelVolume volume;
        volume.width = 32;
        volume.height = 18;
        volume.depth = 64;
        volume.near_plane = kNear;
        volume.far_plane = kAerialFar;
        volume.depth_exponent = 2.0F;
        if (cy::Status configured = aerial.configure(volume); !configured) {
            return configured;
        }
        return aerial.update(atmosphere, tables, eye, camera_view(), sun);
    }
};

/// A sun thirty degrees up and behind the camera's right shoulder, so it lights the walls' faces.
[[nodiscard]] Vec3 high_sun() noexcept {
    return Vec3{0.35F, 0.5F, 0.79F};
}

/// The same azimuth, six degrees up: a late afternoon.
[[nodiscard]] Vec3 low_sun() noexcept {
    return Vec3{0.4F, 0.105F, 0.91F};
}

/// Earth with eight times the aerosol: a hazy, turbid day, from the scattering parameters.
[[nodiscard]] sky::Atmosphere turbid() noexcept {
    sky::Atmosphere hazy = sky::earth_atmosphere();
    hazy.mie_scattering *= 8.0F;
    hazy.mie_extinction *= 8.0F;
    return hazy;
}

// --- The world pipeline's own blocks -------------------------------------------------------------

/// `WorldPush` in samples/10-world/shaders/world.slang, as `stage.cpp` writes it.
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

static_assert(sizeof(WorldPush) == 128, "the world pipeline's push block is 128 bytes");

struct Vertex {
    f32 position[3];
    f32 normal[3];
};

/// Room for the dome and a few walls.
constexpr u32 kMaxVertices = 40'000;
/// The largest table this suite binds: 32 x 18 x 64 froxels, two words each, and the header.
constexpr u64 kAerialWords = sky::kAerialPerspectiveHeaderWords + (u64{32} * 18U * 64U * 2U);

/// One picture, as the half-float bits the target holds: four per texel.
struct Picture {
    cy::Array<u16> bits;
    explicit Picture(cy::Allocator& memory) noexcept : bits(memory) {}
};

[[nodiscard]] f32 half_to_float(u16 bits) noexcept {
    const u32 sign = (static_cast<u32>(bits) & 0x8000U) << 16U;
    const u32 exponent = (static_cast<u32>(bits) >> 10U) & 0x1FU;
    const u32 mantissa = static_cast<u32>(bits) & 0x3FFU;
    u32 out = 0;
    if (exponent == 0) {
        const f32 value = std::ldexp(static_cast<f32>(mantissa), -24);
        std::memcpy(&out, &value, sizeof(out));
        out |= sign;
    } else if (exponent == 31) {
        out = sign | 0x7F800000U | (mantissa << 13U);
    } else {
        out = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
    }
    f32 value = 0.0F;
    std::memcpy(&value, &out, sizeof(value));
    return value;
}

[[nodiscard]] Vec3 texel_of(const Picture& picture, u32 texel) noexcept {
    const usize base = static_cast<usize>(texel) * 4;
    return Vec3{half_to_float(picture.bits[base]), half_to_float(picture.bits[base + 1]),
                half_to_float(picture.bits[base + 2])};
}

/// `tonemap()` in world.slang: Reinhard, then the display gamma. The fragment stage applies it
/// itself, so the target holds display values and every expectation has to be one too.
[[nodiscard]] f32 world_tonemap(f32 radiance) noexcept {
    const f32 mapped = radiance / (1.0F + radiance);
    return std::pow(cy::math::saturate(mapped), 1.0F / 2.2F);
}

[[nodiscard]] Vec3 world_tonemap(Vec3 radiance) noexcept {
    return Vec3{world_tonemap(radiance.x), world_tonemap(radiance.y), world_tonemap(radiance.z)};
}

[[nodiscard]] f32 largest_channel_gap(Vec3 a, Vec3 b) noexcept {
    return cy::math::max(std::fabs(a.x - b.x),
                         cy::math::max(std::fabs(a.y - b.y), std::fabs(a.z - b.z)));
}

// --- The scene -----------------------------------------------------------------------------------

/// A flat wall facing the camera `distance` metres down -Z, spanning the whole width of the view
/// and the elevations between `bottom_degrees` and `top_degrees` as seen from the eye.
struct Wall {
    f32 distance = 1000.0F;
    f32 bottom_degrees = -40.0F;
    f32 top_degrees = 40.0F;
};

/// The geometry one shot draws, in world-relative metres with the eye at (0, kEyeHeight, 0): the
/// dome first, emissive, and the walls after it, lit.
struct Scene {
    cy::Array<Vertex> vertices;
    cy::Array<Vec3> colours;
    u32 dome_vertices = 0;
    u32 wall_vertices = 0;
    Wall walls[2];
    u32 wall_count = 0;

    Scene() noexcept : vertices(allocator()), colours(allocator()) {}

    [[nodiscard]] cy::Status add(Vec3 position, Vec3 normal, Vec3 colour) noexcept {
        if (cy::Status pushed = vertices.push_back(
                Vertex{{position.x, position.y, position.z}, {normal.x, normal.y, normal.z}});
            !pushed) {
            return pushed;
        }
        return colours.push_back(colour);
    }

    /// The dome, coloured as `Stage::build_dynamic` colours it: the atmosphere's clear sky in each
    /// vertex's direction, times the frame's radiance scale. Rings follow the sky view's own
    /// latitude axis, so they crowd the horizon where the sky's gradient is.
    [[nodiscard]] cy::Status build_dome(const Air& air, f32 scale) noexcept {
        constexpr u32 kRings = 120;
        constexpr u32 kSegments = 48;
        const auto direction = [](u32 ring, u32 segment) {
            const f32 v = -0.62F + (1.6F * static_cast<f32>(ring) / static_cast<f32>(kRings));
            const f32 elevation = (v < 0.0F ? -1.0F : 1.0F) * v * v * (cy::math::kPi * 0.5F);
            const f32 azimuth =
                (-1.25F + (2.5F * static_cast<f32>(segment) / static_cast<f32>(kSegments)));
            return Vec3{std::cos(elevation) * std::sin(azimuth), std::sin(elevation),
                        -std::cos(elevation) * std::cos(azimuth)};
        };
        const Vec3 eye{0.0F, kEyeHeight, 0.0F};
        for (u32 ring = 0; ring < kRings; ++ring) {
            for (u32 segment = 0; segment < kSegments; ++segment) {
                const Vec3 corners[4] = {direction(ring, segment), direction(ring, segment + 1),
                                         direction(ring + 1, segment),
                                         direction(ring + 1, segment + 1)};
                for (const u32 corner : {0U, 2U, 1U, 1U, 2U, 3U}) {
                    const Vec3 at = corners[corner];
                    if (cy::Status added =
                            add(eye + (at * kDomeRadius), at, air.sky_view.sample(at) * scale);
                        !added) {
                        return added;
                    }
                }
            }
        }
        dome_vertices = static_cast<u32>(vertices.size());
        return cy::ok();
    }

    [[nodiscard]] cy::Status add_wall(const Wall& wall) noexcept {
        if (wall_count >= std::size(walls)) {
            return cy::fail(cy::ErrorCode::InvalidArgument, "a scene holds two walls");
        }
        const f32 half_width = wall.distance * kTanHalfX * 1.3F;
        const f32 bottom =
            kEyeHeight + (wall.distance * std::tan(wall.bottom_degrees * cy::math::kDegToRad));
        const f32 top =
            kEyeHeight + (wall.distance * std::tan(wall.top_degrees * cy::math::kDegToRad));
        const Vec3 corners[4] = {
            Vec3{-half_width, bottom, -wall.distance}, Vec3{half_width, bottom, -wall.distance},
            Vec3{-half_width, top, -wall.distance}, Vec3{half_width, top, -wall.distance}};
        for (const u32 corner : {0U, 1U, 2U, 2U, 1U, 3U}) {
            if (cy::Status added =
                    add(corners[corner], Vec3{0.0F, 0.0F, 1.0F}, Vec3{kAlbedo, kAlbedo, kAlbedo});
                !added) {
                return added;
            }
        }
        wall_vertices += 6;
        walls[wall_count++] = wall;
        return cy::ok();
    }

    /// The wall a texel's ray meets first, if any, and where on it.
    [[nodiscard]] bool hit(u32 texel, Vec3& offset) const noexcept {
        const Vec3 ray = ray_of(texel);
        for (u32 index = 0; index < wall_count; ++index) {
            const Wall& wall = walls[index];
            const Vec3 at = ray * (wall.distance / -ray.z);
            const f32 elevation = std::atan2(at.y, wall.distance) * cy::math::kRadToDeg;
            if (elevation >= wall.bottom_degrees && elevation <= wall.top_degrees) {
                offset = at;
                return true;
            }
        }
        return false;
    }
};

/// The radiance the lit path computes for a wall, before the air: albedo times the ambient term
/// plus the sun's Lambert term, against a face whose normal is +Z.
[[nodiscard]] f32 wall_lit(Vec3 sun) noexcept {
    return kAlbedo * (kAmbient + (kSun * cy::math::saturate(sun.z)));
}

// --- The fixture ---------------------------------------------------------------------------------

struct Shot {
    /// The words bound at binding 2; null binds the switched-off header.
    const cy::Array<Vec4>* aerial = nullptr;
    /// Draw with the world's shaders as they were before aerial perspective
    /// (`world_before_aerial_perspective_spirv.h`), which read no table at all.
    bool before = false;
    Vec3 sun = high_sun();
};

class WorldPass {
public:
    explicit WorldPass(cy::render_test::DeviceFixture& fixture) noexcept
        : fixture_(fixture), device_(fixture.device()), off_(allocator()) {}

    WorldPass(const WorldPass&) = delete;
    WorldPass& operator=(const WorldPass&) = delete;

    ~WorldPass() {
        (void)device_.wait_idle();
        for (rhi::GraphicsPipelineHandle pipeline : {pipeline_, before_pipeline_}) {
            if (!pipeline.is_null()) {
                device_.destroy_graphics_pipeline(pipeline);
            }
        }
        if (!layout_.is_null()) {
            device_.destroy_pipeline_layout(layout_);
        }
        if (!set_layout_.is_null()) {
            device_.destroy_descriptor_set_layout(set_layout_);
        }
        for (rhi::ShaderModuleHandle module :
             {vertex_, fragment_, before_vertex_, before_fragment_}) {
            if (!module.is_null()) {
                device_.destroy_shader_module(module);
            }
        }
        for (rhi::BufferHandle buffer :
             {vertices_, colours_, field_, placement_, aerial_, readback_, decals_off_}) {
            if (!buffer.is_null()) {
                device_.destroy_buffer(buffer);
            }
        }
    }

    [[nodiscard]] cy::Status prepare() noexcept;
    [[nodiscard]] cy::Status shoot(const Scene& scene, const Shot& shot, Picture& out) noexcept;

    struct DrawState {
        cy::rendering::GraphExecutor* executor = nullptr;
        rhi::GraphicsPipelineHandle pipeline;
        rhi::PipelineLayoutHandle layout;
        rhi::DescriptorSetHandle set;
        rhi::BufferHandle vertices;
        rhi::BufferHandle colours;
        cy::rendering::ResourceId color = cy::rendering::kInvalidResource;
        cy::rendering::ResourceId depth = cy::rendering::kInvalidResource;
        rhi::BufferHandle readback;
        u32 dome_vertices = 0;
        u32 wall_vertices = 0;
        WorldPush push;
    };

private:
    [[nodiscard]] cy::Expected<rhi::BufferHandle, cy::Error> buffer(const char* name, u64 size,
                                                                    rhi::BufferUsage usage,
                                                                    rhi::MemoryUse memory) noexcept;
    [[nodiscard]] cy::Expected<rhi::ShaderModuleHandle, cy::Error> module(
        const char* name, rhi::ShaderStage stage, cy::Span<const u32> spirv) noexcept;
    [[nodiscard]] cy::Expected<rhi::GraphicsPipelineHandle, cy::Error> pipeline(
        const char* name, rhi::ShaderModuleHandle vertex,
        rhi::ShaderModuleHandle fragment) noexcept;

    cy::render_test::DeviceFixture& fixture_;
    rhi::Device& device_;
    rhi::ShaderModuleHandle vertex_;
    rhi::ShaderModuleHandle fragment_;
    rhi::ShaderModuleHandle before_vertex_;
    rhi::ShaderModuleHandle before_fragment_;
    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::DescriptorSetHandle set_;
    rhi::PipelineLayoutHandle layout_;
    rhi::GraphicsPipelineHandle pipeline_;
    rhi::GraphicsPipelineHandle before_pipeline_;
    rhi::BufferHandle vertices_;
    rhi::BufferHandle colours_;
    rhi::BufferHandle field_;
    rhi::BufferHandle placement_;
    rhi::BufferHandle aerial_;
    rhi::BufferHandle readback_;
    rhi::BufferHandle decals_off_;
    /// An unbuilt table's words: the header alone, switched off.
    cy::Array<Vec4> off_;
};

void record_draw(const cy::rendering::PassContext& context, void* user) noexcept {
    auto* state = static_cast<WorldPass::DrawState*>(user);
    rhi::RenderAttachment color;
    color.view = state->executor->view(state->color);
    color.load = rhi::LoadOp::Clear;
    color.store = rhi::StoreOp::Store;
    color.clear.color[3] = 1.0F;
    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, kWidth, kHeight};
    info.color_attachments = cy::Span<const rhi::RenderAttachment>(&color, 1);
    info.depth_attachment.view = state->executor->view(state->depth);
    info.depth_attachment.load = rhi::LoadOp::Clear;
    info.depth_attachment.store = rhi::StoreOp::Store;
    info.depth_attachment.clear = rhi::reversed_z_depth_clear();

    context.commands->begin_rendering(info);
    context.commands->set_viewport(
        rhi::Viewport{0.0F, 0.0F, static_cast<f32>(kWidth), static_cast<f32>(kHeight), 0.0F, 1.0F});
    context.commands->set_scissor(rhi::Rect2D{0, 0, kWidth, kHeight});
    context.commands->bind_graphics_pipeline(state->pipeline);
    context.commands->bind_descriptor_sets(
        state->layout, 0, cy::Span<const rhi::DescriptorSetHandle>(&state->set, 1));
    const rhi::BufferHandle streams[2] = {state->vertices, state->colours};
    const u64 offsets[2] = {0, 0};
    context.commands->bind_vertex_buffers(0, cy::Span<const rhi::BufferHandle>(streams, 2),
                                          cy::Span<const u64>(offsets, 2));
    // The dome through the emissive path, as the sample draws its sky; the walls lit.
    WorldPush push = state->push;
    push.eye[3] = 1.0F;
    context.commands->push_constants(
        state->layout, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
        cy::Span<const u8>(reinterpret_cast<const u8*>(&push), sizeof(WorldPush)));
    context.commands->draw(state->dome_vertices, 1, 0, 0);
    if (state->wall_vertices > 0) {
        push.eye[3] = 0.0F;
        context.commands->push_constants(
            state->layout, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
            cy::Span<const u8>(reinterpret_cast<const u8*>(&push), sizeof(WorldPush)));
        context.commands->draw(state->wall_vertices, 1, state->dome_vertices, 0);
    }
    context.commands->end_rendering();
}

void record_readback(const cy::rendering::PassContext& context, void* user) noexcept {
    auto* state = static_cast<WorldPass::DrawState*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{kWidth, kHeight, 1};
    context.commands->copy_texture_to_buffer(state->executor->texture(state->color),
                                             state->readback,
                                             cy::Span<const rhi::BufferTextureCopy>(&region, 1));
}

cy::Expected<rhi::BufferHandle, cy::Error> WorldPass::buffer(const char* name, u64 size,
                                                             rhi::BufferUsage usage,
                                                             rhi::MemoryUse memory) noexcept {
    rhi::BufferDescription description;
    description.name = name;
    description.size = size;
    description.usage = usage;
    description.memory = memory;
    return device_.create_buffer(description);
}

cy::Expected<rhi::ShaderModuleHandle, cy::Error> WorldPass::module(
    const char* name, rhi::ShaderStage stage, cy::Span<const u32> spirv) noexcept {
    rhi::ShaderModuleDescription description;
    description.name = name;
    description.stage = stage;
    description.entry_point = "main";
    description.spirv = spirv;
    return device_.create_shader_module(description);
}

cy::Expected<rhi::GraphicsPipelineHandle, cy::Error> WorldPass::pipeline(
    const char* name, rhi::ShaderModuleHandle vertex, rhi::ShaderModuleHandle fragment) noexcept {
    const rhi::VertexBinding vertex_bindings[2] = {
        {0, sizeof(Vertex), rhi::VertexInputRate::PerVertex},
        {1, sizeof(f32) * 3, rhi::VertexInputRate::PerVertex}};
    const rhi::VertexAttribute attributes[3] = {{0, 0, rhi::Format::Rgb32Sfloat, 0},
                                                {1, 0, rhi::Format::Rgb32Sfloat, sizeof(f32) * 3},
                                                {2, 1, rhi::Format::Rgb32Sfloat, 0}};
    rhi::ColorAttachmentState color;
    color.format = rhi::Format::Rgba16Sfloat;
    rhi::GraphicsPipelineDescription description;
    description.name = name;
    description.layout = layout_;
    description.vertex_shader = vertex;
    description.fragment_shader = fragment;
    description.vertex_bindings = cy::Span<const rhi::VertexBinding>(vertex_bindings, 2);
    description.vertex_attributes = cy::Span<const rhi::VertexAttribute>(attributes, 3);
    description.color_attachments = cy::Span<const rhi::ColorAttachmentState>(&color, 1);
    description.rasterisation.cull_mode = rhi::CullMode::None;
    description.depth_stencil.format = rhi::Format::D32Sfloat;
    description.depth_stencil.depth_test_enable = true;
    description.depth_stencil.depth_write_enable = true;
    return device_.create_graphics_pipeline(description);
}

cy::Status WorldPass::prepare() noexcept {
    namespace now = cy::sample::world;
    namespace before = cy::render_test::world_before_aerial_perspective;
    const struct {
        rhi::ShaderModuleHandle* handle;
        const char* name;
        rhi::ShaderStage stage;
        cy::Span<const u32> spirv;
    } modules[4] = {
        {&vertex_, "world vertex", rhi::ShaderStage::Vertex,
         cy::Span<const u32>(now::kWorldVertexSpirv, std::size(now::kWorldVertexSpirv))},
        {&fragment_, "world fragment", rhi::ShaderStage::Fragment,
         cy::Span<const u32>(now::kWorldFragmentSpirv, std::size(now::kWorldFragmentSpirv))},
        {&before_vertex_, "world vertex before aerial perspective", rhi::ShaderStage::Vertex,
         cy::Span<const u32>(before::kWorldVertexSpirv, std::size(before::kWorldVertexSpirv))},
        {&before_fragment_, "world fragment before aerial perspective", rhi::ShaderStage::Fragment,
         cy::Span<const u32>(before::kWorldFragmentSpirv, std::size(before::kWorldFragmentSpirv))},
    };
    for (const auto& entry : modules) {
        auto created = module(entry.name, entry.stage, entry.spirv);
        if (!created) {
            return cy::make_unexpected(created.error());
        }
        *entry.handle = *created;
    }

    // THE SAMPLE'S LAYOUT, restated: set 0 with the cloud shadow field, its placement and the
    // aerial perspective table for the fragment stage, and the 128-byte push block for both.
    rhi::DescriptorBinding bindings[4] = {};
    for (u32 index = 0; index < 4; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = rhi::DescriptorKind::StorageBuffer;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Fragment;
    }
    rhi::DescriptorSetLayoutDescription set_description;
    set_description.name = "world set";
    set_description.bindings = cy::Span<const rhi::DescriptorBinding>(bindings, 4);
    auto set_layout = device_.create_descriptor_set_layout(set_description);
    if (!set_layout) {
        return cy::make_unexpected(set_layout.error());
    }
    set_layout_ = *set_layout;
    const rhi::PushConstantRange range{rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
                                       sizeof(WorldPush)};
    rhi::PipelineLayoutDescription layout;
    layout.name = "world layout";
    layout.set_layouts = cy::Span<const rhi::DescriptorSetLayoutHandle>(&set_layout_, 1);
    layout.push_constants = cy::Span<const rhi::PushConstantRange>(&range, 1);
    auto layout_handle = device_.create_pipeline_layout(layout);
    if (!layout_handle) {
        return cy::make_unexpected(layout_handle.error());
    }
    layout_ = *layout_handle;

    // The pipeline from before shares the layout: it never reads binding 2, and a layout may
    // declare a binding its shaders never touch.
    auto created = pipeline("world", vertex_, fragment_);
    if (!created) {
        return cy::make_unexpected(created.error());
    }
    pipeline_ = *created;
    auto created_before =
        pipeline("world before aerial perspective", before_vertex_, before_fragment_);
    if (!created_before) {
        return cy::make_unexpected(created_before.error());
    }
    before_pipeline_ = *created_before;

    auto vertices = buffer("world vertices", sizeof(Vertex) * kMaxVertices,
                           rhi::BufferUsage::Vertex, rhi::MemoryUse::Upload);
    auto colours = buffer("world colours", sizeof(f32) * 3 * kMaxVertices, rhi::BufferUsage::Vertex,
                          rhi::MemoryUse::Upload);
    auto field = buffer("cloud shadow placeholder", 16 * sizeof(u32), rhi::BufferUsage::Storage,
                        rhi::MemoryUse::Upload);
    auto placement = buffer("cloud shadow placement", 4 * sizeof(f32), rhi::BufferUsage::Storage,
                            rhi::MemoryUse::Upload);
    auto aerial = buffer("aerial perspective", kAerialWords * sizeof(Vec4),
                         rhi::BufferUsage::Storage, rhi::MemoryUse::Upload);
    auto readback = buffer("world readback", static_cast<u64>(kTexels) * 4 * sizeof(u16),
                           rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback);
    // Binding 3: the decal table, empty — zeroes are no table, and the lit path leaves every
    // surface as it was. The sample binds the same until a frame with its ground marker writes one.
    auto decals_off = buffer("decal table off", sizeof(u32) * 24, rhi::BufferUsage::Storage,
                             rhi::MemoryUse::Upload);
    if (!vertices || !colours || !field || !placement || !aerial || !readback || !decals_off) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "a world pass buffer did not allocate");
    }
    vertices_ = *vertices;
    colours_ = *colours;
    field_ = *field;
    placement_ = *placement;
    aerial_ = *aerial;
    readback_ = *readback;
    decals_off_ = *decals_off;
    std::memset(device_.buffer_mapped_pointer(decals_off_), 0, sizeof(u32) * 24);
    // Cloud shadows off: the placement's `enabled` word is zero and the field is never read.
    std::memset(device_.buffer_mapped_pointer(field_), 0, 16 * sizeof(u32));
    std::memset(device_.buffer_mapped_pointer(placement_), 0, 4 * sizeof(f32));

    const sky::AerialPerspectiveTable unbuilt(allocator());
    if (cy::Status packed = sky::pack_aerial_perspective(unbuilt, 1.0F, off_); !packed) {
        return packed;
    }

    auto set = device_.allocate_descriptor_set(set_layout_, false);
    if (!set) {
        return cy::make_unexpected(set.error());
    }
    set_ = *set;
    rhi::DescriptorWrite writes[4] = {};
    const rhi::BufferHandle bound[4] = {field_, placement_, aerial_, decals_off_};
    for (u32 index = 0; index < 4; ++index) {
        writes[index].binding = index;
        writes[index].kind = rhi::DescriptorKind::StorageBuffer;
        writes[index].buffer = bound[index];
    }
    return device_.update_descriptor_set(set_, cy::Span<const rhi::DescriptorWrite>(writes, 4));
}

cy::Status WorldPass::shoot(const Scene& scene, const Shot& shot, Picture& out) noexcept {
    if (scene.vertices.size() > kMaxVertices) {
        return cy::fail(cy::ErrorCode::InvalidArgument, "the scene outgrew its vertex buffer");
    }
    std::memcpy(device_.buffer_mapped_pointer(vertices_), scene.vertices.data(),
                scene.vertices.size() * sizeof(Vertex));
    std::memcpy(device_.buffer_mapped_pointer(colours_), scene.colours.data(),
                scene.colours.size() * sizeof(Vec3));
    const cy::Array<Vec4>& words = shot.aerial != nullptr ? *shot.aerial : off_;
    if (words.size() > kAerialWords) {
        return cy::fail(cy::ErrorCode::InvalidArgument, "the table outgrew its buffer");
    }
    std::memcpy(device_.buffer_mapped_pointer(aerial_), words.data(), words.size() * sizeof(Vec4));

    // --- The push block: an infinite reversed-Z perspective from (0, kEyeHeight, 0) along -Z.
    DrawState state;
    state.pipeline = shot.before ? before_pipeline_ : pipeline_;
    state.layout = layout_;
    state.set = set_;
    state.vertices = vertices_;
    state.colours = colours_;
    state.readback = readback_;
    state.dome_vertices = scene.dome_vertices;
    state.wall_vertices = scene.wall_vertices;
    state.push.row0[0] = 1.0F / kTanHalfX;
    state.push.row1[1] = 1.0F / kTanHalfY;
    state.push.row1[3] = -kEyeHeight / kTanHalfY;
    state.push.row2[2] = 0.0F;
    state.push.row2[3] = kNear;
    state.push.row3[2] = -1.0F;
    state.push.row3[3] = 0.0F;
    const Vec3 sun = cy::normalize(shot.sun);
    state.push.light[0] = -sun.x;
    state.push.light[1] = -sun.y;
    state.push.light[2] = -sun.z;
    state.push.eye[1] = kEyeHeight;
    state.push.sun[0] = kSun;
    state.push.sun[1] = kSun;
    state.push.sun[2] = kSun;
    state.push.ambient[0] = kAmbient;
    state.push.ambient[1] = kAmbient;
    state.push.ambient[2] = kAmbient;

    if (cy::Expected<u32, cy::Error> began = device_.begin_frame(); !began) {
        return cy::make_unexpected(began.error());
    }
    cy::rendering::RenderGraph graph(fixture_.allocator());
    cy::rendering::GraphExecutor executor(fixture_.allocator(), device_);
    state.executor = &executor;

    cy::rendering::TextureRequest color_request;
    color_request.name = "world colour";
    color_request.format = rhi::Format::Rgba16Sfloat;
    color_request.width = kWidth;
    color_request.height = kHeight;
    state.color = graph.create_texture(color_request);
    cy::rendering::TextureRequest depth_request;
    depth_request.name = "world depth";
    depth_request.format = rhi::Format::D32Sfloat;
    depth_request.width = kWidth;
    depth_request.height = kHeight;
    state.depth = graph.create_texture(depth_request);
    cy::rendering::BufferRequest readback_request;
    readback_request.name = "world readback";
    readback_request.size = static_cast<u64>(kTexels) * 4 * sizeof(u16);
    readback_request.extra_usage = rhi::BufferUsage::TransferDestination;
    const cy::rendering::ResourceId readback = graph.import_buffer(readback_request, readback_);

    graph.add_pass("world draw", rhi::QueueKind::Graphics)
        .write(state.color, rhi::Access::ColorAttachmentWrite)
        .write(state.depth, rhi::Access::DepthStencilAttachmentWrite)
        .record(&record_draw, &state);
    graph.add_pass("world readback", rhi::QueueKind::Graphics)
        .read(state.color, rhi::Access::TransferRead)
        .write(readback, rhi::Access::TransferWrite)
        .record(&record_readback, &state);
    graph.add_pass("world host", rhi::QueueKind::Graphics)
        .read(readback, rhi::Access::HostRead)
        .side_effect();
    if (cy::Status declared = graph.status(); !declared) {
        return declared;
    }
    auto result =
        executor.execute(graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{});
    if (!result) {
        return cy::make_unexpected(result.error());
    }
    if (cy::Status idle = device_.wait_idle(); !idle) {
        return idle;
    }
    if (cy::Status ended = device_.end_frame(); !ended) {
        return ended;
    }
    const auto* texels = static_cast<const u16*>(device_.buffer_mapped_pointer(readback_));
    if (texels == nullptr) {
        return cy::fail(cy::ErrorCode::Internal, "the readback is not mapped");
    }
    if (cy::Status sized = out.bits.resize(static_cast<usize>(kTexels) * 4); !sized) {
        return sized;
    }
    std::memcpy(out.bits.data(), texels, static_cast<usize>(kTexels) * 4 * sizeof(u16));
    executor.release();
    return cy::ok();
}

/// Texels whose four half-float channels differ between two pictures.
[[nodiscard]] u32 differing(const Picture& a, const Picture& b) noexcept {
    u32 count = 0;
    for (u32 texel = 0; texel < kTexels; ++texel) {
        const usize base = static_cast<usize>(texel) * 4;
        count += std::memcmp(&a.bits[base], &b.bits[base], 4 * sizeof(u16)) != 0 ? 1U : 0U;
    }
    return count;
}

/// The frame's radiance scale, chosen once from the reference atmosphere and then held, so that
/// a changed atmosphere shows as a changed picture rather than being exposed away: the sky one
/// degree above the horizon lands at half of the tone map's input range.
[[nodiscard]] f32 reference_scale(const Air& reference) noexcept {
    const Vec3 horizon = reference.sky_view.sample(
        Vec3{0.0F, std::sin(1.0F * cy::math::kDegToRad), -std::cos(1.0F * cy::math::kDegToRad)});
    return 0.5F / cy::math::max(horizon.y, 1.0e-6F);
}

/// What the device must have drawn for one wall texel: the lit radiance through the table's air.
[[nodiscard]] Vec3 expected_wall(const Air& air, Vec3 offset, f32 scale) noexcept {
    const sky::AerialPerspective through = air.aerial.sample_at(offset);
    const f32 lit = wall_lit(air.sun);
    return world_tonemap(cy::cwise_mul(Vec3{lit, lit, lit}, through.transmittance) +
                         (through.in_scattering * scale));
}

/// The texels of the columns in the middle half of the picture, where the dome's azimuth bands
/// are far from its edges.
[[nodiscard]] bool central_column(u32 texel) noexcept {
    const u32 column = texel % kWidth;
    return column >= kWidth / 4 && column < (kWidth * 3) / 4;
}

/// The seam between a wall whose top edge is the horizon and the dome above it: per column, the
/// wall's topmost texel against the sky texel directly above it.
struct Seam {
    Vec3 mean_wall{0.0F, 0.0F, 0.0F};
    Vec3 mean_sky{0.0F, 0.0F, 0.0F};
    f32 worst_gap = 0.0F;
    u32 columns = 0;
};

[[nodiscard]] Seam measure_seam(const Scene& scene, const Picture& picture) noexcept {
    Seam seam;
    for (u32 column = kWidth / 4; column < (kWidth * 3) / 4; ++column) {
        for (u32 row = 1; row < kHeight; ++row) {
            Vec3 offset;
            const u32 texel = (row * kWidth) + column;
            if (!scene.hit(texel, offset)) {
                continue;
            }
            const Vec3 wall = texel_of(picture, texel);
            const Vec3 above = texel_of(picture, texel - kWidth);
            seam.mean_wall = seam.mean_wall + wall;
            seam.mean_sky = seam.mean_sky + above;
            seam.worst_gap = cy::math::max(seam.worst_gap, largest_channel_gap(wall, above));
            ++seam.columns;
            break;
        }
    }
    if (seam.columns > 0) {
        seam.mean_wall = seam.mean_wall / static_cast<f32>(seam.columns);
        seam.mean_sky = seam.mean_sky / static_cast<f32>(seam.columns);
    }
    return seam;
}

struct Suite {
    cy::render_test::DeviceFixture fixture{"vulkan", "cy_test_render_world_aerial_perspective"};
    [[nodiscard]] bool have_vulkan() const noexcept { return fixture.is(rhi::BackendKind::Vulkan); }
};

/// The horizon scene: the dome, and one wall 800 km out whose top edge is the eye's own horizon.
[[nodiscard]] cy::Status horizon_scene(const Air& air, f32 scale, Scene& scene) noexcept {
    if (cy::Status built = scene.build_dome(air, scale); !built) {
        return built;
    }
    return scene.add_wall(Wall{800'000.0F, -30.0F, 0.0F});
}

}  // namespace

CY_TEST_CASE("world aerial perspective: the device applies the atmosphere's table") {
    Suite suite;
    if (!suite.have_vulkan()) {
        suite.fixture.report_skip();
        return;
    }
    Air air;
    CY_REQUIRE(air.build(sky::earth_atmosphere(), high_sun()));
    const f32 scale = reference_scale(air);
    cy::Array<Vec4> words(allocator());
    CY_REQUIRE(sky::pack_aerial_perspective(air.aerial, scale, words));
    WorldPass pass(suite.fixture);
    CY_REQUIRE(pass.prepare());

    // A NEAR WALL, AND WALLS FURTHER AND FURTHER OUT. Each fills the picture, so every texel is a
    // surface at a known point, and the processor's own sampler says what the device must draw.
    f32 worst_error = 0.0F;
    f32 near_change = 0.0F;
    u32 compared = 0;
    f32 previous_gap = 2.0F;
    u32 approaching = 0;
    f32 first_gap = 0.0F;
    f32 last_gap = 0.0F;
    const f32 distances[] = {6.0F, 2'000.0F, 30'000.0F, 300'000.0F, 800'000.0F};
    for (const f32 distance : distances) {
        Scene scene;
        CY_REQUIRE(scene.build_dome(air, scale));
        CY_REQUIRE(scene.add_wall(Wall{distance, -40.0F, 40.0F}));
        Picture on(allocator());
        Picture off(allocator());
        CY_REQUIRE(pass.shoot(scene, Shot{&words, false, air.sun}, on));
        CY_REQUIRE(pass.shoot(scene, Shot{nullptr, false, air.sun}, off));

        // The gap between the wall and the sky drawn in the same direction, over the band of the
        // picture between the horizon and a degree above it — where a path is long enough, at the
        // far end, that the air rather than the wall decides its colour.
        f32 gap = 0.0F;
        u32 band = 0;
        for (u32 texel = 0; texel < kTexels; ++texel) {
            Vec3 offset;
            if (!scene.hit(texel, offset)) {
                continue;
            }
            const Vec3 drawn = texel_of(on, texel);
            const Vec3 expected = expected_wall(air, offset, scale);
            worst_error = cy::math::max(worst_error, largest_channel_gap(drawn, expected));
            ++compared;
            if (distance < 10.0F) {
                const Vec3 plain = texel_of(off, texel);
                near_change = cy::math::max(near_change, largest_channel_gap(drawn, plain));
            }
            const f32 elevation = elevation_degrees(cy::normalize(offset));
            if (central_column(texel) && elevation >= 0.0F && elevation <= 1.0F) {
                const Vec3 sky_here =
                    world_tonemap(air.sky_view.sample(cy::normalize(offset)) * scale);
                gap += largest_channel_gap(drawn, sky_here);
                ++band;
            }
        }
        gap /= static_cast<f32>(band > 0 ? band : 1U);
        CY_TEST_MESSAGE("wall at ", distance, " m: mean gap to the sky along the horizon ", gap,
                        " over ", band, " texels");
        if (distance == distances[0]) {
            first_gap = gap;
        }
        last_gap = gap;
        // Closer, or already there: past a few hundred kilometres the air has decided the colour
        // and the remaining steps differ by the table's own resolution.
        approaching += gap < previous_gap || gap < 0.015F ? 1U : 0U;
        previous_gap = gap;
    }
    CY_TEST_MESSAGE(compared, " wall texels, worst difference from the processor's table ",
                    worst_error, "; a wall six metres out moved by at most ", near_change,
                    " with aerial perspective on; ", suite.fixture.validation_errors(),
                    " validation errors");

    // The device draws what the table says, texel for texel, after the shader's tone map.
    CY_CHECK_GT(compared, 5U * kTexels / 2U);
    CY_CHECK_LT(worst_error, 0.01F);
    // A near surface keeps its own colour.
    CY_CHECK_LT(near_change, 1.0e-3F);
    // A far one takes the sky's: every step out is closer to it, and the last is close.
    CY_CHECK_EQ(approaching, static_cast<u32>(std::size(distances)));
    CY_CHECK_GT(first_gap, 0.1F);
    CY_CHECK_LT(last_gap, 0.03F);
    CY_CHECK_EQ(suite.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("world aerial perspective: at the horizon the distant surface meets the sky") {
    Suite suite;
    if (!suite.have_vulkan()) {
        suite.fixture.report_skip();
        return;
    }
    Air air;
    CY_REQUIRE(air.build(sky::earth_atmosphere(), high_sun()));
    const f32 scale = reference_scale(air);
    cy::Array<Vec4> words(allocator());
    CY_REQUIRE(sky::pack_aerial_perspective(air.aerial, scale, words));
    WorldPass pass(suite.fixture);
    CY_REQUIRE(pass.prepare());

    Scene scene;
    CY_REQUIRE(horizon_scene(air, scale, scene));
    Picture on(allocator());
    Picture off(allocator());
    CY_REQUIRE(pass.shoot(scene, Shot{&words, false, air.sun}, on));
    CY_REQUIRE(pass.shoot(scene, Shot{nullptr, false, air.sun}, off));

    const Seam with = measure_seam(scene, on);
    const Seam without = measure_seam(scene, off);
    CY_TEST_MESSAGE("the seam over ", with.columns, " columns: wall ", with.mean_wall.x, ",",
                    with.mean_wall.y, ",", with.mean_wall.z, " against sky ", with.mean_sky.x, ",",
                    with.mean_sky.y, ",", with.mean_sky.z, ", worst gap ", with.worst_gap,
                    "; without aerial perspective the worst gap is ", without.worst_gap);
    CY_CHECK_EQ(with.columns, kWidth / 2);
    CY_CHECK_LT(with.worst_gap, 0.03F);
    // And the same comparison CAN fail: without the air, the wall is only a lit grey.
    CY_CHECK_GT(without.worst_gap, 0.1F);
    CY_CHECK_EQ(suite.fixture.validation_errors(), 0U);
}

CY_TEST_CASE(
    "world aerial perspective: a changed atmosphere moves the sky and the distance together") {
    Suite suite;
    if (!suite.have_vulkan()) {
        suite.fixture.report_skip();
        return;
    }
    WorldPass pass(suite.fixture);
    CY_REQUIRE(pass.prepare());

    // ONE SCALE FOR ALL THREE, chosen from the clear high-sun day, so a changed atmosphere is a
    // changed picture rather than being exposed back to the same one.
    Air clear;
    CY_REQUIRE(clear.build(sky::earth_atmosphere(), high_sun()));
    const f32 scale = reference_scale(clear);

    struct Case {
        char name[32] = {};
        sky::Atmosphere atmosphere;
        Vec3 sun;
    };
    const Case cases[3] = {{"clear, sun at 30 degrees", sky::earth_atmosphere(), high_sun()},
                           {"eight times the aerosol", turbid(), high_sun()},
                           {"clear, sun at 6 degrees", sky::earth_atmosphere(), low_sun()}};
    Seam seams[3];
    for (u32 index = 0; index < 3; ++index) {
        Air air;
        CY_REQUIRE(air.build(cases[index].atmosphere, cases[index].sun));
        cy::Array<Vec4> words(allocator());
        CY_REQUIRE(sky::pack_aerial_perspective(air.aerial, scale, words));
        Scene scene;
        CY_REQUIRE(horizon_scene(air, scale, scene));
        Picture picture(allocator());
        CY_REQUIRE(pass.shoot(scene, Shot{&words, false, air.sun}, picture));
        seams[index] = measure_seam(scene, picture);
        CY_TEST_MESSAGE(cases[index].name, ": sky ", seams[index].mean_sky.x, ",",
                        seams[index].mean_sky.y, ",", seams[index].mean_sky.z, " wall ",
                        seams[index].mean_wall.x, ",", seams[index].mean_wall.y, ",",
                        seams[index].mean_wall.z, ", worst gap ", seams[index].worst_gap);
    }

    // EACH CHANGE MOVES THE SKY VISIBLY, MOVES THE DISTANT WALL BY THE SAME AMOUNT IN THE SAME
    // DIRECTION, AND LEAVES THE TWO MEETING. A tuned fog would hold the wall still while the sky
    // moved; a fog tinted from the sky but attenuated on its own curve would move it by a
    // different amount.
    for (u32 index = 1; index < 3; ++index) {
        const Vec3 sky_moved = seams[index].mean_sky - seams[0].mean_sky;
        const Vec3 wall_moved = seams[index].mean_wall - seams[0].mean_wall;
        const f32 sky_size = largest_channel_gap(seams[index].mean_sky, seams[0].mean_sky);
        const f32 disagreement = largest_channel_gap(sky_moved, wall_moved);
        CY_TEST_MESSAGE(cases[index].name, ": the sky moved by up to ", sky_size,
                        ", the wall's move differs from it by at most ", disagreement);
        CY_CHECK_GT(sky_size, 0.05F);
        CY_CHECK_LT(disagreement, 0.03F);
        CY_CHECK_LT(seams[index].worst_gap, 0.03F);
    }
    CY_CHECK_EQ(suite.fixture.validation_errors(), 0U);
}

CY_TEST_CASE(
    "world aerial perspective: off, the frame is the frame before, and the sky is untouched") {
    Suite suite;
    if (!suite.have_vulkan()) {
        suite.fixture.report_skip();
        return;
    }
    Air air;
    CY_REQUIRE(air.build(sky::earth_atmosphere(), high_sun()));
    const f32 scale = reference_scale(air);
    cy::Array<Vec4> words(allocator());
    CY_REQUIRE(sky::pack_aerial_perspective(air.aerial, scale, words));
    WorldPass pass(suite.fixture);
    CY_REQUIRE(pass.prepare());

    // A near wall in the lower half and a far one across the horizon, so the picture holds sky,
    // near ground and distant ground at once.
    Scene scene;
    CY_REQUIRE(scene.build_dome(air, scale));
    CY_REQUIRE(scene.add_wall(Wall{40.0F, -40.0F, -12.0F}));
    CY_REQUIRE(scene.add_wall(Wall{60'000.0F, -30.0F, 0.5F}));

    Picture before(allocator());
    Picture off(allocator());
    Picture on(allocator());
    CY_REQUIRE(pass.shoot(scene, Shot{nullptr, true, air.sun}, before));
    CY_REQUIRE(pass.shoot(scene, Shot{nullptr, false, air.sun}, off));
    CY_REQUIRE(pass.shoot(scene, Shot{&words, false, air.sun}, on));

    u32 sky_texels = 0;
    u32 sky_changed = 0;
    u32 surface_changed = 0;
    for (u32 texel = 0; texel < kTexels; ++texel) {
        Vec3 offset;
        const usize base = static_cast<usize>(texel) * 4;
        const bool changed = std::memcmp(&on.bits[base], &off.bits[base], 4 * sizeof(u16)) != 0;
        if (scene.hit(texel, offset)) {
            surface_changed += changed ? 1U : 0U;
        } else {
            ++sky_texels;
            sky_changed += changed ? 1U : 0U;
        }
    }
    const u32 off_changed = differing(before, off);
    CY_TEST_MESSAGE("texels differing from the frame before aerial perspective with it off: ",
                    off_changed, "; with it on, ", sky_changed, " of ", sky_texels,
                    " sky texels and ", surface_changed, " surface texels changed");
    CY_CHECK_EQ(off_changed, 0U);
    CY_CHECK_GT(sky_texels, 1000U);
    CY_CHECK_EQ(sky_changed, 0U);
    // And the same comparison CAN fail: the distant wall is hazed.
    CY_CHECK_GT(surface_changed, 2000U);
    CY_CHECK_EQ(suite.fixture.validation_errors(), 0U);
}
