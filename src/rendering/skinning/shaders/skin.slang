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
// DUAL QUATERNIONS AND BLEND SHAPES ARE HERE, AND THEY WERE NOT UNTIL M11.c
// ================================================================================================
//
// Until M11.c this file said they were not here and `make_skin_constants` refused both by name. Both
// are implemented now, in this same entry point rather than in a second one, because everything
// except the eleven lines of the blend is shared — the influence decode, the index clamp, the
// unweighted-vertex rule, the blend-shape pass and the frame re-encode — and two entry points is two
// implementations of one algorithm again, which is the mistake this whole file exists to avoid.
//
// A dual-quaternion pose is read from binding 6 INSTEAD of binding 0, as three `float4` rows per
// bone: the rotation, the dual part, and the uniform scale a dual quaternion cannot carry. The sign
// fix against the first contributing bone is not optional — `q` and `-q` are one rotation and their
// sum is zero, which collapses a vertex onto the origin — and it is written here exactly as
// `accumulate_dual_blend` writes it.
//
// Blend shape deltas are applied to the BIND POSE, before the skin, because a delta is authored
// against the modelled shape. The deltas of one shape are sorted by vertex index, which is what
// `BlendShapeSet::add` refuses an unsorted list for, so this does a binary search per active shape
// rather than a scan.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   slangc skin.slang -target spirv -profile spirv_1_5 \
//          -entry skin_vertices -stage compute -o skin_vertices.spv
//   slangc skin.slang -target metal \
//          -entry skin_vertices -stage compute -o skin_vertices.metal
//   python3 embed_spirv.py ../src/skin_spirv.h kSkinVerticesSpirv=skin_vertices.spv
//   python3 embed_msl.py ../src/skin_msl.h skin_vertices.metal
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
    uint active_blend_shapes;
};

static const uint kSkinWriteFrames = 1u << 0u;
static const uint kSkinDualQuaternion = 1u << 1u;
static const uint kSkinGroupSize = 64u;
/// `BlendShapeDelta` is 40 bytes: a vertex index and three `float3`s.
static const uint kBlendShapeDeltaWords = 10u;

/// Three `float4` rows per bone: `GpuBoneMatrix`, which is 48 bytes and a 3x4 affine in ROW-major
/// order. `cy::Mat4` is column-major and `pack_bone_matrix` is the one transposition in the path.
struct SkinSet {
    /// Three `float4` rows per bone: `GpuBoneMatrix`.
    StructuredBuffer<float4> bones;
    /// Bind-pose positions: three words per vertex.
    StructuredBuffer<uint> in_positions;
    /// Bind-pose frames: two words per vertex, four 16-bit signed normalised components.
    StructuredBuffer<uint> in_frames;
    /// `GpuSkinInfluence`: two words per block, one block per four influences.
    StructuredBuffer<uint> influences;
    RWStructuredBuffer<uint> out_positions;
    RWStructuredBuffer<uint> out_frames;
    /// The same pose as dual quaternions, read instead of `bones` when requested.
    StructuredBuffer<float4> bone_dual_quaternions;
    /// `BlendShapeDelta`: ten words per delta, sorted by vertex index within each shape.
    StructuredBuffer<uint> blend_shape_deltas;
    /// `GpuActiveBlendShape`: first, count, weight (as a float), reserved.
    StructuredBuffer<uint4> active_blend_shapes;
};

/// One descriptor set in SPIR-V and one argument buffer in MSL. The Metal backend binds argument
/// buffers at the descriptor-set index, so flattening these fields into nine Metal buffer slots
/// would bypass the RHI's descriptor model.
[[vk::binding(0, 0)]] ParameterBlock<SkinSet> skinSet;
[[vk::push_constant]] SkinConstants constants;

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

// --- The blend shapes ------------------------------------------------------------------------------

