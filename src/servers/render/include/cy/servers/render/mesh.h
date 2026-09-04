#pragma once
// Mesh representation, vertex compression and instancing. Task 4.2.1.
//
// `rendering-geometry-and-resources` — "Mesh representation": a mesh is one or more **surfaces**,
// each with a vertex buffer set, an index buffer, a material slot index and bounds; and vertex data
// is "split into separate streams so passes bind only what they need".
//
// --- WHY THE STREAMS ARE SEPARATE, IN ONE SENTENCE EACH
// -------------------------------------------
//
// The specification's own table gives the reason for each, and it is worth restating because the
// alternative — one interleaved vertex struct — is the shape most engines start with:
//
//   Position       every pass binds it, including depth and shadow, which bind NOTHING else. A
//                  shadow pass over an interleaved vertex reads normals, UVs and colours it will
//                  never use, and shadow passes are bandwidth-bound.
//   NormalTangent  shading passes only.
//   Uv             shading passes, and alpha-tested shadow passes (which need UV0 and nothing
//   more). Color          present only when a mesh has vertex colours, which most do not. Skin
//   skinned meshes only.
//
// So the split is not tidiness: it is what makes "WHEN a shadow pass renders a mesh THEN it SHALL
// bind only the position stream" expressible at all.
//
// --- COMPRESSION IS THE DEFAULT, AND ITS ERROR IS MEASURED RATHER THAN ASSUMED
// --------------------
//
// "Vertex attributes SHALL be compressed by default", and "the engine SHALL detect when
// quantisation would produce visible error and warn". `position_quantisation_error()` below is that
// detection: it returns the worst-case displacement in world units for a surface's bounding box, so
// the importer's warning is a number and a threshold rather than a rule of thumb about mesh size.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/servers/render/handles.h>
#include <cy/servers/render/sort.h>
#include <cy/servers/render/types.h>

namespace cy::render {

// --- Streams --------------------------------------------------------------------------------

enum class VertexStream : u8 {
    Position = 0,
    NormalTangent,
    Uv,
    Color,
    Skin,
    Count,
};

inline constexpr u32 kVertexStreamCount = static_cast<u32>(VertexStream::Count);

[[nodiscard]] const char* vertex_stream_name(VertexStream stream) noexcept;

using VertexStreamMask = u8;

[[nodiscard]] constexpr VertexStreamMask stream_bit(VertexStream stream) noexcept {
    return static_cast<VertexStreamMask>(1U << static_cast<u32>(stream));
}
[[nodiscard]] constexpr bool has_stream(VertexStreamMask mask, VertexStream stream) noexcept {
    return (mask & stream_bit(stream)) != 0U;
}

/// What a depth or shadow pass binds. The specification's scenario, as a constant, so a pass does
/// not assemble the mask itself and get it right by coincidence.
inline constexpr VertexStreamMask kDepthPassStreams = stream_bit(VertexStream::Position);
/// The same for an alpha-tested material, which needs UV0 to run its discard.
inline constexpr VertexStreamMask kMaskedDepthPassStreams =
    stream_bit(VertexStream::Position) | stream_bit(VertexStream::Uv);

// --- Compressed attributes ------------------------------------------------------------------

/// A normal and a tangent, octahedral, eight bytes.
///
/// The tangent's handedness (the bitangent's sign) is packed into the LOW BIT of `tangent[0]`,
/// which is the specification's "with the tangent sign packed into the encoding". It costs one bit
/// of angular precision on one axis of the tangent — about 0.003 degrees at 16 bits — and it saves
/// a separate byte per vertex that would round the stream up to twelve.
struct PackedNormalTangent {
    i16 normal[2] = {0, 0};
    i16 tangent[2] = {0, 0};
};

static_assert(sizeof(PackedNormalTangent) == 8);

/// Encode a unit vector into two 16-bit signed normalised components, octahedrally.
void octahedral_encode(Vec3 unit_vector, i16& out_x, i16& out_y) noexcept;
[[nodiscard]] Vec3 octahedral_decode(i16 x, i16 y) noexcept;

/// Pack a vertex frame. `bitangent_sign` is +1 or −1; anything else is treated as +1, because a
/// zero sign is an unbuilt tangent frame and flipping every such vertex would be worse than
/// choosing one.
[[nodiscard]] PackedNormalTangent pack_normal_tangent(Vec3 normal, Vec3 tangent,
                                                      f32 bitangent_sign) noexcept;
[[nodiscard]] Vec3 unpack_normal(const PackedNormalTangent& packed) noexcept;
[[nodiscard]] Vec3 unpack_tangent(const PackedNormalTangent& packed) noexcept;
[[nodiscard]] f32 unpack_bitangent_sign(const PackedNormalTangent& packed) noexcept;

/// UVs, 16-bit unorm relative to a per-surface bounding rectangle. Outside the rectangle the value
/// clamps, which is why `MeshSurface::uv_bounds` must be the real extent of the surface's UVs and
/// not a guess.
void quantise_uv(Vec2 uv, const Rect& bounds, u16& out_u, u16& out_v) noexcept;
[[nodiscard]] Vec2 dequantise_uv(u16 u, u16 v, const Rect& bounds) noexcept;

/// Positions, 16-bit unorm relative to the surface's bounding box, "with the box supplied per
/// draw".
void quantise_position(Vec3 position, const Aabb& bounds, u16 out[3]) noexcept;
[[nodiscard]] Vec3 dequantise_position(const u16 quantised[3], const Aabb& bounds) noexcept;

/// The worst-case displacement quantising a position into `bounds` can introduce, in world units:
/// half a quantisation step along the longest axis.
[[nodiscard]] f32 position_quantisation_error(const Aabb& bounds) noexcept;

/// Whether a surface should be quantised, given how much error is acceptable.
///
/// The default tolerance is one millimetre, which is the scale at which a quantisation seam becomes
/// visible on a surface a character stands next to. A mesh that fails it keeps full precision and
/// the importer warns — "WHEN a very large mesh would suffer visible quantisation error THEN the
/// importer SHALL warn and default to full precision".
[[nodiscard]] bool should_quantise_positions(const Aabb& bounds,
                                             f32 tolerance_world_units = 0.001F) noexcept;

/// Vertex colours, 8-bit unorm, one word.
[[nodiscard]] u32 pack_color_unorm8(f32 r, f32 g, f32 b, f32 a) noexcept;
void unpack_color_unorm8(u32 packed, f32 out[4]) noexcept;

// --- Surfaces and meshes --------------------------------------------------------------------

enum class IndexWidth : u8 {
    Sixteen = 0,
    ThirtyTwo,
};

/// "Index buffers SHALL use 16-bit indices where the vertex count permits, otherwise 32-bit." The
/// rule as a function, so no importer decides it a second time.
[[nodiscard]] constexpr IndexWidth index_width_for(u32 vertex_count) noexcept {
    return (vertex_count <= 65536U) ? IndexWidth::Sixteen : IndexWidth::ThirtyTwo;
}
[[nodiscard]] constexpr u32 index_byte_size(IndexWidth width) noexcept {
    return (width == IndexWidth::Sixteen) ? 2U : 4U;
}

struct MeshSurface {
    u32 vertex_offset = 0;
    u32 vertex_count = 0;
    u32 index_offset = 0;
    u32 index_count = 0;
    /// Which of the mesh's material slots this surface draws with. An instance supplies the
    /// materials; the surface names the slot, so one mesh renders with different materials per
    /// instance without being duplicated.
    u8 material_slot = 0;
    Aabb bounds = Aabb::empty();
    /// The extent of this surface's UV0, for the 16-bit unorm encoding.
    Rect uv_bounds = Rect::from_min_max(Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F});
};

