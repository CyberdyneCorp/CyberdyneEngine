// SPDX-License-Identifier: MIT
// AERIAL PERSPECTIVE ON THE SHADED SEA, DRAWN BY THE WORLD'S OWN SHADERS ON A DEVICE.
//
// `atmosphere-sky-and-clouds` — "Aerial perspective": "Distance attenuation SHALL be produced by
// the atmosphere model — scattering and transmittance over distance — and applied to opaque
// shading", and a surface "SHALL be attenuated by the transmittance between it and the eye and
// SHALL receive the light the air scatters into that path". `render.world_aerial_perspective`
// answers it for the land, drawn by `samples/10-world/shaders/world.slang`. This suite answers it
// for the sea the same frame draws with `shaders/water.slang` since water shading, whose two input
// pictures are themselves drawn through world.slang's lit path and so already carry air.
//
// The scene is a level camera over a flat sea: water to the left of the view's centre line, flat
// land at the same level to the right, a shallow bed under the water, and a dome coloured from the
// atmosphere's clear sky, all drawn with THAT frame's committed SPIR-V (`world_spirv.h`,
// `water_spirv.h`) and THAT frame's water device half (`samples/10-world/water_surface.cpp`), in the
// three passes `Stage::declare_water` declares. The table is `sky::AerialPerspectiveTable` for the
// same camera, packed by `sky::pack_aerial_perspective()`, bound where the stage binds it.
//
// ================================================================================================
// WHAT THE WATER MUST DO, AND HOW THE PIXELS SHOW IT
// ================================================================================================
//
// With T and S the table's transmittance and in-scattering at a water pixel's surface point and F
// its Fresnel weight, water.slang's header derives what the eye receives:
//
//   F reflected + T ((1 - F) refracted + glitter) + (1 - F) S
//
// Every term but T and S is the frame with aerial perspective off, so each claim is measured from
// pairs of frames that differ only in the table:
//
//   * DARK, the sun and the sky's ambient light zero: nothing is lit, so an off frame's water pixel
//     is the mirrored dome alone, F reflected, and its land pixel is black. On, the land pixel is S
//     and the water pixel is off + (1 - F) S. So S at the water is (on - off) / (1 - F), and S at
//     the land beside it at the same distance is the land pixel itself.
//   * LIT: the water pixel's own light, (1 - F) refracted, is the lit off frame minus the dark one,
//     and on it is multiplied by T once — not twice, which is what the bed already hazed by the
//     refraction picture would give, and not with the reflection, which the mirrored picture has
//     already drawn through the air.
//
// FIVE CLAIMS:
//
//   * THE DEVICE APPLIES THE TABLE TO THE WATER ONCE. Every water pixel, dark and lit, is the
//     formula above with T and S from `AerialPerspectiveTable::sample_at()` at its own surface
//     point, within 0.004 after the tone map; every land pixel is `lit T + S`.
//   * AT THE SHORELINE THERE IS NO STEP. The water column beside the land, row by row at equal
//     distance, receives the in-scattering the land column receives within 3%.
//   * FAR WATER TAKES THE LAND'S IN-SCATTERING. Beyond 2 km the same holds, and the in-scattering
//     compared there is several times the nearest row's, so the comparison is of real haze.
//   * NEAR WATER IS LEFT AS IT WAS LIT. From an eye two metres up, every water pixel within ten
//     metres moves by under 0.001 with the table on.
//   * OFF IS THE FRAME BEFORE. With the table's `enabled` word zero, dark and lit, every texel is
//     bit-identical to the frame drawn with water.slang's fragment stage from before this change
//     (`water_before_aerial_perspective_spirv.h`); with it on, the water moves.
//
// ================================================================================================
// THE MUTATIONS THIS SUITE WAS PROVED AGAINST
// ================================================================================================
//
// Each applied to water.slang, the headers regenerated, the suite run and seen red, the file
// restored and md5-verified: `openspec/changes/add-aerial-perspective-on-water/evidence/
// falsification.txt`.

#include <cy/test/test.h>

#include <cy/backends/rhi/access.h>
#include <cy/core/math/math.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/sky/tables.h>
#include <cy/water/body.h>

#include "10-world/shaders/water_spirv.h"
#include "10-world/shaders/world_spirv.h"
#include "10-world/water_surface.h"
#include "device.h"
#include "water_before_aerial_perspective_spirv.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <iterator>

