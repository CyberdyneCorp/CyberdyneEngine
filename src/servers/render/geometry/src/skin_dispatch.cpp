#include <cy/servers/render/geometry/skin_dispatch.h>

#include <cmath>

namespace cy::render::geometry {
namespace {

/// `Vec3` is the position stream's element and the stream's stride is 12. Asserted rather than
/// assumed, because a padded `Vec3` would make every span here a different buffer from the one a
/// vertex input reads.
static_assert(sizeof(Vec3) == 12, "the position stream is three tightly packed floats");

/// How many 8-byte influence records one vertex occupies. Four influences is one, eight is two.
[[nodiscard]] u32 blocks_for(u32 influences) noexcept {
    return influences >= 8U ? 2U : 1U;
}

}  // namespace

GpuBoneMatrix pack_bone_matrix(const Mat4& skinning_matrix) noexcept {
    GpuBoneMatrix packed;
    for (u32 row = 0; row < 3U; ++row) {
        for (u32 column = 0; column < 4U; ++column) {
            packed.rows[row][column] = skinning_matrix.columns[column][row];
        }
    }
    return packed;
}

u32 skin_weight_sum(const GpuSkinInfluence* influences, u32 blocks) noexcept {
    if (influences == nullptr) {
        return 0;
    }
    u32 total = 0;
    for (u32 block = 0; block < blocks; ++block) {
        for (u32 lane = 0; lane < 4U; ++lane) {
            total += bone_weight_byte_of(influences[block], lane);
        }
    }
    return total;
}

Expected<GpuSkinConstants, Error> make_skin_constants(const SkinningDescriptor& descriptor,
                                                      bool write_frames, u32 first_input_vertex,
                                                      u32 first_output_vertex) noexcept {
    // The descriptor's own rules first, so that a caller gets the diagnostic that names the real
    // mistake rather than a consequence of it.
    if (Status valid = descriptor.validate(); !valid) {
        return make_unexpected(valid.error());
    }
    if (!descriptor.skins()) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument,
                  "a baked instance uses a pose texture or vertex animation and skins nothing; "
                  "SkinningDescriptor::skins() is how a caller asks before building a dispatch"});
    }
    if (descriptor.method == SkinningMethod::DualQuaternion) {
        // Refused by name rather than linear-blended quietly. See the header: a dual-quaternion
        // skin needs the pose AS dual quaternions and `PoseWorld` publishes matrices, so the
        // missing piece is a pose representation in `animation-and-skinning` and not a branch here.
        return make_unexpected(
            Error{ErrorCode::NotImplemented,
                  "dual quaternion skinning needs the pose as dual quaternions and the GPU pose "
                  "world publishes matrices; linear blend is what this dispatch computes, and "
                  "silently substituting it would show as a twisted joint nobody could trace"});
    }
    if (descriptor.blend_shape_count != 0) {
        return make_unexpected(
            Error{ErrorCode::NotImplemented,
                  "this dispatch applies no blend shape deltas; BlendShapeSet is the storage and "
                  "the active-shape compaction, and the deltas belong in this same pass"});
    }

    GpuSkinConstants constants;
    constants.vertex_count = descriptor.vertex_count;
    constants.bone_count = descriptor.bone_count;
    constants.pose_offset = descriptor.pose_offset;
    constants.influences = static_cast<u32>(descriptor.influences);
    constants.flags = write_frames ? kSkinWriteFrames : 0U;
    constants.first_input_vertex = first_input_vertex;
    constants.first_output_vertex = first_output_vertex;
    return constants;
}

