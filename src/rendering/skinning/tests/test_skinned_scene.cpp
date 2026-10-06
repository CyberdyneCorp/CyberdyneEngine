// SPDX-License-Identifier: MIT
// `SkinnedScene` on the null backend: the skinning table, the pose upload and the one pass, decided
// and recorded with no GPU. `integration.skinned_scene`, issue #76 stage 3.
//
// What the device suite (`render.skinned_frame`) photographs, this one counts on every machine:
// how many dispatches one pass records, which constants each pushes, which matrices an upload
// writes, where an instance's window is and when it is reused, and what is refused.

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/skinning/frame_skinning.h>
#include <cy/rendering/skinning/skinned_scene.h>
#include <cy/test/test.h>

#include <cstring>
#include <vector>

using namespace cy;
using cy::render::PackedNormalTangent;
using cy::render::geometry::GpuBoneMatrix;
using cy::render::geometry::GpuSkinInfluence;
using cy::render::geometry::InfluenceCount;
using cy::rendering::skinning::SkinnedInstance;
using cy::rendering::skinning::SkinnedMeshDescription;
using cy::rendering::skinning::SkinnedOutput;
using cy::rendering::skinning::SkinnedScene;
using cy::rendering::skinning::SkinnedSceneDescription;

namespace {

/// Bit-for-bit equality: the GPU must produce exactly what the CPU reference does, so the claim is
/// about the representation. `bugprone-suspicious-memory-comparison` is right that a float has no
/// unique representation, which is the point; comparing members would assert something weaker.
template <class T>
[[nodiscard]] bool identical(const T& left, const T& right) noexcept {
    // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison)
    return std::memcmp(&left, &right, sizeof(T)) == 0;
}

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

class NullFixture {
public:
    NullFixture() noexcept : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_test_integration_skinned_scene";
        device_ = rhi::create_device(allocator_, "null", description, selection_);
    }
    ~NullFixture() {
        if (device_.has_value()) {
            (void)device_.value()->wait_idle();
            rhi::destroy_device(allocator_, device_.value());
        }
    }
    NullFixture(const NullFixture&) = delete;
    NullFixture& operator=(const NullFixture&) = delete;

    [[nodiscard]] bool ok() const noexcept { return device_.has_value(); }
    [[nodiscard]] rhi::Device& device() const noexcept { return *device_.value(); }

private:
    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

/// A mesh of `vertices` vertices, every one bound fully to bone 0, `blocks` records each.
struct TestMesh {
    std::vector<Vec3> positions;
    std::vector<PackedNormalTangent> frames;
    std::vector<GpuSkinInfluence> influences;

    TestMesh(u32 vertices, u32 blocks) {
        const u8 bones[4] = {0, 0, 0, 0};
        const u8 full[4] = {255, 0, 0, 0};
        const u8 none[4] = {0, 0, 0, 0};
        for (u32 vertex = 0; vertex < vertices; ++vertex) {
            positions.push_back(Vec3{static_cast<f32>(vertex), 0.0F, 0.0F});
            frames.push_back(PackedNormalTangent{});
            influences.push_back(render::geometry::skin_influence(bones, full));
            if (blocks == 2U) {
                influences.push_back(render::geometry::skin_influence(bones, none));
            }
        }
    }

    [[nodiscard]] SkinnedMeshDescription description(InfluenceCount count, u32 bones) const {
        SkinnedMeshDescription out;
        out.positions = Span<const Vec3>(positions.data(), positions.size());
        out.frames = Span<const PackedNormalTangent>(frames.data(), frames.size());
        out.influences = Span<const GpuSkinInfluence>(influences.data(), influences.size());
        out.influence_count = count;
        out.bone_count = bones;
        return out;
    }
};

[[nodiscard]] SkinnedSceneDescription sized() noexcept {
    SkinnedSceneDescription description;
    description.max_mesh_vertices = 64;
    description.max_instance_vertices = 64;
    description.max_pose_matrices = 32;
    description.max_meshes = 4;
    description.max_instances = 8;
    return description;
}

[[nodiscard]] u32 count_of(Span<const rhi::null::RecordedCommand> commands,
                           rhi::null::CommandKind kind) noexcept {
    u32 count = 0;
    for (const rhi::null::RecordedCommand& command : commands) {
        count += command.kind == kind ? 1U : 0U;
    }
    return count;
}

/// Declare, compile and execute one frame of `scene` on the null device.
[[nodiscard]] Status run_frame(rhi::Device& device, SkinnedScene& scene, u64 frame) noexcept {
    rendering::RenderGraph graph(allocator());
    if (Status declared = scene.declare(graph, frame); !declared) {
        return declared;
    }
    if (graph.pass_count() == 0) {
        return ok();
    }
    Expected<u32, Error> began = device.begin_frame();
    if (!began.has_value()) {
        return make_unexpected(began.error());
    }
    // An imported buffer's write is a culling root, so the pass survives with no draw reading it.
    rendering::GraphExecutor executor(allocator(), device);
    auto executed =
        executor.execute(graph, rendering::CompileOptions{}, rendering::ExecuteOptions{});
    if (!executed.has_value()) {
        return make_unexpected(executed.error());
    }
    if (Status idle = device.wait_idle(); !idle) {
        return idle;
    }
    return device.end_frame();
}

}  // namespace