namespace {

using cy::f32;
using cy::Mat4;
using cy::u16;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;
using cy::Vec3;
using cy::Vec4;
namespace rhi = cy::rhi;
namespace sky = cy::rendering::sky;
namespace sample = cy::sample::world;
using cy::rendering::ResourceId;

[[nodiscard]] cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

// --- The camera ----------------------------------------------------------------------------------

constexpr u32 kWidth = 160;
constexpr u32 kHeight = 96;
constexpr u32 kTexels = kWidth * kHeight;
/// The sample's 0.95-radian vertical field of view, as its tangent.
constexpr f32 kTanHalfY = 0.5147F;
constexpr f32 kTanHalfX = kTanHalfY * (static_cast<f32>(kWidth) / static_cast<f32>(kHeight));
constexpr f32 kNear = 0.5F;
/// A camera on a cliff: high enough that the water between 0.6 and 17 km is seen at more than one
/// degree below the horizon, where the mirror does not cover the surface's own light.
constexpr f32 kHighEye = 300.0F;
/// A camera at a swimmer's height, whose nearest water is a few metres away.
constexpr f32 kLowEye = 2.0F;

/// Metres. The water plane is y = 0; the bed is kBedDepth under it; the land is level with it.
constexpr f32 kBedDepth = 0.5F;
constexpr f32 kHalfWidth = 60'000.0F;
constexpr f32 kFarEdge = 60'000.0F;
constexpr f32 kDomeRadius = 200'000.0F;
/// Where the aerial perspective volume ends: beyond every surface.
constexpr f32 kAerialFar = 200'000.0F;
constexpr f32 kLandAlbedo = 0.3F;
constexpr f32 kBedAlbedo = 0.5F;
/// `WorldPush::sun` and `::ambient` for the lit frames, in the frame's radiance units.
constexpr f32 kSun = 0.8F;
constexpr f32 kAmbient = 0.15F;
/// The colour the device foam pass writes where there is no open-sea foam: water.slang reads its
/// red channel as foam, so this is "none".
constexpr Vec3 kDeepWater{0.02F, 0.05F, 0.07F};

[[nodiscard]] Vec3 eye_at(f32 height) noexcept {
    return Vec3{0.0F, height, 0.0F};
}

[[nodiscard]] Mat4 world_to_clip(f32 height) noexcept {
    const Mat4 projection = cy::perspective_reversed_z_infinite(
        2.0F * std::atan(kTanHalfY), static_cast<f32>(kWidth) / static_cast<f32>(kHeight), kNear);
    return projection * cy::look_at(eye_at(height), eye_at(height) + Vec3{0.0F, 0.0F, -1.0F});
}

[[nodiscard]] sky::AerialPerspectiveTable::View camera_view() noexcept {
    sky::AerialPerspectiveTable::View view;
    view.forward = Vec3{0.0F, 0.0F, -1.0F};
    view.right = Vec3{1.0F, 0.0F, 0.0F};
    view.up = Vec3{0.0F, 1.0F, 0.0F};
    view.tan_half_fov_x = kTanHalfX;
    view.tan_half_fov_y = kTanHalfY;
    return view;
}

/// The unit ray through a texel's centre. Row zero is the TOP of the picture.
[[nodiscard]] Vec3 ray_of(u32 texel) noexcept {
    const u32 column = texel % kWidth;
    const u32 row = texel / kWidth;
    const f32 ndc_x =
        (((static_cast<f32>(column) + 0.5F) / static_cast<f32>(kWidth)) * 2.0F) - 1.0F;
    const f32 ndc_y = 1.0F - (((static_cast<f32>(row) + 0.5F) / static_cast<f32>(kHeight)) * 2.0F);
    return cy::normalize(Vec3{ndc_x * kTanHalfX, ndc_y * kTanHalfY, -1.0F});
}

/// What a texel's ray meets at the level y = 0, if it meets it inside the scene.
struct Hit {
    bool water = false;
    bool land = false;
    Vec3 offset{0.0F, 0.0F, 0.0F};
    f32 distance = 0.0F;
};

[[nodiscard]] Hit hit_of(u32 texel, f32 height) noexcept {
    const Vec3 ray = ray_of(texel);
    Hit hit;
    if (ray.y >= 0.0F) {
        return hit;
    }
    hit.distance = height / -ray.y;
    hit.offset = ray * hit.distance;
    if (-hit.offset.z > kFarEdge || std::fabs(hit.offset.x) > kHalfWidth) {
        return hit;
    }
    hit.water = hit.offset.x < 0.0F;
    hit.land = hit.offset.x > 0.0F;
    return hit;
}

// --- The atmosphere ------------------------------------------------------------------------------

/// One atmosphere, one sun and one eye height, integrated the way
/// `Stage::update_aerial_perspective` integrates them.
struct Air {
    sky::Atmosphere atmosphere = sky::earth_atmosphere();
    Vec3 sun = cy::normalize(Vec3{0.35F, 0.5F, 0.79F});
    sky::AtmosphereTables tables;
    sky::IncrementalSkyView sky_view;
    sky::AerialPerspectiveTable aerial;

    Air() noexcept : tables(allocator()), sky_view(allocator()), aerial(allocator()) {}

    [[nodiscard]] cy::Status build(f32 height) noexcept {
        if (cy::Status configured = tables.configure(sky::SkyTableQuality::Medium); !configured) {
            return configured;
        }
        if (auto built = tables.build(atmosphere); !built) {
            return cy::make_unexpected(built.error());
        }
        const Vec3 eye = sky::ground_position(atmosphere, height);
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

    /// The frame's radiance scale: the sky one degree above the horizon at half the tone map's
    /// input range, as `render.world_aerial_perspective` chooses it.
    [[nodiscard]] f32 scale() const noexcept {
        const Vec3 horizon = sky_view.sample(Vec3{0.0F, std::sin(1.0F * cy::math::kDegToRad),
                                                  -std::cos(1.0F * cy::math::kDegToRad)});
        return 0.5F / cy::math::max(horizon.y, 1.0e-6F);
    }
};

// --- Numbers the target holds --------------------------------------------------------------------

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

/// The display value a texel holds, per channel.
[[nodiscard]] Vec3 display_at(const Picture& picture, u32 texel) noexcept {
    const usize base = static_cast<usize>(texel) * 4;
    return Vec3{half_to_float(picture.bits[base]), half_to_float(picture.bits[base + 1]),
                half_to_float(picture.bits[base + 2])};
}

/// world.slang's `tonemap()`: Reinhard, then the display gamma.
[[nodiscard]] Vec3 tonemap(Vec3 radiance) noexcept {
    const auto one = [](f32 value) noexcept {
        return std::pow(cy::math::saturate(value / (1.0F + value)), 1.0F / 2.2F);
    };
    return Vec3{one(radiance.x), one(radiance.y), one(radiance.z)};
}

/// water.slang's `untonemap()`: a display value back to the radiance it was given.
[[nodiscard]] Vec3 untonemap(Vec3 display) noexcept {
    const auto one = [](f32 value) noexcept {
        const f32 mapped = cy::math::min(std::pow(cy::math::saturate(value), 2.2F), 0.999F);
        return mapped / (1.0F - mapped);
    };
    return Vec3{one(display.x), one(display.y), one(display.z)};
}

[[nodiscard]] Vec3 radiance_at(const Picture& picture, u32 texel) noexcept {
    return untonemap(display_at(picture, texel));
}

[[nodiscard]] f32 largest_gap(Vec3 a, Vec3 b) noexcept {
    return cy::math::max(std::fabs(a.x - b.x),
                         cy::math::max(std::fabs(a.y - b.y), std::fabs(a.z - b.z)));
}

[[nodiscard]] f32 largest(Vec3 value) noexcept {
    return cy::math::max(value.x, cy::math::max(value.y, value.z));
}

[[nodiscard]] u32 differing(const Picture& a, const Picture& b) noexcept {
    u32 count = 0;
    for (u32 texel = 0; texel < kTexels; ++texel) {
        const usize base = static_cast<usize>(texel) * 4;
        count += std::memcmp(&a.bits[base], &b.bits[base], 4 * sizeof(u16)) != 0 ? 1U : 0U;
    }
    return count;
}

// --- The scene -----------------------------------------------------------------------------------

struct Vertex {
    f32 position[3];
    f32 normal[3];
};

/// The geometry of one eye height: the dome, then the land and the bed (the terrain run), then the
/// water.
struct Scene {
    cy::Array<Vertex> vertices;
    cy::Array<Vec3> colours;
    u32 dome_count = 0;
    u32 terrain_first = 0;
    u32 water_first = 0;

