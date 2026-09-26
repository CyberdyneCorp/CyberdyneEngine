// SPDX-License-Identifier: MIT
// CLOUD SHADOWS IN THE WORLD FRAME, DRAWN BY THE WORLD'S OWN SHADERS ON A DEVICE.
//
// `atmosphere-sky-and-clouds` — "Cloud shadows": "Clouds SHALL cast shadows onto the world through
// a COARSE WORLD-SCALE SHADOW REPRESENTATION ... consumed by terrain, foliage, water, and
// illumination", and the scenario "WHEN clouds drift over terrain THEN a coarse shadow field SHALL
// darken the affected area, sampled by surfaces and by illumination".
//
// The frame that answers it is `samples/10-world`'s: one lit fragment path draws its terrain, its
// foliage and its water, and that path samples the `cloud-shadow` field per fragment through
// `cy/cloud_shadow.slang`. This suite draws with THAT pipeline's committed SPIR-V —
// `samples/10-world/shaders/world_spirv.h`, the bytes the sample ships — over ground lit by a real
// `sky::CloudShadowField` that has marched one known cloud, and reads the pixels back. So what is
// asserted is the shipped shader under the shipped producer, not a probe written to agree with
// them.
//
// ================================================================================================
// FOUR CLAIMS, AND THE PIXEL CLASSES THEY ARE MADE OVER
// ================================================================================================
//
// Every pixel is classified by the PROCESSOR's reading of the same image bytes
// (`environment::sample_field_image`, the shader's algorithm on the CPU): under the cloud, where
// the field lets less than half the sun through, and in full sun, where it lets all of it through.
//
//   * UNDER THE CLOUD THE GROUND IS DARKER, and by the field's amount: each shadowed pixel's
//     radiance is the Lambert term with the sun multiplied by the processor's field value, to the
//     precision of the half-float target. A shader that sampled the field at the wrong place, or
//     sampled it and ignored it, misses one or the other.
//   * IN FULL SUN THE GROUND IS UNCHANGED, bit for bit, against the same frame with cloud shadows
//     off. A shadow that leaked past its cloud — a field read at the wrong origin, or a sun
//     re-scaled to compensate — changes these.
//   * OFF, AND UNDER A CLEAR SKY, THE FRAME IS THE SAME FRAME. Every pixel, bit for bit, against
//     the frame the world's shaders drew BEFORE cloud shadows
//     (`world_before_cloud_shadows_spirv.h`, pinned at 2614fb0): the frame with the field disabled
//     and the frame with the field enabled over a sky with no cloud in it. "Cloud shadows cost
//     nothing where there is no cloud" is what keeps every committed capture of this world valid.
//   * THE SKY AND THE AMBIENT TERM ARE NOT ATTENUATED. The emissive path the dome is drawn with,
//     and ground lit by the ambient term alone, are bit-identical with the field on and off, over
//     the darkest cloud there is. A cloud redistributes skylight rather than removing it — the
//     ambient term is already the sky's own mean radiance under that cloud — and multiplying it
//     again would darken an overcast day twice.
//
// ================================================================================================
// THE MUTATIONS THIS SUITE WAS PROVED AGAINST
// ================================================================================================
//
// Each applied to the shader, the SPIR-V regenerated, the suite run and seen red, the file restored
// and md5-verified: `openspec/changes/add-cloud-shadows/evidence/falsification.txt` records each
// mutation and the assertions it turned red.

#include <cy/test/test.h>

#include <cy/backends/rhi/access.h>
#include <cy/core/math/math.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/environment/field.h>
#include <cy/environment/gpu.h>
#include <cy/environment/store.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/sky/cloud_shadows.h>
#include <cy/rendering/sky/clouds.h>
#include <cy/world/coordinates.h>

#include "10-world/shaders/world_spirv.h"
#include "device.h"
#include "world_before_cloud_shadows_spirv.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <utility>

