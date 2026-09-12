#pragma once
// Skinning: the records the compute pass reads and writes, and the reference that says what it
// must compute. M8.d.
//
// `rendering-geometry-and-resources` — "Skinning": "Skinned meshes SHALL be transformed by a
// **compute pass** writing into per-instance output vertex buffers, so the result is reusable
// across passes (depth, shadows, main) without re-skinning", with "up to 4 or 8 influences per
// vertex (selectable per mesh)", and "Bone matrices SHALL be read from the **GPU pose world**".
//
// ================================================================================================
// WHAT skinning.h LEFT, AND WHAT THIS FILE IS
// ================================================================================================
//
// `skinning.h` is a DESCRIPTION: `SkinningDescriptor` says what a compute pass must do, and its
// header records that M8.b built the pose world it reads from. What neither it nor anything else in
// the tree held was the arrow from a bone matrix to a moved vertex. Before this file there was no
// skinning arithmetic anywhere in `src/` — no shader, no CPU path, no function that multiplied a
// vertex by a bone matrix. A reader of `skinning.h`'s confident closing note could reasonably have
// concluded that skinned meshes were drawn. They were not.
//
// This file is the layer-2 half of closing that: the LAYOUT of every buffer the dispatch binds, and
// a CPU implementation of exactly the algorithm the shader runs. `src/rendering/skinning/` is the
// other half — it owns the device, the compute pipeline and the buffers, and its suite compares the
// dispatch's output against `cpu_reference_skin` word for word.
//
// The split, and the reason for the reference, are `culling/gpu_cull.h`'s and this file only obeys
// them: a compute pass needs a device and layer 2 may not name one, and a renderer whose skinning
// can only be verified by looking at a picture is a renderer whose skinning is verified by nobody.
//
// ================================================================================================
// EVERY BUFFER IS A VERTEX BUFFER'S LAYOUT, NOT A CONVENIENT ONE
// ================================================================================================
//
// The output positions are `Vec3` at stride 12 and the output frames are four 16-bit words at
// stride 8, because those are `render::VertexStream::Position` (unquantised) and
// `render::VertexStream::NormalTangent`, which is what a vertex input binds. A skinning pass whose
// output had to be repacked before it could be drawn would have moved the cost rather than paid it,
// and the whole point of the requirement's "so the result is reusable across passes" is that the
// depth prepass, the shadow passes and the opaque pass all bind the same bytes.
//
// ================================================================================================
// WHAT IS DELIBERATELY NOT HERE
// ================================================================================================
//
// **Dual quaternion skinning.** `SkinningMethod::DualQuaternion` is a declared option and this
// dispatch does not implement it, because a dual-quaternion skin needs the pose AS DUAL
// QUATERNIONS and `cy::animation::PoseWorld` publishes matrices — `Skeleton::to_skinning` composes
// `model * inverse_bind` and hands back `Mat4`. Deriving a rotation quaternion per vertex per
// influence inside the dispatch is the wrong place for that work by two orders of magnitude, and
// adding a second pose representation is `animation-and-skinning`'s to add, not this module's.
// `make_skin_constants` therefore REFUSES the method by name rather than silently linear-blending
// and producing a candy-wrapper twist nobody could trace back to an ignored field.
//
// **Blend shapes.** `BlendShapeSet` in skinning.h is the storage and the active-shape compaction;
// applying the deltas belongs in this same dispatch and is not written yet.
// `make_skin_constants` refuses a descriptor with a non-zero `blend_shape_count` for the same
// reason.
//
// Both refusals follow `GpuCullPass`'s refusal of `kGpuCullOcclusion`: a dispatch that quietly
// ignored a flag would be indistinguishable from one that honoured it over empty data.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/servers/render/geometry/skinning.h>
#include <cy/servers/render/mesh.h>

#include <cstddef>