namespace {

/// Rotate a direction by the blended matrix's linear part.
///
/// THE LINEAR PART AND NOT THE INVERSE TRANSPOSE. The exact answer for a non-uniformly scaled bone
/// is the inverse transpose, and computing one per vertex — a 3x3 inverse, in a loop over every
/// vertex of every skinned mesh every frame — is the cost this engine does not pay: skeletal bone
/// matrices are rigid or uniformly scaled in every rig the animation runtime can author, because
/// `Transform` carries a `Vec3` scale that a bind pose propagates uniformly down a chain, and for
/// those the two agree up to the normalisation that follows anyway. A rig that non-uniformly scales
/// a bone gets a normal that is off by the shear, which is stated here rather than discovered.
[[nodiscard]] Vec3 rotate_by_blend(const f32 blended[3][4], Vec3 direction) noexcept {
    return Vec3{(blended[0][0] * direction.x) + (blended[0][1] * direction.y) +
                    (blended[0][2] * direction.z),
                (blended[1][0] * direction.x) + (blended[1][1] * direction.y) +
                    (blended[1][2] * direction.z),
                (blended[2][0] * direction.x) + (blended[2][1] * direction.y) +
                    (blended[2][2] * direction.z)};
}

/// Apply the blended matrix to a position: the linear part, then the translation column.
[[nodiscard]] Vec3 apply_blend(const f32 blended[3][4], Vec3 position) noexcept {
    return Vec3{(blended[0][0] * position.x) + (blended[0][1] * position.y) +
                    (blended[0][2] * position.z) + blended[0][3],
                (blended[1][0] * position.x) + (blended[1][1] * position.y) +
                    (blended[1][2] * position.z) + blended[1][3],
                (blended[2][0] * position.x) + (blended[2][1] * position.y) +
                    (blended[2][2] * position.z) + blended[2][3]};
}

/// Accumulate the blended skinning matrix for one vertex, and its weight sum.
///
/// THE BLEND IS ACCUMULATED AS A MATRIX AND APPLIED ONCE. Transforming the position by each bone
/// and summing the results is the same arithmetic in exact terms and differs in the last bits, and
/// the shader does it this way because it is one multiply-add per component per influence instead
/// of three. The reference follows the shader, not the other way round.
///
/// Its cognitive complexity is 19 — the warning band — and it stays there deliberately: the two
/// loops, the zero-weight skip, the index clamp and the four-by-four accumulation are the shader's
/// own shape, line for line, and a split that read better here would make the two files stop
/// corresponding. That correspondence is the property the whole arrangement exists for.
void accumulate_blend(Span<const GpuBoneMatrix> bones, const GpuSkinInfluence* influence,
                      u32 blocks, u32 bone_count, u32 pose_offset, f32 out[3][4],
                      f32& out_weight_sum) noexcept {
    f32 weight_sum = 0.0F;
    for (u32 block = 0; block < blocks; ++block) {
        for (u32 lane = 0; lane < 4U; ++lane) {
            const f32 weight = bone_weight_of(influence[block], lane);
            if (weight == 0.0F) {
                continue;
            }
            u32 bone = bone_index_of(influence[block], lane);
            if (bone >= bone_count) {
                bone = bone_count - 1U;
            }
            const usize index = static_cast<usize>(pose_offset) + bone;
            if (index >= bones.size()) {
                continue;
            }
            const GpuBoneMatrix& matrix = bones[index];
            for (u32 row = 0; row < 3U; ++row) {
                for (u32 column = 0; column < 4U; ++column) {
                    out[row][column] += matrix.rows[row][column] * weight;
                }
            }
            weight_sum += weight;
        }
    }
    out_weight_sum = weight_sum;
}

[[nodiscard]] Vec3 normalised_or_identity(Vec3 vector, Vec3 fallback) noexcept {
    const f32 length_squared =
        (vector.x * vector.x) + (vector.y * vector.y) + (vector.z * vector.z);
    if (!(length_squared > 0.0F)) {
        return fallback;
    }
    const f32 inverse = 1.0F / std::sqrt(length_squared);
    return Vec3{vector.x * inverse, vector.y * inverse, vector.z * inverse};
}

}  // namespace

Vec3 skin_position(Span<const GpuBoneMatrix> bones, const GpuSkinInfluence* influence, u32 blocks,
                   u32 bone_count, u32 pose_offset, Vec3 position, f32& out_weight_sum) noexcept {
    f32 blended[3][4] = {};
    accumulate_blend(bones, influence, blocks, bone_count, pose_offset, blended, out_weight_sum);
    if (out_weight_sum == 0.0F) {
        // An unweighted vertex is not moved. Answering with the origin instead would collapse every
        // such vertex onto the character's root and produce the spike an artist recognises but
        // cannot locate.
        return position;
    }
    return apply_blend(blended, position);
}