namespace {

using cy::f32;
using cy::f64;
using cy::i32;
using cy::u16;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;
using cy::Vec3;
using cy::world::WorldVec3d;
namespace rhi = cy::rhi;
namespace sky = cy::rendering::sky;

[[nodiscard]] cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

[[nodiscard]] cy::world::PartitionConfig partition() noexcept {
    cy::world::PartitionConfig config;
    config.partition = 1;
    config.base_cell_size = 128.0F;
    config.levels = 3;
    config.level_ratio = 4;
    return config;
}

// --- The cloud -----------------------------------------------------------------------------------

/// A kilometre-cell weather map, clear except for one cell; one layer. The same shape
/// `src/rendering/sky/tests/test_cloud_shadows.cpp` gives its `KnownCloud`, spelled here rather
/// than shared because a render suite reaching into another module's test directory would couple
/// two directories' fixtures for twenty lines.
constexpr i32 kCloudCell = 20;
constexpr f32 kMapCellMetres = 1000.0F;

[[nodiscard]] f64 cloud_centre() noexcept {
    return (static_cast<f64>(kCloudCell) + 0.5) * static_cast<f64>(kMapCellMetres);
}

struct Sky {
    sky::CloudWeatherMap map;
    sky::CloudField field;

    /// `with_cloud` false is the clear sky: the same map, the same layer, and no cell covered.
    [[nodiscard]] cy::Status build(bool with_cloud) noexcept {
        if (cy::Status configured = map.configure(40, kMapCellMetres); !configured) {
            return configured;
        }
        if (with_cloud) {
            if (cy::Status set = map.set(kCloudCell, kCloudCell, 1.0F, 0.5F, 1.0F); !set) {
                return set;
            }
        }
        field.map = &map;
        field.layers = sky::CloudLayerSet{};
        field.seed = 0xC10D5ULL;
        return field.layers.add(sky::default_cloud_layers().layers[0]);
    }
};

/// The sun, from the ground: forty-odd degrees up, so the shadow lands a kilometre from its cloud.
[[nodiscard]] Vec3 sun_direction() noexcept {
    return cy::normalize(Vec3{0.3F, 0.8F, 0.2F});
}

/// The world-relative origin every vertex is measured from, as `samples/10-world` measures its own
/// from `World::centre()`: the ground the known cloud's shadow falls on.
[[nodiscard]] WorldVec3d frame_centre() noexcept {
    return WorldVec3d{cloud_centre() - 800.0, 0.0, cloud_centre() - 550.0};
}

/// Metres from the frame's centre to its edge. The picture is a square of ground this wide either
/// way, seen from straight above.
constexpr f32 kHalfExtent = 2400.0F;

/// The producer, the store it writes and the image the pass binds, kept together because the image
/// is a view of the store's tiles and the store is a view of the producer's claim.
struct ShadowField {
    cy::environment::FieldRegistry registry;
    cy::environment::FieldStore store;
    sky::CloudShadowField producer;
    cy::environment::FieldGpuImage image;

    ShadowField() noexcept
        : registry(allocator()), store(allocator(), registry, partition()), image(allocator()) {}

    [[nodiscard]] cy::Status build(const sky::CloudField& clouds) noexcept {
        sky::CloudShadowQuality quality;
        quality.regional_cell_metres = 128.0F;
        quality.macro_cell_metres = 1024.0F;
        quality.radius_metres = 3072.0F;
        quality.steps = 12;
        if (cy::Status attached = producer.attach(registry, quality); !attached) {
            return attached;
        }
        cy::Expected<bool, cy::Error> wrote =
            producer.update(store, clouds, sun_direction(), 0.0, frame_centre(), 1.0F);
        if (!wrote) {
            return cy::make_unexpected(wrote.error());
        }
        cy::Expected<cy::environment::FieldGpuImage, cy::Error> built =
            cy::environment::build_field_image(store, sky::cloud_shadow_field_id(),
                                               cy::environment::FieldResidency::Regional);
        if (!built) {
            return cy::make_unexpected(built.error());
        }
        image = std::move(built).value();
        return cy::ok();
    }

