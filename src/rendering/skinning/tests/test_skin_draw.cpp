// THE SKINNED MESH, DRAWN. M8.d.
//
// ================================================================================================
// WHAT THIS SUITE IS FOR
// ================================================================================================
//
// `test_skin_pass.cpp` proves the dispatch computes what `cpu_reference_skin` computes. That is a
// comparison between two buffers and it says nothing about whether anything can DRAW the result —
// which is the requirement's actual claim: "writing into per-instance output vertex buffers, so the
// result is reusable across passes". This suite binds the compute pass's output buffer as vertex
// buffer 0 of a graphics pipeline and rasterises it.
//
// Nothing on the CPU ever writes the vertices that are drawn here. `upload_mesh` writes the BIND
// POSE into a separate input buffer; the buffer the draw binds is device-local, is written only by
// `skin.slang`, and is read only by a vertex fetch. If the dispatch did not run, the draw would
// read whatever the allocator left in device memory.
//
// ================================================================================================
// THE ASSERTION IS A PIXEL WHOSE ADDRESS WAS COMPUTED, NOT A GOLDEN IMAGE
// ================================================================================================
//
// The bar below lies along +X from (1, 0, 0) to (3, 0, 0) and is bound entirely to the forearm, so
// the elbow's quarter turn about (1, 0, 0) sends it to lie along +Y from (1, 0, 0) to (1, 2, 0) —
// (x, y) becomes (1 − y, x − 1). The view maps world x and y to pixels by
//
//     pixel_x = x·8 + 32          pixel_y = 32 − y·8
//
// which is the matrix three lines below, written out. So the four probes are arithmetic:
//
//     (48, 32)  world (2.06, −0.06)  covered at rest, EMPTY once the elbow turns
//     (40, 24)  world (1.06,  0.94)  empty at rest, COVERED once it turns
//     (40, 16)  world (1.06,  1.94)  the far end, covered once it turns — inside the tip at y = 2
//     (40, 10)  world (1.06,  2.69)  past the tip, empty either way
//
// Two renders of the SAME code path with one argument changed — the elbow's angle — so there is no
// tolerance to tune and no reference to regenerate. Set the angle to zero in both and the case goes
// red.
//
// ================================================================================================
// THE BARRIER BETWEEN THE DISPATCH AND THE DRAW IS DERIVED
// ================================================================================================
//
// `SkinPass::declare` declares the output buffer WRITTEN by a compute pass; the draw pass below
// declares it READ with `Access::VertexAttributeRead`. Nothing here emits a barrier, and
// synchronisation validation is on and its error count is asserted — which is what makes "the graph
// derives it" a measured claim rather than a design note.

#include "skin_fixture.h"

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/pipeline.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/skinning/skin_pass.h>

#include "shaders/conventions_spirv.h"

#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace cy;
using namespace cy::skin_test;
using cy::render::PackedNormalTangent;
using cy::render::geometry::GpuSkinInfluence;
using cy::render::geometry::SkinningDescriptor;
using cy::rendering::PassContext;
using cy::rendering::ResourceId;
using cy::rendering::skinning::SkinPass;
using cy::rendering::skinning::SkinPassDescription;
using cy::rhi::Access;
using cy::rhi::QueueKind;