/// One level of a traditional LOD chain.
///
/// `rendering-geometry-and-resources` makes the threshold **screen coverage** rather than distance:
/// coverage is what actually determines whether detail is visible, and it is correct for a field of
/// view change and for a resolution change without anybody re-authoring the numbers.
struct MeshLod {
    u32 first_surface = 0;
    u32 surface_count = 0;
    /// Used while the mesh covers at least this fraction of the screen's height. Descending across
    /// the chain.
    f32 screen_coverage_threshold = 0.0F;
    u32 triangle_count = 0;
};

/// Which level to draw at a given coverage. Returns the last level when nothing matches, which is
/// the coarsest — a mesh too small for any threshold is drawn at its cheapest, never dropped.
[[nodiscard]] u32 select_lod(Span<const MeshLod> chain, f32 screen_coverage, f32 bias) noexcept;

/// Bytes one vertex occupies in a stream, in the engine's packed layout.
///
/// Written down here because it is the number a memory report multiplies and the number a vertex
/// buffer is sized from, and two places computing it independently is how a buffer ends up one
/// stride short. `Position` answers 6 when positions are quantised and 12 when they are not, which
/// is why it takes the flag rather than being a constant.
[[nodiscard]] u32 vertex_stream_byte_size(VertexStream stream, bool positions_quantised) noexcept;

struct MeshDescription {
    Name name;
    VertexStreamMask streams = stream_bit(VertexStream::Position);
    IndexWidth index_width = IndexWidth::Sixteen;
    bool positions_quantised = false;
    u32 vertex_count = 0;
    u32 index_count = 0;
    u8 material_slot_count = 1;
    Aabb bounds = Aabb::empty();
};

/// Total device bytes for a mesh's vertex and index buffers, from its description alone. What the
/// memory report charges the `Meshes` category, computed without asking a device — which is what
/// lets the number exist before anything is uploaded.
[[nodiscard]] u64 mesh_byte_size(const MeshDescription& desc) noexcept;

/// A mesh as the server holds it: the description, its surfaces and its LOD chain.
struct Mesh {
    MeshDescription desc;
    Array<MeshSurface> surfaces;
    Array<MeshLod> lods;

    explicit Mesh(Allocator& allocator) noexcept : surfaces(allocator), lods(allocator) {}
};

// --- Instancing -----------------------------------------------------------------------------

/// A run of adjacent draws merged into one instanced draw.
///
/// "WHEN 500 entities share one mesh and material THEN submission SHALL merge them into instanced
/// draws without the content author doing anything." The merge is possible because the sort key
/// (sort.h) puts pipeline, material and mesh above depth, so draws that can merge are already
/// adjacent — merging is a linear scan and never a search.
struct InstancedBatch {
    /// Index of the first draw in the sorted list, and how many follow it.
    u32 first_draw = 0;
    u32 draw_count = 0;
    u32 mesh = 0;
    u32 material = 0;
    u32 pipeline = 0;
    u32 surface = 0;
};

/// Merge a SORTED draw list into instanced batches. Returns the number of batches produced.
///
/// The list must be sorted by `sort_draws()`; a merge over an unsorted list would produce batches
/// whose membership depends on the input order, which is the determinism defect this whole area
/// exists to avoid. Asserted in a development build.
[[nodiscard]] Expected<u32, Error> build_instanced_batches(Span<const DrawItem> sorted_draws,
                                                           Array<InstancedBatch>& out) noexcept;

}  // namespace cy::render