    /// The processor's answer for a world-relative position: the shader's algorithm over the bytes
    /// the shader is given.
    [[nodiscard]] f32 at(f32 relative_x, f32 relative_z) const noexcept {
        const WorldVec3d centre = frame_centre();
        const WorldVec3d absolute{centre.x + static_cast<f64>(relative_x), 0.0,
                                  centre.z + static_cast<f64>(relative_z)};
        const cy::environment::FieldImageLocal local =
            cy::environment::image_local(image, absolute);
        const f32 value =
            cy::environment::sample_field_image(image.words.span(), local.x, local.y, local.z).x();
        return cy::math::saturate(value);
    }
};

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

/// `WorldCloudShadow::placement` in world.slang.
struct Placement {
    f32 origin_x = 0.0F;
    f32 origin_z = 0.0F;
    f32 enabled = 0.0F;
    f32 unused = 0.0F;
};

/// One vertex of binding 0: position and normal, world-relative.
struct GroundVertex {
    f32 position[3];
    f32 normal[3];
};

constexpr u32 kExtent = 96;
constexpr u32 kTexels = kExtent * kExtent;
constexpr u32 kGroundVertices = 6;
constexpr f32 kAlbedo = 0.5F;

/// One picture, as the half-float bits the target holds: four per texel.
struct Picture {
    cy::Array<u16> bits;
    explicit Picture(cy::Allocator& memory) noexcept : bits(memory) {}
    [[nodiscard]] u16 red(u32 texel) const noexcept { return bits[static_cast<usize>(texel) * 4]; }
};

/// What one draw is asked to do.
struct Shot {
    bool cloud_shadows = true;
    /// The field this draw binds, or null to bind the placeholder the sample binds before its
    /// first frame.
    const ShadowField* field = nullptr;
    /// Draw the ground through the emissive path the sky dome uses, rather than lit.
    bool emissive = false;
    /// The sun's colour. Zero lights the ground by the ambient term alone.
    f32 sun = 1.0F;
    /// Draw with the world's shaders as they were before cloud shadows existed
    /// (`world_before_cloud_shadows_spirv.h`), which read no field at all.
    bool before = false;
};

[[nodiscard]] f32 half_to_float(u16 bits) noexcept {
    const u32 sign = (static_cast<u32>(bits) & 0x8000U) << 16U;
    const u32 exponent = (static_cast<u32>(bits) >> 10U) & 0x1FU;
    const u32 mantissa = static_cast<u32>(bits) & 0x3FFU;
    u32 out = 0;
    if (exponent == 0) {
        // Subnormal or zero: the target never holds a subnormal this suite depends on, and a zero
        // is exact either way.
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

/// `tonemap()` in world.slang: Reinhard, then the display gamma. The fragment stage applies it
/// itself, so the target holds display values and the expectation has to be one too.
[[nodiscard]] f32 world_tonemap(f32 radiance) noexcept {
    const f32 mapped = radiance / (1.0F + radiance);
    return std::pow(cy::math::saturate(mapped), 1.0F / 2.2F);
}

/// The world-relative ground position a texel's centre sees. The pass maps x to clip x and z to
/// clip y, and the Vulkan backend flips the viewport so that clip y points UP the image: row zero
/// is the far edge in +z.
[[nodiscard]] Vec3 ground_at(u32 texel) noexcept {
    const u32 column = texel % kExtent;
    const u32 row = texel / kExtent;
    const f32 u = (((static_cast<f32>(column) + 0.5F) / static_cast<f32>(kExtent)) * 2.0F) - 1.0F;
    const f32 v = 1.0F - (((static_cast<f32>(row) + 0.5F) / static_cast<f32>(kExtent)) * 2.0F);
    return Vec3{u * kHalfExtent, 0.0F, v * kHalfExtent};
}

// --- The fixture ---------------------------------------------------------------------------------

/// The world pipeline, created from the sample's committed SPIR-V, and the buffers a shot needs.
class WorldPass {
public:
    explicit WorldPass(cy::render_test::DeviceFixture& fixture) noexcept
        : fixture_(fixture), device_(fixture.device()) {}

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
             {vertices_, colours_, field_, placement_, placeholder_, readback_}) {
            if (!buffer.is_null()) {
                device_.destroy_buffer(buffer);
            }
        }
    }

    [[nodiscard]] cy::Status prepare(const ShadowField& widest) noexcept;
    [[nodiscard]] cy::Status shoot(const Shot& shot, Picture& out) noexcept;

    struct DrawState {
        cy::rendering::GraphExecutor* executor = nullptr;
        rhi::GraphicsPipelineHandle pipeline;
        rhi::PipelineLayoutHandle layout;
        rhi::DescriptorSetHandle set;
        rhi::BufferHandle vertices;
        rhi::BufferHandle colours;
        rhi::BufferHandle readback;
        cy::rendering::ResourceId color = cy::rendering::kInvalidResource;
        cy::rendering::ResourceId depth = cy::rendering::kInvalidResource;
        WorldPush push;
    };

private:
    [[nodiscard]] cy::Expected<rhi::BufferHandle, cy::Error> buffer(const char* name, u64 size,
                                                                    rhi::BufferUsage usage,
                                                                    rhi::MemoryUse memory) noexcept;
    [[nodiscard]] cy::Status bind(rhi::BufferHandle field) noexcept;
    [[nodiscard]] cy::Expected<rhi::ShaderModuleHandle, cy::Error> module(
        const char* name, rhi::ShaderStage stage, cy::Span<const u32> spirv) noexcept;
    [[nodiscard]] cy::Expected<rhi::GraphicsPipelineHandle, cy::Error> pipeline(
        const char* name, rhi::ShaderModuleHandle vertex,
        rhi::ShaderModuleHandle fragment) noexcept;

    cy::render_test::DeviceFixture& fixture_;
    rhi::Device& device_;
    rhi::ShaderModuleHandle vertex_;
    rhi::ShaderModuleHandle fragment_;
    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::DescriptorSetHandle set_;
    rhi::PipelineLayoutHandle layout_;
    rhi::GraphicsPipelineHandle pipeline_;
    rhi::ShaderModuleHandle before_vertex_;
    rhi::ShaderModuleHandle before_fragment_;
    rhi::GraphicsPipelineHandle before_pipeline_;
    rhi::BufferHandle vertices_;
    rhi::BufferHandle colours_;
    rhi::BufferHandle field_;
    rhi::BufferHandle placement_;
    rhi::BufferHandle placeholder_;
    rhi::BufferHandle readback_;
    u64 field_bytes_ = 0;
};

void record_draw(const cy::rendering::PassContext& context, void* user) noexcept {
    auto* state = static_cast<WorldPass::DrawState*>(user);
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
    context.commands->bind_graphics_pipeline(state->pipeline);
    context.commands->bind_descriptor_sets(
        state->layout, 0, cy::Span<const rhi::DescriptorSetHandle>(&state->set, 1));
    const rhi::BufferHandle streams[2] = {state->vertices, state->colours};
    const u64 offsets[2] = {0, 0};
    context.commands->bind_vertex_buffers(0, cy::Span<const rhi::BufferHandle>(streams, 2),
                                          cy::Span<const u64>(offsets, 2));
    context.commands->push_constants(
        state->layout, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
        cy::Span<const u8>(reinterpret_cast<const u8*>(&state->push), sizeof(WorldPush)));
    context.commands->draw(kGroundVertices, 1, 0, 0);
    context.commands->end_rendering();
}

void record_readback(const cy::rendering::PassContext& context, void* user) noexcept {
    auto* state = static_cast<WorldPass::DrawState*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{kExtent, kExtent, 1};
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

cy::Status WorldPass::bind(rhi::BufferHandle field) noexcept {
    rhi::DescriptorWrite writes[2] = {};
    writes[0].binding = 0;
    writes[0].kind = rhi::DescriptorKind::StorageBuffer;
    writes[0].buffer = field;
    writes[1].binding = 1;
    writes[1].kind = rhi::DescriptorKind::StorageBuffer;
    writes[1].buffer = placement_;
    return device_.update_descriptor_set(set_, cy::Span<const rhi::DescriptorWrite>(writes, 2));
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
        {0, sizeof(GroundVertex), rhi::VertexInputRate::PerVertex},
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

cy::Status WorldPass::prepare(const ShadowField& widest) noexcept {
    namespace now = cy::sample::world;
    namespace before = cy::render_test::world_before_cloud_shadows;
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
        {&before_vertex_, "world vertex before cloud shadows", rhi::ShaderStage::Vertex,
         cy::Span<const u32>(before::kWorldVertexSpirv, std::size(before::kWorldVertexSpirv))},
        {&before_fragment_, "world fragment before cloud shadows", rhi::ShaderStage::Fragment,
         cy::Span<const u32>(before::kWorldFragmentSpirv, std::size(before::kWorldFragmentSpirv))},
    };
    for (const auto& entry : modules) {
        auto created = module(entry.name, entry.stage, entry.spirv);
        if (!created) {
            return cy::make_unexpected(created.error());
        }
        *entry.handle = *created;
    }

    // THE SAMPLE'S LAYOUT, restated: set 0 with the field and its placement for the fragment
    // stage, and the 128-byte push block for both stages. A layout that disagreed with the shader
    // is a validation error the fixture counts.
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

    // The pipeline from before cloud shadows shares the layout: it reads no set, and a layout may
    // declare a set its shaders never touch.
    auto created = pipeline("world", vertex_, fragment_);
    if (!created) {
        return cy::make_unexpected(created.error());
    }
    pipeline_ = *created;
    auto created_before = pipeline("world before cloud shadows", before_vertex_, before_fragment_);
    if (!created_before) {
        return cy::make_unexpected(created_before.error());
    }
    before_pipeline_ = *created_before;

    // --- The ground: one flat square facing up, grey, covering the whole picture.
    auto vertices = buffer("ground", sizeof(GroundVertex) * kGroundVertices,
                           rhi::BufferUsage::Vertex, rhi::MemoryUse::Upload);
    auto colours = buffer("ground colour", sizeof(f32) * 3 * kGroundVertices,
                          rhi::BufferUsage::Vertex, rhi::MemoryUse::Upload);
    auto field = buffer("cloud shadow field", widest.image.words.size() * sizeof(u32),
                        rhi::BufferUsage::Storage, rhi::MemoryUse::Upload);
    auto placement = buffer("cloud shadow placement", sizeof(Placement), rhi::BufferUsage::Storage,
                            rhi::MemoryUse::Upload);
    auto placeholder = buffer("cloud shadow placeholder", 16 * sizeof(u32),
                              rhi::BufferUsage::Storage, rhi::MemoryUse::Upload);
    auto readback = buffer("world readback", static_cast<u64>(kTexels) * 4 * sizeof(u16),
                           rhi::BufferUsage::TransferDestination, rhi::MemoryUse::Readback);
    if (!vertices || !colours || !field || !placement || !placeholder || !readback) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "a world pass buffer did not allocate");
    }
    vertices_ = *vertices;
    colours_ = *colours;
    field_ = *field;
    placement_ = *placement;
    placeholder_ = *placeholder;
    readback_ = *readback;
    field_bytes_ = widest.image.words.size() * sizeof(u32);
    std::memset(device_.buffer_mapped_pointer(placeholder_), 0, 16 * sizeof(u32));

