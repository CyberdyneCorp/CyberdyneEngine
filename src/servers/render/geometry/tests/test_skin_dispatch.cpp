// The skinning arithmetic, against values worked out on paper. M8.d.
//
// ================================================================================================
// WHY EVERY EXPECTED VALUE HERE IS A FRACTION AND NOT A CAPTURED NUMBER
// ================================================================================================
//
// `cpu_reference_skin` is the expected value the compute pass in `src/rendering/skinning/` is
// compared against. A reference checked against numbers somebody ran the reference to obtain is a
// reference that agrees with itself, which is the one property it cannot be allowed to have — so
// every case below states the pose, states the vertex, and states where a person computing the
// blend by hand says that vertex lands.
//
// THE POSE IS THE ELBOW, because it is the smallest rig in which the answer is not the input. Bone
// 0 is the upper arm at rest, so its skinning matrix is the identity. Bone 1 is the forearm rotated
// a quarter turn about +Z around an elbow at (1, 0, 0), so its skinning matrix is
// `T(1,0,0) · Rz(90°) · T(-1,0,0)` — which, written out, is
//
//     [ 0 -1  0 |  1 ]      because Rz(90°) sends (1,0,0) to (0,1,0), so the translation that
//     [ 1  0  0 | -1 ]      keeps the elbow fixed is  e − R·e  =  (1,0,0) − (0,1,0)  =  (1,−1,0).
//     [ 0  0  1 |  0 ]
//
// A vertex at (2, 0, 0) — one unit past the elbow — therefore lands at (1, 1, 0) under bone 1 alone
// and stays at (2, 0, 0) under bone 0 alone, and a linear blend of the two matrices applied to it
// gives `w0·(2,0,0) + w1·(1,1,0)` because a matrix blend is linear in the matrix.

#include <cy/core/math/matrix.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/geometry/skin_dispatch.h>
#include <cy/test/test.h>

#include <cmath>
#include <vector>

using namespace cy::render::geometry;
using cy::f32;
using cy::Mat4;
using cy::Span;
using cy::u32;
using cy::u8;
using cy::Vec3;
using cy::Vec4;
using cy::render::PackedNormalTangent;
using cy::render::unpack_bitangent_sign;
using cy::render::unpack_normal;
using cy::render::unpack_tangent;

namespace {

/// Absolute rather than relative, because half of these components are zero and a relative
/// comparison against zero passes for any value at all. 1e-6 is about eight ulps at unit scale.
constexpr f32 kAbsolute = 1e-6F;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

void check_close(Vec3 measured, f32 x, f32 y, f32 z) {
    CY_CHECK(std::fabs(measured.x - x) < kAbsolute);
    CY_CHECK(std::fabs(measured.y - y) < kAbsolute);
    CY_CHECK(std::fabs(measured.z - z) < kAbsolute);
}

/// The elbow pose, written out as the two matrices the header comment derives.
std::vector<GpuBoneMatrix> elbow_pose() {
    GpuBoneMatrix upper_arm;  // the identity, which is its default
    GpuBoneMatrix forearm;
    forearm.rows[0][0] = 0.0F;
    forearm.rows[0][1] = -1.0F;
    forearm.rows[0][2] = 0.0F;
    forearm.rows[0][3] = 1.0F;
    forearm.rows[1][0] = 1.0F;
    forearm.rows[1][1] = 0.0F;
    forearm.rows[1][2] = 0.0F;
    forearm.rows[1][3] = -1.0F;
    forearm.rows[2][0] = 0.0F;
    forearm.rows[2][1] = 0.0F;
    forearm.rows[2][2] = 1.0F;
    forearm.rows[2][3] = 0.0F;
    return {upper_arm, forearm};
}

GpuSkinInfluence influence(u8 i0, u8 w0, u8 i1 = 0, u8 w1 = 0, u8 i2 = 0, u8 w2 = 0, u8 i3 = 0,
                           u8 w3 = 0) {
    const u8 indices[4] = {i0, i1, i2, i3};
    const u8 weights[4] = {w0, w1, w2, w3};
    return skin_influence(indices, weights);
}

SkinningDescriptor elbow_descriptor(u32 vertices) {
    SkinningDescriptor descriptor;
    descriptor.vertex_count = vertices;
    descriptor.bone_count = 2;
    descriptor.retained_bones = 2;
    return descriptor;
}

}  // namespace

CY_TEST_CASE(
    "skin dispatch: a vertex bound entirely to the rotated bone lands where the pose "
    "puts it") {
    // ONE BONE AT FULL WEIGHT, so the answer is the bone's own matrix applied to the vertex and the
    // arithmetic is exact — 255/255 is 1.0 with no rounding anywhere, and every product below is a
    // 0 or a 1 times a small integer. This is the case that would catch a transposed matrix, a
    // dropped translation column, or a weight scale of 1/256 instead of 1/255.
    const std::vector<GpuBoneMatrix> pose = elbow_pose();
    const Vec3 rest{2.0F, 0.0F, 0.0F};
    const GpuSkinInfluence weights = influence(1, 255);

    const cy::Expected<GpuSkinConstants, cy::Error> constants =
        make_skin_constants(elbow_descriptor(1), false, 0, 0);
    CY_REQUIRE(constants.has_value());

    Vec3 skinned{0.0F, 0.0F, 0.0F};
    SkinInputs inputs;
    inputs.bones = Span<const GpuBoneMatrix>(pose.data(), pose.size());
    inputs.positions = Span<const Vec3>(&rest, 1);
    inputs.influences = Span<const GpuSkinInfluence>(&weights, 1);
    SkinOutputs outputs;
    outputs.positions = Span<Vec3>(&skinned, 1);
    CY_REQUIRE(cpu_reference_skin(*constants, inputs, outputs).has_value());

    // A quarter turn about the elbow at (1,0,0) carries (2,0,0) to (1,1,0). Exactly.
    CY_CHECK(skinned.x == 1.0F);
    CY_CHECK(skinned.y == 1.0F);
    CY_CHECK(skinned.z == 0.0F);
}

