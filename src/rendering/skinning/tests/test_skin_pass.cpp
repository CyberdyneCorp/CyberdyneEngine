// The skinning compute dispatch, checked against `cpu_reference_skin` BY COMPARING BUFFERS.
// M8.d.
//
// ================================================================================================
// WHAT THIS SUITE ASSERTS, AND WHY IT IS NOT A SCREENSHOT
// ================================================================================================
//
// `src/servers/render/geometry/` holds a CPU implementation of exactly the algorithm skin.slang
// runs, and `src/servers/render/geometry/tests/test_skin_dispatch.cpp` checks THAT against values
// worked out on paper — so the chain is: paper → reference → dispatch, and no link in it is a
// number somebody captured from a run.
//
// Every case here skins one mesh twice, once through `cpu_reference_skin` and once through
// `SkinPass`, and compares the two output buffers vertex by vertex.
//
// ================================================================================================
// FLOATING POINT: WHAT IS ASSERTED EXACTLY AND WHAT IS NOT
// ================================================================================================
//
// Positions are compared to an ABSOLUTE tolerance, `kPositionTolerance`, and the suite reports the
// largest difference it saw so that a tolerance quietly absorbing a real divergence would show up
// as a number that moved. The reason it is not bit equality: a SPIR-V driver may contract a
// multiply and an add into a fused multiply-add and a C++ compiler on this host may not, and the
// blended matrix is eight such products per component. A test that demanded the last bit would be
// asserting a property of the driver.
//
// Frames are compared to `kFrameTolerance` SNORM STEPS, not to a float tolerance, because they are
// re-encoded: the output is two octahedral pairs of 16-bit signed normalised components, the
// encode ends in a rounding, and a difference of one step is the rounding disagreeing about a
// halfway value. One step of 32767 is about a thousandth of a degree.
//
// ================================================================================================
// VALIDATION IS COUNTED
// ================================================================================================
//
// `rhi-and-render-graph`: a frame that renders but trips validation is not a frame that works.
// Every case asserts the device's cumulative validation error count is still zero afterwards.

#include "skin_fixture.h"

#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/skinning/skin_pass.h>

#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace cy;
using namespace cy::skin_test;
using cy::render::PackedNormalTangent;
using cy::render::geometry::GpuSkinInfluence;
using cy::render::geometry::InfluenceCount;
using cy::render::geometry::PoseSource;
using cy::render::geometry::SkinningDescriptor;
using cy::render::geometry::SkinningMethod;
using cy::rendering::skinning::SkinPass;
using cy::rendering::skinning::SkinPassDescription;

namespace {

/// Metres. MEASURED WORST DIFFERENCE ON THIS ENGINE'S REFERENCE MACHINE: 1.19e-7, which is one ulp
/// at unit scale — so the tolerance is about eight times the divergence actually seen, and a wrong
/// bone or a dropped translation column fails it by six orders of magnitude. It is not bit
/// equality because a SPIR-V driver may contract a multiply and an add into a fused multiply-add
/// where a C++ compiler on the host does not, and the blended matrix is eight such products per
/// component.
constexpr f32 kPositionTolerance = 1.0e-6F;

/// Steps of 1/32767, on each of the four octahedral components. MEASURED: zero — the re-encode
/// agrees exactly here. Two steps of headroom because the encode ends in a rounding and a driver
/// that disagreed about one halfway value would differ by one, and the octahedral fold can carry
/// that into the second component.
constexpr i32 kFrameTolerance = 2;

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

/// A strip of vertices along +X, half bound to the upper arm and half to the forearm, with a band
/// in the middle split between them.
///
/// 200 VERTICES, WHICH IS NOT A MULTIPLE OF 64. The dispatch is `[numthreads(64,1,1)]`, so a count
/// that divided evenly would never run the `vertex >= constants.vertex_count` guard and a dispatch
/// that wrote past the end would pass every case.
struct ArmMesh {
    static constexpr u32 kVertices = 200;

    std::vector<Vec3> positions;
    std::vector<PackedNormalTangent> frames;
    std::vector<GpuSkinInfluence> influences;

