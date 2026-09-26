// SPDX-License-Identifier: MIT
// WATER SHADING IN THE WORLD FRAME, DRAWN BY THE WORLD'S OWN SHADERS ON A DEVICE.
//
// `water` — "Water surface shading": the closure accounts for "reflection, refraction,
// wavelength-dependent absorption and scattering through the water column, surface roughness,
// normals, and foam coverage", and "Colour with depth SHALL follow physically-inspired attenuation
// over the water column thickness, so deep water darkens and shifts hue". And "Caustics": the
// surface-derived tier is the default, and "caustic patterns SHALL follow the simulated water
// surface rather than an unrelated looping texture".
//
// The frame that answers them is `samples/10-world`'s. This suite draws a small scene of its own —
// a sloping bed that rises out of the water, a flat water surface over it and an emissive sky
// ceiling whose colour is a linear ramp — with THAT frame's committed shaders
// (`samples/10-world/shaders/world_spirv.h` for the world, `water_spirv.h` for the water) and THAT
// frame's water device half (`samples/10-world/water_surface.cpp`: the pipeline, the set, the four
// targets and `build_water_params()`), in the same three passes the stage declares: the refraction
// picture, the mirrored reflection picture, and the frame that samples them. Then it reads the
// pixels back and checks each one against the processor's answer for the ray through it, computed
// with src/water/'s own functions.
//
// ================================================================================================
// FIVE CLAIMS
// ================================================================================================
//
//   * OFF, THE FRAME IS THE FRAME BEFORE. Water drawn through world.slang's lit path, as the stage
//     draws it with water shading off, is byte-identical to `references/world_water_off.png`,
//     drawn from the world shaders as they were before water shading existed. And the same
//     comparison can fail: with water shading on, the picture moves.
//   * THE REFLECTION IS THE SKY, MIRRORED. With the bed unlit and the water's own light off, every
//     water pixel is Schlick's Fresnel times the ceiling's radiance where the MIRRORED eye ray
//     meets it. A reflection read at the wrong place, or unmirrored, or with the bed leaking
//     through the mirrored near plane, misses it.
//   * DEEPER WATER IS DARKER, BY THE ABSORPTION LAW. Each water pixel is the bed's radiance times
//     `water::water_transmittance()` over the path from the surface to the bed, plus
//     `water::water_in_scatter()` over the same path lit by the sky and the sun, times one minus
//     Fresnel. Red goes first: the hue shifts as the bed deepens.
//   * FOAM IS ONLY AT THE SHORE. With shoreline foam on, every pixel whose water column is deeper
//     than the foam band is bit-identical to the frame without it, and every pixel in the shallow
//     half of the band is brighter.
//   * CAUSTICS MOVE, AND THEY ARE THE SURFACE'S. Two instants of the water clock draw different
//     beds and nothing else differs; at each instant, the bed's light is focused by the analytic
//     Laplacian of the very trains `select_caustic_trains()` picked from the displacement model.
//
// ================================================================================================
// THE MUTATIONS THIS SUITE WAS PROVED AGAINST
// ================================================================================================
//
// Each applied to the shader or to water_surface.cpp, the headers regenerated, the suite run and
// seen red, the file restored and md5-verified: `openspec/changes/add-water-shading/evidence/
// falsification.txt` records each mutation and the assertions it turned red.
//
// ================================================================================================
// REGENERATING THE REFERENCE
// ================================================================================================
//
// `CY_RENDER_UPDATE_GOLDEN=1 ctest -R render.world_water` writes `references/world_water_off.png`
// and then FAILS, naming what it wrote — `test_golden_frame.cpp`'s mechanism: a regenerating run
// can never be a passing one. It was written from world.slang's SPIR-V at 0f1dfd1, which this
// change does not touch.

#include <cy/test/test.h>

#include <cy/backends/rhi/access.h>
#include <cy/core/math/math.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/water/body.h>
#include <cy/water/displacement.h>
#include <cy/water/shading.h>

#include "10-world/shaders/world_spirv.h"
#include "10-world/water_surface.h"
#include "device.h"
#include "golden.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <new>
#include <numbers>