CY_TEST_CASE("skin dispatch: a vertex split between two bones lands on the blend of the two") {
    // The weight bytes are 128 and 127 because 255 does not halve: the dispatch reads a weight as
    // `byte / 255`, so the nearest thing to an even split is 128/255 and 127/255, and the expected
    // value is written as those fractions rather than as 0.5 — which is the difference between a
    // test that knows the encoding and one that assumes it.
    const std::vector<GpuBoneMatrix> pose = elbow_pose();
    const Vec3 rest{2.0F, 0.0F, 0.0F};
    const GpuSkinInfluence weights = influence(0, 128, 1, 127);

    const cy::Expected<GpuSkinConstants, cy::Error> constants =
        make_skin_constants(elbow_descriptor(1), false, 0, 0);
    CY_REQUIRE(constants.has_value());

    Vec3 skinned{0.0F, 0.0F, 0.0F};
    SkinInputs inputs;
    inputs.bones = Span<const GpuBoneMatrix>(pose.data(), pose.size());
    inputs.positions = Span<const Vec3>(&rest, 1);
    inputs.influences = Span<const GpuSkinInfluence>(&weights, 1);
    SkinOutputs outputs;
    outputs.positions = Span<Vec3>(&skinned, 1);
    CY_REQUIRE(cpu_reference_skin(*constants, inputs, outputs).has_value());

    //   p' = (128/255)·(2,0,0) + (127/255)·(1,1,0)
    //      = ((256 + 127)/255, 127/255, 0)
    //      = (383/255, 127/255, 0)
    check_close(skinned, 383.0F / 255.0F, 127.0F / 255.0F, 0.0F);
    // And it is INSIDE the arc the two rigid answers bound, which is linear blend skinning's
    // signature and the reason dual quaternion skinning exists. |p'| < 2.
    const f32 length = std::sqrt((skinned.x * skinned.x) + (skinned.y * skinned.y));
    CY_CHECK(length < 2.0F);
}

CY_TEST_CASE("skin dispatch: eight influences read two records per vertex") {
    // The second record is not an alignment pad: an eight-influence mesh is two consecutive
    // `GpuSkinInfluence`s and the lanes of the second one carry weight like any other. Here every
    // lane of the first record is empty and the whole vertex is bound through lane 5, so a
    // dispatch that read only the first record would leave the vertex at rest — and this case would
    // report (2,0,0) instead of (1,1,0).
    const std::vector<GpuBoneMatrix> pose = elbow_pose();
    const Vec3 rest{2.0F, 0.0F, 0.0F};
    const GpuSkinInfluence weights[2] = {influence(0, 0), influence(0, 0, 1, 255)};

    SkinningDescriptor descriptor = elbow_descriptor(1);
    descriptor.influences = InfluenceCount::Eight;
    const cy::Expected<GpuSkinConstants, cy::Error> constants =
        make_skin_constants(descriptor, false, 0, 0);
    CY_REQUIRE(constants.has_value());
    CY_CHECK_EQ(constants->influences, 8U);

    Vec3 skinned{0.0F, 0.0F, 0.0F};
    SkinInputs inputs;
    inputs.bones = Span<const GpuBoneMatrix>(pose.data(), pose.size());
    inputs.positions = Span<const Vec3>(&rest, 1);
    inputs.influences = Span<const GpuSkinInfluence>(weights, 2);
    SkinOutputs outputs;
    outputs.positions = Span<Vec3>(&skinned, 1);
    CY_REQUIRE(cpu_reference_skin(*constants, inputs, outputs).has_value());
    check_close(skinned, 1.0F, 1.0F, 0.0F);
}