    ArmMesh() {
        positions.reserve(kVertices);
        frames.reserve(kVertices);
        influences.reserve(kVertices);
        for (u32 index = 0; index < kVertices; ++index) {
            const f32 along = static_cast<f32>(index) / static_cast<f32>(kVertices - 1);
            const f32 x = along * 4.0F;
            const f32 y = ((index % 2U) == 0U) ? 0.15F : -0.15F;
            positions.push_back(Vec3{x, y, 0.0F});
            // The frame turns along the strip so the case is not one direction repeated: a
            // dispatch that wrote the first vertex's frame everywhere would pass otherwise.
            const f32 angle = along * 2.0F * std::numbers::pi_v<f32>;
            frames.push_back(cy::render::pack_normal_tangent(
                Vec3{std::cos(angle), 0.0F, std::sin(angle)},
                Vec3{0.0F, std::cos(angle), -std::sin(angle)}, (index % 3U) == 0U ? -1.0F : 1.0F));

            // Below the elbow: the upper arm. Above it: the forearm. Across it: a graded blend, the
            // case a single-bone rig cannot produce.
            u8 indices[4] = {0, 1, 0, 0};
            u8 weights[4] = {0, 0, 0, 0};
            if (x <= 0.8F) {
                weights[0] = 255;
            } else if (x >= 1.2F) {
                weights[1] = 255;
            } else {
                const f32 t = (x - 0.8F) / 0.4F;
                const auto forearm = static_cast<u8>(t * 255.0F);
                weights[1] = forearm;
                weights[0] = static_cast<u8>(255U - forearm);
            }
            influences.push_back(cy::render::geometry::skin_influence(indices, weights));
        }
    }
};

SkinningDescriptor arm_descriptor(u32 vertices, u32 pose_offset = 0) {
    SkinningDescriptor descriptor;
    descriptor.vertex_count = vertices;
    descriptor.bone_count = 2;
    descriptor.retained_bones = 2;
    descriptor.source = pose_offset == 0 ? PoseSource::UploadedPerSkin : PoseSource::GpuPoseWorld;
    descriptor.pose_offset = pose_offset;
    return descriptor;
}

/// Run one dispatch and hand back what it wrote. One frame, drained before it returns.
struct DispatchResult {
    std::vector<Vec3> positions;
    std::vector<PackedNormalTangent> frames;
    u32 vertex_offset = 0;
};

[[nodiscard]] bool run_dispatch(DeviceFixture& gpu, SkinPass& pass, const ArmMesh& mesh,
                                const SkinningDescriptor& descriptor,
                                Span<const Mat4> skinning_matrices, u64 frame_index,
                                DispatchResult& out) {
    if (!pass.upload(descriptor, skinning_matrices, frame_index).has_value()) {
        return false;
    }
    if (!gpu.device().begin_frame().has_value()) {
        return false;
    }
    cy::rendering::RenderGraph graph(allocator());
    if (!pass.declare(graph).has_value()) {
        return false;
    }
    cy::rendering::GraphExecutor executor(allocator(), gpu.device());
    if (!executor.execute(graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{})
             .has_value()) {
        return false;
    }
    if (!gpu.device().wait_idle().has_value()) {
        return false;
    }
    // ENDED, not merely waited on: `begin_frame` recycles the oldest in-flight frame's pools, and a
    // device whose frames are all still open refuses to start another. The double-buffering case
    // below runs two frames and is the one that needs it.
    if (!gpu.device().end_frame().has_value()) {
        return false;
    }

    Expected<Span<const Vec3>, Error> positions = pass.read_back_positions();
    Expected<Span<const PackedNormalTangent>, Error> frames = pass.read_back_frames();
    if (!positions.has_value() || !frames.has_value()) {
        return false;
    }
    out.vertex_offset = pass.vertex_offset();
    out.positions.assign(positions->begin(), positions->end());
    out.frames.assign(frames->begin(), frames->end());
    (void)mesh;
    return true;
}

/// The reference's answer for the same description, written into buffers the same size as the
/// device's so that an offset mistake shows as a mismatch rather than as an out-of-range read.
void reference_answer(const ArmMesh& mesh, const cy::render::geometry::GpuSkinConstants& constants,
                      Span<const Mat4> skinning_matrices, u32 capacity,
                      std::vector<Vec3>& positions, std::vector<PackedNormalTangent>& frames) {
    std::vector<cy::render::geometry::GpuBoneMatrix> bones;
    bones.reserve(skinning_matrices.size());
    for (const Mat4& matrix : skinning_matrices) {
        bones.push_back(cy::render::geometry::pack_bone_matrix(matrix));
    }
    positions.assign(static_cast<usize>(capacity) * 2U, Vec3{0.0F, 0.0F, 0.0F});
    frames.assign(static_cast<usize>(capacity) * 2U, PackedNormalTangent{});

    cy::render::geometry::SkinInputs inputs;
    inputs.bones = Span<const cy::render::geometry::GpuBoneMatrix>(bones.data(), bones.size());
    inputs.positions = Span<const Vec3>(mesh.positions.data(), mesh.positions.size());
    inputs.frames = Span<const PackedNormalTangent>(mesh.frames.data(), mesh.frames.size());
    inputs.influences =
        Span<const GpuSkinInfluence>(mesh.influences.data(), mesh.influences.size());
    cy::render::geometry::SkinOutputs outputs;
    outputs.positions = Span<Vec3>(positions.data(), positions.size());
    outputs.frames = Span<PackedNormalTangent>(frames.data(), frames.size());
    CY_REQUIRE(cpu_reference_skin(constants, inputs, outputs).has_value());
}

/// Compare one range, reporting the worst difference so a tolerance cannot silently absorb a real
/// divergence.
void compare(Span<const Vec3> expected, Span<const Vec3> measured, u32 first, u32 count,
             const char* what) {
    f32 worst = 0.0F;
    for (u32 index = 0; index < count; ++index) {
        const Vec3& want = expected[first + index];
        const Vec3& got = measured[first + index];
        worst = std::fmax(worst, std::fabs(got.x - want.x));
        worst = std::fmax(worst, std::fabs(got.y - want.y));
        worst = std::fmax(worst, std::fabs(got.z - want.z));
    }
    std::fprintf(stderr, "%s: worst position difference %g over %u vertices\n", what,
                 static_cast<double>(worst), count);
    CY_CHECK(worst <= kPositionTolerance);
}

void compare_frames(Span<const PackedNormalTangent> expected, Span<const PackedNormalTangent> got,
                    u32 first, u32 count) {
    i32 worst = 0;
    for (u32 index = 0; index < count; ++index) {
        const PackedNormalTangent& want = expected[first + index];
        const PackedNormalTangent& have = got[first + index];
        const i32 differences[4] = {
            static_cast<i32>(have.normal[0]) - static_cast<i32>(want.normal[0]),
            static_cast<i32>(have.normal[1]) - static_cast<i32>(want.normal[1]),
            // The tangent's low bit is the bitangent sign, which must match EXACTLY: a flipped
            // bitangent is a normal map read inside out, and it is not a rounding.
            static_cast<i32>(have.tangent[0] & ~1) - static_cast<i32>(want.tangent[0] & ~1),
            static_cast<i32>(have.tangent[1]) - static_cast<i32>(want.tangent[1]),
        };
        CY_CHECK((have.tangent[0] & 1) == (want.tangent[0] & 1));
        for (const i32 difference : differences) {
            worst = std::max(worst, difference < 0 ? -difference : difference);
        }
    }
    std::fprintf(stderr, "frames: worst difference %d snorm steps over %u vertices\n", worst,
                 count);
    CY_CHECK(worst <= kFrameTolerance);
}

}  // namespace