    Scene() noexcept : vertices(allocator()), colours(allocator()) {}

    [[nodiscard]] cy::Status add(Vec3 position, Vec3 normal, Vec3 colour) noexcept {
        if (cy::Status pushed = vertices.push_back(
                Vertex{{position.x, position.y, position.z}, {normal.x, normal.y, normal.z}});
            !pushed) {
            return pushed;
        }
        return colours.push_back(colour);
    }

    /// A level quad at height `y` over x in [x0, x1] and z in [-kFarEdge, 0].
    [[nodiscard]] cy::Status add_quad(f32 x0, f32 x1, f32 y, Vec3 colour) noexcept {
        const Vec3 corners[4] = {Vec3{x0, y, 0.0F}, Vec3{x1, y, 0.0F}, Vec3{x0, y, -kFarEdge},
                                 Vec3{x1, y, -kFarEdge}};
        for (const u32 corner : {0U, 1U, 2U, 2U, 1U, 3U}) {
            if (cy::Status added = add(corners[corner], Vec3{0.0F, 1.0F, 0.0F}, colour); !added) {
                return added;
            }
        }
        return cy::ok();
    }

    /// The dome, coloured as `Stage::build_dynamic` colours it: the atmosphere's clear sky in each
    /// vertex's direction, times the frame's radiance scale.
    [[nodiscard]] cy::Status add_dome(const Air& air, f32 height) noexcept {
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
        const f32 scale = air.scale();
        for (u32 ring = 0; ring < kRings; ++ring) {
            for (u32 segment = 0; segment < kSegments; ++segment) {
                const Vec3 corners[4] = {direction(ring, segment), direction(ring, segment + 1),
                                         direction(ring + 1, segment),
                                         direction(ring + 1, segment + 1)};
                for (const u32 corner : {0U, 2U, 1U, 1U, 2U, 3U}) {
                    const Vec3 at = corners[corner];
                    if (cy::Status added = add(eye_at(height) + (at * kDomeRadius), at,
                                               air.sky_view.sample(at) * scale);
                        !added) {
                        return added;
                    }
                }
            }
        }
        dome_count = static_cast<u32>(vertices.size());
        return cy::ok();
    }

    [[nodiscard]] cy::Status build(const Air& air, f32 height) noexcept {
        if (cy::Status dome = add_dome(air, height); !dome) {
            return dome;
        }
        terrain_first = static_cast<u32>(vertices.size());
        const Vec3 land{kLandAlbedo, kLandAlbedo, kLandAlbedo};
        const Vec3 bed{kBedAlbedo, kBedAlbedo, kBedAlbedo};
        if (cy::Status added = add_quad(0.0F, kHalfWidth, 0.0F, land); !added) {
            return added;
        }
        if (cy::Status added = add_quad(-kHalfWidth, 0.0F, -kBedDepth, bed); !added) {
            return added;
        }
        water_first = static_cast<u32>(vertices.size());
        return add_quad(-kHalfWidth, 0.0F, 0.0F, kDeepWater);
    }
};

// --- The world pipeline's blocks, as `stage.cpp` writes them -------------------------------------

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

void write_rows(WorldPush& push, const Vec4 (&rows)[4]) noexcept {
    f32* const targets[4] = {push.row0, push.row1, push.row2, push.row3};
    for (usize row = 0; row < 4; ++row) {
        targets[row][0] = rows[row].x;
        targets[row][1] = rows[row].y;
        targets[row][2] = rows[row].z;
        targets[row][3] = rows[row].w;
    }
}

// --- The device side -----------------------------------------------------------------------------

/// What one frame is asked to draw.
struct Shot {
    f32 height = kHighEye;
    /// The words bound at binding 2; null binds the switched-off header.
    const cy::Array<Vec4>* aerial = nullptr;
    /// The sun and the sky's ambient light on, or both zero.
    bool lit = false;
    /// Draw the water with water.slang's fragment stage from before this change.
    bool before = false;
};

struct Draw {
    u32 first = 0;
    u32 count = 0;
    bool water = false;
    WorldPush push;
};

struct PassState {
    cy::rendering::GraphExecutor* executor = nullptr;
    rhi::Device* device = nullptr;
    rhi::GraphicsPipelineHandle pipeline;
    rhi::GraphicsPipelineHandle water_pipeline;
    rhi::PipelineLayoutHandle layout;
    rhi::DescriptorSetHandle set;
    rhi::BufferHandle vertices;
    rhi::BufferHandle colours;
    const sample::WaterSurface* surface = nullptr;
    sample::WaterTargets targets;
    bool binds_water = false;
    cy::Status bound = cy::ok();
    ResourceId color = cy::rendering::kInvalidResource;
    ResourceId depth = cy::rendering::kInvalidResource;
    Draw draws[3];
    u32 draw_count = 0;
};

void bind_for(const cy::rendering::PassContext& context, const PassState& state,
              const Draw& draw) noexcept {
    if (draw.water) {
        const rhi::DescriptorSetHandle sets[2] = {state.set, state.surface->set()};
        context.commands->bind_graphics_pipeline(state.water_pipeline);
        context.commands->bind_descriptor_sets(state.surface->layout(), 0,
                                               cy::Span<const rhi::DescriptorSetHandle>(sets, 2));
        return;
    }
    context.commands->bind_graphics_pipeline(state.pipeline);
    context.commands->bind_descriptor_sets(state.layout, 0,
                                           cy::Span<const rhi::DescriptorSetHandle>(&state.set, 1));
}

void record_pass(const cy::rendering::PassContext& context, void* user) noexcept {
    auto* state = static_cast<PassState*>(user);
    if (state->binds_water) {
        state->bound = state->surface->bind(*state->device, *state->executor, state->targets);
    }
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
    const rhi::BufferHandle streams[2] = {state->vertices, state->colours};
    const u64 offsets[2] = {0, 0};
    context.commands->bind_vertex_buffers(0, cy::Span<const rhi::BufferHandle>(streams, 2),
                                          cy::Span<const u64>(offsets, 2));
    for (u32 index = 0; index < state->draw_count; ++index) {
        const Draw& draw = state->draws[index];
        if (draw.water && !state->bound) {
            continue;
        }
        bind_for(context, *state, draw);
        const rhi::PipelineLayoutHandle layout =
            draw.water ? state->surface->layout() : state->layout;
        context.commands->push_constants(
            layout, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
            cy::Span<const u8>(reinterpret_cast<const u8*>(&draw.push), sizeof(WorldPush)));
        context.commands->draw(draw.count, 1, draw.first, 0);
    }
    context.commands->end_rendering();
}

struct ReadbackState {
    cy::rendering::GraphExecutor* executor = nullptr;
    ResourceId color = cy::rendering::kInvalidResource;
    rhi::BufferHandle buffer;
};

void record_readback(const cy::rendering::PassContext& context, void* user) noexcept {
    auto* state = static_cast<ReadbackState*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{kWidth, kHeight, 1};
    context.commands->copy_texture_to_buffer(state->executor->texture(state->color), state->buffer,
                                             cy::Span<const rhi::BufferTextureCopy>(&region, 1));
}

constexpr u32 kMaxVertices = 40'000;
constexpr u64 kAerialWords = sky::kAerialPerspectiveHeaderWords + (u64{32} * 18U * 64U * 2U);

/// The world pipeline from the sample's committed SPIR-V, the sample's own water surface, the
/// water pipeline from before this change on the same layout, and the buffers a shot needs.
class WaterAirScene {
public:
    explicit WaterAirScene(cy::render_test::DeviceFixture& fixture) noexcept
        : fixture_(fixture), device_(fixture.device()), off_(allocator()) {}