CY_TEST_CASE("skin dispatch: the normal and tangent frame turns with the bone") {
    // A skinning pass that moved positions and left normals alone would shade the character as
    // though it were still in the bind pose — which looks like a lighting bug and is not one. The
    // frame is rotated by the blended matrix's linear part and re-encoded, so the tolerance here is
    // the octahedral encoding's own, not the arithmetic's.
    const std::vector<GpuBoneMatrix> pose = elbow_pose();
    const Vec3 rest{2.0F, 0.0F, 0.0F};
    const GpuSkinInfluence weights = influence(1, 255);
    // +X normal with a +Y tangent, and a negative bitangent sign so the case also asserts that the
    // sign bit survives a round trip rather than being lost in the re-encode.
    const PackedNormalTangent frame =
        cy::render::pack_normal_tangent(Vec3{1.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}, -1.0F);

    SkinningDescriptor descriptor = elbow_descriptor(1);
    const cy::Expected<GpuSkinConstants, cy::Error> constants =
        make_skin_constants(descriptor, true, 0, 0);
    CY_REQUIRE(constants.has_value());
    CY_CHECK((constants->flags & kSkinWriteFrames) != 0U);

    Vec3 skinned{0.0F, 0.0F, 0.0F};
    PackedNormalTangent skinned_frame;
    SkinInputs inputs;
    inputs.bones = Span<const GpuBoneMatrix>(pose.data(), pose.size());
    inputs.positions = Span<const Vec3>(&rest, 1);
    inputs.frames = Span<const PackedNormalTangent>(&frame, 1);
    inputs.influences = Span<const GpuSkinInfluence>(&weights, 1);
    SkinOutputs outputs;
    outputs.positions = Span<Vec3>(&skinned, 1);
    outputs.frames = Span<PackedNormalTangent>(&skinned_frame, 1);
    CY_REQUIRE(cpu_reference_skin(*constants, inputs, outputs).has_value());

    // Rz(90°) sends +X to +Y and +Y to −X. Two 16-bit octahedral components carry a direction to
    // well under a hundredth of a degree, so 1e-3 is three orders of magnitude of headroom.
    const Vec3 normal = unpack_normal(skinned_frame);
    CY_CHECK(std::fabs(normal.x - 0.0F) < 1e-3F);
    CY_CHECK(std::fabs(normal.y - 1.0F) < 1e-3F);
    const Vec3 tangent = unpack_tangent(skinned_frame);
    CY_CHECK(std::fabs(tangent.x + 1.0F) < 1e-3F);
    CY_CHECK(std::fabs(tangent.y - 0.0F) < 1e-3F);
    CY_CHECK(unpack_bitangent_sign(skinned_frame) == -1.0F);
}

CY_TEST_CASE("skin dispatch: an unweighted vertex is left where it is, not collapsed to the root") {
    // Every lane at weight zero. The answer is the rest position: dividing by a zero weight sum
    // would produce a NaN, and answering with the origin — which is what an unguarded accumulation
    // produces — drags the vertex to the character's root and makes the spike an artist can see and
    // cannot locate.
    const std::vector<GpuBoneMatrix> pose = elbow_pose();
    const Vec3 rest{2.0F, 3.0F, -4.0F};
    const GpuSkinInfluence weights = influence(1, 0);

    const cy::Expected<GpuSkinConstants, cy::Error> constants =
        make_skin_constants(elbow_descriptor(1), false, 0, 0);
    CY_REQUIRE(constants.has_value());

    Vec3 skinned{99.0F, 99.0F, 99.0F};
    SkinInputs inputs;
    inputs.bones = Span<const GpuBoneMatrix>(pose.data(), pose.size());
    inputs.positions = Span<const Vec3>(&rest, 1);
    inputs.influences = Span<const GpuSkinInfluence>(&weights, 1);
    SkinOutputs outputs;
    outputs.positions = Span<Vec3>(&skinned, 1);
    CY_REQUIRE(cpu_reference_skin(*constants, inputs, outputs).has_value());
    CY_CHECK(skinned.x == 2.0F);
    CY_CHECK(skinned.y == 3.0F);
    CY_CHECK(skinned.z == -4.0F);
}

CY_TEST_CASE("skin dispatch: a bone index past the skin's bone count is clamped, not followed") {
    // On a device an index past the buffer is undefined behaviour and on the CPU it is somebody
    // else's memory. The clamp is to the last bone, which keeps the vertex attached to the
    // character rather than at an address.
    const std::vector<GpuBoneMatrix> pose = elbow_pose();
    const Vec3 rest{2.0F, 0.0F, 0.0F};
    const GpuSkinInfluence weights = influence(200, 255);

    const cy::Expected<GpuSkinConstants, cy::Error> constants =
        make_skin_constants(elbow_descriptor(1), false, 0, 0);
    CY_REQUIRE(constants.has_value());

    Vec3 skinned{0.0F, 0.0F, 0.0F};
    SkinInputs inputs;
    inputs.bones = Span<const GpuBoneMatrix>(pose.data(), pose.size());
    inputs.positions = Span<const Vec3>(&rest, 1);
    inputs.influences = Span<const GpuSkinInfluence>(&weights, 1);
    SkinOutputs outputs;
    outputs.positions = Span<Vec3>(&skinned, 1);
    CY_REQUIRE(cpu_reference_skin(*constants, inputs, outputs).has_value());
    // Bone 1 is the last one, so the clamped answer is the forearm's.
    check_close(skinned, 1.0F, 1.0F, 0.0F);
}

CY_TEST_CASE("skin dispatch: the pose offset addresses this skin's slice of the shared world") {
    // `PoseWorld::matrix_offset(handle)` is where an instance's bones begin in the shared world,
    // and it MOVES EVERY PUBLISH because the world is double buffered. This case puts the elbow's
    // two matrices at offset 2 behind two identities, so a dispatch that ignored the offset would
    // report the rest position and a dispatch that applied it twice would run off the end.
    std::vector<GpuBoneMatrix> world(4);
    const std::vector<GpuBoneMatrix> pose = elbow_pose();
    world[2] = pose[0];
    world[3] = pose[1];

    const Vec3 rest{2.0F, 0.0F, 0.0F};
    const GpuSkinInfluence weights = influence(1, 255);

    SkinningDescriptor descriptor = elbow_descriptor(1);
    descriptor.source = PoseSource::GpuPoseWorld;
    descriptor.pose_offset = 2;
    const cy::Expected<GpuSkinConstants, cy::Error> constants =
        make_skin_constants(descriptor, false, 0, 0);
    CY_REQUIRE(constants.has_value());

    Vec3 skinned{0.0F, 0.0F, 0.0F};
    SkinInputs inputs;
    inputs.bones = Span<const GpuBoneMatrix>(world.data(), world.size());
    inputs.positions = Span<const Vec3>(&rest, 1);
    inputs.influences = Span<const GpuSkinInfluence>(&weights, 1);
    SkinOutputs outputs;
    outputs.positions = Span<Vec3>(&skinned, 1);
    CY_REQUIRE(cpu_reference_skin(*constants, inputs, outputs).has_value());
    check_close(skinned, 1.0F, 1.0F, 0.0F);

    // And a pose that does not reach the skin's slice is refused rather than read past.
    SkinInputs short_pose = inputs;
    short_pose.bones = Span<const GpuBoneMatrix>(world.data(), 3);
    CY_CHECK(!cpu_reference_skin(*constants, short_pose, outputs).has_value());
}

