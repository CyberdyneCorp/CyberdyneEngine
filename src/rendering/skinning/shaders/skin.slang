// Linear blend skinning, as one compute entry point. M8.d.
//
// `rendering-geometry-and-resources` — "Skinning": "Skinned meshes SHALL be transformed by a
// **compute pass** writing into per-instance output vertex buffers, so the result is reusable
// across passes (depth, shadows, main) without re-skinning."
//
// ================================================================================================
// THIS FILE IS THE SECOND IMPLEMENTATION OF ONE ALGORITHM, AND THAT IS THE POINT
// ================================================================================================
//
// `cy::render::geometry::cpu_reference_skin`
// (src/servers/render/geometry/src/skin_dispatch.cpp) computes exactly what this entry point
// computes. It exists so that this file can be checked against a buffer rather than against a
// screenshot, which is what `tests/test_skin_pass.cpp` does: it runs both over one pose and
// compares the skinned positions and the re-encoded normal-tangent frames.
//
// So every expression below is written in the order the reference writes it. THE BLEND IS
// ACCUMULATED AS A MATRIX AND APPLIED ONCE, rather than the position being transformed by each bone
// and the results summed — the two are the same in exact arithmetic and differ in the last bits,
// and one of them is a third of the multiplies. A "simplification" here is a test failure there.
//
// ================================================================================================
// EVERY BUFFER IS ADDRESSED AS WORDS
// ================================================================================================
//
// The position streams are three tightly packed floats at stride 12 and the frame streams are four
// 16-bit components at stride 8, because those are `render::VertexStream::Position` and
// `::NormalTangent` — the bytes a vertex input binds. A `StructuredBuffer<float3>` would be the
// natural spelling and its stride is a property of the compiler's layout rules rather than of the
// buffer, so the streams are bound as `uint` and the packing is stated here, once, in the same
// words the C++ states it in.
//
// ================================================================================================
// WHAT IS NOT HERE
// ================================================================================================
//
// Dual quaternion skinning, and blend shapes. `make_skin_constants` refuses both by name rather
// than letting a dispatch ignore a field; see skin_dispatch.h's header for why the missing piece of
// the first is a pose representation in `animation-and-skinning` and not a branch in this file.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   slangc skin.slang -target spirv -profile spirv_1_5 \
//          -entry skin_vertices -stage compute -o skin_vertices.spv
//   python3 embed_spirv.py ../src/skin_spirv.h kSkinVerticesSpirv=skin_vertices.spv
//   clang-format -i ../src/skin_spirv.h

// --- The constants, matching `cy::render::geometry::GpuSkinConstants` word for word --------------

struct SkinConstants {
    uint vertex_count;
    uint bone_count;
    uint pose_offset;
    uint influences;
    uint flags;
    uint first_input_vertex;
    uint first_output_vertex;
    uint reserved;
};

[[vk::push_constant]] SkinConstants constants;

static const uint kSkinWriteFrames = 1u << 0u;
static const uint kSkinGroupSize = 64u;

/// Three `float4` rows per bone: `GpuBoneMatrix`, which is 48 bytes and a 3x4 affine in ROW-major
/// order. `cy::Mat4` is column-major and `pack_bone_matrix` is the one transposition in the path.
[[vk::binding(0, 0)]] StructuredBuffer<float4> bones;
/// Bind-pose positions: three words per vertex.
[[vk::binding(1, 0)]] StructuredBuffer<uint> in_positions;
/// Bind-pose frames: two words per vertex, four 16-bit signed normalised components.
[[vk::binding(2, 0)]] StructuredBuffer<uint> in_frames;
/// `GpuSkinInfluence`: two words per block, one block per four influences.
[[vk::binding(3, 0)]] StructuredBuffer<uint> influences;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> out_positions;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> out_frames;

// --- The encodings, as `src/servers/render/src/mesh.cpp` writes them -----------------------------

/// `std::round`, which rounds HALVES AWAY FROM ZERO. HLSL's `round` rounds halves to even, and the
/// two disagree for exactly the values a normal pointing down an axis produces — so the C++'s rule
/// is spelled out rather than delegated to a builtin that has a different one.
float round_half_away(float value) {
    return value < 0.0f ? -floor(-value + 0.5f) : floor(value + 0.5f);
}

int to_snorm16(float value) {
    return int(round_half_away(clamp(value, -1.0f, 1.0f) * 32767.0f));
}

float from_snorm16(int value) {
    const float scaled = float(value) / 32767.0f;
    return scaled < -1.0f ? -1.0f : scaled;
}

/// Sign extension out of a packed word. Two components per word, low half first, which is what a
/// little-endian `i16 pair[2]` is.
int low_half(uint word) {
    return int(word << 16u) >> 16;
}
int high_half(uint word) {
    return int(word) >> 16;
}
uint pack_halves(int low, int high) {
    return (uint(low) & 0xFFFFu) | (uint(high) << 16u);
}

float sign_or_positive(float value) {
    return value < 0.0f ? -1.0f : 1.0f;
}

/// `cy::render::octahedral_decode`.
float3 octahedral_decode(int x, int y) {
    const float fx = from_snorm16(x);
    const float fy = from_snorm16(y);
    float3 vector = float3(fx, fy, 1.0f - abs(fx) - abs(fy));
    if (vector.z < 0.0f) {
        const float unfolded_x = (1.0f - abs(vector.y)) * sign_or_positive(vector.x);
        const float unfolded_y = (1.0f - abs(vector.x)) * sign_or_positive(vector.y);
        vector.x = unfolded_x;
        vector.y = unfolded_y;
    }
    const float length_squared = dot(vector, vector);
    return length_squared > 0.0f ? vector * (1.0f / sqrt(length_squared))
                                 : float3(0.0f, 0.0f, 1.0f);
}