namespace {

constexpr u32 kExtent = 64;
constexpr u32 kTexels = kExtent * kExtent;

/// The bar, as two triangles. Six vertices rather than four and an index buffer, because this suite
/// is about where vertices land and an index buffer would be one more thing between the dispatch
/// and the rasteriser.
constexpr u32 kBarVertices = 6;

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

/// `ConventionPush` in tests/render/shaders/conventions.slang: four explicit ROWS and a colour.
/// Four rows rather than a `float4x4` for that file's own reason — a matrix in a push block has a
/// layout that is a property of the compiler's flags rather than of either side.
struct DrawPush {
    f32 row0[4] = {1.0F, 0.0F, 0.0F, 0.0F};
    f32 row1[4] = {0.0F, 1.0F, 0.0F, 0.0F};
    f32 row2[4] = {0.0F, 0.0F, 1.0F, 0.0F};
    f32 row3[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    f32 color[4] = {1.0F, 1.0F, 1.0F, 1.0F};
};

static_assert(sizeof(DrawPush) == 80, "the push block must fit the 128-byte portability limit");

/// The view: world x and y to pixels, as the header comment writes it out.
///
///   clip.x =  x / 4      so  pixel_x = (clip.x·0.5 + 0.5)·64 = x·8 + 32
///   clip.y =  y / 4      so  pixel_y = 32 − y·8: the Vulkan backend writes a NEGATIVE viewport
///                        height (`vulkan_command_buffer.cpp`, and tests/render/
///                        test_axis_conventions.cpp asserts it against an image), so +Y in clip
///                        space is UP on the screen and no sign is needed here
///   clip.z =  0.5        a constant depth, in front of a reversed-Z clear of zero
///   clip.w =  1
DrawPush bar_view() noexcept {
    DrawPush push;
    push.row0[0] = 0.25F;
    push.row0[1] = 0.0F;
    push.row1[0] = 0.0F;
    push.row1[1] = 0.25F;
    push.row2[2] = 0.0F;
    push.row2[3] = 0.5F;
    push.color[0] = 1.0F;
    push.color[1] = 1.0F;
    push.color[2] = 1.0F;
    return push;
}

std::vector<Vec3> bar_mesh() {
    // Along +X from the elbow, a fifth of a unit thick.
    const Vec3 a{1.0F, -0.1F, 0.0F};
    const Vec3 b{3.0F, -0.1F, 0.0F};
    const Vec3 c{3.0F, 0.1F, 0.0F};
    const Vec3 d{1.0F, 0.1F, 0.0F};
    return {a, b, c, a, c, d};
}

/// What the draw callback needs. A struct rather than captures, because a record callback is a
/// plain function pointer — the engine has no exceptions and no per-frame allocation on this path.
struct DrawState {
    cy::rendering::GraphExecutor* executor = nullptr;
    rhi::GraphicsPipelineHandle pipeline;
    rhi::PipelineLayoutHandle layout;
    rhi::BufferHandle vertices;
    rhi::BufferHandle color_readback;
    ResourceId color = cy::rendering::kInvalidResource;
    ResourceId depth = cy::rendering::kInvalidResource;
    DrawPush push{};
    u32 first_vertex = 0;
};

void record_draw(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<DrawState*>(user);

    rhi::RenderAttachment color;
    color.view = state->executor->view(state->color);
    color.load = rhi::LoadOp::Clear;
    color.store = rhi::StoreOp::Store;
    color.clear.color[0] = 0.0F;
    color.clear.color[1] = 0.0F;
    color.clear.color[2] = 0.0F;
    color.clear.color[3] = 1.0F;

    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, kExtent, kExtent};
    info.color_attachments = Span<const rhi::RenderAttachment>(&color, 1);
    info.depth_attachment.view = state->executor->view(state->depth);
    info.depth_attachment.load = rhi::LoadOp::Clear;
    info.depth_attachment.store = rhi::StoreOp::Store;
    // Reversed-Z, so the clear is zero and a nearer fragment has a GREATER depth.
    info.depth_attachment.clear = rhi::reversed_z_depth_clear();

    context.commands->begin_rendering(info);
    context.commands->set_viewport(rhi::Viewport{0.0F, 0.0F, static_cast<f32>(kExtent),
                                                 static_cast<f32>(kExtent), 0.0F, 1.0F});
    context.commands->set_scissor(rhi::Rect2D{0, 0, kExtent, kExtent});
    context.commands->bind_graphics_pipeline(state->pipeline);
    const u64 offset = 0;
    // THE COMPUTE PASS'S OUTPUT, BOUND AS A VERTEX BUFFER. Offset zero and the half selected
    // through `first_vertex`, because the output is double buffered inside one buffer and a
    // per-draw byte offset is not something a bound stream can carry.
    context.commands->bind_vertex_buffers(0, Span<const rhi::BufferHandle>(&state->vertices, 1),
                                          Span<const u64>(&offset, 1));
    context.commands->push_constants(
        state->layout, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&state->push), sizeof(DrawPush)));
    context.commands->draw(kBarVertices, 1, state->first_vertex, 0);
    context.commands->end_rendering();
}