CY_TEST_CASE("skin dispatch: pack_bone_matrix is the one transposition in the path") {
    // `cy::Mat4` is column-major and applies to column vectors; the dispatch's record is three
    // ROWS. A shader that read the sixteen floats of a `Mat4` as rows would apply the transpose of
    // the intended rotation — which is a plausible-looking animation and the classic silent
    // corruption. This case pins the convention by applying both forms to the same point.
    const Mat4 matrix =
        Mat4::from_columns(Vec4{0.0F, 1.0F, 0.0F, 0.0F}, Vec4{-1.0F, 0.0F, 0.0F, 0.0F},
                           Vec4{0.0F, 0.0F, 1.0F, 0.0F}, Vec4{1.0F, -1.0F, 0.0F, 1.0F});
    const GpuBoneMatrix packed = pack_bone_matrix(matrix);

    const Vec3 point{2.0F, 0.0F, 0.0F};
    const Vec4 through_mat4 = matrix * Vec4{point.x, point.y, point.z, 1.0F};
    f32 weight_sum = 0.0F;
    const GpuSkinInfluence weights = influence(0, 255);
    const Vec3 through_record =
        skin_position(Span<const GpuBoneMatrix>(&packed, 1), &weights, 1, 1, 0, point, weight_sum);

    CY_CHECK(weight_sum == 1.0F);
    check_close(through_record, through_mat4.x, through_mat4.y, through_mat4.z);
    // And it is the elbow's answer, so a `Mat4` built the engine's way and a record built by hand
    // agree about what the quarter turn does.
    check_close(through_record, 1.0F, 1.0F, 0.0F);
}

CY_TEST_CASE("skin dispatch: what the dispatch still refuses, now that it refuses neither method") {
    // Until M11.c this case asserted that `SkinningMethod::DualQuaternion` and a non-zero
    // `blend_shape_count` were refused with `NotImplemented`. Both are implemented; what remains
    // refused is what the dispatch genuinely cannot execute.

    // A baked instance has no skeleton and skins nothing, and the descriptor's own rules are
    // checked first, so an invalid descriptor names its own defect.
    SkinningDescriptor baked = elbow_descriptor(1);
    baked.tier = AnimationTier::Baked;
    baked.retained_bones = 0;
    CY_CHECK(!make_skin_constants(baked, false, 0, 0).has_value());

    // AN ACTIVE LIST LONGER THAN THE AUTHORED SET. The active list is a compaction of the mesh's
    // shapes, so a dispatch told to read more of them than exist would read past the end of a
    // storage buffer — undefined behaviour on the device, with no diagnostic there.
    SkinningDescriptor shapes = elbow_descriptor(1);
    shapes.blend_shape_count = 2;
    CY_CHECK(make_skin_constants(shapes, false, 0, 0, 2).has_value());
    const cy::Expected<GpuSkinConstants, cy::Error> too_many =
        make_skin_constants(shapes, false, 0, 0, 3);
    CY_REQUIRE(!too_many.has_value());
    CY_CHECK(too_many.error().code == cy::ErrorCode::InvalidArgument);

    // And both methods now REACH a constant block, with the method visible in the flags rather than
    // in a second entry point.
    SkinningDescriptor dual = elbow_descriptor(1);
    dual.method = SkinningMethod::DualQuaternion;
    const cy::Expected<GpuSkinConstants, cy::Error> accepted =
        make_skin_constants(dual, true, 0, 0);
    CY_REQUIRE(accepted.has_value());
    CY_CHECK((accepted->flags & kSkinDualQuaternion) != 0U);
    CY_CHECK((make_skin_constants(elbow_descriptor(1), true, 0, 0)->flags & kSkinDualQuaternion) ==
             0U);
}