namespace cy::render::geometry {

// --- The pose, as the dispatch reads it ---------------------------------------------------------

/// One skinning matrix: the three rows of a 3x4 affine transform, as three `float4`s.
///
/// THREE ROWS AND NOT FOUR. A skinning matrix is `model * inverse_bind` and its fourth row is
/// `(0, 0, 0, 1)` for every pose the animation runtime can produce, because `Transform` is
/// translation, rotation and scale and none of them writes a projective row. Storing it would cost
/// a quarter of the pose world's bandwidth to carry a constant.
///
/// ROW-MAJOR HERE AND COLUMN-MAJOR IN `Mat4`, which is the one transposition in the whole path and
/// is why `pack_bone_matrix` exists rather than a `memcpy`. `cy::Mat4` is `Vec4 columns[4]` and
/// applies to column vectors; a shader that read those sixteen floats as rows would rotate every
/// vertex by the inverse of the intended rotation, which looks like a plausible animation and is
/// the classic silent corruption `Mat4::from_translation`'s own comment warns about.
struct alignas(16) GpuBoneMatrix {
    f32 rows[3][4] = {{1.0F, 0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F, 0.0F}};
};

static_assert(sizeof(GpuBoneMatrix) == 48, "GpuBoneMatrix is three shader-visible float4 rows");

/// Transpose one `Mat4` into the dispatch's row form. The only place the convention changes.
[[nodiscard]] GpuBoneMatrix pack_bone_matrix(const Mat4& skinning_matrix) noexcept;

// --- The influences, as the cooked mesh holds them -----------------------------------------------

/// Four bone influences for one vertex: four 8-bit bone indices and four 8-bit weights.
///
/// EIGHT BYTES, WHICH IS `vertex_stream_byte_size(VertexStream::Skin)` EXACTLY, and that is not a
/// coincidence — this is that stream, read by a compute shader instead of by a vertex fetch. An
/// eight-influence mesh is two of these per vertex, consecutively, which is the same file's "Eight
/// influences double it".
///
/// The two words are the layout a shader sees; `bone_index_of` and `bone_weight_of` are how C++
/// reads them, so that the byte order is stated once instead of being open-coded at every use.
///
/// WEIGHTS ARE NOT RENORMALISED BY THE DISPATCH. `weight = byte / 255`, and a cooked mesh whose
/// bytes do not sum to 255 is darker or brighter than it should be at that vertex — which is the
/// importer's defect to fix, at cook time, once, rather than a division this dispatch performs per
/// vertex per frame forever. `skin_weight_sum` is how a cook step or a test checks it.
struct GpuSkinInfluence {
    /// Bone indices, one per byte, lane 0 in the low byte. Relative to the skin's first bone.
    u32 indices = 0;
    /// Weights, one per byte, lane 0 in the low byte, 255 meaning 1.
    u32 weights = 0;
};

static_assert(sizeof(GpuSkinInfluence) == 8, "GpuSkinInfluence is VertexStream::Skin's 8 bytes");

[[nodiscard]] constexpr u32 bone_index_of(const GpuSkinInfluence& influence, u32 lane) noexcept {
    return (influence.indices >> (lane * 8U)) & 0xFFU;
}
[[nodiscard]] constexpr u32 bone_weight_byte_of(const GpuSkinInfluence& influence,
                                                u32 lane) noexcept {
    return (influence.weights >> (lane * 8U)) & 0xFFU;
}
[[nodiscard]] constexpr f32 bone_weight_of(const GpuSkinInfluence& influence, u32 lane) noexcept {
    return static_cast<f32>(bone_weight_byte_of(influence, lane)) * (1.0F / 255.0F);
}

/// Pack four lanes. `weights` are the bytes, not fractions, so that a caller writing an exact split
/// writes an exact split — 128 and 127, not 0.5 and 0.5 rounded twice.
[[nodiscard]] constexpr GpuSkinInfluence skin_influence(const u8 indices[4],
                                                        const u8 weights[4]) noexcept {
    GpuSkinInfluence packed;
    for (u32 lane = 0; lane < 4U; ++lane) {
        packed.indices |= static_cast<u32>(indices[lane]) << (lane * 8U);
        packed.weights |= static_cast<u32>(weights[lane]) << (lane * 8U);
    }
    return packed;
}

/// The sum of one vertex's weight BYTES across `blocks` consecutive records. 255 is a correctly
/// cooked vertex; anything else is a content defect the dispatch will faithfully reproduce.
[[nodiscard]] u32 skin_weight_sum(const GpuSkinInfluence* influences, u32 blocks) noexcept;

// --- The constant block --------------------------------------------------------------------------

/// What the dispatch is told that a buffer's own length cannot say.
///
/// Every field changes the bytes the dispatch writes, which is why this is a struct with a name
/// rather than a handful of arguments: `src/rendering/skinning/` pushes it verbatim and
/// `cpu_reference_skin` reads the same one, so there is no second place for a default to differ.
enum GpuSkinFlagBits : u32 {
    /// Skin the normal and tangent frame as well as the position. Off for a shadow-only skin, which
    /// binds `render::kDepthPassStreams` and reads no frame at all — and off is not an optimisation
    /// there, it is the difference between a dispatch that reads a stream the pass will not bind
    /// and one that does not.
    kSkinWriteFrames = 1U << 0U,
};

/// Eight words, matching the shader's push constant block.
struct GpuSkinConstants {
    /// How many vertices this dispatch covers. One thread each.
    u32 vertex_count = 0;
    /// How many bones the skin's slice of the pose holds. A dispatch clamps every index against it
    /// rather than trusting the cooked stream, because an out-of-range index in a storage buffer is
    /// undefined behaviour on the device and a visible explosion of the mesh on the CPU.
    u32 bone_count = 0;
    /// Where this skin's bones begin in the buffer bound at binding 0 — `PoseWorld::matrix_offset`
    /// when the source is the shared world, and 0 when the skin uploads its own pose.
    u32 pose_offset = 0;
    /// 4 or 8.
    u32 influences = 4;
    /// `GpuSkinFlagBits`.
    u32 flags = 0;
    /// Where this skin's vertices begin in the input streams.
    u32 first_input_vertex = 0;
    /// Where they begin in the output streams. Distinct from the input's because several skins
    /// share one output buffer, and because the output is double buffered — the second frame's
    /// range is the first's plus the vertex count, which is what `SkinnedBuffers` indexes.
    u32 first_output_vertex = 0;
    u32 reserved = 0;
};

static_assert(sizeof(GpuSkinConstants) == 32, "the push block is eight words");

/// Derive the constant block from a validated descriptor, refusing what the dispatch cannot do.
///
/// The refusals are the two documented in the header comment — dual quaternion skinning and blend
/// shapes — plus whatever `SkinningDescriptor::validate()` already refuses, which is run first so
/// that a caller gets one diagnostic rather than a second-order one.
[[nodiscard]] Expected<GpuSkinConstants, Error> make_skin_constants(
    const SkinningDescriptor& descriptor, bool write_frames, u32 first_input_vertex,
    u32 first_output_vertex) noexcept;

// --- The reference ---------------------------------------------------------------------------

/// Everything the dispatch reads. Every span is indexed from ZERO, not from the constants' offsets;
/// the offsets are applied by the reference exactly as the shader applies them, so that a caller
/// that gets an offset wrong gets the same wrong answer from both.
struct SkinInputs {
    /// The skinning matrices — `model * inverse_bind` — as `Skeleton::to_skinning` produces them
    /// and `PoseWorld` publishes them, already transposed by `pack_bone_matrix`.
    Span<const GpuBoneMatrix> bones;
    /// Bind-pose positions, one per vertex, stride 12.
    Span<const Vec3> positions;
    /// Bind-pose frames, one per vertex. May be empty when `kSkinWriteFrames` is clear.
    Span<const PackedNormalTangent> frames;
    /// One record per vertex at four influences, two at eight.
    Span<const GpuSkinInfluence> influences;
};

/// Everything it writes. `frames` may be empty when `kSkinWriteFrames` is clear.
///
/// THE OUTPUT FRAME IS THE ENGINE'S COOKED ENCODING, `render::PackedNormalTangent`, and not the
/// encoding the forward pipeline's vertex input happens to bind. `rhi::Format` has no
/// `Rgba16Snorm`, so `cy::rendering::pipeline` binds its normal stream as `Rgba16Sfloat` carrying
/// the same octahedral pair as half floats — `frame_pipelines.h` records that gap at length and
/// says closing it means adding a format to `src/backends/rhi/`. Writing the cooked encoding here
/// keeps the dispatch honest about which representation it produces; a skinned mesh drawn through
/// that vertex input needs the gap closed, and this module will not paper over it by inventing a
/// second frame encoding for one consumer.
struct SkinOutputs {
    Span<Vec3> positions;
    Span<PackedNormalTangent> frames;
};

/// Skin one range of vertices on the CPU, computing exactly what the compute pass must.
///
/// EVERY EXPRESSION IS WRITTEN IN THE ORDER THE SHADER WRITES IT, including the ones that invite
/// reassociation. The blended matrix is accumulated per component and then applied, rather than the
/// position being transformed by each bone and the results summed — the two are the same in exact
/// arithmetic and differ in the last bits in floating point, and the comparison this function
/// exists to be the expected value of is exact.
///
/// Deterministic and allocation-free: the output is a pure function of the inputs and the
/// constants, which is what lets a device test compare buffers rather than tolerances.
[[nodiscard]] Status cpu_reference_skin(const GpuSkinConstants& constants, const SkinInputs& inputs,
                                        const SkinOutputs& outputs) noexcept;

/// Transform one position by one blended skinning matrix, as the dispatch does it.
///
/// Exposed because it is what a hand-computed expected value is written against: a test that says
/// "this vertex, under this pose, lands here" wants to state the arithmetic once and compare, not
/// to reimplement the blend.
[[nodiscard]] Vec3 skin_position(Span<const GpuBoneMatrix> bones, const GpuSkinInfluence* influence,
                                 u32 blocks, u32 bone_count, u32 pose_offset, Vec3 position,
                                 f32& out_weight_sum) noexcept;

}  // namespace cy::render::geometry