CY_TEST_CASE("skinning: the dispatch and the CPU reference skin one arm to the same vertices") {
    DeviceFixture gpu("cy_test_render_skinning");
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }
    const ArmMesh mesh;
    const std::vector<Mat4> pose = elbow_pose(std::numbers::pi_v<f32> * 0.5F);

    SkinPassDescription description;
    description.max_vertices = ArmMesh::kVertices;
    description.max_bones = 2;
    description.with_frames = true;
    description.read_back = true;

    SkinPass pass;
    CY_REQUIRE(pass.create(allocator(), gpu.device(), description).has_value());
    CY_REQUIRE(pass.upload_mesh(
                       Span<const Vec3>(mesh.positions.data(), mesh.positions.size()),
                       Span<const PackedNormalTangent>(mesh.frames.data(), mesh.frames.size()),
                       Span<const GpuSkinInfluence>(mesh.influences.data(), mesh.influences.size()))
                   .has_value());

    const SkinningDescriptor descriptor = arm_descriptor(ArmMesh::kVertices);
    DispatchResult measured;
    CY_REQUIRE(run_dispatch(gpu, pass, mesh, descriptor, Span<const Mat4>(pose.data(), pose.size()),
                            0, measured));

    std::vector<Vec3> expected_positions;
    std::vector<PackedNormalTangent> expected_frames;
    reference_answer(mesh, pass.constants(), Span<const Mat4>(pose.data(), pose.size()),
                     ArmMesh::kVertices, expected_positions, expected_frames);

    compare(Span<const Vec3>(expected_positions.data(), expected_positions.size()),
            Span<const Vec3>(measured.positions.data(), measured.positions.size()),
            measured.vertex_offset, ArmMesh::kVertices, "elbow at a quarter turn");
    compare_frames(Span<const PackedNormalTangent>(expected_frames.data(), expected_frames.size()),
                   Span<const PackedNormalTangent>(measured.frames.data(), measured.frames.size()),
                   measured.vertex_offset, ArmMesh::kVertices);

    // AND THE ANSWER IS NOT THE INPUT. A dispatch that copied its input would agree with a
    // reference that did the same, so the case also asserts that the far end of the arm moved to
    // where the elbow's quarter turn puts it: (4,±0.15,0) becomes (1∓0.15, 3, 0).
    const Vec3& tip = measured.positions[measured.vertex_offset + ArmMesh::kVertices - 1];
    CY_CHECK(std::fabs(tip.x - 1.15F) < 1.0e-4F);
    CY_CHECK(std::fabs(tip.y - 3.0F) < 1.0e-4F);
    CY_CHECK_EQ(gpu.validation_errors(), 0U);
}