CY_TEST_CASE(
    "dual quaternion skinning: the elbow keeps its length where a matrix blend shortens it") {
    // ============================================================================================
    // THE CANDY WRAPPER, IN NUMBERS WORKED OUT ON PAPER
    // ============================================================================================
    //
    // This is the whole reason `SkinningMethod::DualQuaternion` is a declared option, and it is why
    // a dispatch that quietly linear-blended one would be a defect rather than an approximation.
    //
    // The pose is this file's elbow: bone 0 at rest, bone 1 a quarter turn about +Z around an elbow
    // at (1, 0, 0). The vertex is at (2, 0, 0) — ONE UNIT past the elbow — split evenly between the
    // two bones, which is the joint's own seam and the place the artefact appears.
    //
    //   MATRIX BLEND  averages the two matrices, so it averages the two ANSWERS:
    //                 0.5·(2,0,0) + 0.5·(1,1,0) = (1.5, 0.5, 0).
    //                 Its distance from the elbow is |(0.5, 0.5, 0)| = 0.70711.
    //                 The limb lost 29% of its thickness. That is the candy wrapper.
    //
    //   DUAL BLEND    averages the two TRANSFORMS and renormalises, which for two rotations about
    //                 one axis is the rotation halfway between them — Rz(45°) about the elbow:
    //                 (1,0,0) + Rz(45°)·(1,0,0) = (1.70711, 0.70711, 0).
    //                 Its distance from the elbow is exactly 1.
    const std::vector<GpuBoneMatrix> matrix_pose = elbow_pose();

    // The same pose, converted ONCE PER BONE — which is the cost argument this file's header makes.
    Mat4 identity;
    Mat4 forearm;
    forearm.columns[0] = Vec4{0.0F, 1.0F, 0.0F, 0.0F};
    forearm.columns[1] = Vec4{-1.0F, 0.0F, 0.0F, 0.0F};
    forearm.columns[2] = Vec4{0.0F, 0.0F, 1.0F, 0.0F};
    forearm.columns[3] = Vec4{1.0F, -1.0F, 0.0F, 1.0F};

    // `pack_bone_matrix` and `pack_bone_dual_quaternion` are two encodings of ONE matrix, and this
    // is the assertion that they are: the row form must be the elbow this file already uses.
    const GpuBoneMatrix repacked = pack_bone_matrix(forearm);
    CY_CHECK(std::fabs(repacked.rows[0][1] + 1.0F) < kAbsolute);
    CY_CHECK(std::fabs(repacked.rows[0][3] - 1.0F) < kAbsolute);
    CY_CHECK(std::fabs(repacked.rows[1][0] - 1.0F) < kAbsolute);
    CY_CHECK(std::fabs(repacked.rows[1][3] + 1.0F) < kAbsolute);

    const std::vector<GpuBoneDualQuaternion> dual_pose = {pack_bone_dual_quaternion(identity),
                                                          pack_bone_dual_quaternion(forearm)};
    // Rz(90°) is (0, 0, sin 45°, cos 45°) and the dual part of a rotation about an elbow at
    // (1, 0, 0) works out to (0, -sin 45°, 0, 0). Stated, not captured.
    constexpr f32 kHalfRoot2 = 0.70710678F;
    CY_CHECK(std::fabs(dual_pose[1].real[2] - kHalfRoot2) < 1e-5F);
    CY_CHECK(std::fabs(dual_pose[1].real[3] - kHalfRoot2) < 1e-5F);
    CY_CHECK(std::fabs(dual_pose[1].dual[1] + kHalfRoot2) < 1e-5F);
    CY_CHECK(std::fabs(dual_pose[1].scale[0] - 1.0F) < 1e-5F);

    const Vec3 rest[1] = {Vec3{2.0F, 0.0F, 0.0F}};
    const GpuSkinInfluence weights[1] = {influence(0, 128, 1, 127)};

    const auto distance_from_elbow = [](Vec3 skinned) {
        const Vec3 arm{skinned.x - 1.0F, skinned.y, skinned.z};
        return std::sqrt((arm.x * arm.x) + (arm.y * arm.y) + (arm.z * arm.z));
    };

    // The matrix path, unchanged, and it shortens the limb.
    Vec3 linear[1] = {};
    {
        const cy::Expected<GpuSkinConstants, cy::Error> constants =
            make_skin_constants(elbow_descriptor(1), false, 0, 0);
        CY_REQUIRE(constants.has_value());
        SkinInputs inputs;
        inputs.bones = Span<const GpuBoneMatrix>(matrix_pose.data(), matrix_pose.size());
        inputs.positions = Span<const Vec3>(rest, 1);
        inputs.influences = Span<const GpuSkinInfluence>(weights, 1);
        SkinOutputs outputs;
        outputs.positions = Span<Vec3>(linear, 1);
        CY_REQUIRE(cpu_reference_skin(*constants, inputs, outputs).has_value());
    }
    // 128/255 and 127/255, not exactly a half, so the expected value is the weighted blend.
    const f32 w1 = 127.0F / 255.0F;
    check_close(linear[0], 2.0F - w1, w1, 0.0F);
    CY_CHECK_LT(distance_from_elbow(linear[0]), 0.71F);

    // The dual path, on the same vertex and the same weights, and it does not.
    Vec3 dual[1] = {};
    {
        SkinningDescriptor descriptor = elbow_descriptor(1);
        descriptor.method = SkinningMethod::DualQuaternion;
        const cy::Expected<GpuSkinConstants, cy::Error> constants =
            make_skin_constants(descriptor, false, 0, 0);
        CY_REQUIRE(constants.has_value());
        SkinInputs inputs;
        inputs.bone_dual_quaternions =
            Span<const GpuBoneDualQuaternion>(dual_pose.data(), dual_pose.size());
        inputs.positions = Span<const Vec3>(rest, 1);
        inputs.influences = Span<const GpuSkinInfluence>(weights, 1);
        SkinOutputs outputs;
        outputs.positions = Span<Vec3>(dual, 1);
        CY_REQUIRE(cpu_reference_skin(*constants, inputs, outputs).has_value());
    }
    CY_TEST_MESSAGE("elbow at 50/50: matrix blend ", distance_from_elbow(linear[0]),
                    " from the elbow, dual quaternion ", distance_from_elbow(dual[0]));
    CY_CHECK(std::fabs(distance_from_elbow(dual[0]) - 1.0F) < 1e-4F);

    // AND THE TWO METHODS AGREE WHERE THEY MUST: a vertex bound entirely to one bone is a rigid
    // transform and there is nothing to blend, so both land on the pose's own answer. A dual path
    // that disagreed here would be a conversion bug rather than a blending choice.
    const GpuSkinInfluence whole[1] = {influence(1, 255)};
    Vec3 dual_whole[1] = {};
    SkinningDescriptor descriptor = elbow_descriptor(1);
    descriptor.method = SkinningMethod::DualQuaternion;
    const cy::Expected<GpuSkinConstants, cy::Error> constants =
        make_skin_constants(descriptor, false, 0, 0);
    CY_REQUIRE(constants.has_value());
    SkinInputs inputs;
    inputs.bone_dual_quaternions =
        Span<const GpuBoneDualQuaternion>(dual_pose.data(), dual_pose.size());
    inputs.positions = Span<const Vec3>(rest, 1);
    inputs.influences = Span<const GpuSkinInfluence>(whole, 1);
    SkinOutputs outputs;
    outputs.positions = Span<Vec3>(dual_whole, 1);
    CY_REQUIRE(cpu_reference_skin(*constants, inputs, outputs).has_value());
    check_close(dual_whole[0], 1.0F, 1.0F, 0.0F);

    // A DUAL-QUATERNION SKIN READS THE DUAL POSE AND NOT THE MATRIX ONE, and handing it the wrong
    // buffer is refused rather than read as zeros: a pose of zero dual quaternions normalises to
    // nothing and every vertex would sit at its bind position, which is a plausible-looking
    // statue nobody could trace.
    SkinInputs wrong_buffer;
    wrong_buffer.bones = Span<const GpuBoneMatrix>(matrix_pose.data(), matrix_pose.size());
    wrong_buffer.positions = Span<const Vec3>(rest, 1);
    wrong_buffer.influences = Span<const GpuSkinInfluence>(whole, 1);
    CY_CHECK(!cpu_reference_skin(*constants, wrong_buffer, outputs).has_value());
}