void record_color_readback(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<DrawState*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{kExtent, kExtent, 1};
    context.commands->copy_texture_to_buffer(state->executor->texture(state->color),
                                             state->color_readback,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

/// The graphics half: one pipeline that reads a `float3` position stream and shades flat.
///
/// The shaders are tests/render/shaders/conventions.slang's, compiled into
/// `conventions_spirv.h` — a vertex stage that is four dot products against a push block and a
/// fragment stage that returns a colour. Reused rather than rewritten because a second pair would
/// be a second place for the clip convention to drift.
class DrawPipeline {
public:
    explicit DrawPipeline(rhi::Device& device) noexcept : device_(&device) {}

    ~DrawPipeline() {
        if (device_ == nullptr) {
            return;
        }
        device_->destroy_graphics_pipeline(pipeline_);
        device_->destroy_pipeline_layout(layout_);
        device_->destroy_shader_module(vertex_);
        device_->destroy_shader_module(fragment_);
        device_->destroy_buffer(readback_);
    }

    DrawPipeline(const DrawPipeline&) = delete;
    DrawPipeline& operator=(const DrawPipeline&) = delete;

    [[nodiscard]] Status create() noexcept {
        rhi::ShaderModuleDescription vertex;
        vertex.name = "skinned draw vertex";
        vertex.stage = rhi::ShaderStage::Vertex;
        vertex.entry_point = "main";
        vertex.spirv = Span<const u32>(render_test::kConventionVertexSpirv,
                                       sizeof(render_test::kConventionVertexSpirv) / sizeof(u32));
        Expected<rhi::ShaderModuleHandle, Error> vertex_module =
            device_->create_shader_module(vertex);
        if (!vertex_module.has_value()) {
            return make_unexpected(vertex_module.error());
        }
        vertex_ = *vertex_module;

        rhi::ShaderModuleDescription fragment;
        fragment.name = "skinned draw fragment";
        fragment.stage = rhi::ShaderStage::Fragment;
        fragment.entry_point = "main";
        fragment.spirv =
            Span<const u32>(render_test::kConventionFragmentSpirv,
                            sizeof(render_test::kConventionFragmentSpirv) / sizeof(u32));
        Expected<rhi::ShaderModuleHandle, Error> fragment_module =
            device_->create_shader_module(fragment);
        if (!fragment_module.has_value()) {
            return make_unexpected(fragment_module.error());
        }
        fragment_ = *fragment_module;

        const rhi::PushConstantRange range{rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
                                           sizeof(DrawPush)};
        rhi::PipelineLayoutDescription layout;
        layout.name = "skinned draw layout";
        layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
        Expected<rhi::PipelineLayoutHandle, Error> layout_handle =
            device_->create_pipeline_layout(layout);
        if (!layout_handle.has_value()) {
            return make_unexpected(layout_handle.error());
        }
        layout_ = *layout_handle;

        // STRIDE 12, `Rgb32Sfloat`. That is `render::VertexStream::Position` unquantised, and it is
        // exactly what the skinning dispatch writes — which is the whole point: no repack stands
        // between the compute pass and the vertex fetch.
        const rhi::VertexBinding binding{0, sizeof(f32) * 3, rhi::VertexInputRate::PerVertex};
        const rhi::VertexAttribute attribute{0, 0, rhi::Format::Rgb32Sfloat, 0};
        rhi::ColorAttachmentState color;
        color.format = rhi::Format::Rgba8Unorm;

        rhi::GraphicsPipelineDescription pipeline;
        pipeline.name = "skinned draw";
        pipeline.layout = layout_;
        pipeline.vertex_shader = vertex_;
        pipeline.fragment_shader = fragment_;
        pipeline.vertex_bindings = Span<const rhi::VertexBinding>(&binding, 1);
        pipeline.vertex_attributes = Span<const rhi::VertexAttribute>(&attribute, 1);
        pipeline.color_attachments = Span<const rhi::ColorAttachmentState>(&color, 1);
        // NO CULLING. The bar's winding reverses when the elbow turns past a half turn, and a case
        // about where vertices land that silently culled its own geometry would report a skinning
        // error as an empty image.
        pipeline.rasterisation.cull_mode = rhi::CullMode::None;
        pipeline.depth_stencil.format = rhi::Format::D32Sfloat;
        pipeline.depth_stencil.depth_test_enable = true;
        pipeline.depth_stencil.depth_write_enable = true;
        Expected<rhi::GraphicsPipelineHandle, Error> created =
            device_->create_graphics_pipeline(pipeline);
        if (!created.has_value()) {
            return make_unexpected(created.error());
        }
        pipeline_ = *created;

        rhi::BufferDescription readback;
        readback.name = "skinned draw colour readback";
        readback.size = static_cast<u64>(kTexels) * sizeof(u32);
        readback.usage = rhi::BufferUsage::TransferDestination;
        readback.memory = rhi::MemoryUse::Readback;
        Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(readback);
        if (!buffer.has_value()) {
            return make_unexpected(buffer.error());
        }
        readback_ = *buffer;
        return ok();
    }

    [[nodiscard]] rhi::GraphicsPipelineHandle pipeline() const noexcept { return pipeline_; }
    [[nodiscard]] rhi::PipelineLayoutHandle layout() const noexcept { return layout_; }
    [[nodiscard]] rhi::BufferHandle readback() const noexcept { return readback_; }

private:
    rhi::Device* device_ = nullptr;
    rhi::ShaderModuleHandle vertex_;
    rhi::ShaderModuleHandle fragment_;
    rhi::PipelineLayoutHandle layout_;
    rhi::GraphicsPipelineHandle pipeline_;
    rhi::BufferHandle readback_;
};

/// One skin, one draw, one read-back. The whole frame.
///
/// Named `render_frame` rather than `render` because `using namespace cy;` brings the namespace
/// `cy::render` into scope and the two names are then ambiguous.
[[nodiscard]] bool render_frame(DeviceFixture& gpu, SkinPass& pass, DrawPipeline& graphics,
                                const SkinningDescriptor& descriptor, Span<const Mat4> pose,
                                std::vector<u32>& texels, std::vector<Vec3>& skinned) {
    if (!pass.upload(descriptor, pose, 0).has_value()) {
        return false;
    }
    if (!gpu.device().begin_frame().has_value()) {
        return false;
    }
    cy::rendering::RenderGraph graph(allocator());
    cy::rendering::GraphExecutor executor(allocator(), gpu.device());
    if (!pass.declare(graph).has_value()) {
        return false;
    }

    cy::rendering::TextureRequest color_request;
    color_request.name = "skinned draw colour";
    color_request.format = rhi::Format::Rgba8Unorm;
    color_request.width = kExtent;
    color_request.height = kExtent;
    const ResourceId color = graph.create_texture(color_request);

    cy::rendering::TextureRequest depth_request;
    depth_request.name = "skinned draw depth";
    depth_request.format = rhi::Format::D32Sfloat;
    depth_request.width = kExtent;
    depth_request.height = kExtent;
    const ResourceId depth = graph.create_texture(depth_request);

    cy::rendering::BufferRequest readback_request;
    readback_request.name = "skinned draw colour readback";
    readback_request.size = static_cast<u64>(kTexels) * sizeof(u32);
    readback_request.extra_usage = rhi::BufferUsage::TransferDestination;
    const ResourceId color_out = graph.import_buffer(readback_request, graphics.readback());

    DrawState state;
    state.executor = &executor;
    state.pipeline = graphics.pipeline();
    state.layout = graphics.layout();
    state.vertices = pass.output_positions();
    state.color_readback = graphics.readback();
    state.color = color;
    state.depth = depth;
    state.push = bar_view();
    state.first_vertex = pass.vertex_offset();

    // THE DEPENDENCY THAT MAKES THIS A TEST OF THE GRAPH AS WELL. The draw READS the buffer the
    // dispatch WROTE, as a vertex attribute. No barrier is written here; the graph derives one, and
    // synchronisation validation is on.
    graph.add_pass("skinned draw", QueueKind::Graphics)
        .read(pass.positions_resource(), Access::VertexAttributeRead)
        .write(color, Access::ColorAttachmentWrite)
        .write(depth, Access::DepthStencilAttachmentWrite)
        .record(&record_draw, &state);
    graph.add_pass("skinned draw readback", QueueKind::Graphics)
        .read(color, Access::TransferRead)
        .write(color_out, Access::TransferWrite)
        .record(&record_color_readback, &state);
    graph.add_pass("skinned draw host", QueueKind::Graphics)
        .read(color_out, Access::HostRead)
        .side_effect();
    if (!graph.status().has_value()) {
        return false;
    }
    if (!executor.execute(graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{})
             .has_value()) {
        return false;
    }
    if (!gpu.device().wait_idle().has_value()) {
        return false;
    }
    if (!gpu.device().end_frame().has_value()) {
        return false;
    }

    const auto* pixels =
        static_cast<const u32*>(gpu.device().buffer_mapped_pointer(graphics.readback()));
    if (pixels == nullptr) {
        return false;
    }
    texels.assign(pixels, pixels + kTexels);

    Expected<Span<const Vec3>, Error> positions = pass.read_back_positions();
    if (!positions.has_value()) {
        return false;
    }
    skinned.assign(positions->begin() + pass.vertex_offset(),
                   positions->begin() + pass.vertex_offset() + kBarVertices);
    executor.release();
    return true;
}

/// Whether anything was drawn at a texel. The clear is black and the bar is white, so the red
/// channel answers.
[[nodiscard]] bool covered(const std::vector<u32>& texels, u32 x, u32 y) noexcept {
    return (texels[(static_cast<usize>(y) * kExtent) + x] & 0xFFU) > 128U;
}

[[nodiscard]] u32 lit_texels(const std::vector<u32>& texels) noexcept {
    u32 total = 0;
    for (const u32 texel : texels) {
        if ((texel & 0xFFU) > 128U) {
            ++total;
        }
    }
    return total;
}

}  // namespace

CY_TEST_CASE(
    "skinned draw: the rasteriser reads the vertices the compute pass wrote, and they "
    "moved") {
    DeviceFixture gpu("cy_test_render_skinned_draw");
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }
    const std::vector<Vec3> mesh = bar_mesh();
    std::vector<GpuSkinInfluence> influences;
    influences.reserve(kBarVertices);
    for (u32 index = 0; index < kBarVertices; ++index) {
        influences.push_back(bound_to(1));  // every vertex on the forearm
    }

    SkinPassDescription description;
    description.max_vertices = kBarVertices;
    description.max_bones = 2;
    // No normal-tangent stream: this pipeline's vertex input is a position and nothing else, and a
    // frame stream created and never bound would be a buffer nobody reads.
    description.with_frames = false;
    description.read_back = true;

    SkinPass pass;
    CY_REQUIRE(pass.create(allocator(), gpu.device(), description).has_value());
    CY_REQUIRE(pass.upload_mesh(Span<const Vec3>(mesh.data(), mesh.size()),
                                Span<const PackedNormalTangent>(),
                                Span<const GpuSkinInfluence>(influences.data(), influences.size()))
                   .has_value());

    DrawPipeline graphics(gpu.device());
    CY_REQUIRE(graphics.create().has_value());

    SkinningDescriptor descriptor;
    descriptor.vertex_count = kBarVertices;
    descriptor.bone_count = 2;
    descriptor.retained_bones = 2;

    // --- At rest. The forearm's matrix is the identity, so the bar stays along +X.
    const std::vector<Mat4> rest = elbow_pose(0.0F);
    std::vector<u32> at_rest;
    std::vector<Vec3> rest_vertices;
    CY_REQUIRE(render_frame(gpu, pass, graphics, descriptor,
                            Span<const Mat4>(rest.data(), rest.size()), at_rest, rest_vertices));

    // --- And turned a quarter turn about the elbow at (1, 0, 0).
    const std::vector<Mat4> turned_pose = elbow_pose(std::numbers::pi_v<f32> * 0.5F);
    std::vector<u32> turned;
    std::vector<Vec3> turned_vertices;
    CY_REQUIRE(render_frame(gpu, pass, graphics, descriptor,
                            Span<const Mat4>(turned_pose.data(), turned_pose.size()), turned,
                            turned_vertices));

    // 1. THE VERTICES THE DRAW READ. Vertex 1 of the bar is (3, −0.1, 0) at rest; the quarter turn
    //    sends (x, y) to (1 − y, x − 1), so it lands at (1.1, 2, 0). Worked out on paper, not
    //    captured.
    CY_CHECK(std::fabs(rest_vertices[1].x - 3.0F) < 1.0e-5F);
    CY_CHECK(std::fabs(rest_vertices[1].y + 0.1F) < 1.0e-5F);
    CY_CHECK(std::fabs(turned_vertices[1].x - 1.1F) < 1.0e-5F);
    CY_CHECK(std::fabs(turned_vertices[1].y - 2.0F) < 1.0e-5F);

    // 2. AND THE PIXELS AGREE WITH THEM. Each address is `pixel = (x·8 + 32, 32 − y·8)` evaluated
    //    at a point the bar covers in one pose and not the other.
    CY_CHECK(covered(at_rest, 48, 32));       // world (2.06, −0.06): on the bar at rest
    CY_CHECK_FALSE(covered(turned, 48, 32));  // and nowhere near it once the elbow turns

    CY_CHECK_FALSE(covered(at_rest, 40, 24));  // world (1.06, 0.94): above the resting bar
    CY_CHECK(covered(turned, 40, 24));         // and on it once it points up

    CY_CHECK(covered(turned, 40, 16));        // world (1.06, 1.94): just inside the tip at y = 2
    CY_CHECK_FALSE(covered(turned, 40, 10));  // world (1.06, 2.69): past it

    // 3. The bar did not merely move, it is still a bar: the same area is lit either way, to within
    //    the rasterisation of a rotated rectangle. A dispatch that collapsed the mesh to a point
    //    would satisfy every "not covered" check above and fail this one.
    const u32 rest_area = lit_texels(at_rest);
    const u32 turned_area = lit_texels(turned);
    std::fprintf(stderr, "skinned draw: %u lit texels at rest, %u turned\n", rest_area,
                 turned_area);
    CY_CHECK(rest_area > 0U);
    CY_CHECK(turned_area > 0U);
    const u32 difference =
        rest_area > turned_area ? rest_area - turned_area : turned_area - rest_area;
    CY_CHECK(difference * 4U < rest_area);

    CY_CHECK_EQ(gpu.validation_errors(), 0U);
}