    WaterAirScene(const WaterAirScene&) = delete;
    WaterAirScene& operator=(const WaterAirScene&) = delete;

    ~WaterAirScene() {
        (void)device_.wait_idle();
        surface_.destroy(device_);
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
             {vertex_, fragment_, water_vertex_, before_fragment_}) {
            if (!module.is_null()) {
                device_.destroy_shader_module(module);
            }
        }
        for (rhi::BufferHandle buffer :
             {vertices_, colours_, field_, placement_, aerial_, readback_}) {
            if (!buffer.is_null()) {
                device_.destroy_buffer(buffer);
            }
        }
    }

    [[nodiscard]] cy::Status prepare() noexcept;
    [[nodiscard]] cy::Status shoot(const Scene& scene, const Air& air, const Shot& shot,
                                   Picture& out) noexcept;
    /// What `build_water_params()` handed the shader on the last shot.
    [[nodiscard]] const sample::WaterParams& params() const noexcept { return params_; }

private:
    [[nodiscard]] cy::Expected<rhi::BufferHandle, cy::Error> buffer(const char* name, u64 size,
                                                                    rhi::BufferUsage usage,
                                                                    rhi::MemoryUse memory) noexcept;
    [[nodiscard]] cy::Expected<rhi::ShaderModuleHandle, cy::Error> module(
        const char* name, rhi::ShaderStage stage, cy::Span<const u32> spirv) noexcept;
    [[nodiscard]] cy::Status prepare_modules() noexcept;
    [[nodiscard]] cy::Status prepare_world_pipeline() noexcept;
    [[nodiscard]] cy::Status prepare_before_pipeline() noexcept;
    [[nodiscard]] cy::Status prepare_buffers() noexcept;
    [[nodiscard]] cy::Status upload(const Scene& scene, const Shot& shot) noexcept;
    [[nodiscard]] cy::Status execute(cy::rendering::RenderGraph& graph,
                                     cy::rendering::GraphExecutor& executor, const PassState& frame,
                                     Picture& out) noexcept;

    cy::render_test::DeviceFixture& fixture_;
    rhi::Device& device_;
    rhi::ShaderModuleHandle vertex_;
    rhi::ShaderModuleHandle fragment_;
    rhi::ShaderModuleHandle water_vertex_;
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
    sample::WaterSurface surface_;
    sample::WaterParams params_;
    /// An unbuilt table's words: the header alone, switched off.
    cy::Array<Vec4> off_;
};

cy::Expected<rhi::BufferHandle, cy::Error> WaterAirScene::buffer(const char* name, u64 size,
                                                                 rhi::BufferUsage usage,
                                                                 rhi::MemoryUse memory) noexcept {
    rhi::BufferDescription description;
    description.name = name;
    description.size = size;
    description.usage = usage;
    description.memory = memory;
    return device_.create_buffer(description);
}

cy::Expected<rhi::ShaderModuleHandle, cy::Error> WaterAirScene::module(
    const char* name, rhi::ShaderStage stage, cy::Span<const u32> spirv) noexcept {
    rhi::ShaderModuleDescription description;
    description.name = name;
    description.stage = stage;
    description.entry_point = "main";
    description.spirv = spirv;
    return device_.create_shader_module(description);
}

cy::Status WaterAirScene::prepare_modules() noexcept {
    namespace now = cy::sample::world;
    namespace before = cy::render_test::water_before_aerial_perspective;
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
        {&water_vertex_, "water vertex", rhi::ShaderStage::Vertex,
         cy::Span<const u32>(now::kWaterVertexSpirv, std::size(now::kWaterVertexSpirv))},
        {&before_fragment_, "water fragment before aerial perspective", rhi::ShaderStage::Fragment,
         cy::Span<const u32>(before::kWaterFragmentSpirv, std::size(before::kWaterFragmentSpirv))},
    };
    for (const auto& entry : modules) {
        auto created = module(entry.name, entry.stage, entry.spirv);
        if (!created) {
            return cy::make_unexpected(created.error());
        }
        *entry.handle = *created;
    }
    return cy::ok();
}

/// The pipeline description every pipeline here shares: world.slang's vertex streams and states,
/// which water_surface.cpp restates for the water.
[[nodiscard]] rhi::GraphicsPipelineDescription shared_description(
    const rhi::VertexBinding (&streams)[2], const rhi::VertexAttribute (&attributes)[3],
    const rhi::ColorAttachmentState& color) noexcept {
    rhi::GraphicsPipelineDescription description;
    description.vertex_bindings = cy::Span<const rhi::VertexBinding>(streams, 2);
    description.vertex_attributes = cy::Span<const rhi::VertexAttribute>(attributes, 3);
    description.color_attachments = cy::Span<const rhi::ColorAttachmentState>(&color, 1);
    description.rasterisation.cull_mode = rhi::CullMode::None;
    description.depth_stencil.format = rhi::Format::D32Sfloat;
    description.depth_stencil.depth_test_enable = true;
    description.depth_stencil.depth_write_enable = true;
    return description;
}