CY_TEST_CASE(
    "dual quaternion skinning: two bones on opposite hemispheres blend rather than cancelling") {
    // `q` AND `-q` ARE THE SAME ROTATION AND THEIR SUM IS ZERO. This is the one defect a
    // dual-quaternion skin has that a matrix skin does not, it is invisible in any test whose bones
    // were all converted from nearby rotations, and it presents as a vertex snapping to the origin.
    //
    // The pose here is built to have it: bone 1 is the elbow's rotation with EVERY COMPONENT
    // NEGATED, which is the identical rotation and the identical translation. A blend without the
    // sign fix gets a real part of `0.5·q + 0.5·(−q)` = 0, cannot normalise, and answers with the
    // bind pose; with the fix it is the same 45° answer as the case above.
    Mat4 identity;
    Mat4 forearm;
    forearm.columns[0] = Vec4{0.0F, 1.0F, 0.0F, 0.0F};
    forearm.columns[1] = Vec4{-1.0F, 0.0F, 0.0F, 0.0F};
    forearm.columns[2] = Vec4{0.0F, 0.0F, 1.0F, 0.0F};
    forearm.columns[3] = Vec4{1.0F, -1.0F, 0.0F, 1.0F};

    std::vector<GpuBoneDualQuaternion> pose = {pack_bone_dual_quaternion(identity),
                                               pack_bone_dual_quaternion(forearm)};
    for (u32 component = 0; component < 4U; ++component) {
        pose[1].real[component] = -pose[1].real[component];
        pose[1].dual[component] = -pose[1].dual[component];
    }

    const Vec3 rest[1] = {Vec3{2.0F, 0.0F, 0.0F}};
    const GpuSkinInfluence weights[1] = {influence(0, 128, 1, 127)};
    SkinningDescriptor descriptor = elbow_descriptor(1);
    descriptor.method = SkinningMethod::DualQuaternion;
    const cy::Expected<GpuSkinConstants, cy::Error> constants =
        make_skin_constants(descriptor, false, 0, 0);
    CY_REQUIRE(constants.has_value());

    Vec3 skinned[1] = {};
    SkinInputs inputs;
    inputs.bone_dual_quaternions = Span<const GpuBoneDualQuaternion>(pose.data(), pose.size());
    inputs.positions = Span<const Vec3>(rest, 1);
    inputs.influences = Span<const GpuSkinInfluence>(weights, 1);
    SkinOutputs outputs;
    outputs.positions = Span<Vec3>(skinned, 1);
    CY_REQUIRE(cpu_reference_skin(*constants, inputs, outputs).has_value());

    const Vec3 arm{skinned[0].x - 1.0F, skinned[0].y, skinned[0].z};
    const f32 length = std::sqrt((arm.x * arm.x) + (arm.y * arm.y) + (arm.z * arm.z));
    CY_TEST_MESSAGE("antipodal bones: vertex at (", skinned[0].x, ", ", skinned[0].y, ", ",
                    skinned[0].z, "), ", length, " from the elbow");
    CY_CHECK(std::fabs(length - 1.0F) < 1e-4F);
    // And it is NOT the bind pose, which is what a cancelled blend would have produced.
    CY_CHECK(std::fabs(skinned[0].x - 2.0F) > 0.1F);
}

