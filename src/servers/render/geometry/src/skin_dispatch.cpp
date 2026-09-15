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

GpuBoneDualQuaternion pack_bone_dual_quaternion(const Mat4& skinning_matrix) noexcept {
    // The linear part's column lengths ARE the scale, one per axis. Their mean is the uniform scale
    // this representation carries; three different lengths mean a non-uniformly scaled bone and the
    // difference is the shear that is lost, which the header states rather than approximates.
    const Vec3 axis_x{skinning_matrix.columns[0][0], skinning_matrix.columns[0][1],
                      skinning_matrix.columns[0][2]};
    const Vec3 axis_y{skinning_matrix.columns[1][0], skinning_matrix.columns[1][1],
                      skinning_matrix.columns[1][2]};
    const Vec3 axis_z{skinning_matrix.columns[2][0], skinning_matrix.columns[2][1],
                      skinning_matrix.columns[2][2]};
    const f32 length_x = std::sqrt(dot(axis_x, axis_x));
    const f32 length_y = std::sqrt(dot(axis_y, axis_y));
    const f32 length_z = std::sqrt(dot(axis_z, axis_z));
    const f32 scale = (length_x + length_y + length_z) * (1.0F / 3.0F);

    GpuBoneDualQuaternion packed;
    if (!(length_x > 0.0F) || !(length_y > 0.0F) || !(length_z > 0.0F)) {
        // A degenerate bone: keep the identity rotation and a zero scale, which collapses the
        // vertex rather than producing a NaN that spreads through the whole blend.
        packed.scale[0] = 0.0F;
        return packed;
    }

    // The rotation, from the normalised linear part. Shepperd's method: pick the branch whose
    // divisor is largest, because the naive `w = sqrt(1 + trace) / 2` loses every bit of precision
    // at a half turn, which is a pose every elbow reaches.
    const f32 m00 = axis_x.x / length_x;
    const f32 m10 = axis_x.y / length_x;
    const f32 m20 = axis_x.z / length_x;
    const f32 m01 = axis_y.x / length_y;
    const f32 m11 = axis_y.y / length_y;
    const f32 m21 = axis_y.z / length_y;
    const f32 m02 = axis_z.x / length_z;
    const f32 m12 = axis_z.y / length_z;
    const f32 m22 = axis_z.z / length_z;

    f32 quaternion[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    const f32 trace = m00 + m11 + m22;
    if (trace > 0.0F) {
        const f32 root = std::sqrt(trace + 1.0F);
        const f32 half = 0.5F / root;
        quaternion[3] = 0.5F * root;
        quaternion[0] = (m21 - m12) * half;
        quaternion[1] = (m02 - m20) * half;
        quaternion[2] = (m10 - m01) * half;
    } else {
        const f32 diagonal[3] = {m00, m11, m22};
        u32 largest = 0;
        if (diagonal[1] > diagonal[largest]) {
            largest = 1;
        }
        if (diagonal[2] > diagonal[largest]) {
            largest = 2;
        }
        const u32 next = (largest + 1U) % 3U;
        const u32 after = (largest + 2U) % 3U;
        const f32 rotation[3][3] = {{m00, m01, m02}, {m10, m11, m12}, {m20, m21, m22}};
        const f32 root = std::sqrt(rotation[largest][largest] - rotation[next][next] -
                                   rotation[after][after] + 1.0F);
        const f32 half = 0.5F / root;
        quaternion[largest] = 0.5F * root;
        quaternion[next] = (rotation[next][largest] + rotation[largest][next]) * half;
        quaternion[after] = (rotation[after][largest] + rotation[largest][after]) * half;
        quaternion[3] = (rotation[after][next] - rotation[next][after]) * half;
    }

    // The dual part: `0.5 * (t, 0) * real`, with the translation read from the fourth column.
    const f32 tx = skinning_matrix.columns[3][0];
    const f32 ty = skinning_matrix.columns[3][1];
    const f32 tz = skinning_matrix.columns[3][2];
    const f32 rx = quaternion[0];
    const f32 ry = quaternion[1];
    const f32 rz = quaternion[2];
    const f32 rw = quaternion[3];

    for (u32 index = 0; index < 4U; ++index) {
        packed.real[index] = quaternion[index];
    }
    packed.dual[0] = 0.5F * ((tx * rw) + (ty * rz) - (tz * ry));
    packed.dual[1] = 0.5F * ((ty * rw) + (tz * rx) - (tx * rz));
    packed.dual[2] = 0.5F * ((tz * rw) + (tx * ry) - (ty * rx));
    packed.dual[3] = -0.5F * ((tx * rx) + (ty * ry) + (tz * rz));
    packed.scale[0] = scale;
    return packed;
}

u32 active_blend_shapes(const BlendShapeSet& shapes, f32 threshold, Span<u32> scratch,
                        Span<GpuActiveBlendShape> out) noexcept {
    const u32 found = shapes.active(threshold, scratch);
    const auto room = static_cast<u32>(out.size() < scratch.size() ? out.size() : scratch.size());
    const u32 written = found < room ? found : room;
    for (u32 index = 0; index < written; ++index) {
        const u32 shape = scratch[index];
        const Span<const BlendShapeRange> ranges = shapes.ranges();
        if (shape >= ranges.size()) {
            continue;
        }
        out[index].first = ranges[shape].first;
        out[index].count = ranges[shape].count;
        out[index].weight = shapes.weight(shape);
        out[index].reserved = 0;
    }
    return written;
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
                                                      u32 first_output_vertex,
                                                      u32 active_blend_shape_count) noexcept {
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
    // AN ACTIVE LIST LONGER THAN THE AUTHORED SET is the one refusal left here, and it is a real
    // one: the list is built per frame from a set the descriptor describes, and a dispatch told to
    // read more shapes than the mesh has would read past the end of a storage buffer, which is
    // undefined behaviour on the device and has no diagnostic there.
    if (active_blend_shape_count > descriptor.blend_shape_count) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument,
                  "more active blend shapes than the mesh authors; the active list is a compaction "
                  "of the authored set and cannot be longer than it"});
    }

    GpuSkinConstants constants;
    constants.active_blend_shapes = active_blend_shape_count;
    constants.vertex_count = descriptor.vertex_count;
    constants.bone_count = descriptor.bone_count;
    constants.pose_offset = descriptor.pose_offset;
    constants.influences = static_cast<u32>(descriptor.influences);
    constants.flags = write_frames ? kSkinWriteFrames : 0U;
    if (descriptor.method == SkinningMethod::DualQuaternion) {
        constants.flags |= kSkinDualQuaternion;
    }
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