    const f32 corners[kGroundVertices][2] = {{-1.0F, -1.0F}, {1.0F, -1.0F}, {1.0F, 1.0F},
                                             {-1.0F, -1.0F}, {1.0F, 1.0F},  {-1.0F, 1.0F}};
    auto* ground = static_cast<GroundVertex*>(device_.buffer_mapped_pointer(vertices_));
    auto* albedo = static_cast<f32*>(device_.buffer_mapped_pointer(colours_));
    for (u32 index = 0; index < kGroundVertices; ++index) {
        // A little past the picture's edge, so every texel centre is covered.
        ground[index] = GroundVertex{
            {corners[index][0] * kHalfExtent * 1.1F, 0.0F, corners[index][1] * kHalfExtent * 1.1F},
            {0.0F, 1.0F, 0.0F}};
        albedo[(index * 3) + 0] = kAlbedo;
        albedo[(index * 3) + 1] = kAlbedo;
        albedo[(index * 3) + 2] = kAlbedo;
    }

    auto set = device_.allocate_descriptor_set(set_layout_, false);
    if (!set) {
        return cy::make_unexpected(set.error());
    }
    set_ = *set;
    return bind(placeholder_);
}

cy::Status WorldPass::shoot(const Shot& shot, Picture& out) noexcept {
    // --- What the stage uploads: the image, where it sits, and whether this frame reads it.
    Placement placement;
    rhi::BufferHandle bound = placeholder_;
    if (shot.field != nullptr) {
        const cy::environment::FieldGpuImage& image = shot.field->image;
        const u64 bytes = image.words.size() * sizeof(u32);
        if (bytes > field_bytes_) {
            return cy::fail(cy::ErrorCode::InvalidArgument, "the field image outgrew its buffer");
        }
        std::memcpy(device_.buffer_mapped_pointer(field_), image.words.data(), bytes);
        bound = field_;
        placement.origin_x = static_cast<f32>(frame_centre().x - image.origin_x);
        placement.origin_z = static_cast<f32>(frame_centre().z - image.origin_z);
        placement.enabled = shot.cloud_shadows ? 1.0F : 0.0F;
    }
    std::memcpy(device_.buffer_mapped_pointer(placement_), &placement, sizeof(placement));
    if (cy::Status rebound = bind(bound); !rebound) {
        return rebound;
    }

    // --- The push block, as `Stage::shoot` writes it: an orthographic view straight down.
    DrawState state;
    state.pipeline = shot.before ? before_pipeline_ : pipeline_;
    state.layout = layout_;
    state.set = set_;
    state.vertices = vertices_;
    state.colours = colours_;
    state.readback = readback_;
    const f32 inverse = 1.0F / kHalfExtent;
    state.push.row0[0] = inverse;
    state.push.row1[0] = 0.0F;
    state.push.row1[1] = 0.0F;
    state.push.row1[2] = inverse;
    state.push.row2[2] = 0.0F;
    state.push.row2[3] = 0.5F;
    const Vec3 travel = -sun_direction();
    state.push.light[0] = travel.x;
    state.push.light[1] = travel.y;
    state.push.light[2] = travel.z;
    state.push.eye[1] = 9'000.0F;
    state.push.eye[3] = shot.emissive ? 1.0F : 0.0F;
    state.push.sun[0] = shot.sun;
    state.push.sun[1] = shot.sun;
    state.push.sun[2] = shot.sun;

    if (cy::Expected<u32, cy::Error> began = device_.begin_frame(); !began) {
        return cy::make_unexpected(began.error());
    }
    cy::rendering::RenderGraph graph(fixture_.allocator());
    cy::rendering::GraphExecutor executor(fixture_.allocator(), device_);
    state.executor = &executor;

    cy::rendering::TextureRequest color_request;
    color_request.name = "world colour";
    color_request.format = rhi::Format::Rgba16Sfloat;
    color_request.width = kExtent;
    color_request.height = kExtent;
    state.color = graph.create_texture(color_request);
    cy::rendering::TextureRequest depth_request;
    depth_request.name = "world depth";
    depth_request.format = rhi::Format::D32Sfloat;
    depth_request.width = kExtent;
    depth_request.height = kExtent;
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
        if (std::memcmp(&a.bits[base], &b.bits[base], 4 * sizeof(u16)) != 0) {
            ++count;
        }
    }
    return count;
}