constexpr rhi::VertexBinding kStreams[2] = {{0, sizeof(Vertex), rhi::VertexInputRate::PerVertex},
                                            {1, sizeof(f32) * 3, rhi::VertexInputRate::PerVertex}};
constexpr rhi::VertexAttribute kAttributes[3] = {
    {0, 0, rhi::Format::Rgb32Sfloat, 0},
    {1, 0, rhi::Format::Rgb32Sfloat, sizeof(f32) * 3},
    {2, 1, rhi::Format::Rgb32Sfloat, 0}};

cy::Status WaterAirScene::prepare_world_pipeline() noexcept {
    // Set 0 exactly as the stage makes it: the cloud shadow field, its placement and the aerial
    // perspective table, for the fragment stage.
    rhi::DescriptorBinding bindings[3] = {};
    for (u32 index = 0; index < 3; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = rhi::DescriptorKind::StorageBuffer;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Fragment;
    }
    rhi::DescriptorSetLayoutDescription set_description;
    set_description.name = "world set";
    set_description.bindings = cy::Span<const rhi::DescriptorBinding>(bindings, 3);
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

    rhi::ColorAttachmentState color;
    color.format = rhi::Format::Rgba16Sfloat;
    rhi::GraphicsPipelineDescription description = shared_description(kStreams, kAttributes, color);
    description.name = "world";
    description.layout = layout_;
    description.vertex_shader = vertex_;
    description.fragment_shader = fragment_;
    auto pipeline = device_.create_graphics_pipeline(description);
    if (!pipeline) {
        return cy::make_unexpected(pipeline.error());
    }
    pipeline_ = *pipeline;
    return cy::ok();
}

cy::Status WaterAirScene::prepare_before_pipeline() noexcept {
    // The sample's water surface first, whose layout the pipeline from before shares: it reads
    // bindings 0 and 1 of set 0 and never binding 2, and a layout may declare a binding its
    // shaders never touch.
    if (cy::Status made =
            surface_.create(device_, set_layout_, rhi::Format::Rgba16Sfloat, kWidth, kHeight);
        !made) {
        return made;
    }
    rhi::ColorAttachmentState color;
    color.format = rhi::Format::Rgba16Sfloat;
    rhi::GraphicsPipelineDescription description = shared_description(kStreams, kAttributes, color);
    description.name = "world water before aerial perspective";
    description.layout = surface_.layout();
    description.vertex_shader = water_vertex_;
    description.fragment_shader = before_fragment_;
    auto pipeline = device_.create_graphics_pipeline(description);
    if (!pipeline) {
        return cy::make_unexpected(pipeline.error());
    }
    before_pipeline_ = *pipeline;
    return cy::ok();
}

cy::Status WaterAirScene::prepare_buffers() noexcept {
    auto vertices = buffer("scene", sizeof(Vertex) * kMaxVertices, rhi::BufferUsage::Vertex,
                           rhi::MemoryUse::Upload);
    auto colours = buffer("scene colour", sizeof(f32) * 3 * kMaxVertices, rhi::BufferUsage::Vertex,
                          rhi::MemoryUse::Upload);
    auto field = buffer("cloud shadow placeholder", 16 * sizeof(u32), rhi::BufferUsage::Storage,
                        rhi::MemoryUse::Upload);
    auto placement = buffer("cloud shadow placement", 4 * sizeof(f32), rhi::BufferUsage::Storage,
                            rhi::MemoryUse::Upload);
    auto aerial = buffer("aerial perspective", kAerialWords * sizeof(Vec4),
                         rhi::BufferUsage::Storage, rhi::MemoryUse::Upload);
    auto readback = buffer("scene readback", static_cast<u64>(kTexels) * 4 * sizeof(u16),
                           rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback);
    if (!vertices || !colours || !field || !placement || !aerial || !readback) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "a water scene buffer did not allocate");
    }
    vertices_ = *vertices;
    colours_ = *colours;
    field_ = *field;
    placement_ = *placement;
    aerial_ = *aerial;
    readback_ = *readback;
    // Cloud shadows off: the placement's `enabled` word is zero and the field is never read.
    std::memset(device_.buffer_mapped_pointer(field_), 0, 16 * sizeof(u32));
    std::memset(device_.buffer_mapped_pointer(placement_), 0, 4 * sizeof(f32));
    const sky::AerialPerspectiveTable unbuilt(allocator());
    return sky::pack_aerial_perspective(unbuilt, 1.0F, off_);
}

cy::Status WaterAirScene::prepare() noexcept {
    for (auto step : {&WaterAirScene::prepare_modules, &WaterAirScene::prepare_world_pipeline,
                      &WaterAirScene::prepare_before_pipeline, &WaterAirScene::prepare_buffers}) {
        if (cy::Status done = (this->*step)(); !done) {
            return done;
        }
    }
    auto set = device_.allocate_descriptor_set(set_layout_, false);
    if (!set) {
        return cy::make_unexpected(set.error());
    }
    set_ = *set;
    rhi::DescriptorWrite writes[3] = {};
    const rhi::BufferHandle bound[3] = {field_, placement_, aerial_};
    for (u32 index = 0; index < 3; ++index) {
        writes[index].binding = index;
        writes[index].kind = rhi::DescriptorKind::StorageBuffer;
        writes[index].buffer = bound[index];
    }
    return device_.update_descriptor_set(set_, cy::Span<const rhi::DescriptorWrite>(writes, 3));
}

cy::Status WaterAirScene::upload(const Scene& scene, const Shot& shot) noexcept {
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

    // The water's parameters, as `Stage::declare_water` builds them, with the look's levers that
    // are not physics — foam, caustics, the wave offsets of the reads — off: the surface is flat.
    sample::WaterLook look;
    look.foam_band_metres = 0.0F;
    look.caustic_strength = 0.0F;
    sample::WaterView view;
    view.world_to_clip = world_to_clip(shot.height);
    view.width = kWidth;
    view.height = kHeight;
    auto params = sample::build_water_params(cy::water::clear_sea_optics(), look, view,
                                             cy::Span<const cy::water::WaveTrain>());
    if (!params) {
        return cy::make_unexpected(params.error());
    }
    params_ = *params;
    return surface_.upload(device_, params_);
}