CY_TEST_CASE(
    "blend shapes: the active shapes' deltas move the bind pose, and the skin carries the result") {
    // `rendering-geometry-and-resources`: blend shape deltas are applied "in the same compute pass
    // as skinning", and "WHEN 50 blend shapes exist and 5 have non-zero weight THEN only the 5
    // active shapes' deltas SHALL be read and applied."
    //
    // THE ORDER IS THE ASSERTION. A delta is authored against the modelled shape, so it is added to
    // the bind pose and the skin carries the result into the world. Applying it after the skin
    // would move the vertex along the model's axes after it had been rotated onto the character's,
    // and the difference is what this case measures: the vertex at (2, 0, 0) is bound entirely to
    // the rotated bone and carries a delta of (1, 0, 0) at full weight.
    //
    //   deltas first:  (2,0,0) + (1,0,0) = (3,0,0), then the elbow's quarter turn -> (1, 2, 0)
    //   deltas after:  (2,0,0) skinned -> (1,1,0), then + (1,0,0)                 -> (2, 1, 0)
    BlendShapeSet shapes(allocator());
    const BlendShapeDelta stretch[1] = {BlendShapeDelta{0, {1.0F, 0.0F, 0.0F}, {}, {}}};
    const cy::Expected<u32, cy::Error> stretch_index =
        shapes.add(Span<const BlendShapeDelta>(stretch, 1));
    CY_REQUIRE(stretch_index.has_value());
    // A second shape, left at rest, so that "only the active shapes are read" is a claim with
    // something to exclude rather than a sentence about an empty set.
    const BlendShapeDelta ruin[1] = {BlendShapeDelta{0, {0.0F, 0.0F, 99.0F}, {}, {}}};
    CY_REQUIRE(shapes.add(Span<const BlendShapeDelta>(ruin, 1)).has_value());
    CY_REQUIRE(shapes.set_weight(*stretch_index, 1.0F));

    u32 scratch[4] = {};
    GpuActiveBlendShape active[4] = {};
    const u32 count = active_blend_shapes(shapes, 1e-3F, Span<u32>(scratch, 4),
                                          Span<GpuActiveBlendShape>(active, 4));
    CY_REQUIRE_EQ(count, 1U);
    CY_CHECK_EQ(active[0].count, 1U);
    CY_CHECK(std::fabs(active[0].weight - 1.0F) < kAbsolute);

    const std::vector<GpuBoneMatrix> pose = elbow_pose();
    const Vec3 rest[1] = {Vec3{2.0F, 0.0F, 0.0F}};
    const GpuSkinInfluence weights[1] = {influence(1, 255)};
    SkinningDescriptor descriptor = elbow_descriptor(1);
    descriptor.blend_shape_count = 2;
    const cy::Expected<GpuSkinConstants, cy::Error> constants =
        make_skin_constants(descriptor, false, 0, 0, count);
    CY_REQUIRE(constants.has_value());
    CY_CHECK_EQ(constants->active_blend_shapes, 1U);

    Vec3 skinned[1] = {};
    SkinInputs inputs;
    inputs.bones = Span<const GpuBoneMatrix>(pose.data(), pose.size());
    inputs.positions = Span<const Vec3>(rest, 1);
    inputs.influences = Span<const GpuSkinInfluence>(weights, 1);
    inputs.blend_shape_deltas = shapes.deltas();
    inputs.active_shapes = Span<const GpuActiveBlendShape>(active, count);
    SkinOutputs outputs;
    outputs.positions = Span<Vec3>(skinned, 1);
    CY_REQUIRE(cpu_reference_skin(*constants, inputs, outputs).has_value());

    CY_TEST_MESSAGE("blend shape then skin: (", skinned[0].x, ", ", skinned[0].y, ", ",
                    skinned[0].z, ")");
    check_close(skinned[0], 1.0F, 2.0F, 0.0F);

    // THE SHAPE AT REST WAS NOT READ. Its delta is +99 on z and the answer's z is zero, which is
    // the compaction working rather than a weight of zero multiplying it away — the second is also
    // correct arithmetic and costs a delta list read per vertex per frame forever.
    CY_CHECK(std::fabs(skinned[0].z) < kAbsolute);

    // Waking it changes the answer, so the exclusion above is a decision and not an absence.
    CY_REQUIRE(shapes.set_weight(1, 0.5F));
    const u32 both = active_blend_shapes(shapes, 1e-3F, Span<u32>(scratch, 4),
                                         Span<GpuActiveBlendShape>(active, 4));
    CY_REQUIRE_EQ(both, 2U);
    const cy::Expected<GpuSkinConstants, cy::Error> awake =
        make_skin_constants(descriptor, false, 0, 0, both);
    CY_REQUIRE(awake.has_value());
    inputs.active_shapes = Span<const GpuActiveBlendShape>(active, both);
    CY_REQUIRE(cpu_reference_skin(*awake, inputs, outputs).has_value());
    check_close(skinned[0], 1.0F, 2.0F, 49.5F);
}