/// `cy::render::octahedral_encode`, writing the two components into one word.
uint octahedral_encode(float3 unit_vector) {
    const float magnitude = abs(unit_vector.x) + abs(unit_vector.y) + abs(unit_vector.z);
    if (magnitude <= 0.0f) {
        // A zero vector has no direction to encode; +Z is the answer a shading path survives.
        return 0u;
    }
    const float inv = 1.0f / magnitude;
    float x = unit_vector.x * inv;
    float y = unit_vector.y * inv;
    if (unit_vector.z < 0.0f) {
        const float folded_x = (1.0f - abs(y)) * sign_or_positive(x);
        const float folded_y = (1.0f - abs(x)) * sign_or_positive(y);
        x = folded_x;
        y = folded_y;
    }
    return pack_halves(to_snorm16(x), to_snorm16(y));
}

float3 normalised_or(float3 vector, float3 fallback) {
    const float length_squared = dot(vector, vector);
    return length_squared > 0.0f ? vector * (1.0f / sqrt(length_squared)) : fallback;
}

// --- The dispatch ---------------------------------------------------------------------------------

[numthreads(64, 1, 1)]
void skin_vertices(uint3 thread: SV_DispatchThreadID) {
    const uint vertex = thread.x;
    if (vertex >= constants.vertex_count) {
        return;
    }
    const uint in_index = constants.first_input_vertex + vertex;
    const uint out_index = constants.first_output_vertex + vertex;
    const uint blocks = constants.influences >= 8u ? 2u : 1u;

    float4 blended[3] = {float4(0, 0, 0, 0), float4(0, 0, 0, 0), float4(0, 0, 0, 0)};
    float weight_sum = 0.0f;
    for (uint block = 0u; block < blocks; ++block) {
        const uint base = (in_index * blocks + block) * 2u;
        const uint packed_indices = influences[base];
        const uint packed_weights = influences[base + 1u];
        for (uint lane = 0u; lane < 4u; ++lane) {
            const float weight = float((packed_weights >> (lane * 8u)) & 0xFFu) * (1.0f / 255.0f);
            if (weight == 0.0f) {
                continue;
            }
            // CLAMPED, NOT TRUSTED. An index past the skin's bone count is a cooked-data defect,
            // and reading a storage buffer out of bounds is undefined behaviour on the device.
            uint bone = (packed_indices >> (lane * 8u)) & 0xFFu;
            if (bone >= constants.bone_count) {
                bone = constants.bone_count - 1u;
            }
            const uint row = (constants.pose_offset + bone) * 3u;
            blended[0] += bones[row + 0u] * weight;
            blended[1] += bones[row + 1u] * weight;
            blended[2] += bones[row + 2u] * weight;
            weight_sum += weight;
        }
    }

    const uint position_base = in_index * 3u;
    const float3 rest = float3(asfloat(in_positions[position_base + 0u]),
                               asfloat(in_positions[position_base + 1u]),
                               asfloat(in_positions[position_base + 2u]));

    float3 skinned = rest;
    if (weight_sum != 0.0f) {
        // The linear part and the translation column, in the reference's order.
        skinned = float3(dot(blended[0].xyz, rest) + blended[0].w,
                         dot(blended[1].xyz, rest) + blended[1].w,
                         dot(blended[2].xyz, rest) + blended[2].w);
    }
    const uint out_position_base = out_index * 3u;
    out_positions[out_position_base + 0u] = asuint(skinned.x);
    out_positions[out_position_base + 1u] = asuint(skinned.y);
    out_positions[out_position_base + 2u] = asuint(skinned.z);

    if ((constants.flags & kSkinWriteFrames) == 0u) {
        return;
    }
    const uint frame_base = in_index * 2u;
    const uint normal_word = in_frames[frame_base + 0u];
    const uint tangent_word = in_frames[frame_base + 1u];
    const uint out_frame_base = out_index * 2u;
    if (weight_sum == 0.0f) {
        out_frames[out_frame_base + 0u] = normal_word;
        out_frames[out_frame_base + 1u] = tangent_word;
        return;
    }

    // The bitangent sign lives in the low bit of the tangent's x component, so it is cleared before
    // the decode and restored after the re-encode — `pack_normal_tangent` and `unpack_tangent` say
    // the same thing on the C++ side.
    const int tangent_x = low_half(tangent_word) & ~1;
    const float bitangent_sign = (tangent_word & 1u) != 0u ? -1.0f : 1.0f;
    const float3 normal = octahedral_decode(low_half(normal_word), high_half(normal_word));
    const float3 tangent = octahedral_decode(tangent_x, high_half(tangent_word));

    // THE LINEAR PART AND NOT THE INVERSE TRANSPOSE. See skin_dispatch.cpp for why: a per-vertex
    // 3x3 inverse is the cost this engine does not pay, and the two agree for the rigid and
    // uniformly scaled bones the animation runtime authors.
    const float3 moved_normal =
        float3(dot(blended[0].xyz, normal), dot(blended[1].xyz, normal), dot(blended[2].xyz, normal));
    const float3 moved_tangent = float3(dot(blended[0].xyz, tangent), dot(blended[1].xyz, tangent),
                                        dot(blended[2].xyz, tangent));

    const uint encoded_normal = octahedral_encode(normalised_or(moved_normal, float3(0, 0, 1)));
    uint encoded_tangent = octahedral_encode(normalised_or(moved_tangent, float3(1, 0, 0)));
    encoded_tangent &= ~1u;
    if (bitangent_sign < 0.0f) {
        encoded_tangent |= 1u;
    }
    out_frames[out_frame_base + 0u] = encoded_normal;
    out_frames[out_frame_base + 1u] = encoded_tangent;
}