CY_TEST_CASE("skinned scene: one pass records one dispatch per posed instance") {
    NullFixture fixture;
    CY_REQUIRE(fixture.ok());
    SkinnedScene scene(allocator());
    CY_REQUIRE(scene.create(fixture.device(), sized()).has_value());
    const TestMesh mesh(10, 1);
    Expected<u32, Error> id = scene.add_mesh(mesh.description(InfluenceCount::Four, 2));
    CY_REQUIRE(id.has_value());
    SkinnedInstance instances[3];
    for (SkinnedInstance& instance : instances) {
        Expected<SkinnedInstance, Error> added = scene.add_instance(*id);
        CY_REQUIRE(added.has_value());
        instance = *added;
    }
    // Two of three posed: the third has no pose this frame and is left out, not skinned at zero.
    CY_REQUIRE(scene.set_pose(instances[0], 0).has_value());
    CY_REQUIRE(scene.set_pose(instances[1], 4).has_value());
    rhi::null::clear_command_log(fixture.device());
    CY_REQUIRE(run_frame(fixture.device(), scene, 0).has_value());
    CY_CHECK_EQ(scene.stats().passes, 1U);
    CY_CHECK_EQ(scene.stats().dispatches, 2U);
    const Span<const rhi::null::RecordedCommand> log = rhi::null::command_log(fixture.device());
    CY_CHECK_EQ(count_of(log, rhi::null::CommandKind::Dispatch), 2U);
    CY_CHECK_EQ(count_of(log, rhi::null::CommandKind::BindComputePipeline), 1U);

    // Each dispatch pushed its own window and its own pose.
    Expected<render::geometry::GpuSkinConstants, Error> first = scene.constants(instances[0]);
    Expected<render::geometry::GpuSkinConstants, Error> second = scene.constants(instances[1]);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(first->pose_offset, 0U);
    CY_CHECK_EQ(second->pose_offset, 4U);
    CY_CHECK_EQ(first->vertex_count, 10U);
    CY_CHECK_NE(first->first_output_vertex, second->first_output_vertex);
    CY_CHECK_FALSE(scene.constants(instances[2]).has_value());
    SkinnedOutput unposed;
    CY_CHECK_FALSE(scene.output(instances[2], unposed));
}

CY_TEST_CASE("skinned scene: no posed instance declares nothing") {
    NullFixture fixture;
    CY_REQUIRE(fixture.ok());
    SkinnedScene scene(allocator());
    CY_REQUIRE(scene.create(fixture.device(), sized()).has_value());
    rendering::RenderGraph graph(allocator());
    CY_REQUIRE(scene.declare(graph, 0).has_value());
    CY_CHECK_EQ(graph.pass_count(), 0U);
    CY_CHECK(scene.vertex_reads().empty());
}