CY_TEST_CASE("blend shapes: a delta moves the normal and tangent frame, not only the position") {
    // A shape that changes a surface's shape changes its normals, and a dispatch that moved only
    // positions would leave a frowning face lit as though it were smiling. The frame is decoded,
    // the delta is added, and the SKIN rotates the result — which is the same order the position
    // takes and for the same reason.
    BlendShapeSet shapes(allocator());
    // The bind normal is +Z and the delta tips it towards +X by one unit, so the shape's normal is
    // (1, 0, 1) normalised — 45 degrees — and under the identity pose that is what comes back.
    const BlendShapeDelta tip[1] = {BlendShapeDelta{0, {}, {1.0F, 0.0F, 0.0F}, {}}};
    CY_REQUIRE(shapes.add(Span<const BlendShapeDelta>(tip, 1)).has_value());
    CY_REQUIRE(shapes.set_weight(0, 1.0F));

    u32 scratch[2] = {};
    GpuActiveBlendShape active[2] = {};
    const u32 count = active_blend_shapes(shapes, 1e-3F, Span<u32>(scratch, 2),
                                          Span<GpuActiveBlendShape>(active, 2));
    CY_REQUIRE_EQ(count, 1U);

    const std::vector<GpuBoneMatrix> pose = elbow_pose();
    const Vec3 rest[1] = {Vec3{0.0F, 0.0F, 0.0F}};
    const PackedNormalTangent frames[1] = {
        cy::render::pack_normal_tangent(Vec3{0.0F, 0.0F, 1.0F}, Vec3{1.0F, 0.0F, 0.0F}, 1.0F)};
    const GpuSkinInfluence weights[1] = {influence(0, 255)};  // the bone at rest: identity

    SkinningDescriptor descriptor = elbow_descriptor(1);
    descriptor.blend_shape_count = 1;
    const cy::Expected<GpuSkinConstants, cy::Error> constants =
        make_skin_constants(descriptor, true, 0, 0, count);
    CY_REQUIRE(constants.has_value());

    Vec3 skinned[1] = {};
    PackedNormalTangent out_frames[1] = {};
    SkinInputs inputs;
    inputs.bones = Span<const GpuBoneMatrix>(pose.data(), pose.size());
    inputs.positions = Span<const Vec3>(rest, 1);
    inputs.frames = Span<const PackedNormalTangent>(frames, 1);
    inputs.influences = Span<const GpuSkinInfluence>(weights, 1);
    inputs.blend_shape_deltas = shapes.deltas();
    inputs.active_shapes = Span<const GpuActiveBlendShape>(active, count);
    SkinOutputs outputs;
    outputs.positions = Span<Vec3>(skinned, 1);
    outputs.frames = Span<PackedNormalTangent>(out_frames, 1);
    CY_REQUIRE(cpu_reference_skin(*constants, inputs, outputs).has_value());

    const Vec3 normal = unpack_normal(out_frames[0]);
    constexpr f32 kHalfRoot2 = 0.70710678F;
    CY_TEST_MESSAGE("blend-shaped normal: (", normal.x, ", ", normal.y, ", ", normal.z, ")");
    // 1e-4, which is the octahedral encoding's own resolution at 16 bits and not a tolerance
    // chosen until this passed.
    CY_CHECK(std::fabs(normal.x - kHalfRoot2) < 1e-4F);
    CY_CHECK(std::fabs(normal.z - kHalfRoot2) < 1e-4F);
    // The bitangent sign is the input's: a skin rotates a frame and a delta moves it, and neither
    // can mirror it.
    CY_CHECK(unpack_bitangent_sign(out_frames[0]) > 0.0F);
}

CY_TEST_CASE("skin dispatch: a correctly cooked vertex's weight bytes sum to 255") {
    // The dispatch does not renormalise: `weight = byte / 255` and nothing divides by the sum,
    // because that division would run per vertex per frame forever to correct a defect the cook
    // step can fix once. This is the check a cook step or an importer test runs.
    const GpuSkinInfluence exact = influence(0, 128, 1, 127);
    CY_CHECK_EQ(skin_weight_sum(&exact, 1), 255U);

    const GpuSkinInfluence truncated = influence(0, 200, 1, 40);
    CY_CHECK_EQ(skin_weight_sum(&truncated, 1), 240U);

    // Eight influences sum across both records.
    const GpuSkinInfluence pair[2] = {influence(0, 100, 1, 100), influence(2, 55)};
    CY_CHECK_EQ(skin_weight_sum(pair, 2), 255U);
}

CY_TEST_CASE(
    "skin dispatch: input and output ranges are independent, which is what double "
    "buffering needs") {
    // "Output buffers SHALL be double buffered so the previous frame's positions are available for
    // motion vectors." The second frame's range is the first's plus the vertex count, in the same
    // buffer, which is what `SkinnedBuffers` indexes — so the dispatch has to be able to read from
    // one place and write to another.
    const std::vector<GpuBoneMatrix> pose = elbow_pose();
    const Vec3 rest[3] = {Vec3{9.0F, 9.0F, 9.0F}, Vec3{2.0F, 0.0F, 0.0F}, Vec3{9.0F, 9.0F, 9.0F}};
    const GpuSkinInfluence weights[3] = {influence(0, 255), influence(1, 255), influence(0, 255)};

    const cy::Expected<GpuSkinConstants, cy::Error> base =
        make_skin_constants(elbow_descriptor(1), false, 1, 2);
    CY_REQUIRE(base.has_value());

    Vec3 skinned[4] = {};
    SkinInputs inputs;
    inputs.bones = Span<const GpuBoneMatrix>(pose.data(), pose.size());
    inputs.positions = Span<const Vec3>(rest, 3);
    inputs.influences = Span<const GpuSkinInfluence>(weights, 3);
    SkinOutputs outputs;
    outputs.positions = Span<Vec3>(skinned, 4);
    CY_REQUIRE(cpu_reference_skin(*base, inputs, outputs).has_value());

    check_close(skinned[2], 1.0F, 1.0F, 0.0F);
    // Nothing outside the declared output range was touched.
    check_close(skinned[0], 0.0F, 0.0F, 0.0F);
    check_close(skinned[1], 0.0F, 0.0F, 0.0F);
    check_close(skinned[3], 0.0F, 0.0F, 0.0F);
}