CY_TEST_CASE("skinning: the pose offset selects this skin's slice of the shared world") {
    // `PoseWorld::matrix_offset(handle)` is where an instance's bones begin in the shared world and
    // it moves every publish. The pose below puts the elbow's two matrices behind two identities,
    // so a dispatch that ignored the offset would leave the arm in its bind pose and one that
    // applied it twice would read past the end.
    DeviceFixture gpu("cy_test_render_skinning_offset");
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }
    const ArmMesh mesh;
    const std::vector<Mat4> elbow = elbow_pose(std::numbers::pi_v<f32> * 0.5F);
    std::vector<Mat4> world = {Mat4::identity(), Mat4::identity(), elbow[0], elbow[1]};

    SkinPassDescription description;
    description.max_vertices = ArmMesh::kVertices;
    description.max_bones = 4;
    description.with_frames = true;
    description.read_back = true;

    SkinPass pass;
    CY_REQUIRE(pass.create(allocator(), gpu.device(), description).has_value());
    CY_REQUIRE(pass.upload_mesh(
                       Span<const Vec3>(mesh.positions.data(), mesh.positions.size()),
                       Span<const PackedNormalTangent>(mesh.frames.data(), mesh.frames.size()),
                       Span<const GpuSkinInfluence>(mesh.influences.data(), mesh.influences.size()))
                   .has_value());

    const SkinningDescriptor descriptor = arm_descriptor(ArmMesh::kVertices, 2);
    DispatchResult measured;
    CY_REQUIRE(run_dispatch(gpu, pass, mesh, descriptor,
                            Span<const Mat4>(world.data(), world.size()), 0, measured));

    std::vector<Vec3> expected_positions;
    std::vector<PackedNormalTangent> expected_frames;
    reference_answer(mesh, pass.constants(), Span<const Mat4>(world.data(), world.size()),
                     ArmMesh::kVertices, expected_positions, expected_frames);
    compare(Span<const Vec3>(expected_positions.data(), expected_positions.size()),
            Span<const Vec3>(measured.positions.data(), measured.positions.size()),
            measured.vertex_offset, ArmMesh::kVertices, "offset pose");

    const Vec3& tip = measured.positions[measured.vertex_offset + ArmMesh::kVertices - 1];
    CY_CHECK(std::fabs(tip.y - 3.0F) < 1.0e-4F);
    CY_CHECK_EQ(gpu.validation_errors(), 0U);
}