CY_TEST_CASE("skinned scene: the output halves alternate and the previous one is named") {
    NullFixture fixture;
    CY_REQUIRE(fixture.ok());
    SkinnedScene scene(allocator());
    CY_REQUIRE(scene.create(fixture.device(), sized()).has_value());
    const TestMesh mesh(12, 1);
    Expected<u32, Error> id = scene.add_mesh(mesh.description(InfluenceCount::Four, 1));
    CY_REQUIRE(id.has_value());
    Expected<SkinnedInstance, Error> instance = scene.add_instance(*id);
    CY_REQUIRE(instance.has_value());
    CY_REQUIRE(scene.set_pose(*instance, 0).has_value());

    SkinnedOutput frame0;
    CY_REQUIRE(run_frame(fixture.device(), scene, 0).has_value());
    CY_REQUIRE(scene.output(*instance, frame0));
    // The first frame has no previous half.
    CY_CHECK_FALSE(frame0.has_previous);
    SkinnedOutput frame1;
    CY_REQUIRE(run_frame(fixture.device(), scene, 1).has_value());
    CY_REQUIRE(scene.output(*instance, frame1));
    CY_CHECK(frame1.has_previous);
    CY_CHECK_EQ(frame1.previous_vertex, frame0.current_vertex);
    CY_CHECK_NE(frame1.current_vertex, frame0.current_vertex);
    CY_CHECK_EQ(frame1.vertex_count, 12U);
    // A frame skipped: the other half is two frames old, so it is not offered as the previous one.
    SkinnedOutput frame3;
    CY_REQUIRE(run_frame(fixture.device(), scene, 3).has_value());
    CY_REQUIRE(scene.output(*instance, frame3));
    CY_CHECK_FALSE(frame3.has_previous);

    // The frame's draw takes exactly those windows.
    rendering::pipeline::DrawGeometry draw;
    rendering::skinning::SkinnedDrawMesh drawn;
    drawn.index_count = 6;
    drawn.static_vertex_offset = 40;
    CY_REQUIRE(rendering::skinning::skinned_draw_geometry(scene, *instance, drawn, draw));
    CY_CHECK(draw.skinned());
    CY_CHECK_EQ(draw.vertex_offset, static_cast<i32>(frame3.current_vertex));
    CY_CHECK_EQ(draw.static_vertex_offset, 40);
    CY_CHECK_FALSE(draw.has_previous_vertices);
}

CY_TEST_CASE("skinned scene: an upload writes the dirty range and nothing outside it") {
    NullFixture fixture;
    CY_REQUIRE(fixture.ok());
    SkinnedScene scene(allocator());
    CY_REQUIRE(scene.create(fixture.device(), sized()).has_value());
    std::vector<Mat4> world(16, Mat4::from_translation(Vec3{1.0F, 2.0F, 3.0F}));
    CY_REQUIRE(scene.upload_poses(Span<const Mat4>(world.data(), world.size()), 0, 16).has_value());
    CY_CHECK_EQ(scene.stats().uploaded_matrices, 16U);

    // Every matrix changes; only [4, 7) is said to have.
    for (Mat4& matrix : world) {
        matrix = Mat4::from_translation(Vec3{9.0F, 9.0F, 9.0F});
    }
    CY_REQUIRE(scene.upload_poses(Span<const Mat4>(world.data(), world.size()), 4, 3).has_value());
    CY_CHECK_EQ(scene.stats().uploaded_matrices, 3U);
    CY_CHECK_EQ(scene.stats().uploaded_matrices_total, 19U);
    const Span<const GpuBoneMatrix> device = scene.pose_buffer();
    const GpuBoneMatrix old_value =
        render::geometry::pack_bone_matrix(Mat4::from_translation(Vec3{1.0F, 2.0F, 3.0F}));
    const GpuBoneMatrix new_value =
        render::geometry::pack_bone_matrix(Mat4::from_translation(Vec3{9.0F, 9.0F, 9.0F}));
    for (u32 index = 0; index < 16U; ++index) {
        const bool inside = index >= 4U && index < 7U;
        const GpuBoneMatrix& expected = inside ? new_value : old_value;
        CY_CHECK(identical(device[index], expected));
    }
    // Nothing published is nothing written.
    CY_REQUIRE(scene.upload_poses(Span<const Mat4>(world.data(), world.size()), 0, 0).has_value());
    CY_CHECK_EQ(scene.stats().uploaded_matrices, 0U);
}

CY_TEST_CASE("skinned scene: an eight-influence mesh's records never overlap another mesh's") {
    NullFixture fixture;
    CY_REQUIRE(fixture.ok());
    SkinnedScene scene(allocator());
    CY_REQUIRE(scene.create(fixture.device(), sized()).has_value());
    const TestMesh four(5, 1);
    const TestMesh eight(5, 2);
    Expected<u32, Error> a = scene.add_mesh(four.description(InfluenceCount::Four, 1));
    Expected<u32, Error> b = scene.add_mesh(eight.description(InfluenceCount::Eight, 1));
    Expected<u32, Error> c = scene.add_mesh(four.description(InfluenceCount::Four, 1));
    CY_REQUIRE(a.has_value());
    CY_REQUIRE(b.has_value());
    CY_REQUIRE(c.has_value());
    // The dispatch reads records at `vertex * blocks`: a at [0, 5), b at [2 * first_b, ...),
    // and c's first vertex must start past b's last record.
    Expected<SkinnedInstance, Error> ia = scene.add_instance(*a);
    Expected<SkinnedInstance, Error> ib = scene.add_instance(*b);
    Expected<SkinnedInstance, Error> ic = scene.add_instance(*c);
    CY_REQUIRE(ia.has_value());
    CY_REQUIRE(ib.has_value());
    CY_REQUIRE(ic.has_value());
    for (const SkinnedInstance instance : {*ia, *ib, *ic}) {
        CY_REQUIRE(scene.set_pose(instance, 0).has_value());
    }
    CY_REQUIRE(run_frame(fixture.device(), scene, 0).has_value());
    const u32 first_b = scene.constants(*ib)->first_input_vertex;
    const u32 first_c = scene.constants(*ic)->first_input_vertex;
    CY_CHECK_GE(first_b, 5U);
    CY_CHECK_EQ(scene.constants(*ib)->influences, 8U);
    CY_CHECK_GE(first_c, (first_b + 5U) * 2U);
}