/// Everything the cases share: the device, the pass, the cloudy field and the clear one.
struct Scene {
    cy::render_test::DeviceFixture fixture{"vulkan", "cy_test_render_world_cloud_shadow"};
    Sky cloudy;
    Sky clear;
    ShadowField cloudy_field;
    ShadowField clear_field;

    [[nodiscard]] bool have_vulkan() const noexcept { return fixture.is(rhi::BackendKind::Vulkan); }
    [[nodiscard]] cy::Status build() noexcept {
        if (cy::Status built = cloudy.build(true); !built) {
            return built;
        }
        if (cy::Status built = clear.build(false); !built) {
            return built;
        }
        if (cy::Status built = cloudy_field.build(cloudy.field); !built) {
            return built;
        }
        return clear_field.build(clear.field);
    }
};

}  // namespace

CY_TEST_CASE("world cloud shadows: ground under a cloud is darker, beside it unchanged") {
    Scene scene;
    if (!scene.have_vulkan()) {
        scene.fixture.report_skip();
        return;
    }
    CY_REQUIRE(scene.build());
    WorldPass pass(scene.fixture);
    CY_REQUIRE(pass.prepare(scene.cloudy_field));

    Picture on(allocator());
    Picture off(allocator());
    CY_REQUIRE(pass.shoot(Shot{true, &scene.cloudy_field, false, 1.0F}, on));
    CY_REQUIRE(pass.shoot(Shot{false, &scene.cloudy_field, false, 1.0F}, off));

    // THE EXPECTED RADIANCE, from the processor: albedo times the ambient term plus the sun's
    // Lambert term, the sun multiplied by the field's value at the texel's ground.
    const Vec3 sun = sun_direction();
    const f32 lambert = cy::math::saturate(sun.y);
    u32 shadowed = 0;
    u32 shadowed_darker = 0;
    u32 in_sun = 0;
    u32 in_sun_changed = 0;
    u32 brighter = 0;
    f32 worst_relative = 0.0F;
    f32 darkest_field = 1.0F;
    for (u32 texel = 0; texel < kTexels; ++texel) {
        const Vec3 ground = ground_at(texel);
        const f32 through = scene.cloudy_field.at(ground.x, ground.z);
        darkest_field = cy::math::min(darkest_field, through);
        const f32 lit_on = half_to_float(on.red(texel));
        const f32 lit_off = half_to_float(off.red(texel));
        brighter += lit_on > lit_off ? 1U : 0U;
        if (through < 0.5F) {
            ++shadowed;
            shadowed_darker += lit_on < lit_off ? 1U : 0U;
            const f32 expected = world_tonemap(kAlbedo * (0.1F + (through * lambert)));
            worst_relative = cy::math::max(worst_relative, std::fabs(lit_on - expected) / expected);
        } else if (through == 1.0F) {
            ++in_sun;
            const usize base = static_cast<usize>(texel) * 4;
            in_sun_changed +=
                std::memcmp(&on.bits[base], &off.bits[base], 4 * sizeof(u16)) != 0 ? 1U : 0U;
        }
    }
    CY_TEST_MESSAGE(shadowed, " texels under the cloud (", shadowed_darker, " darker, worst ",
                    worst_relative, " from the processor's field), ", in_sun, " in full sun (",
                    in_sun_changed, " changed), ", brighter, " brighter anywhere; darkest field ",
                    darkest_field, "; ", scene.fixture.validation_errors(), " validation errors");

    // The picture HAS both halves, or neither claim below is about anything.
    CY_CHECK_GT(shadowed, 200U);
    CY_CHECK_GT(in_sun, 1000U);
    CY_CHECK_LT(darkest_field, 0.2F);
    // Under the cloud: every texel darker, by the field's own amount.
    CY_CHECK_EQ(shadowed_darker, shadowed);
    CY_CHECK_LT(worst_relative, 0.01F);
    // Beside it: every texel exactly the frame without cloud shadows. And nowhere brighter.
    CY_CHECK_EQ(in_sun_changed, 0U);
    CY_CHECK_EQ(brighter, 0U);
    CY_CHECK_EQ(scene.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("world cloud shadows: off, or under a clear sky, the frame is the same frame") {
    Scene scene;
    if (!scene.have_vulkan()) {
        scene.fixture.report_skip();
        return;
    }
    CY_REQUIRE(scene.build());
    WorldPass pass(scene.fixture);
    CY_REQUIRE(pass.prepare(scene.cloudy_field));

    // THE REFERENCE is the frame this world drew before cloud shadows existed: the same ground,
    // the same push block, drawn with the world's shaders as they were at 2614fb0, which read no
    // field. Against it, four ways of saying "no cloud shadow": the placeholder the stage binds
    // before its first frame, the cloudy field bound and switched off, the clear field bound and
    // switched off, and the clear field switched ON — a clear sky costs the picture nothing even
    // when the field is being read.
    Picture before(allocator());
    Picture placeholder(allocator());
    Picture cloudy_off(allocator());
    Picture clear_off(allocator());
    Picture clear_on(allocator());
    Picture cloudy_on(allocator());
    Shot reference{false, &scene.cloudy_field, false, 1.0F};
    reference.before = true;
    CY_REQUIRE(pass.shoot(reference, before));
    CY_REQUIRE(pass.shoot(Shot{false, nullptr, false, 1.0F}, placeholder));
    CY_REQUIRE(pass.shoot(Shot{false, &scene.cloudy_field, false, 1.0F}, cloudy_off));
    CY_REQUIRE(pass.shoot(Shot{false, &scene.clear_field, false, 1.0F}, clear_off));
    CY_REQUIRE(pass.shoot(Shot{true, &scene.clear_field, false, 1.0F}, clear_on));
    CY_REQUIRE(pass.shoot(Shot{true, &scene.cloudy_field, false, 1.0F}, cloudy_on));

    const u32 placeholder_changed = differing(before, placeholder);
    const u32 off_changed = differing(before, cloudy_off);
    const u32 clear_changed = differing(before, clear_off);
    const u32 clear_on_changed = differing(before, clear_on);
    const u32 cloudy_changed = differing(before, cloudy_on);
    CY_TEST_MESSAGE("texels differing from the frame before cloud shadows: placeholder ",
                    placeholder_changed, ", cloudy off ", off_changed, ", clear off ",
                    clear_changed, ", clear on ", clear_on_changed, ", cloudy on ", cloudy_changed);
    CY_CHECK_EQ(placeholder_changed, 0U);
    CY_CHECK_EQ(off_changed, 0U);
    CY_CHECK_EQ(clear_changed, 0U);
    CY_CHECK_EQ(clear_on_changed, 0U);
    // And the same comparison CAN fail: under the cloud, with the field read, the frame moves.
    CY_CHECK_GT(cloudy_changed, 200U);
    CY_CHECK_EQ(scene.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("world cloud shadows: the sky and the ambient term are not attenuated") {
    Scene scene;
    if (!scene.have_vulkan()) {
        scene.fixture.report_skip();
        return;
    }
    CY_REQUIRE(scene.build());
    WorldPass pass(scene.fixture);
    CY_REQUIRE(pass.prepare(scene.cloudy_field));

    // THE SKY: the emissive path the dome is drawn with, over the cloud's own shadow. A sky is not
    // a surface and the cloud in it is already drawn; a field applied to it would darken the
    // cloud's own picture by its shadow.
    Picture sky_on(allocator());
    Picture sky_off(allocator());
    CY_REQUIRE(pass.shoot(Shot{true, &scene.cloudy_field, true, 1.0F}, sky_on));
    CY_REQUIRE(pass.shoot(Shot{false, &scene.cloudy_field, true, 1.0F}, sky_off));

    // THE AMBIENT TERM: the same ground with the sun switched off, so the ambient term is all that
    // lights it. The ground is under the darkest cloud there is and must not notice.
    Picture ambient_on(allocator());
    Picture ambient_off(allocator());
    CY_REQUIRE(pass.shoot(Shot{true, &scene.cloudy_field, false, 0.0F}, ambient_on));
    CY_REQUIRE(pass.shoot(Shot{false, &scene.cloudy_field, false, 0.0F}, ambient_off));

    // Both pictures are of something: the ambient ground is lit, and it is lit at the ambient
    // term's own level rather than at zero.
    u32 ambient_lit = 0;
    for (u32 texel = 0; texel < kTexels; ++texel) {
        const f32 level = world_tonemap(kAlbedo * 0.1F);
        ambient_lit +=
            std::fabs(half_to_float(ambient_on.red(texel)) - level) < level * 1e-2F ? 1U : 0U;
    }
    const u32 sky_changed = differing(sky_on, sky_off);
    const u32 ambient_changed = differing(ambient_on, ambient_off);
    CY_TEST_MESSAGE("sky texels changed by the field ", sky_changed,
                    ", ambient-lit texels changed ", ambient_changed,
                    ", ambient-lit texels at the ambient level ", ambient_lit);
    CY_CHECK_EQ(ambient_lit, kTexels);
    CY_CHECK_EQ(sky_changed, 0U);
    CY_CHECK_EQ(ambient_changed, 0U);
    CY_CHECK_EQ(scene.fixture.validation_errors(), 0U);
}