CY_TEST_CASE(
    "skinning: consecutive frames write the two halves of the output, and the previous "
    "one survives") {
    // "Output buffers SHALL be double buffered so the previous frame's positions are available for
    // motion vectors." Two frames at two different elbow angles: after the second, the half the
    // first wrote must still hold the first's answer. A single-buffered output would have
    // overwritten it and this case would report the second angle twice.
    DeviceFixture gpu("cy_test_render_skinning_double");
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }
    const ArmMesh mesh;
    const std::vector<Mat4> first_pose = elbow_pose(0.0F);
    const std::vector<Mat4> second_pose = elbow_pose(std::numbers::pi_v<f32> * 0.5F);

    SkinPassDescription description;
    description.max_vertices = ArmMesh::kVertices;
    description.max_bones = 2;
    description.with_frames = true;
    description.read_back = true;

    SkinPass pass;
    CY_REQUIRE(pass.create(allocator(), gpu.device(), description).has_value());
    CY_REQUIRE(pass.upload_mesh(
                       Span<const Vec3>(mesh.positions.data(), mesh.positions.size()),
                       Span<const PackedNormalTangent>(mesh.frames.data(), mesh.frames.size()),
                       Span<const GpuSkinInfluence>(mesh.influences.data(), mesh.influences.size()))
                   .has_value());
    const SkinningDescriptor descriptor = arm_descriptor(ArmMesh::kVertices);

    // ONE FRAME AT A TIME, drained before the next begins. `SkinPass` owns one descriptor set
    // naming one set of buffers; two dispatches in flight over them is a write-after-write the
    // graph cannot see, because it has no cross-frame state for an imported buffer. The remedy is a
    // pass per frame in flight, and this suite is not the place to pretend otherwise.
    DispatchResult at_rest;
    CY_REQUIRE(run_dispatch(gpu, pass, mesh, descriptor,
                            Span<const Mat4>(first_pose.data(), first_pose.size()), 0, at_rest));
    DispatchResult turned;
    CY_REQUIRE(run_dispatch(gpu, pass, mesh, descriptor,
                            Span<const Mat4>(second_pose.data(), second_pose.size()), 1, turned));

    CY_CHECK_NE(at_rest.vertex_offset, turned.vertex_offset);
    CY_CHECK_EQ(turned.vertex_offset, pass.vertex_offset());
    CY_CHECK_EQ(at_rest.vertex_offset, pass.previous_vertex_offset());

    // The second frame's half holds the quarter turn: the arm's tip is up at y = 3.
    const Vec3& current_tip = turned.positions[turned.vertex_offset + ArmMesh::kVertices - 1];
    CY_CHECK(std::fabs(current_tip.y - 3.0F) < 1.0e-4F);
    // The first frame's half still holds the rest pose: the tip is out at x = 4, unmoved.
    const Vec3& previous_tip =
        turned.positions[pass.previous_vertex_offset() + ArmMesh::kVertices - 1];
    CY_CHECK(std::fabs(previous_tip.x - 4.0F) < 1.0e-4F);
    CY_CHECK(std::fabs(previous_tip.y - 3.0F) > 1.0F);
    CY_CHECK_EQ(gpu.validation_errors(), 0U);
}