CY_TEST_CASE("skinned scene: a removed instance's window is reused and no other moves") {
    NullFixture fixture;
    CY_REQUIRE(fixture.ok());
    SkinnedScene scene(allocator());
    CY_REQUIRE(scene.create(fixture.device(), sized()).has_value());
    const TestMesh mesh(8, 1);
    Expected<u32, Error> id = scene.add_mesh(mesh.description(InfluenceCount::Four, 1));
    CY_REQUIRE(id.has_value());
    Expected<SkinnedInstance, Error> first = scene.add_instance(*id);
    Expected<SkinnedInstance, Error> second = scene.add_instance(*id);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_REQUIRE(scene.set_pose(*second, 0).has_value());
    CY_REQUIRE(run_frame(fixture.device(), scene, 0).has_value());
    const u32 second_window = scene.constants(*second)->first_output_vertex;

    CY_REQUIRE(scene.remove_instance(*first).has_value());
    CY_CHECK_FALSE(scene.live(*first));
    CY_CHECK_FALSE(scene.remove_instance(*first).has_value());
    CY_CHECK_FALSE(scene.set_pose(*first, 0).has_value());
    Expected<SkinnedInstance, Error> third = scene.add_instance(*id);
    CY_REQUIRE(third.has_value());
    CY_CHECK_EQ(third->index, first->index);
    CY_CHECK_NE(third->generation, first->generation);
    CY_REQUIRE(scene.set_pose(*third, 0).has_value());
    CY_REQUIRE(run_frame(fixture.device(), scene, 0).has_value());
    CY_CHECK_EQ(scene.constants(*second)->first_output_vertex, second_window);
    CY_CHECK_EQ(scene.constants(*third)->first_output_vertex, 0U);
}

CY_TEST_CASE("skinned scene: what it refuses, by name") {
    NullFixture fixture;
    CY_REQUIRE(fixture.ok());
    SkinnedScene scene(allocator());
    SkinnedSceneDescription zero = sized();
    zero.max_pose_matrices = 0;
    CY_CHECK_FALSE(scene.create(fixture.device(), zero).has_value());
    CY_REQUIRE(scene.create(fixture.device(), sized()).has_value());
    CY_CHECK_FALSE(scene.create(fixture.device(), sized()).has_value());

    // Frames and influences that do not match the positions.
    const TestMesh mesh(4, 1);
    SkinnedMeshDescription broken = mesh.description(InfluenceCount::Eight, 1);
    CY_CHECK_FALSE(scene.add_mesh(broken).has_value());
    CY_CHECK_FALSE(scene.add_mesh(mesh.description(InfluenceCount::Four, 0)).has_value());
    // A mesh past the input buffers.
    const TestMesh huge(65, 1);
    CY_CHECK_FALSE(scene.add_mesh(huge.description(InfluenceCount::Four, 1)).has_value());
    CY_CHECK_FALSE(scene.add_instance(7).has_value());

    Expected<u32, Error> id = scene.add_mesh(mesh.description(InfluenceCount::Four, 4));
    CY_REQUIRE(id.has_value());
    Expected<SkinnedInstance, Error> instance = scene.add_instance(*id);
    CY_REQUIRE(instance.has_value());
    // Bones past the pose buffer: a stale `matrix_offset` reads off its end.
    CY_CHECK_FALSE(scene.set_pose(*instance, 30).has_value());
    // A range past the world, and a world past the buffer.
    std::vector<Mat4> world(40, Mat4::identity());
    CY_CHECK_FALSE(scene.upload_poses(Span<const Mat4>(world.data(), 8), 4, 8).has_value());
    CY_CHECK_FALSE(
        scene.upload_poses(Span<const Mat4>(world.data(), world.size()), 30, 8).has_value());
}