namespace {

using cy::f32;
using cy::f64;
using cy::i32;
using cy::Mat4;
using cy::u16;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;
using cy::Vec3;
using cy::Vec4;
namespace rhi = cy::rhi;
namespace water = cy::water;
namespace sample = cy::sample::world;
using cy::rendering::ResourceId;

// --- The scene -----------------------------------------------------------------------------------
//
// World-relative metres. The water is the plane y = 0; the bed rises out of it at z = 20 and falls
// away at one in twenty beyond; the sky is a ceiling 400 m up whose colour is linear in x and z,
// so what the rasteriser interpolates across it is exactly the ramp.

constexpr u32 kExtent = 160;
constexpr u32 kTexels = kExtent * kExtent;
constexpr f32 kFieldOfView = 0.95F;
constexpr f32 kLevel = 0.0F;
constexpr f32 kShoreHeight = 1.0F;
constexpr f32 kBedSlope = 0.05F;
constexpr f32 kBedAlbedo = 0.5F;
constexpr f32 kCeiling = 400.0F;
constexpr f32 kCeilingHalf = 20'000.0F;
constexpr f32 kQuadHalfWidth = 600.0F;
constexpr f32 kQuadNear = -100.0F;
constexpr f32 kQuadFar = 2'000.0F;
/// The colour the device foam pass writes where there is no open-sea foam; water.slang reads its
/// red channel as that foam, so this is "none".
constexpr Vec3 kDeepWater{0.02F, 0.05F, 0.07F};

[[nodiscard]] Vec3 eye() noexcept {
    return Vec3{0.0F, 30.0F, -30.0F};
}

[[nodiscard]] Vec3 look_target() noexcept {
    return Vec3{0.0F, 0.0F, 150.0F};
}

/// The direction light TRAVELS, as `WorldPush::light` carries it.
[[nodiscard]] Vec3 light_travel() noexcept {
    return cy::normalize(Vec3{0.25F, -0.85F, 0.35F});
}

[[nodiscard]] f32 bed_height(f32 z) noexcept {
    return kShoreHeight - (kBedSlope * z);
}

[[nodiscard]] Vec3 sky_colour(f32 x, f32 z, f32 scale) noexcept {
    return Vec3{scale * (0.25F + (0.15F * x / kCeilingHalf)),
                scale * (0.45F + (0.25F * z / kCeilingHalf)), scale * 0.85F};
}

[[nodiscard]] Mat4 world_to_clip() noexcept {
    const Mat4 projection = cy::perspective_reversed_z(kFieldOfView, 1.0F, 1.0F, 24'000.0F);
    return projection * cy::look_at(eye(), look_target());
}

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

void write_row(f32 (&out)[4], Vec4 row) noexcept {
    out[0] = row.x;
    out[1] = row.y;
    out[2] = row.z;
    out[3] = row.w;
}

struct Vertex {
    f32 position[3];
    f32 normal[3];
};

/// Six vertices a quad, three quads: the ceiling, the bed, the water.
constexpr u32 kCeilingFirst = 0;
constexpr u32 kBedFirst = 6;
constexpr u32 kWaterFirst = 12;
constexpr u32 kVertices = 18;

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

/// water.slang's `untonemap()`, for one channel: the inverse of world.slang's `tonemap()`.
[[nodiscard]] f32 untonemap(f32 display) noexcept {
    const f32 mapped = cy::math::min(std::pow(cy::math::saturate(display), 2.2F), 0.999F);
    return mapped / (1.0F - mapped);
}

/// The radiance a texel was given, per channel.
[[nodiscard]] Vec3 radiance_at(const Picture& picture, u32 texel) noexcept {
    const usize base = static_cast<usize>(texel) * 4;
    return Vec3{untonemap(half_to_float(picture.bits[base + 0])),
                untonemap(half_to_float(picture.bits[base + 1])),
                untonemap(half_to_float(picture.bits[base + 2]))};
}

[[nodiscard]] bool same_texel(const Picture& a, const Picture& b, u32 texel) noexcept {
    const usize base = static_cast<usize>(texel) * 4;
    return std::memcmp(&a.bits[base], &b.bits[base], 4 * sizeof(u16)) == 0;
}

/// The picture as the 8-bit image a reference holds.
[[nodiscard]] cy::Status to_image(const Picture& picture, cy::render_test::Image& out) noexcept {
    cy::Array<u32> texels(out.texels.allocator());
    if (cy::Status sized = texels.resize(kTexels); !sized) {
        return sized;
    }
    for (u32 texel = 0; texel < kTexels; ++texel) {
        u32 packed = 0xFF000000U;
        for (u32 channel = 0; channel < 3; ++channel) {
            const f32 value = cy::math::saturate(
                half_to_float(picture.bits[(static_cast<usize>(texel) * 4) + channel]));
            packed |= static_cast<u32>(std::lround(value * 255.0F)) << (channel * 8U);
        }
        texels[texel] = packed;
    }
    return cy::render_test::adopt(out, texels.span(), kExtent, kExtent);
}

// --- What the processor says each pixel sees
// ------------------------------------------------------

enum class Seen : u8 { Nothing = 0, Sky, Bed, Water };

/// One pixel's eye ray, and what it meets.
struct Ray {
    Seen seen = Seen::Nothing;
    Vec3 direction{0.0F, 0.0F, 1.0F};
    /// Where the ray meets the water, and the bed behind it — `has_bed` false where the bed falls
    /// away faster than the ray descends and the ray never reaches it.
    Vec3 surface{0.0F, 0.0F, 0.0F};
    Vec3 bed{0.0F, 0.0F, 0.0F};
    bool has_bed = false;
};

[[nodiscard]] bool inside_quad(const Vec3& at) noexcept {
    return std::fabs(at.x) <= kQuadHalfWidth && at.z >= kQuadNear && at.z <= kQuadFar;
}

[[nodiscard]] Ray trace(const Mat4& inverse, u32 texel) noexcept {
    const u32 column_index = texel % kExtent;
    const u32 row_index = texel / kExtent;
    const f32 column = static_cast<f32>(column_index) + 0.5F;
    const f32 row = static_cast<f32>(row_index) + 0.5F;
    const f32 ndc_x = (column * 2.0F / static_cast<f32>(kExtent)) - 1.0F;
    const f32 ndc_y = 1.0F - (row * 2.0F / static_cast<f32>(kExtent));
    const auto unproject = [&inverse, ndc_x, ndc_y](f32 depth) noexcept {
        const Vec4 clip{ndc_x, ndc_y, depth, 1.0F};
        const Vec4 point{cy::dot(inverse.row(0), clip), cy::dot(inverse.row(1), clip),
                         cy::dot(inverse.row(2), clip), cy::dot(inverse.row(3), clip)};
        return Vec3{point.x / point.w, point.y / point.w, point.z / point.w};
    };
    Ray ray;
    ray.direction = cy::normalize(unproject(0.5F) - eye());
    const Vec3 origin = eye();
    const Vec3 d = ray.direction;

    // The bed, y = h0 - s z: origin.y + t d.y = h0 - s (origin.z + t d.z).
    const f32 bed_denominator = d.y + (kBedSlope * d.z);
    const f32 t_bed =
        bed_denominator < 0.0F ? (bed_height(origin.z) - origin.y) / bed_denominator : -1.0F;
    const Vec3 bed = origin + (d * t_bed);
    const bool bed_hit = t_bed > 0.0F && inside_quad(bed);
    const f32 t_water = d.y < 0.0F ? (kLevel - origin.y) / d.y : -1.0F;
    const Vec3 surface = origin + (d * t_water);
    const bool water_hit = t_water > 0.0F && inside_quad(surface);

    if (water_hit && (!bed_hit || t_bed > t_water)) {
        ray.seen = Seen::Water;
        ray.surface = surface;
        ray.bed = bed;
        ray.has_bed = bed_hit;
    } else if (bed_hit) {
        ray.seen = Seen::Bed;
        ray.bed = bed;
    } else if (d.y > 0.0F) {
        ray.seen = Seen::Sky;
    }
    return ray;
}

/// Pixels whose four neighbours see the same kind of thing: nothing in them straddles an edge.
struct Classes {
    Ray rays[kTexels];
    bool interior[kTexels] = {};
    /// The most bed, in metres, between this pixel and any horizontal or vertical neighbour in its
    /// 3x3 neighbourhood: an upper bound on what the device's derivatives call the footprint,
    /// whichever pair of a 2x2 quad they difference. Zero where the neighbourhood has no bed.
    f32 footprint[kTexels] = {};

    void build() noexcept {
        const cy::Expected<Mat4, cy::Error> inverse = cy::inverse(world_to_clip());
        for (u32 texel = 0; texel < kTexels; ++texel) {
            rays[texel] = trace(*inverse, texel);
        }
        for (u32 texel = 0; texel < kTexels; ++texel) {
            const u32 x = texel % kExtent;
            const u32 y = texel / kExtent;
            if (x == 0 || y == 0 || x + 1 == kExtent || y + 1 == kExtent) {
                continue;
            }
            const Seen seen = rays[texel].seen;
            interior[texel] = rays[texel - 1].seen == seen && rays[texel + 1].seen == seen &&
                              rays[texel - kExtent].seen == seen &&
                              rays[texel + kExtent].seen == seen &&
                              rays[texel - 1].has_bed == rays[texel].has_bed &&
                              rays[texel + 1].has_bed == rays[texel].has_bed &&
                              rays[texel - kExtent].has_bed == rays[texel].has_bed &&
                              rays[texel + kExtent].has_bed == rays[texel].has_bed;
        }
        for (u32 texel = 0; texel < kTexels; ++texel) {
            footprint[texel] = neighbourhood_footprint(texel);
        }
    }

    [[nodiscard]] f32 step(u32 from, u32 to) const noexcept {
        if (!rays[from].has_bed || !rays[to].has_bed) {
            return 1.0e9F;
        }
        const Vec3 d = rays[to].bed - rays[from].bed;
        return std::sqrt((d.x * d.x) + (d.z * d.z));
    }

    [[nodiscard]] f32 neighbourhood_footprint(u32 texel) const noexcept {
        const u32 x = texel % kExtent;
        const u32 y = texel / kExtent;
        if (x < 2 || y < 2 || x + 2 >= kExtent || y + 2 >= kExtent) {
            return 1.0e9F;
        }
        f32 widest = 0.0F;
        for (u32 row = y - 1; row <= y + 1; ++row) {
            for (u32 column = x - 1; column <= x + 1; ++column) {
                const u32 at = (row * kExtent) + column;
                widest = cy::math::max(widest, cy::math::max(step(at, at + 1), step(at, at - 1)));
                widest = cy::math::max(
                    widest, cy::math::max(step(at, at + kExtent), step(at, at - kExtent)));
            }
        }
        return widest;
    }
};

/// Schlick's Fresnel for a flat surface seen along `direction`, with the closure's own F0.
[[nodiscard]] f32 fresnel(const Vec3& direction) noexcept {
    const f32 f0 =
        water::build_closure(water::clear_sea_optics(), Vec3{0.0F, 1.0F, 0.0F}, 0.0F, 0.0F).f0;
    const f32 cosine = cy::math::saturate(-direction.y);
    return f0 + ((1.0F - f0) * std::pow(1.0F - cosine, 5.0F));
}

// --- The device side
// ------------------------------------------------------------------------------

/// What one frame is asked to draw.
struct Shot {
    /// Through water.slang, or through world.slang as the stage draws with water shading off.
    bool water_shading = true;
    f32 sun = 1.0F;
    f32 ambient = 0.15F;
    /// Multiplies the ceiling's ramp; zero is a black sky.
    f32 sky = 1.0F;
    /// The water run's specular strength, `WorldPush::sun.w`. The stage's is 24.
    f32 specular = 0.0F;
    sample::WaterLook look = [] {
        sample::WaterLook quiet;
        quiet.foam_band_metres = 0.0F;
        quiet.caustic_strength = 0.0F;
        return quiet;
    }();
    f64 water_time = 0.0;
    cy::Span<const water::WaveTrain> trains;
};

struct Draw {
    u32 first = 0;
    bool water = false;
    WorldPush push;
};

struct PassState {
    cy::rendering::GraphExecutor* executor = nullptr;
    rhi::Device* device = nullptr;
    rhi::GraphicsPipelineHandle pipeline;
    rhi::PipelineLayoutHandle layout;
    rhi::DescriptorSetHandle cloud_shadow;
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
    info.render_area = rhi::Rect2D{0, 0, kExtent, kExtent};
    info.color_attachments = cy::Span<const rhi::RenderAttachment>(&color, 1);
    info.depth_attachment.view = state->executor->view(state->depth);
    info.depth_attachment.load = rhi::LoadOp::Clear;
    info.depth_attachment.store = rhi::StoreOp::Store;
    info.depth_attachment.clear = rhi::reversed_z_depth_clear();

    context.commands->begin_rendering(info);
    context.commands->set_viewport(rhi::Viewport{0.0F, 0.0F, static_cast<f32>(kExtent),
                                                 static_cast<f32>(kExtent), 0.0F, 1.0F});
    context.commands->set_scissor(rhi::Rect2D{0, 0, kExtent, kExtent});
    const rhi::BufferHandle streams[2] = {state->vertices, state->colours};
    const u64 offsets[2] = {0, 0};
    context.commands->bind_vertex_buffers(0, cy::Span<const rhi::BufferHandle>(streams, 2),
                                          cy::Span<const u64>(offsets, 2));
    for (u32 index = 0; index < state->draw_count; ++index) {
        const Draw& draw = state->draws[index];
        const bool water = draw.water && state->bound;
        if (draw.water && !water) {
            continue;
        }
        const rhi::PipelineLayoutHandle layout = water ? state->surface->layout() : state->layout;
        if (water) {
            const rhi::DescriptorSetHandle sets[2] = {state->cloud_shadow, state->surface->set()};
            context.commands->bind_graphics_pipeline(state->surface->pipeline());
            context.commands->bind_descriptor_sets(
                layout, 0, cy::Span<const rhi::DescriptorSetHandle>(sets, 2));
        } else {
            context.commands->bind_graphics_pipeline(state->pipeline);
            context.commands->bind_descriptor_sets(
                layout, 0, cy::Span<const rhi::DescriptorSetHandle>(&state->cloud_shadow, 1));
        }
        context.commands->push_constants(
            layout, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
            cy::Span<const u8>(reinterpret_cast<const u8*>(&draw.push), sizeof(WorldPush)));
        context.commands->draw(6, 1, draw.first, 0);
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
    region.texture_extent = rhi::Extent3D{kExtent, kExtent, 1};
    context.commands->copy_texture_to_buffer(state->executor->texture(state->color), state->buffer,
                                             cy::Span<const rhi::BufferTextureCopy>(&region, 1));
}

/// The world pipeline from the sample's committed SPIR-V, the sample's own water surface, and the
/// buffers a shot needs.
class WaterScene {
public:
    explicit WaterScene(cy::render_test::DeviceFixture& fixture) noexcept
        : fixture_(fixture), device_(fixture.device()) {}

    WaterScene(const WaterScene&) = delete;
    WaterScene& operator=(const WaterScene&) = delete;

    ~WaterScene() {
        (void)device_.wait_idle();
        surface_.destroy(device_);
        if (!pipeline_.is_null()) {
            device_.destroy_graphics_pipeline(pipeline_);
        }
        if (!layout_.is_null()) {
            device_.destroy_pipeline_layout(layout_);
        }
        if (!set_layout_.is_null()) {
            device_.destroy_descriptor_set_layout(set_layout_);
        }
        for (rhi::ShaderModuleHandle module : {vertex_, fragment_}) {
            if (!module.is_null()) {
                device_.destroy_shader_module(module);
            }
        }
        for (rhi::BufferHandle buffer : {vertices_, colours_, field_, placement_, readback_}) {
            if (!buffer.is_null()) {
                device_.destroy_buffer(buffer);
            }
        }
    }

    [[nodiscard]] cy::Status prepare() noexcept;
    [[nodiscard]] cy::Status shoot(const Shot& shot, Picture& out) noexcept;
    /// What `build_water_params()` handed the shader on the last shot.
    [[nodiscard]] const sample::WaterParams& params() const noexcept { return params_; }

private:
    [[nodiscard]] cy::Expected<rhi::BufferHandle, cy::Error> buffer(const char* name, u64 size,
                                                                    rhi::BufferUsage usage,
                                                                    rhi::MemoryUse memory) noexcept;
    void write_geometry(f32 sky) noexcept;
    [[nodiscard]] static WorldPush push_for(const Shot& shot) noexcept;

    cy::render_test::DeviceFixture& fixture_;
    rhi::Device& device_;
    rhi::ShaderModuleHandle vertex_;
    rhi::ShaderModuleHandle fragment_;
    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::DescriptorSetHandle set_;
    rhi::PipelineLayoutHandle layout_;
    rhi::GraphicsPipelineHandle pipeline_;
    rhi::BufferHandle vertices_;
    rhi::BufferHandle colours_;
    rhi::BufferHandle field_;
    rhi::BufferHandle placement_;
    rhi::BufferHandle readback_;
    sample::WaterSurface surface_;
    sample::WaterParams params_;
};

cy::Expected<rhi::BufferHandle, cy::Error> WaterScene::buffer(const char* name, u64 size,
                                                              rhi::BufferUsage usage,
                                                              rhi::MemoryUse memory) noexcept {
    rhi::BufferDescription description;
    description.name = name;
    description.size = size;
    description.usage = usage;
    description.memory = memory;
    return device_.create_buffer(description);
}

cy::Status WaterScene::prepare() noexcept {
    namespace shaders = cy::sample::world;
    const struct {
        rhi::ShaderModuleHandle* handle;
        const char* name;
        rhi::ShaderStage stage;
        cy::Span<const u32> spirv;
    } modules[2] = {
        {&vertex_, "world vertex", rhi::ShaderStage::Vertex,
         cy::Span<const u32>(shaders::kWorldVertexSpirv, std::size(shaders::kWorldVertexSpirv))},
        {&fragment_, "world fragment", rhi::ShaderStage::Fragment,
         cy::Span<const u32>(shaders::kWorldFragmentSpirv,
                             std::size(shaders::kWorldFragmentSpirv))},
    };
    for (const auto& entry : modules) {
        rhi::ShaderModuleDescription description;
        description.name = entry.name;
        description.stage = entry.stage;
        description.entry_point = "main";
        description.spirv = entry.spirv;
        auto created = device_.create_shader_module(description);
        if (!created) {
            return cy::make_unexpected(created.error());
        }
        *entry.handle = *created;
    }

    // Set 0 exactly as the stage makes it: the cloud shadow field and its placement, here saying
    // "off" — this suite is about the water, and full sun is the sun it reasons about.
    rhi::DescriptorBinding bindings[2] = {};
    for (u32 index = 0; index < 2; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = rhi::DescriptorKind::StorageBuffer;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Fragment;
    }
    rhi::DescriptorSetLayoutDescription set_description;
    set_description.name = "world cloud shadow";
    set_description.bindings = cy::Span<const rhi::DescriptorBinding>(bindings, 2);
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

    const rhi::VertexBinding streams[2] = {{0, sizeof(Vertex), rhi::VertexInputRate::PerVertex},
                                           {1, sizeof(f32) * 3, rhi::VertexInputRate::PerVertex}};
    const rhi::VertexAttribute attributes[3] = {{0, 0, rhi::Format::Rgb32Sfloat, 0},
                                                {1, 0, rhi::Format::Rgb32Sfloat, sizeof(f32) * 3},
                                                {2, 1, rhi::Format::Rgb32Sfloat, 0}};
    rhi::ColorAttachmentState color;
    color.format = rhi::Format::Rgba16Sfloat;
    rhi::GraphicsPipelineDescription description;
    description.name = "world";
    description.layout = layout_;
    description.vertex_shader = vertex_;
    description.fragment_shader = fragment_;
    description.vertex_bindings = cy::Span<const rhi::VertexBinding>(streams, 2);
    description.vertex_attributes = cy::Span<const rhi::VertexAttribute>(attributes, 3);
    description.color_attachments = cy::Span<const rhi::ColorAttachmentState>(&color, 1);
    description.rasterisation.cull_mode = rhi::CullMode::None;
    description.depth_stencil.format = rhi::Format::D32Sfloat;
    description.depth_stencil.depth_test_enable = true;
    description.depth_stencil.depth_write_enable = true;
    auto pipeline = device_.create_graphics_pipeline(description);
    if (!pipeline) {
        return cy::make_unexpected(pipeline.error());
    }
    pipeline_ = *pipeline;

    // THE SAMPLE'S OWN WATER SURFACE, on set 0's layout, at this suite's extent.
    if (cy::Status made =
            surface_.create(device_, set_layout_, rhi::Format::Rgba16Sfloat, kExtent, kExtent);
        !made) {
        return made;
    }

    auto vertices = buffer("scene", sizeof(Vertex) * kVertices, rhi::BufferUsage::Vertex,
                           rhi::MemoryUse::Upload);
    auto colours = buffer("scene colour", sizeof(f32) * 3 * kVertices, rhi::BufferUsage::Vertex,
                          rhi::MemoryUse::Upload);
    auto field = buffer("cloud shadow placeholder", 16 * sizeof(u32), rhi::BufferUsage::Storage,
                        rhi::MemoryUse::Upload);
    auto placement = buffer("cloud shadow placement", 4 * sizeof(f32), rhi::BufferUsage::Storage,
                            rhi::MemoryUse::Upload);
    auto readback = buffer("scene readback", static_cast<u64>(kTexels) * 4 * sizeof(u16),
                           rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback);
    if (!vertices || !colours || !field || !placement || !readback) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "a water scene buffer did not allocate");
    }
    vertices_ = *vertices;
    colours_ = *colours;
    field_ = *field;
    placement_ = *placement;
    readback_ = *readback;
    std::memset(device_.buffer_mapped_pointer(field_), 0, 16 * sizeof(u32));
    std::memset(device_.buffer_mapped_pointer(placement_), 0, 4 * sizeof(f32));

    auto set = device_.allocate_descriptor_set(set_layout_, false);
    if (!set) {
        return cy::make_unexpected(set.error());
    }
    set_ = *set;
    rhi::DescriptorWrite writes[2] = {};
    writes[0].binding = 0;
    writes[0].kind = rhi::DescriptorKind::StorageBuffer;
    writes[0].buffer = field_;
    writes[1].binding = 1;
    writes[1].kind = rhi::DescriptorKind::StorageBuffer;
    writes[1].buffer = placement_;
    return device_.update_descriptor_set(set_, cy::Span<const rhi::DescriptorWrite>(writes, 2));
}

void WaterScene::write_geometry(f32 sky) noexcept {
    auto* vertices = static_cast<Vertex*>(device_.buffer_mapped_pointer(vertices_));
    auto* colours = static_cast<f32*>(device_.buffer_mapped_pointer(colours_));
    const f32 corners[6][2] = {{-1.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F},
                               {-1.0F, 0.0F}, {1.0F, 1.0F}, {-1.0F, 1.0F}};
    const Vec3 bed_normal = cy::normalize(Vec3{0.0F, 1.0F, kBedSlope});
    for (u32 index = 0; index < 6; ++index) {
        const f32 u = corners[index][0];
        const f32 v = corners[index][1];
        // The ceiling: a square 40 km across, its colour the ramp at its own corners.
        const f32 cx = u * kCeilingHalf;
        const f32 cz = ((v * 2.0F) - 1.0F) * kCeilingHalf;
        vertices[kCeilingFirst + index] = Vertex{{cx, kCeiling, cz}, {0.0F, -1.0F, 0.0F}};
        const Vec3 ramp = sky_colour(cx, cz, sky);
        // The bed and the water share their footprint.
        const f32 x = u * kQuadHalfWidth;
        const f32 z = kQuadNear + (v * (kQuadFar - kQuadNear));
        vertices[kBedFirst + index] =
            Vertex{{x, bed_height(z), z}, {bed_normal.x, bed_normal.y, bed_normal.z}};
        vertices[kWaterFirst + index] = Vertex{{x, kLevel, z}, {0.0F, 1.0F, 0.0F}};
        const Vec3 colour[3] = {ramp, Vec3{kBedAlbedo, kBedAlbedo, kBedAlbedo}, kDeepWater};
        for (u32 quad = 0; quad < 3; ++quad) {
            f32* out = colours + (((static_cast<usize>(quad) * 6) + index) * 3);
            out[0] = colour[quad].x;
            out[1] = colour[quad].y;
            out[2] = colour[quad].z;
        }
    }
}

WorldPush WaterScene::push_for(const Shot& shot) noexcept {
    WorldPush push;
    const Mat4 matrix = world_to_clip();
    write_row(push.row0, matrix.row(0));
    write_row(push.row1, matrix.row(1));
    write_row(push.row2, matrix.row(2));
    write_row(push.row3, matrix.row(3));
    const Vec3 travel = light_travel();
    push.light[0] = travel.x;
    push.light[1] = travel.y;
    push.light[2] = travel.z;
    push.eye[0] = eye().x;
    push.eye[1] = eye().y;
    push.eye[2] = eye().z;
    for (u32 channel = 0; channel < 3; ++channel) {
        push.sun[channel] = shot.sun;
        push.ambient[channel] = shot.ambient;
    }
    return push;
}

cy::Status WaterScene::shoot(const Shot& shot, Picture& out) noexcept {
    write_geometry(shot.sky);
    const WorldPush lit = push_for(shot);
    WorldPush emissive = lit;
    emissive.eye[3] = 1.0F;
    WorldPush surface = lit;
    surface.sun[3] = shot.specular;

    sample::WaterView view;
    view.world_to_clip = world_to_clip();
    view.width = kExtent;
    view.height = kExtent;
    view.level = kLevel;
    view.water_time = shot.water_time;
    auto params =
        sample::build_water_params(water::clear_sea_optics(), shot.look, view, shot.trains);
    if (!params) {
        return cy::make_unexpected(params.error());
    }
    params_ = *params;
    if (cy::Status uploaded = surface_.upload(device_, params_); !uploaded) {
        return uploaded;
    }

    if (cy::Expected<u32, cy::Error> began = device_.begin_frame(); !began) {
        return cy::make_unexpected(began.error());
    }
    cy::rendering::RenderGraph graph(fixture_.allocator());
    cy::rendering::GraphExecutor executor(fixture_.allocator(), device_);

    PassState base;
    base.executor = &executor;
    base.device = &device_;
    base.pipeline = pipeline_;
    base.layout = layout_;
    base.cloud_shadow = set_;
    base.vertices = vertices_;
    base.colours = colours_;
    base.surface = &surface_;

    // THE TWO PICTURES, as `Stage::declare_water` declares them: the bed alone from the frame's
    // camera, and the world mirrored in the water with the near plane on it.
    PassState refraction = base;
    PassState reflection = base;
    sample::WaterTargets targets;
    if (shot.water_shading) {
        targets = surface_.import(graph);
        refraction.color = targets.refraction;
        refraction.depth = targets.refraction_depth;
        refraction.draws[0] = Draw{kBedFirst, false, lit};
        refraction.draw_count = 1;

        Vec4 rows[4];
        sample::mirrored_rows(world_to_clip(), kLevel, rows);
        WorldPush mirrored_emissive = emissive;
        WorldPush mirrored_lit = lit;
        for (WorldPush* push : {&mirrored_emissive, &mirrored_lit}) {
            write_row(push->row0, rows[0]);
            write_row(push->row1, rows[1]);
            write_row(push->row2, rows[2]);
            write_row(push->row3, rows[3]);
        }
        reflection.color = targets.reflection;
        reflection.depth = targets.reflection_depth;
        reflection.draws[0] = Draw{kCeilingFirst, false, mirrored_emissive};
        reflection.draws[1] = Draw{kBedFirst, false, mirrored_lit};
        reflection.draw_count = 2;

        graph.add_pass("water refraction", rhi::QueueKind::Graphics)
            .write(targets.refraction, rhi::Access::ColorAttachmentWrite)
            .write(targets.refraction_depth, rhi::Access::DepthStencilAttachmentWrite)
            .record(&record_pass, &refraction);
        graph.add_pass("water reflection", rhi::QueueKind::Graphics)
            .write(targets.reflection, rhi::Access::ColorAttachmentWrite)
            .write(targets.reflection_depth, rhi::Access::DepthStencilAttachmentWrite)
            .record(&record_pass, &reflection);
    }

    // THE FRAME: the sky, the bed, and the water — through water.slang, or through world.slang
    // with the stage's specular lobe, which is the frame with water shading off.
    PassState frame = base;
    cy::rendering::TextureRequest color_request;
    color_request.name = "scene colour";
    color_request.format = rhi::Format::Rgba16Sfloat;
    color_request.width = kExtent;
    color_request.height = kExtent;
    frame.color = graph.create_texture(color_request);
    cy::rendering::TextureRequest depth_request;
    depth_request.name = "scene depth";
    depth_request.format = rhi::Format::D32Sfloat;
    depth_request.width = kExtent;
    depth_request.height = kExtent;
    frame.depth = graph.create_texture(depth_request);
    frame.draws[0] = Draw{kCeilingFirst, false, emissive};
    frame.draws[1] = Draw{kBedFirst, false, lit};
    frame.draws[2] = Draw{kWaterFirst, shot.water_shading, surface};
    frame.draw_count = 3;
    frame.binds_water = shot.water_shading;
    frame.targets = targets;
    auto builder = graph.add_pass("scene", rhi::QueueKind::Graphics);
    builder.write(frame.color, rhi::Access::ColorAttachmentWrite)
        .write(frame.depth, rhi::Access::DepthStencilAttachmentWrite);
    if (shot.water_shading) {
        builder.read(targets.refraction, rhi::Access::FragmentSampledRead)
            .read(targets.refraction_depth, rhi::Access::FragmentSampledRead)
            .read(targets.reflection, rhi::Access::FragmentSampledRead);
    }
    builder.record(&record_pass, &frame);

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

/// The bed's radiance as world.slang lit it: albedo times ambient plus the sun's Lambert term.
[[nodiscard]] f32 bed_radiance(const Shot& shot) noexcept {
    const Vec3 normal = cy::normalize(Vec3{0.0F, 1.0F, kBedSlope});
    const f32 lambert = cy::math::saturate(cy::dot(normal, -light_travel()));
    return kBedAlbedo * (shot.ambient + (shot.sun * lambert));
}

/// The light the column scatters back, as water.slang lights it: the sky and the sun on a level
/// surface.
[[nodiscard]] f32 column_light(const Shot& shot) noexcept {
    return shot.ambient + (shot.sun * cy::math::saturate(-light_travel().y));
}

/// The refracted radiance under a water pixel: src/water/'s transmittance and in-scatter over the
/// path from the surface to the bed, with the bed's own light scaled by `bed_scale`.
[[nodiscard]] Vec3 refracted(const Shot& shot, const Ray& ray, f32 bed_scale) noexcept {
    const water::WaterOptics optics = water::clear_sea_optics();
    const f32 path = ray.has_bed ? cy::length(ray.bed - ray.surface) : 1.0e4F;
    const Vec3 transmittance = water::water_transmittance(optics, path);
    const Vec3 scatter = water::water_in_scatter(optics, path);
    const f32 bed = ray.has_bed ? bed_radiance(shot) * bed_scale : 0.0F;
    const f32 light = column_light(shot);
    return Vec3{(bed * transmittance.x) + (scatter.x * light),
                (bed * transmittance.y) + (scatter.y * light),
                (bed * transmittance.z) + (scatter.z * light)};
}

/// water.slang's `causticFocus`, over the same parameter block, in the same f32 arithmetic.
[[nodiscard]] f32 caustic_focus(const sample::WaterParams& params, f32 x, f32 z,
                                f32 depth) noexcept {
    f32 laplacian = 0.0F;
    const auto count = static_cast<u32>(params.caustic[0]);
    for (u32 index = 0; index < count; ++index) {
        const f32(&train)[4] = params.trains[index];
        const f32 theta = (((train[0] * x) + (train[1] * z)) * train[2]) + params.phases[index];
        laplacian -= train[3] * train[2] * train[2] * std::cos(theta);
    }
    const f32 jacobian = 1.0F + (depth * params.surface[3] * laplacian);
    return cy::math::min(1.0F / cy::math::max(std::fabs(jacobian), 1e-4F), params.caustic[1]);
}

/// How far a pixel's radiance is from what was expected, relative, per the worst channel. A floor
/// under the denominator keeps a near-black expectation from turning rounding into a failure.
[[nodiscard]] f32 relative_error(const Vec3& measured, const Vec3& expected) noexcept {
    const auto one = [](f32 got, f32 want) noexcept {
        return std::fabs(got - want) / cy::math::max(want, 0.02F);
    };
    return cy::math::max(one(measured.x, expected.x),
                         cy::math::max(one(measured.y, expected.y), one(measured.z, expected.z)));
}

/// A sea state with strong caustics: trains long enough that this suite's pixels resolve them over
/// the near shallows, and far taller than a real swell of that length, so that over a column of a
/// few metres the surface still bends the sun by more than a tenth.
[[nodiscard]] water::DisplacementModel caustic_sea() noexcept {
    water::DisplacementModel model;
    model.seed = 0xCA05ULL;
    water::DisplacementBand band;
    band.wavelength_min = 8.0F;
    band.wavelength_max = 24.0F;
    band.amplitude = 4.0F;
    band.direction_degrees = 20.0F;
    band.spread_degrees = 25.0F;
    band.steepness = 0.5F;
    band.wave_count = 4;
    (void)model.add(band);
    return model;
}

const char* reference_path() noexcept {
    static char storage[1024];
    std::snprintf(storage, sizeof(storage), "%s/references/world_water_off.png",
                  CY_RENDER_TEST_DIR);
    return storage;
}

bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

/// Everything the cases share.
struct Scene {
    cy::render_test::DeviceFixture fixture{"vulkan", "cy_test_render_world_water"};
    Classes* classes = nullptr;

    Scene() noexcept {
        classes = new (std::nothrow) Classes();
        if (classes != nullptr) {
            classes->build();
        }
    }
    ~Scene() { delete classes; }
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    [[nodiscard]] bool have_vulkan() const noexcept { return fixture.is(rhi::BackendKind::Vulkan); }
};

}  // namespace

CY_TEST_CASE("world water: off, the frame is the frame before water shading") {
    Scene scene;
    if (!scene.have_vulkan()) {
        scene.fixture.report_skip();
        return;
    }
    WaterScene pass(scene.fixture);
    CY_REQUIRE(pass.prepare());

    // The stage's own water run with shading off: world.slang's lit path, the stage's specular.
    Shot off;
    off.water_shading = false;
    off.specular = 24.0F;
    Shot on = off;
    on.water_shading = true;
    on.look = sample::WaterLook{};
    Picture before(scene.fixture.allocator());
    Picture shaded(scene.fixture.allocator());
    CY_REQUIRE(pass.shoot(off, before));
    CY_REQUIRE(pass.shoot(on, shaded));

    cy::render_test::Image rendered(scene.fixture.allocator());
    CY_REQUIRE(to_image(before, rendered));
    cy::render_test::Image moved(scene.fixture.allocator());
    CY_REQUIRE(to_image(shaded, moved));

    if (updating_references()) {
        CY_CHECK(cy::render_test::write_png(reference_path(), rendered).has_value());
        std::fprintf(stderr,
                     "CY_RENDER_UPDATE_GOLDEN: wrote %s. Look at it, then commit it — this run "
                     "fails on purpose so that a regenerating run can never be a passing one.\n",
                     reference_path());
        CY_CHECK_FALSE(updating_references());
        return;
    }
    cy::render_test::Image reference(scene.fixture.allocator());
    const cy::Status read = cy::render_test::read_png(reference_path(), reference);
    CY_REQUIRE(read.has_value());
    CY_REQUIRE_EQ(reference.texels.size(), rendered.texels.size());

    // BYTE FOR BYTE, and not within a tolerance: the same shaders on the same machine draw the same
    // bytes, and a water run that picked up anything from this change would move some of them.
    u32 off_differs = 0;
    u32 on_differs = 0;
    for (usize texel = 0; texel < reference.texels.size(); ++texel) {
        off_differs += reference.texels[texel] != rendered.texels[texel] ? 1U : 0U;
        on_differs += reference.texels[texel] != moved.texels[texel] ? 1U : 0U;
    }
    CY_TEST_MESSAGE("texels differing from the reference drawn before water shading: off ",
                    off_differs, ", on ", on_differs, "; ", scene.fixture.validation_errors(),
                    " validation errors");
    CY_CHECK_EQ(off_differs, 0U);
    // And the comparison can fail: shaded, the water moves.
    CY_CHECK_GT(on_differs, 2000U);
    CY_CHECK_EQ(scene.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("world water: the reflection is the sky mirrored in the surface, by Fresnel") {
    Scene scene;
    if (!scene.have_vulkan()) {
        scene.fixture.report_skip();
        return;
    }
    CY_REQUIRE(scene.classes != nullptr);
    WaterScene pass(scene.fixture);
    CY_REQUIRE(pass.prepare());

    // NOTHING BUT THE REFLECTION: an unlit bed and no light for the column to scatter, so every
    // water pixel is Fresnel times whatever the mirrored picture holds there.
    Shot shot;
    shot.sun = 0.0F;
    shot.ambient = 0.0F;
    Picture picture(scene.fixture.allocator());
    CY_REQUIRE(pass.shoot(shot, picture));

    u32 compared = 0;
    u32 within = 0;
    f32 worst = 0.0F;
    f32 dimmest = 1.0e9F;
    f32 brightest = 0.0F;
    for (u32 texel = 0; texel < kTexels; ++texel) {
        const Ray& ray = scene.classes->rays[texel];
        if (!scene.classes->interior[texel] || ray.seen != Seen::Water) {
            continue;
        }
        // Where the MIRRORED eye ray meets the ceiling.
        const Vec3 up{ray.direction.x, -ray.direction.y, ray.direction.z};
        const f32 reach = (kCeiling - ray.surface.y) / up.y;
        const Vec3 at = ray.surface + (up * reach);
        const Vec3 sky = sky_colour(at.x, at.z, shot.sky);
        const f32 f = fresnel(ray.direction);
        const Vec3 expected{sky.x * f, sky.y * f, sky.z * f};
        const Vec3 measured = radiance_at(picture, texel);
        const f32 error = relative_error(measured, expected);
        worst = cy::math::max(worst, error);
        within += error < 0.02F ? 1U : 0U;
        dimmest = cy::math::min(dimmest, measured.z);
        brightest = cy::math::max(brightest, measured.z);
        ++compared;
    }
    CY_TEST_MESSAGE(compared, " water texels, ", within,
                    " within 2% of Fresnel times the mirrored "
                    "sky, worst ",
                    worst, "; blue from ", dimmest, " to ", brightest, "; ",
                    scene.fixture.validation_errors(), " validation errors");
    CY_CHECK_GT(compared, 3000U);
    CY_CHECK_EQ(within, compared);
    // Fresnel is doing something: grazing water is several times brighter than steep water.
    CY_CHECK_GT(brightest, dimmest * 4.0F);
    CY_CHECK_EQ(scene.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("world water: deeper water is darker, by the absorption law") {
    Scene scene;
    if (!scene.have_vulkan()) {
        scene.fixture.report_skip();
        return;
    }
    CY_REQUIRE(scene.classes != nullptr);
    WaterScene pass(scene.fixture);
    CY_REQUIRE(pass.prepare());

    // NOTHING BUT THE COLUMN: a black sky, so the reflection adds nothing, and a lit bed.
    Shot shot;
    shot.sky = 0.0F;
    Picture picture(scene.fixture.allocator());
    CY_REQUIRE(pass.shoot(shot, picture));

    // Bins of path length, a metre and a half wide, to show the trend the law predicts.
    constexpr u32 kBins = 12;
    constexpr f32 kBinMetres = 1.5F;
    f32 red[kBins] = {};
    f32 blue[kBins] = {};
    u32 count[kBins] = {};
    u32 compared = 0;
    u32 within = 0;
    f32 worst = 0.0F;
    for (u32 texel = 0; texel < kTexels; ++texel) {
        const Ray& ray = scene.classes->rays[texel];
        if (!scene.classes->interior[texel] || ray.seen != Seen::Water || !ray.has_bed) {
            continue;
        }
        const Vec3 column = refracted(shot, ray, 1.0F);
        const f32 transmitted = 1.0F - fresnel(ray.direction);
        const Vec3 expected{column.x * transmitted, column.y * transmitted, column.z * transmitted};
        const Vec3 measured = radiance_at(picture, texel);
        const f32 error = relative_error(measured, expected);
        worst = cy::math::max(worst, error);
        within += error < 0.02F ? 1U : 0U;
        ++compared;
        const auto bin = static_cast<u32>(cy::length(ray.bed - ray.surface) / kBinMetres);
        if (bin < kBins) {
            red[bin] += measured.x;
            blue[bin] += measured.z;
            ++count[bin];
        }
    }
    u32 populated = 0;
    u32 darker = 0;
    u32 bluer = 0;
    f32 previous_red = 1.0e9F;
    f32 previous_ratio = 1.0e9F;
    for (u32 bin = 0; bin < kBins; ++bin) {
        if (count[bin] < 20) {
            continue;
        }
        const f32 mean_red = red[bin] / static_cast<f32>(count[bin]);
        const f32 ratio = red[bin] / blue[bin];
        if (populated > 0) {
            darker += mean_red < previous_red ? 1U : 0U;
            bluer += ratio < previous_ratio ? 1U : 0U;
        }
        previous_red = mean_red;
        previous_ratio = ratio;
        ++populated;
    }
    CY_TEST_MESSAGE(compared, " water texels over the bed, ", within,
                    " within 2% of the "
                    "absorption law, worst ",
                    worst, "; ", populated, " depth bins, ", darker,
                    " darker than the one before and ", bluer, " bluer; ",
                    scene.fixture.validation_errors(), " validation errors");
    CY_CHECK_GT(compared, 2000U);
    CY_CHECK_EQ(within, compared);
    CY_CHECK_GE(populated, 6U);
    CY_CHECK_EQ(darker, populated - 1);
    CY_CHECK_EQ(bluer, populated - 1);
    CY_CHECK_EQ(scene.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("world water: shoreline foam is only where the column is shallow") {
    Scene scene;
    if (!scene.have_vulkan()) {
        scene.fixture.report_skip();
        return;
    }
    CY_REQUIRE(scene.classes != nullptr);
    WaterScene pass(scene.fixture);
    CY_REQUIRE(pass.prepare());

    Shot without;
    Shot with = without;
    with.look.foam_band_metres = 1.2F;
    Picture plain(scene.fixture.allocator());
    Picture foamed(scene.fixture.allocator());
    CY_REQUIRE(pass.shoot(without, plain));
    CY_REQUIRE(pass.shoot(with, foamed));

    u32 deep = 0;
    u32 deep_changed = 0;
    u32 shallow = 0;
    u32 shallow_brighter = 0;
    for (u32 texel = 0; texel < kTexels; ++texel) {
        const Ray& ray = scene.classes->rays[texel];
        if (!scene.classes->interior[texel] || ray.seen != Seen::Water) {
            continue;
        }
        const f32 column = ray.has_bed ? kLevel - ray.bed.y : 1.0e4F;
        if (column > with.look.foam_band_metres + 0.05F) {
            ++deep;
            deep_changed += same_texel(plain, foamed, texel) ? 0U : 1U;
        } else if (column < with.look.foam_band_metres * 0.5F) {
            ++shallow;
            const Vec3 a = radiance_at(plain, texel);
            const Vec3 b = radiance_at(foamed, texel);
            shallow_brighter += (b.x + b.y + b.z) > (a.x + a.y + a.z) * 1.1F ? 1U : 0U;
        }
    }
    CY_TEST_MESSAGE(deep, " texels over a column deeper than the band (", deep_changed,
                    " changed), ", shallow, " in its shallow half (", shallow_brighter,
                    " brighter); ", scene.fixture.validation_errors(), " validation errors");
    CY_CHECK_GT(deep, 3000U);
    CY_CHECK_EQ(deep_changed, 0U);
    CY_CHECK_GT(shallow, 40U);
    CY_CHECK_EQ(shallow_brighter, shallow);
    CY_CHECK_EQ(scene.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("world water: caustics move with the water's clock and follow its surface") {
    Scene scene;
    if (!scene.have_vulkan()) {
        scene.fixture.report_skip();
        return;
    }
    CY_REQUIRE(scene.classes != nullptr);
    WaterScene pass(scene.fixture);
    CY_REQUIRE(pass.prepare());

    const water::DisplacementModel sea = caustic_sea();
    water::WaveTrain trains[sample::kCausticTrains];
    const u32 train_count = sample::select_caustic_trains(sea, cy::Span<water::WaveTrain>(trains));
    CY_REQUIRE(train_count > 0U);

    // A black sky, so the reflection adds nothing and each pixel is the refracted bed alone.
    Shot still;
    still.sky = 0.0F;
    still.trains = cy::Span<const water::WaveTrain>(trains, train_count);
    still.water_time = 10.0;
    Shot moving = still;
    moving.look.caustic_strength = 1.0F;

    Picture plain_early(scene.fixture.allocator());
    Picture plain_late(scene.fixture.allocator());
    Picture early(scene.fixture.allocator());
    Picture late(scene.fixture.allocator());
    CY_REQUIRE(pass.shoot(still, plain_early));
    moving.water_time = 10.0;
    CY_REQUIRE(pass.shoot(moving, early));
    const sample::WaterParams early_params = pass.params();
    moving.water_time = 11.3;
    CY_REQUIRE(pass.shoot(moving, late));
    const sample::WaterParams late_params = pass.params();
    still.water_time = 11.3;
    CY_REQUIRE(pass.shoot(still, plain_late));

    // The bed's share of the sun, and how much of the sun reaches it — water.slang's own terms.
    const f32 sun_on_bed = still.sun * cy::math::saturate(-light_travel().y);
    const f32 share = sun_on_bed / (sun_on_bed + still.ambient);
    const f32 green =
        water::clear_sea_optics().absorption.y + water::clear_sea_optics().scattering.y;

    f32 shortest_k = 0.0F;
    for (u32 index = 0; index < train_count; ++index) {
        shortest_k =
            cy::math::max(shortest_k, (2.0F * std::numbers::pi_v<f32>) / trains[index].wavelength);
    }
    u32 compared = 0;
    u32 resolved = 0;
    u32 moved = 0;
    u32 plain_moved = 0;
    u32 within = 0;
    u32 focused = 0;
    f32 worst = 0.0F;
    for (u32 texel = 0; texel < kTexels; ++texel) {
        const Ray& ray = scene.classes->rays[texel];
        if (!scene.classes->interior[texel] || ray.seen != Seen::Water || !ray.has_bed) {
            continue;
        }
        ++compared;
        moved += same_texel(early, late, texel) ? 0U : 1U;
        plain_moved += same_texel(plain_early, plain_late, texel) ? 0U : 1U;
        const f32 depth = kLevel - ray.bed.y;
        const f32 reach = std::exp(-green * depth);
        // Compared only where every train is resolved at full strength: there the device's
        // footprint filter is exactly one and the focus is the unfiltered Laplacian's.
        if (shortest_k * scene.classes->footprint[texel] * (2.0F / std::numbers::pi_v<f32>) >
            0.8F) {
            continue;
        }
        ++resolved;
        for (const auto* pair : {&early_params, &late_params}) {
            const Picture& picture = pair == &early_params ? early : late;
            const f32 focus = caustic_focus(*pair, ray.bed.x, ray.bed.z, depth);
            focused += std::fabs(focus - 1.0F) > 0.1F ? 1U : 0U;
            const f32 scale = 1.0F + (share * reach * (focus - 1.0F));
            const Vec3 column = refracted(still, ray, scale);
            const f32 transmitted = 1.0F - fresnel(ray.direction);
            const Vec3 expected{column.x * transmitted, column.y * transmitted,
                                column.z * transmitted};
            const f32 error = relative_error(radiance_at(picture, texel), expected);
            worst = cy::math::max(worst, error);
            within += error < 0.03F ? 1U : 0U;
        }
    }
    CY_TEST_MESSAGE(train_count, " trains; ", compared, " texels over the bed, ", moved,
                    " moved between the two instants (", plain_moved, " without caustics), ",
                    resolved, " with every train resolved, ", within, " of ", resolved * 2,
                    " readings within 3% of the surface's own focus, worst ", worst, "; ", focused,
                    " readings focused or spread by more than 10%; ",
                    scene.fixture.validation_errors(), " validation errors");
    CY_CHECK_GT(compared, 2000U);
    // They move, and nothing else does.
    CY_CHECK_GT(moved, compared / 3);
    CY_CHECK_EQ(plain_moved, 0U);
    // They are the surface's: the bed's light at each instant is the Laplacian's focus. Allowed a
    // hundredth of the pixels for a Jacobian near zero, where one ulp of the phase is a large step.
    CY_CHECK_GT(resolved, 1500U);
    CY_CHECK_GE(within * 100, resolved * 2 * 99);
    // And the comparison is about something: the surface bends the light by more than a tenth.
    CY_CHECK_GT(focused, resolved / 4);
    CY_CHECK_EQ(scene.fixture.validation_errors(), 0U);
}