CY_TEST_CASE("skinning: eight influences read both records per vertex") {
    // The lanes of the second record carry weight like any other. Every vertex here is bound
    // through lane 5 and nothing at all through the first record, so a dispatch that read only the
    // first would leave the whole mesh in its bind pose.
    DeviceFixture gpu("cy_test_render_skinning_eight");
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }
    ArmMesh mesh;
    std::vector<GpuSkinInfluence> wide;
    wide.reserve(static_cast<usize>(ArmMesh::kVertices) * 2U);
    for (u32 index = 0; index < ArmMesh::kVertices; ++index) {
        const u8 empty_indices[4] = {0, 0, 0, 0};
        const u8 empty_weights[4] = {0, 0, 0, 0};
        wide.push_back(cy::render::geometry::skin_influence(empty_indices, empty_weights));
        const u8 indices[4] = {0, 1, 0, 0};
        const u8 weights[4] = {0, 255, 0, 0};
        wide.push_back(cy::render::geometry::skin_influence(indices, weights));
    }
    mesh.influences = wide;

    SkinPassDescription description;
    description.max_vertices = ArmMesh::kVertices;
    description.max_bones = 2;
    description.with_frames = true;
    description.read_back = true;

    SkinPass pass;
    CY_REQUIRE(pass.create(allocator(), gpu.device(), description).has_value());
    CY_REQUIRE(pass.upload_mesh(
                       Span<const Vec3>(mesh.positions.data(), mesh.positions.size()),
                       Span<const PackedNormalTangent>(mesh.frames.data(), mesh.frames.size()),
                       Span<const GpuSkinInfluence>(mesh.influences.data(), mesh.influences.size()))
                   .has_value());

    SkinningDescriptor descriptor = arm_descriptor(ArmMesh::kVertices);
    descriptor.influences = InfluenceCount::Eight;
    const std::vector<Mat4> pose = elbow_pose(std::numbers::pi_v<f32> * 0.5F);
    DispatchResult measured;
    CY_REQUIRE(run_dispatch(gpu, pass, mesh, descriptor, Span<const Mat4>(pose.data(), pose.size()),
                            0, measured));

    std::vector<Vec3> expected_positions;
    std::vector<PackedNormalTangent> expected_frames;
    reference_answer(mesh, pass.constants(), Span<const Mat4>(pose.data(), pose.size()),
                     ArmMesh::kVertices, expected_positions, expected_frames);
    compare(Span<const Vec3>(expected_positions.data(), expected_positions.size()),
            Span<const Vec3>(measured.positions.data(), measured.positions.size()),
            measured.vertex_offset, ArmMesh::kVertices, "eight influences");

    // Every vertex is on the forearm, so the whole strip turned: the tip is at y = 3.
    const Vec3& tip = measured.positions[measured.vertex_offset + ArmMesh::kVertices - 1];
    CY_CHECK(std::fabs(tip.y - 3.0F) < 1.0e-4F);
    CY_CHECK_EQ(gpu.validation_errors(), 0U);
}

CY_TEST_CASE(
    "skinning: the pass refuses what the dispatch cannot do, before a command buffer "
    "exists") {
    DeviceFixture gpu("cy_test_render_skinning_refusals");
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }
    const ArmMesh mesh;
    SkinPassDescription description;
    description.max_vertices = ArmMesh::kVertices;
    description.max_bones = 2;

    SkinPass pass;
    CY_REQUIRE(pass.create(allocator(), gpu.device(), description).has_value());

    // A dispatch before the bind pose has been written would skin whatever the allocator left.
    const std::vector<Mat4> pose = elbow_pose(0.0F);
    SkinningDescriptor descriptor = arm_descriptor(ArmMesh::kVertices);
    CY_CHECK(!pass.upload(descriptor, Span<const Mat4>(pose.data(), pose.size()), 0).has_value());

    CY_REQUIRE(pass.upload_mesh(
                       Span<const Vec3>(mesh.positions.data(), mesh.positions.size()),
                       Span<const PackedNormalTangent>(mesh.frames.data(), mesh.frames.size()),
                       Span<const GpuSkinInfluence>(mesh.influences.data(), mesh.influences.size()))
                   .has_value());

    SkinningDescriptor dual = descriptor;
    dual.method = SkinningMethod::DualQuaternion;
    const Status refused_dual = pass.upload(dual, Span<const Mat4>(pose.data(), pose.size()), 0);
    CY_REQUIRE(!refused_dual.has_value());
    CY_CHECK(refused_dual.error().code == ErrorCode::NotImplemented);

    SkinningDescriptor shapes = descriptor;
    shapes.blend_shape_count = 4;
    CY_CHECK(!pass.upload(shapes, Span<const Mat4>(pose.data(), pose.size()), 0).has_value());

    // A pose that does not reach the skin's slice.
    SkinningDescriptor offset = descriptor;
    offset.source = PoseSource::GpuPoseWorld;
    offset.pose_offset = 1;
    CY_CHECK(!pass.upload(offset, Span<const Mat4>(pose.data(), pose.size()), 0).has_value());

    // And declaring a frame nothing was uploaded for.
    cy::rendering::RenderGraph graph(allocator());
    SkinPass fresh;
    CY_REQUIRE(fresh.create(allocator(), gpu.device(), description).has_value());
    CY_CHECK(!fresh.declare(graph).has_value());
    CY_CHECK_EQ(gpu.validation_errors(), 0U);
}