/// One shape's delta index for one vertex, or `0xFFFFFFFF`. A BINARY SEARCH, matching
/// `find_delta` in skin_dispatch.cpp line for line.
uint find_delta(uint first, uint count, uint vertex) {
    uint low = 0u;
    uint high = count;
    while (low < high) {
        const uint middle = low + ((high - low) / 2u);
        const uint index = first + middle;
        const uint at = skinSet.blend_shape_deltas[index * kBlendShapeDeltaWords];
        if (at == vertex) {
            return index;
        }
        if (at < vertex) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    return 0xFFFFFFFFu;
}

float3 delta_vector(uint index, uint component_offset) {
    const uint base = (index * kBlendShapeDeltaWords) + component_offset;
    return float3(asfloat(skinSet.blend_shape_deltas[base + 0u]), asfloat(skinSet.blend_shape_deltas[base + 1u]),
                  asfloat(skinSet.blend_shape_deltas[base + 2u]));
}

// --- The dual-quaternion blend ----------------------------------------------------------------------

float3 dual_rotate(float4 real, float3 direction) {
    return direction + 2.0f * cross(real.xyz, cross(real.xyz, direction) + real.w * direction);
}

float3 dual_translation(float4 real, float4 dual) {
    return 2.0f * (real.w * dual.xyz - dual.w * real.xyz + cross(real.xyz, dual.xyz));
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
    const bool frames = (constants.flags & kSkinWriteFrames) != 0u;
    const bool dual = (constants.flags & kSkinDualQuaternion) != 0u;
    const bool shapes = constants.active_blend_shapes != 0u;

    // THE UNTOUCHED VERTEX IS COPIED WORD FOR WORD. Octahedral encoding is not a round trip, so a
    // dispatch that decoded and re-encoded a vertex nothing moved would differ from the input by one
    // unit and `tests/test_skin_pass.cpp` compares buffers rather than tolerances.
    uint weight_bytes = 0u;
    for (uint probe = 0u; probe < blocks; ++probe) {
        const uint packed = skinSet.influences[((in_index * blocks) + probe) * 2u + 1u];
        weight_bytes += (packed & 0xFFu) + ((packed >> 8u) & 0xFFu) + ((packed >> 16u) & 0xFFu) +
                        ((packed >> 24u) & 0xFFu);
    }
    const uint position_base = in_index * 3u;
    const uint out_position_base = out_index * 3u;
    const uint frame_base = in_index * 2u;
    const uint out_frame_base = out_index * 2u;
    if (!shapes && weight_bytes == 0u) {
        skinSet.out_positions[out_position_base + 0u] = skinSet.in_positions[position_base + 0u];
        skinSet.out_positions[out_position_base + 1u] = skinSet.in_positions[position_base + 1u];
        skinSet.out_positions[out_position_base + 2u] = skinSet.in_positions[position_base + 2u];
        if (frames) {
            skinSet.out_frames[out_frame_base + 0u] = skinSet.in_frames[frame_base + 0u];
            skinSet.out_frames[out_frame_base + 1u] = skinSet.in_frames[frame_base + 1u];
        }
        return;
    }

    // --- The bind pose, with the active shapes applied ------------------------------------------
    float3 rest = float3(asfloat(skinSet.in_positions[position_base + 0u]),
                         asfloat(skinSet.in_positions[position_base + 1u]),
                         asfloat(skinSet.in_positions[position_base + 2u]));
    const uint normal_word = skinSet.in_frames[frame_base + 0u];
    const uint tangent_word = skinSet.in_frames[frame_base + 1u];
    // The bitangent sign lives in the low bit of the tangent's x component, so it is cleared before
    // the decode and restored after the re-encode.
    const int tangent_x = low_half(tangent_word) & ~1;
    const float bitangent_sign = (tangent_word & 1u) != 0u ? -1.0f : 1.0f;
    float3 normal = float3(0.0f, 0.0f, 1.0f);
    float3 tangent = float3(1.0f, 0.0f, 0.0f);
    if (frames) {
        normal = octahedral_decode(low_half(normal_word), high_half(normal_word));
        tangent = octahedral_decode(tangent_x, high_half(tangent_word));
    }

    for (uint slot = 0u; slot < constants.active_blend_shapes; ++slot) {
        const uint4 shape = skinSet.active_blend_shapes[slot];
        const uint delta = find_delta(shape.x, shape.y, in_index);
        if (delta == 0xFFFFFFFFu) {
            continue;
        }
        const float weight = asfloat(shape.z);
        rest += delta_vector(delta, 1u) * weight;
        if (frames) {
            normal += delta_vector(delta, 4u) * weight;
            tangent += delta_vector(delta, 7u) * weight;
        }
    }

    // --- The blend ------------------------------------------------------------------------------
    float3 skinned = rest;
    float3 moved_normal = normal;
    float3 moved_tangent = tangent;
    float weight_sum = 0.0f;

    if (dual) {
        float4 blend_real = float4(0, 0, 0, 0);
        float4 blend_dual = float4(0, 0, 0, 0);
        float blend_scale = 0.0f;
        float4 pivot = float4(0, 0, 0, 0);
        bool have_pivot = false;
        for (uint block = 0u; block < blocks; ++block) {
            const uint base = (in_index * blocks + block) * 2u;
            const uint packed_indices = skinSet.influences[base];
            const uint packed_weights = skinSet.influences[base + 1u];
            for (uint lane = 0u; lane < 4u; ++lane) {
                const float weight =
                    float((packed_weights >> (lane * 8u)) & 0xFFu) * (1.0f / 255.0f);
                if (weight == 0.0f) {
                    continue;
                }
                uint bone = (packed_indices >> (lane * 8u)) & 0xFFu;
                if (bone >= constants.bone_count) {
                    bone = constants.bone_count - 1u;
                }
                const uint row = (constants.pose_offset + bone) * 3u;
                const float4 real = skinSet.bone_dual_quaternions[row + 0u];
                const float4 dual_part = skinSet.bone_dual_quaternions[row + 1u];
                if (!have_pivot) {
                    pivot = real;
                    have_pivot = true;
                }
                // `q` AND `-q` ARE THE SAME ROTATION AND THEIR SUM IS ZERO. Every later bone is
                // flipped onto the first one's hemisphere; without this a vertex between two bones
                // converted onto opposite hemispheres collapses onto the origin.
                const float signed_weight = dot(pivot, real) < 0.0f ? -weight : weight;
                blend_real += real * signed_weight;
                blend_dual += dual_part * signed_weight;
                blend_scale += skinSet.bone_dual_quaternions[row + 2u].x * weight;
                weight_sum += weight;
            }
        }
        const float length_squared = dot(blend_real, blend_real);
        if (weight_sum != 0.0f && length_squared > 0.0f) {
            const float inverse = 1.0f / sqrt(length_squared);
            blend_real *= inverse;
            blend_dual *= inverse;
            // Scale, rotate, translate — the order a `model * inverse_bind` matrix applies them in.
            skinned = dual_rotate(blend_real, rest * blend_scale) +
                      dual_translation(blend_real, blend_dual);
            if (frames) {
                moved_normal = dual_rotate(blend_real, normal);
                moved_tangent = dual_rotate(blend_real, tangent);
            }
        }
    } else {
        float4 blended[3] = {float4(0, 0, 0, 0), float4(0, 0, 0, 0), float4(0, 0, 0, 0)};
        for (uint block = 0u; block < blocks; ++block) {
            const uint base = (in_index * blocks + block) * 2u;
            const uint packed_indices = skinSet.influences[base];
            const uint packed_weights = skinSet.influences[base + 1u];
            for (uint lane = 0u; lane < 4u; ++lane) {
                const float weight =
                    float((packed_weights >> (lane * 8u)) & 0xFFu) * (1.0f / 255.0f);
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
                blended[0] += skinSet.bones[row + 0u] * weight;
                blended[1] += skinSet.bones[row + 1u] * weight;
                blended[2] += skinSet.bones[row + 2u] * weight;
                weight_sum += weight;
            }
        }
        if (weight_sum != 0.0f) {
            // The linear part and the translation column, in the reference's order.
            skinned = float3(dot(blended[0].xyz, rest) + blended[0].w,
                             dot(blended[1].xyz, rest) + blended[1].w,
                             dot(blended[2].xyz, rest) + blended[2].w);
            if (frames) {
                // THE LINEAR PART AND NOT THE INVERSE TRANSPOSE. See skin_dispatch.cpp for why: a
                // per-vertex 3x3 inverse is the cost this engine does not pay.
                moved_normal = float3(dot(blended[0].xyz, normal), dot(blended[1].xyz, normal),
                                      dot(blended[2].xyz, normal));
                moved_tangent = float3(dot(blended[0].xyz, tangent), dot(blended[1].xyz, tangent),
                                       dot(blended[2].xyz, tangent));
            }
        }
    }

    skinSet.out_positions[out_position_base + 0u] = asuint(skinned.x);
    skinSet.out_positions[out_position_base + 1u] = asuint(skinned.y);
    skinSet.out_positions[out_position_base + 2u] = asuint(skinned.z);
    if (!frames) {
        return;
    }

    const uint encoded_normal = octahedral_encode(normalised_or(moved_normal, float3(0, 0, 1)));
    uint encoded_tangent = octahedral_encode(normalised_or(moved_tangent, float3(1, 0, 0)));
    encoded_tangent &= ~1u;
    if (bitangent_sign < 0.0f) {
        encoded_tangent |= 1u;
    }
    skinSet.out_frames[out_frame_base + 0u] = encoded_normal;
    skinSet.out_frames[out_frame_base + 1u] = encoded_tangent;
}