namespace {

/// Refuse a dispatch whose spans do not cover the range the constants name.
///
/// A SEPARATE FUNCTION BECAUSE THE SHADER HAS NO EQUIVALENT. Every check here is one the device
/// cannot make: a storage buffer read past its end is undefined behaviour there and there is no
/// diagnostic to return. So the reference makes them, by name, and `SkinPass::upload` makes the
/// same ones before a command buffer exists — which is the only place a caller can still be told.
[[nodiscard]] Status check_ranges(const GpuSkinConstants& constants, const SkinInputs& inputs,
                                  const SkinOutputs& outputs) noexcept {
    if (constants.vertex_count == 0) {
        return fail(ErrorCode::InvalidArgument, "a skinning dispatch over no vertices");
    }
    if (constants.bone_count == 0) {
        return fail(ErrorCode::InvalidArgument, "a skinning dispatch with no bones");
    }
    const usize last_input =
        static_cast<usize>(constants.first_input_vertex) + constants.vertex_count;
    const usize last_output =
        static_cast<usize>(constants.first_output_vertex) + constants.vertex_count;
    if (inputs.positions.size() < last_input || outputs.positions.size() < last_output) {
        return fail(ErrorCode::OutOfRange,
                    "the position streams are shorter than the dispatch's vertex range");
    }
    if (inputs.influences.size() < last_input * blocks_for(constants.influences)) {
        return fail(ErrorCode::OutOfRange,
                    "the influence stream is shorter than the dispatch's vertex range; an "
                    "eight-influence mesh needs two records per vertex");
    }
    if ((constants.flags & kSkinWriteFrames) != 0U &&
        (inputs.frames.size() < last_input || outputs.frames.size() < last_output)) {
        return fail(ErrorCode::OutOfRange,
                    "kSkinWriteFrames is set and a normal-tangent stream is shorter than the "
                    "dispatch's vertex range");
    }
    if (inputs.bones.size() < static_cast<usize>(constants.pose_offset) + constants.bone_count) {
        return fail(ErrorCode::OutOfRange,
                    "the pose does not hold this skin's bones at the offset it was given; "
                    "PoseWorld::matrix_offset moves every publish because the world is double "
                    "buffered, so a cached offset reads the previous frame or past the end");
    }
    return ok();
}

/// Write one vertex's skinned frame: decode, rotate by the blend's linear part, re-encode.
void skin_frame(const f32 blended[3][4], const PackedNormalTangent& packed,
                PackedNormalTangent& out) noexcept {
    const Vec3 normal = rotate_by_blend(blended, unpack_normal(packed));
    const Vec3 tangent = rotate_by_blend(blended, unpack_tangent(packed));
    out = pack_normal_tangent(normalised_or_identity(normal, Vec3{0.0F, 0.0F, 1.0F}),
                              normalised_or_identity(tangent, Vec3{1.0F, 0.0F, 0.0F}),
                              unpack_bitangent_sign(packed));
}

}  // namespace

Status cpu_reference_skin(const GpuSkinConstants& constants, const SkinInputs& inputs,
                          const SkinOutputs& outputs) noexcept {
    if (Status checked = check_ranges(constants, inputs, outputs); !checked) {
        return checked;
    }
    const u32 blocks = blocks_for(constants.influences);
    const bool frames = (constants.flags & kSkinWriteFrames) != 0U;

    for (u32 vertex = 0; vertex < constants.vertex_count; ++vertex) {
        const u32 in_index = constants.first_input_vertex + vertex;
        const u32 out_index = constants.first_output_vertex + vertex;
        const GpuSkinInfluence* influence =
            inputs.influences.data() + (static_cast<usize>(in_index) * blocks);

        f32 blended[3][4] = {};
        f32 weight_sum = 0.0F;
        accumulate_blend(inputs.bones, influence, blocks, constants.bone_count,
                         constants.pose_offset, blended, weight_sum);

        const Vec3 position = inputs.positions[in_index];
        if (weight_sum == 0.0F) {
            outputs.positions[out_index] = position;
            if (frames) {
                outputs.frames[out_index] = inputs.frames[in_index];
            }
            continue;
        }
        outputs.positions[out_index] = apply_blend(blended, position);

        if (frames) {
            skin_frame(blended, inputs.frames[in_index], outputs.frames[out_index]);
        }
    }
    return ok();
}

}  // namespace cy::render::geometry