[[nodiscard]] WorldPush push_for(const Air& air, const Shot& shot, const Vec4 (&rows)[4]) noexcept {
    WorldPush push;
    write_rows(push, rows);
    push.light[0] = -air.sun.x;
    push.light[1] = -air.sun.y;
    push.light[2] = -air.sun.z;
    push.eye[1] = shot.height;
    for (u32 channel = 0; channel < 3; ++channel) {
        push.sun[channel] = shot.lit ? kSun : 0.0F;
        push.ambient[channel] = shot.lit ? kAmbient : 0.0F;
    }
    return push;
}

[[nodiscard]] WorldPush emissive(WorldPush push) noexcept {
    push.eye[3] = 1.0F;
    return push;
}

cy::Status WaterAirScene::shoot(const Scene& scene, const Air& air, const Shot& shot,
                                Picture& out) noexcept {
    if (cy::Status uploaded = upload(scene, shot); !uploaded) {
        return uploaded;
    }
    const Mat4 matrix = world_to_clip(shot.height);
    Vec4 rows[4] = {matrix.row(0), matrix.row(1), matrix.row(2), matrix.row(3)};
    const WorldPush lit = push_for(air, shot, rows);
    sample::mirrored_rows(matrix, 0.0F, rows);
    const WorldPush mirrored = push_for(air, shot, rows);
    const u32 terrain_count = scene.water_first - scene.terrain_first;

    if (cy::Expected<u32, cy::Error> began = device_.begin_frame(); !began) {
        return cy::make_unexpected(began.error());
    }
    cy::rendering::RenderGraph graph(fixture_.allocator());
    cy::rendering::GraphExecutor executor(fixture_.allocator(), device_);

    PassState base;
    base.executor = &executor;
    base.device = &device_;
    base.pipeline = pipeline_;
    base.water_pipeline = shot.before ? before_pipeline_ : surface_.pipeline();
    base.layout = layout_;
    base.set = set_;
    base.vertices = vertices_;
    base.colours = colours_;
    base.surface = &surface_;
    base.targets = surface_.import(graph);

    // THE TWO PICTURES, as `Stage::declare_water` declares them: the terrain alone from the frame's
    // camera, and the dome and the terrain mirrored in the water with the near plane on it.
    PassState refraction = base;
    refraction.color = base.targets.refraction;
    refraction.depth = base.targets.refraction_depth;
    refraction.draws[0] = Draw{scene.terrain_first, terrain_count, false, lit};
    refraction.draw_count = 1;
    PassState reflection = base;
    reflection.color = base.targets.reflection;
    reflection.depth = base.targets.reflection_depth;
    reflection.draws[0] = Draw{0, scene.dome_count, false, emissive(mirrored)};
    reflection.draws[1] = Draw{scene.terrain_first, terrain_count, false, mirrored};
    reflection.draw_count = 2;
    graph.add_pass("water refraction", rhi::QueueKind::Graphics)
        .write(refraction.color, rhi::Access::ColorAttachmentWrite)
        .write(refraction.depth, rhi::Access::DepthStencilAttachmentWrite)
        .record(&record_pass, &refraction);
    graph.add_pass("water reflection", rhi::QueueKind::Graphics)
        .write(reflection.color, rhi::Access::ColorAttachmentWrite)
        .write(reflection.depth, rhi::Access::DepthStencilAttachmentWrite)
        .record(&record_pass, &reflection);

    // THE FRAME: the dome, the terrain, and the water through water.slang.
    PassState frame = base;
    cy::rendering::TextureRequest color_request;
    color_request.name = "scene colour";
    color_request.format = rhi::Format::Rgba16Sfloat;
    color_request.width = kWidth;
    color_request.height = kHeight;
    frame.color = graph.create_texture(color_request);
    cy::rendering::TextureRequest depth_request;
    depth_request.name = "scene depth";
    depth_request.format = rhi::Format::D32Sfloat;
    depth_request.width = kWidth;
    depth_request.height = kHeight;
    frame.depth = graph.create_texture(depth_request);
    frame.draws[0] = Draw{0, scene.dome_count, false, emissive(lit)};
    frame.draws[1] = Draw{scene.terrain_first, terrain_count, false, lit};
    frame.draws[2] = Draw{scene.water_first, 6, true, lit};
    frame.draw_count = 3;
    frame.binds_water = true;
    graph.add_pass("scene", rhi::QueueKind::Graphics)
        .write(frame.color, rhi::Access::ColorAttachmentWrite)
        .write(frame.depth, rhi::Access::DepthStencilAttachmentWrite)
        .read(base.targets.refraction, rhi::Access::FragmentSampledRead)
        .read(base.targets.refraction_depth, rhi::Access::FragmentSampledRead)
        .read(base.targets.reflection, rhi::Access::FragmentSampledRead)
        .record(&record_pass, &frame);
    return execute(graph, executor, frame, out);
}