// --- The dual-quaternion blend ------------------------------------------------------------------

/// The blended pose for one vertex, in the dual-quaternion form.
struct DualBlend {
    f32 real[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    f32 dual[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    f32 scale = 0.0F;
};

/// Accumulate the blended DUAL QUATERNION for one vertex, and its weight sum.
///
/// LINEAR BLENDING OF DUAL QUATERNIONS, followed by one normalisation — DLB, not the iterative
/// ScLERP. It is what a shader can afford and what every engine ships; the difference from the exact
/// screw interpolation is below the octahedral encoding's own quantum for the angles a joint bends
/// through.
///
/// THE SIGN FIX IS NOT OPTIONAL AND IT IS WHY THIS IS NOT A COPY OF `accumulate_blend`. `q` and
/// `-q` are the same rotation and the blend of the two is ZERO, so a rig whose two influencing
/// bones happen to have been converted onto opposite hemispheres collapses that vertex onto the
/// origin. The first contributing bone is the pivot and every later one is flipped onto its
/// hemisphere, which is the standard remedy and the one the shader repeats line for line.
void accumulate_dual_blend(Span<const GpuBoneDualQuaternion> bones,
                           const GpuSkinInfluence* influence, u32 blocks, u32 bone_count,
                           u32 pose_offset, DualBlend& out, f32& out_weight_sum) noexcept {
    f32 weight_sum = 0.0F;
    f32 pivot[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    bool have_pivot = false;
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
            const GpuBoneDualQuaternion& pose = bones[index];
            if (!have_pivot) {
                for (u32 component = 0; component < 4U; ++component) {
                    pivot[component] = pose.real[component];
                }
                have_pivot = true;
            }
            const f32 alignment = (pivot[0] * pose.real[0]) + (pivot[1] * pose.real[1]) +
                                  (pivot[2] * pose.real[2]) + (pivot[3] * pose.real[3]);
            const f32 signed_weight = alignment < 0.0F ? -weight : weight;
            for (u32 component = 0; component < 4U; ++component) {
                out.real[component] += pose.real[component] * signed_weight;
                out.dual[component] += pose.dual[component] * signed_weight;
            }
            out.scale += pose.scale[0] * weight;
            weight_sum += weight;
        }
    }
    out_weight_sum = weight_sum;
}

/// Normalise the blend. False for a blend with no length, which is a vertex nothing can place.
[[nodiscard]] bool normalise_dual(DualBlend& blend) noexcept {
    const f32 length_squared = (blend.real[0] * blend.real[0]) + (blend.real[1] * blend.real[1]) +
                               (blend.real[2] * blend.real[2]) + (blend.real[3] * blend.real[3]);
    if (!(length_squared > 0.0F)) {
        return false;
    }
    const f32 inverse = 1.0F / std::sqrt(length_squared);
    for (u32 component = 0; component < 4U; ++component) {
        blend.real[component] *= inverse;
        blend.dual[component] *= inverse;
    }
    return true;
}

/// Rotate a direction by the blend's rotation. `v + 2 r x (r x v + w v)`, the form with no matrix
/// in it.
[[nodiscard]] Vec3 dual_rotate(const DualBlend& blend, Vec3 direction) noexcept {
    const Vec3 axis{blend.real[0], blend.real[1], blend.real[2]};
    const f32 angle = blend.real[3];
    const Vec3 inner = cross(axis, direction) + (direction * angle);
    return direction + (cross(axis, inner) * 2.0F);
}

/// The blend's translation: `2 (w_d r_r - w_r r_d + r_r x r_d)`.
[[nodiscard]] Vec3 dual_translation(const DualBlend& blend) noexcept {
    const Vec3 real_axis{blend.real[0], blend.real[1], blend.real[2]};
    const Vec3 dual_axis{blend.dual[0], blend.dual[1], blend.dual[2]};
    const Vec3 combined = (dual_axis * blend.real[3]) - (real_axis * blend.dual[3]) +
                          cross(real_axis, dual_axis);
    return combined * 2.0F;
}

/// Scale, rotate, translate — the order a `model * inverse_bind` matrix applies them in, so that the
/// two methods agree on a rigid pose rather than differing by where the scale went.
[[nodiscard]] Vec3 dual_apply(const DualBlend& blend, Vec3 position) noexcept {
    return dual_rotate(blend, position * blend.scale) + dual_translation(blend);
}

// --- The blend shapes ----------------------------------------------------------------------------

/// One vertex's bind-pose attributes, before the skin and after the shapes.
struct VertexSource {
    Vec3 position{};
    Vec3 normal{0.0F, 0.0F, 1.0F};
    Vec3 tangent{1.0F, 0.0F, 0.0F};
};

/// One shape's delta for one vertex, or null.
///
/// A BINARY SEARCH, because `BlendShapeSet::add` refuses a delta list that is not sorted by vertex
/// index — that refusal exists precisely so this can be a search rather than a scan, and so the
/// shader can do the same thing with the same bounds. A shape covering a tenth of a mesh costs
/// log2(n) reads per vertex instead of n.
[[nodiscard]] const BlendShapeDelta* find_delta(Span<const BlendShapeDelta> deltas, u32 first,
                                                u32 count, u32 vertex) noexcept {
    u32 low = 0;
    u32 high = count;
    while (low < high) {
        const u32 middle = low + ((high - low) / 2U);
        const usize index = static_cast<usize>(first) + middle;
        if (index >= deltas.size()) {
            return nullptr;
        }
        const u32 at = deltas[index].vertex;
        if (at == vertex) {
            return &deltas[index];
        }
        if (at < vertex) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return nullptr;
}

/// Apply every active shape's delta for one vertex, BEFORE the skin.
///
/// The order is the requirement's: a delta is authored against the modelled shape, so it is added
/// to the bind pose and the skin carries the result into the world. Applying a delta to a skinned
/// position would move a vertex along the model's axes after it had been rotated onto the
/// character's, which is a bug that looks like a rig problem.
void apply_blend_shapes(const GpuSkinConstants& constants, const SkinInputs& inputs, u32 vertex,
                        bool frames, VertexSource& source) noexcept {
    for (u32 slot = 0; slot < constants.active_blend_shapes; ++slot) {
        if (slot >= inputs.active_shapes.size()) {
            return;
        }
        const GpuActiveBlendShape& shape = inputs.active_shapes[slot];
        const BlendShapeDelta* delta =
            find_delta(inputs.blend_shape_deltas, shape.first, shape.count, vertex);
        if (delta == nullptr) {
            continue;
        }
        const f32 weight = shape.weight;
        source.position.x += delta->position[0] * weight;
        source.position.y += delta->position[1] * weight;
        source.position.z += delta->position[2] * weight;
        if (!frames) {
            continue;
        }
        source.normal.x += delta->normal[0] * weight;
        source.normal.y += delta->normal[1] * weight;
        source.normal.z += delta->normal[2] * weight;
        source.tangent.x += delta->tangent[0] * weight;
        source.tangent.y += delta->tangent[1] * weight;
        source.tangent.z += delta->tangent[2] * weight;
    }
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
    const bool dual = (constants.flags & kSkinDualQuaternion) != 0U;
    const usize pose_needed = static_cast<usize>(constants.pose_offset) + constants.bone_count;
    const usize pose_held = dual ? inputs.bone_dual_quaternions.size() : inputs.bones.size();
    if (pose_held < pose_needed) {
        return fail(ErrorCode::OutOfRange,
                    "the pose does not hold this skin's bones at the offset it was given; "
                    "PoseWorld::matrix_offset moves every publish because the world is double "
                    "buffered, so a cached offset reads the previous frame or past the end — and "
                    "a dual-quaternion skin reads `bone_dual_quaternions` rather than `bones`");
    }
    if (constants.active_blend_shapes > inputs.active_shapes.size()) {
        return fail(ErrorCode::OutOfRange,
                    "the constants name more active blend shapes than the active list holds");
    }
    for (u32 slot = 0; slot < constants.active_blend_shapes; ++slot) {
        const GpuActiveBlendShape& shape = inputs.active_shapes[slot];
        if (static_cast<usize>(shape.first) + shape.count > inputs.blend_shape_deltas.size()) {
            return fail(ErrorCode::OutOfRange,
                        "an active blend shape's range runs past the end of the delta array; a "
                        "storage buffer read past its end is undefined behaviour on the device and "
                        "has no diagnostic there, which is why this one is made here");
        }
    }
    return ok();
}

/// Re-encode one vertex's skinned frame from two already-rotated directions.
///
/// THE BITANGENT SIGN IS THE INPUT'S. A skin rotates a frame and a blend shape moves it; neither can
/// mirror it, and re-deriving the sign from the rotated pair would turn a numerical wobble at a
/// degenerate vertex into a flipped normal map.
void write_frame(Vec3 normal, Vec3 tangent, const PackedNormalTangent& packed,
                 PackedNormalTangent& out) noexcept {
    out = pack_normal_tangent(normalised_or_identity(normal, Vec3{0.0F, 0.0F, 1.0F}),
                              normalised_or_identity(tangent, Vec3{1.0F, 0.0F, 0.0F}),
                              unpack_bitangent_sign(packed));
}

/// One vertex, under whichever blend the constants selected.
///
/// The two methods are one function because everything except eleven lines is shared — the
/// unweighted-vertex rule, the frame's rotation-only treatment and the re-encode are the same
/// algorithm and must not become two.
void skin_one_vertex(const GpuSkinConstants& constants, const SkinInputs& inputs,
                     const SkinOutputs& outputs, u32 in_index, u32 out_index,
                     const VertexSource& source) noexcept {
    const bool frames = (constants.flags & kSkinWriteFrames) != 0U;
    const bool dual = (constants.flags & kSkinDualQuaternion) != 0U;
    const u32 blocks = blocks_for(constants.influences);
    const GpuSkinInfluence* influence =
        inputs.influences.data() + (static_cast<usize>(in_index) * blocks);

    f32 weight_sum = 0.0F;
    Vec3 position = source.position;
    Vec3 normal = source.normal;
    Vec3 tangent = source.tangent;

    if (dual) {
        DualBlend blend;
        accumulate_dual_blend(inputs.bone_dual_quaternions, influence, blocks, constants.bone_count,
                              constants.pose_offset, blend, weight_sum);
        if (weight_sum != 0.0F && normalise_dual(blend)) {
            position = dual_apply(blend, source.position);
            normal = dual_rotate(blend, source.normal);
            tangent = dual_rotate(blend, source.tangent);
        }
    } else {
        f32 blended[3][4] = {};
        accumulate_blend(inputs.bones, influence, blocks, constants.bone_count,
                         constants.pose_offset, blended, weight_sum);
        if (weight_sum != 0.0F) {
            position = apply_blend(blended, source.position);
            normal = rotate_by_blend(blended, source.normal);
            tangent = rotate_by_blend(blended, source.tangent);
        }
    }

    outputs.positions[out_index] = position;
    if (frames) {
        write_frame(normal, tangent, inputs.frames[in_index], outputs.frames[out_index]);
    }
}

}  // namespace

Status cpu_reference_skin(const GpuSkinConstants& constants, const SkinInputs& inputs,
                          const SkinOutputs& outputs) noexcept {
    if (Status checked = check_ranges(constants, inputs, outputs); !checked) {
        return checked;
    }
    const u32 blocks = blocks_for(constants.influences);
    const bool frames = (constants.flags & kSkinWriteFrames) != 0U;
    const bool shapes = constants.active_blend_shapes != 0U;

    for (u32 vertex = 0; vertex < constants.vertex_count; ++vertex) {
        const u32 in_index = constants.first_input_vertex + vertex;
        const u32 out_index = constants.first_output_vertex + vertex;

        // THE UNTOUCHED VERTEX IS COPIED WORD FOR WORD, and this branch is why: with no shape to
        // apply and no weight to blend, the frame's two words are the input's two words rather than
        // a decode and a re-encode of them. Octahedral encoding is not a round trip — re-encoding a
        // decoded pair moves the last bit at a great many directions — so a dispatch that decoded
        // and re-encoded here would differ from the input by one unit on vertices nothing moved, and
        // `tests/test_skin_pass.cpp` compares buffers rather than tolerances.
        if (!shapes) {
            const GpuSkinInfluence* influence =
                inputs.influences.data() + (static_cast<usize>(in_index) * blocks);
            if (skin_weight_sum(influence, blocks) == 0U) {
                outputs.positions[out_index] = inputs.positions[in_index];
                if (frames) {
                    outputs.frames[out_index] = inputs.frames[in_index];
                }
                continue;
            }
        }

        VertexSource source;
        source.position = inputs.positions[in_index];
        if (frames) {
            source.normal = unpack_normal(inputs.frames[in_index]);
            source.tangent = unpack_tangent(inputs.frames[in_index]);
        }
        if (shapes) {
            apply_blend_shapes(constants, inputs, in_index, frames, source);
        }
        skin_one_vertex(constants, inputs, outputs, in_index, out_index, source);
    }
    return ok();
}

}  // namespace cy::render::geometry