cy::Status WaterAirScene::execute(cy::rendering::RenderGraph& graph,
                                  cy::rendering::GraphExecutor& executor, const PassState& frame,
                                  Picture& out) noexcept {
    ReadbackState readback;
    readback.executor = &executor;
    readback.color = frame.color;
    readback.buffer = readback_;
    cy::rendering::BufferRequest readback_request;
    readback_request.name = "scene readback";
    readback_request.size = static_cast<u64>(kTexels) * 4 * sizeof(u16);
    readback_request.extra_usage = rhi::BufferUsage::TransferDestination;
    const ResourceId copied = graph.import_buffer(readback_request, readback_);
    graph.add_pass("scene readback", rhi::QueueKind::Graphics)
        .read(frame.color, rhi::Access::TransferRead)
        .write(copied, rhi::Access::TransferWrite)
        .record(&record_readback, &readback);
    graph.add_pass("scene host", rhi::QueueKind::Graphics)
        .read(copied, rhi::Access::HostRead)
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
    if (!frame.bound) {
        return frame.bound;
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

// --- The processor's answers ---------------------------------------------------------------------

/// Schlick's Fresnel for the flat surface at `offset` from the eye, with the parameter block's F0.
[[nodiscard]] f32 fresnel(const sample::WaterParams& params, Vec3 offset) noexcept {
    const f32 f0 = params.in_scatter[3];
    const f32 cosine = cy::math::saturate(-offset.y / cy::length(offset));
    return f0 + ((1.0F - f0) * std::pow(1.0F - cosine, 5.0F));
}

/// The four frames every claim at one eye height is measured from, and what they were drawn from.
struct Frames {
    Picture dark_off{allocator()};
    Picture dark_on{allocator()};
    Picture lit_off{allocator()};
    Picture lit_on{allocator()};
    sample::WaterParams params;
    f32 scale = 1.0F;
};

struct Suite {
    cy::render_test::DeviceFixture fixture{"vulkan",
                                           "cy_test_render_world_water_aerial_perspective"};
    [[nodiscard]] bool have_vulkan() const noexcept { return fixture.is(rhi::BackendKind::Vulkan); }
};

/// Everything one eye height needs: its atmosphere, its scene, the packed table and the four frames.
struct Take {
    Air air;
    Scene scene;
    cy::Array<Vec4> words{allocator()};
    Frames frames;

    [[nodiscard]] cy::Status shoot(WaterAirScene& device, f32 height) noexcept {
        if (cy::Status built = air.build(height); !built) {
            return built;
        }
        if (cy::Status built = scene.build(air, height); !built) {
            return built;
        }
        frames.scale = air.scale();
        if (cy::Status packed = sky::pack_aerial_perspective(air.aerial, frames.scale, words);
            !packed) {
            return packed;
        }
        const struct {
            Picture* picture;
            bool lit;
            bool on;
        } shots[4] = {{&frames.dark_off, false, false},
                      {&frames.dark_on, false, true},
                      {&frames.lit_off, true, false},
                      {&frames.lit_on, true, true}};
        for (const auto& entry : shots) {
            const Shot shot{height, entry.on ? &words : nullptr, entry.lit, false};
            if (cy::Status drawn = device.shoot(scene, air, shot, *entry.picture); !drawn) {
                return drawn;
            }
        }
        frames.params = device.params();
        return cy::ok();
    }

    /// The table's answer at `offset`, its in-scattering in the frame's units.
    [[nodiscard]] sky::AerialPerspective air_at(Vec3 offset) const noexcept {
        sky::AerialPerspective through = air.aerial.sample_at(offset);
        through.in_scattering = through.in_scattering * frames.scale;
        return through;
    }
};

/// What the device must have drawn for a water pixel with the table on: the frame with it off,
/// with the surface's own light — the lit frame minus the dark one — through T, and (1 - F) S.
[[nodiscard]] Vec3 expected_water(const Take& take, u32 texel, const Hit& hit, bool lit) noexcept {
    const sky::AerialPerspective through = take.air_at(hit.offset);
    const f32 f = fresnel(take.frames.params, hit.offset);
    const Vec3 mirror = radiance_at(take.frames.dark_off, texel);
    Vec3 own{0.0F, 0.0F, 0.0F};
    if (lit) {
        own = radiance_at(take.frames.lit_off, texel) - mirror;
    }
    return mirror + cy::cwise_mul(own, through.transmittance) +
           (through.in_scattering * (1.0F - f));
}

/// The same for a land pixel: world.slang's `lit T + S`.
[[nodiscard]] Vec3 expected_land(const Take& take, u32 texel, const Hit& hit, bool lit) noexcept {
    const sky::AerialPerspective through = take.air_at(hit.offset);
    const Picture& off = lit ? take.frames.lit_off : take.frames.dark_off;
    return cy::cwise_mul(radiance_at(off, texel), through.transmittance) + through.in_scattering;
}

/// The in-scattering the device added at a pixel, from the dark pair: the land pixel itself, or
/// the water pixel's change divided by what the mirror does not cover.
[[nodiscard]] Vec3 measured_in_scattering(const Take& take, u32 texel, const Hit& hit) noexcept {
    const Vec3 on = radiance_at(take.frames.dark_on, texel);
    if (hit.land) {
        return on;
    }
    const f32 f = fresnel(take.frames.params, hit.offset);
    return (on - radiance_at(take.frames.dark_off, texel)) / (1.0F - f);
}

/// Rows whose water is seen at more than a degree below the horizon, where one minus Fresnel is at
/// least a tenth and dividing by it keeps the half-float's rounding under a per cent of the haze.
constexpr f32 kLeastUncovered = 0.1F;

/// One row's shoreline: the water column just left of the view's centre line and the land column
/// just right of it, at the same distance.
struct ShoreRow {
    bool valid = false;
    f32 distance = 0.0F;
    Vec3 water{0.0F, 0.0F, 0.0F};
    Vec3 land{0.0F, 0.0F, 0.0F};
    f32 relative_gap = 0.0F;
};

[[nodiscard]] ShoreRow shore_row(const Take& take, u32 row) noexcept {
    ShoreRow result;
    const u32 water_texel = (row * kWidth) + (kWidth / 2) - 1;
    const u32 land_texel = water_texel + 1;
    const Hit water = hit_of(water_texel, kHighEye);
    const Hit land = hit_of(land_texel, kHighEye);
    if (!water.water || !land.land ||
        1.0F - fresnel(take.frames.params, water.offset) < kLeastUncovered) {
        return result;
    }
    result.valid = true;
    result.distance = water.distance;
    result.water = measured_in_scattering(take, water_texel, water);
    result.land = measured_in_scattering(take, land_texel, land);
    result.relative_gap =
        largest_gap(result.water, result.land) / cy::math::max(largest(result.land), 1.0e-4F);
    return result;
}

}  // namespace

CY_TEST_CASE("world water aerial perspective: the device applies the table to the water once") {
    Suite suite;
    if (!suite.have_vulkan()) {
        suite.fixture.report_skip();
        return;
    }
    WaterAirScene device(suite.fixture);
    CY_REQUIRE(device.prepare());
    Take take;
    CY_REQUIRE(take.shoot(device, kHighEye));

    f32 worst_water = 0.0F;
    f32 worst_land = 0.0F;
    u32 water_texels = 0;
    u32 land_texels = 0;
    for (const bool lit : {false, true}) {
        const Picture& on = lit ? take.frames.lit_on : take.frames.dark_on;
        for (u32 texel = 0; texel < kTexels; ++texel) {
            const Hit hit = hit_of(texel, kHighEye);
            if (!hit.water && !hit.land) {
                continue;
            }
            const Vec3 expected = hit.water ? expected_water(take, texel, hit, lit)
                                            : expected_land(take, texel, hit, lit);
            const f32 gap = largest_gap(display_at(on, texel), tonemap(expected));
            f32& worst = hit.water ? worst_water : worst_land;
            worst = cy::math::max(worst, gap);
            (hit.water ? water_texels : land_texels) += 1;
        }
    }
    CY_TEST_MESSAGE(water_texels, " water and ", land_texels,
                    " land texels over the dark and the lit frame; worst gap from the processor's "
                    "table, display values: water ",
                    worst_water, ", land ", worst_land);
    CY_CHECK(water_texels > 2000U);
    CY_CHECK(land_texels > 2000U);
    CY_CHECK(worst_water < 0.004F);
    CY_CHECK(worst_land < 0.004F);
    CY_CHECK(suite.fixture.validation_errors() == 0U);
}

CY_TEST_CASE("world water aerial perspective: at the shoreline the haze has no step") {
    Suite suite;
    if (!suite.have_vulkan()) {
        suite.fixture.report_skip();
        return;
    }
    WaterAirScene device(suite.fixture);
    CY_REQUIRE(device.prepare());
    Take take;
    CY_REQUIRE(take.shoot(device, kHighEye));

    u32 rows = 0;
    f32 worst = 0.0F;
    f32 nearest = 1.0e9F;
    f32 farthest = 0.0F;
    for (u32 row = kHeight / 2; row < kHeight; ++row) {
        const ShoreRow shore = shore_row(take, row);
        if (!shore.valid) {
            continue;
        }
        ++rows;
        worst = cy::math::max(worst, shore.relative_gap);
        nearest = cy::math::min(nearest, shore.distance);
        farthest = cy::math::max(farthest, shore.distance);
    }
    CY_TEST_MESSAGE(rows, " shoreline rows from ", nearest, " m to ", farthest,
                    " m: worst gap between the water's in-scattering and the land's beside it ",
                    worst);
    CY_CHECK(rows >= 30U);
    CY_CHECK(worst < 0.03F);
    CY_CHECK(suite.fixture.validation_errors() == 0U);
}

CY_TEST_CASE("world water aerial perspective: far water takes the land's in-scattering") {
    Suite suite;
    if (!suite.have_vulkan()) {
        suite.fixture.report_skip();
        return;
    }
    WaterAirScene device(suite.fixture);
    CY_REQUIRE(device.prepare());
    Take take;
    CY_REQUIRE(take.shoot(device, kHighEye));

    constexpr f32 kFar = 2'000.0F;
    u32 far_rows = 0;
    f32 worst = 0.0F;
    f32 nearest_haze = 1.0e9F;
    f32 farthest_haze = 0.0F;
    Vec3 farthest_water{0.0F, 0.0F, 0.0F};
    Vec3 farthest_land{0.0F, 0.0F, 0.0F};
    f32 farthest_distance = 0.0F;
    for (u32 row = kHeight / 2; row < kHeight; ++row) {
        const ShoreRow shore = shore_row(take, row);
        if (!shore.valid) {
            continue;
        }
        nearest_haze = cy::math::min(nearest_haze, largest(shore.land));
        if (shore.distance < kFar) {
            continue;
        }
        ++far_rows;
        worst = cy::math::max(worst, shore.relative_gap);
        if (shore.distance > farthest_distance) {
            farthest_distance = shore.distance;
            farthest_haze = largest(shore.land);
            farthest_water = shore.water;
            farthest_land = shore.land;
        }
    }
    CY_TEST_MESSAGE(far_rows, " rows beyond ", kFar, " m; at ", farthest_distance,
                    " m the water's in-scattering is ", farthest_water.x, ",", farthest_water.y,
                    ",", farthest_water.z, " and the land's ", farthest_land.x, ",",
                    farthest_land.y, ",", farthest_land.z, "; worst gap ", worst,
                    "; the nearest row's haze ", nearest_haze);
    CY_CHECK(far_rows >= 8U);
    CY_CHECK(worst < 0.03F);
    CY_CHECK(farthest_haze > 4.0F * nearest_haze);
    CY_CHECK(suite.fixture.validation_errors() == 0U);
}

CY_TEST_CASE("world water aerial perspective: near water is left as it was lit") {
    Suite suite;
    if (!suite.have_vulkan()) {
        suite.fixture.report_skip();
        return;
    }
    WaterAirScene device(suite.fixture);
    CY_REQUIRE(device.prepare());
    Take take;
    CY_REQUIRE(take.shoot(device, kLowEye));

    constexpr f32 kNearWater = 10.0F;
    u32 compared = 0;
    f32 worst = 0.0F;
    f32 table_haze = 0.0F;
    for (u32 texel = 0; texel < kTexels; ++texel) {
        const Hit hit = hit_of(texel, kLowEye);
        if (!hit.water || hit.distance > kNearWater) {
            continue;
        }
        ++compared;
        worst = cy::math::max(worst, largest_gap(display_at(take.frames.lit_on, texel),
                                                 display_at(take.frames.lit_off, texel)));
        const sky::AerialPerspective through = take.air_at(hit.offset);
        const Vec3 lost = Vec3{1.0F, 1.0F, 1.0F} - through.transmittance;
        table_haze =
            cy::math::max(table_haze, cy::math::max(largest(lost), largest(through.in_scattering)));
    }
    CY_TEST_MESSAGE(compared, " water texels within ", kNearWater,
                    " m of an eye 2 m up: worst change with the table on ", worst,
                    " (display); the table's own haze there ", table_haze);
    CY_CHECK(compared > 200U);
    CY_CHECK(table_haze < 1.0e-3F);
    CY_CHECK(worst < 0.001F);
    CY_CHECK(suite.fixture.validation_errors() == 0U);
}

CY_TEST_CASE("world water aerial perspective: off, the frame is the frame before") {
    Suite suite;
    if (!suite.have_vulkan()) {
        suite.fixture.report_skip();
        return;
    }
    WaterAirScene device(suite.fixture);
    CY_REQUIRE(device.prepare());
    Take take;
    CY_REQUIRE(take.shoot(device, kHighEye));

    u32 off_differing = 0;
    u32 on_differing = 0;
    for (const bool lit : {false, true}) {
        Picture before(allocator());
        const Shot shot{kHighEye, nullptr, lit, true};
        CY_REQUIRE(device.shoot(take.scene, take.air, shot, before));
        off_differing += differing(lit ? take.frames.lit_off : take.frames.dark_off, before);
        on_differing += differing(lit ? take.frames.lit_on : take.frames.dark_on, before);
    }
    CY_TEST_MESSAGE("texels differing from the water's shaders before this change: ",
                    off_differing, " with the table off, ", on_differing, " with it on");
    CY_CHECK(off_differing == 0U);
    CY_CHECK(on_differing > 2000U);
    CY_CHECK(suite.fixture.validation_errors() == 0U);
}
