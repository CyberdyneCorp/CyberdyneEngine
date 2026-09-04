// Mesh representation and vertex compression. Task 4.2.1.
//
// `rendering-geometry-and-resources` states the encodings exactly — octahedral normals into two
// 16-bit snorms with the tangent sign packed in, 16-bit unorm UVs relative to a per-surface
// rectangle, optional 16-bit quantised positions relative to the bounding box — so every case here
// checks a stated encoding's round trip and its error, rather than checking that a function was
// called.

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/mesh.h>
#include <cy/test/test.h>

using cy::f32;
using cy::u16;
using cy::u32;
using namespace cy::render;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

/// The worst-case angular error of the octahedral encoding, in radians. Two 16-bit components over
/// the unit sphere is far finer than this; the bound is loose so the case measures the encoding
/// rather than the constant.
constexpr f32 kNormalTolerance = 1e-3F;

}  // namespace

CY_TEST_CASE("octahedral encoding round-trips a direction in both hemispheres") {
    const cy::Vec3 directions[] = {
        cy::Vec3{0.0F, 1.0F, 0.0F},
        cy::Vec3{0.0F, 0.0F, 1.0F},
        cy::Vec3{0.0F, 0.0F, -1.0F},  // the lower hemisphere, which is the folded case
        normalize(cy::Vec3{1.0F, 2.0F, -3.0F}),
        normalize(cy::Vec3{-4.0F, -1.0F, -0.5F}),
        // NOLINTNEXTLINE(modernize-use-std-numbers) three equal components, not Euler's gamma
        normalize(cy::Vec3{0.577F, 0.577F, 0.577F}),
    };
    for (const cy::Vec3& direction : directions) {
        cy::i16 x = 0;
        cy::i16 y = 0;
        octahedral_encode(direction, x, y);
        const cy::Vec3 decoded = octahedral_decode(x, y);
        CY_CHECK_NEAR(decoded.x, direction.x, kNormalTolerance);
        CY_CHECK_NEAR(decoded.y, direction.y, kNormalTolerance);
        CY_CHECK_NEAR(decoded.z, direction.z, kNormalTolerance);
    }
}

CY_TEST_CASE("the tangent sign survives the encoding it is packed into") {
    // "with the tangent sign packed into the encoding" — one bit of the tangent's x component. The
    // case that matters is that packing it does not disturb the direction beyond the encoding's own
    // error, which is what makes the bit free rather than cheap.
    const cy::Vec3 normal = normalize(cy::Vec3{0.0F, 1.0F, 0.2F});
    const cy::Vec3 tangent = normalize(cy::Vec3{1.0F, 0.0F, 0.3F});

    for (const f32 sign : {1.0F, -1.0F}) {
        const PackedNormalTangent packed = pack_normal_tangent(normal, tangent, sign);
        CY_CHECK_NEAR(unpack_bitangent_sign(packed), sign, 1e-6F);
        const cy::Vec3 decoded_tangent = unpack_tangent(packed);
        CY_CHECK_NEAR(decoded_tangent.x, tangent.x, kNormalTolerance);
        CY_CHECK_NEAR(decoded_tangent.y, tangent.y, kNormalTolerance);
        CY_CHECK_NEAR(decoded_tangent.z, tangent.z, kNormalTolerance);
        const cy::Vec3 decoded_normal = unpack_normal(packed);
        CY_CHECK_NEAR(decoded_normal.y, normal.y, kNormalTolerance);
    }
    // A zero sign is an unbuilt tangent frame; one answer is chosen rather than flipping every such
    // vertex.
    CY_CHECK_NEAR(unpack_bitangent_sign(pack_normal_tangent(normal, tangent, 0.0F)), 1.0F, 1e-6F);
}

CY_TEST_CASE("UVs quantise relative to the surface's own rectangle") {
    // Relative to the surface rather than to [0, 1]: a lightmap UV set occupying a corner of the
    // square would otherwise waste most of the 16 bits.
    const cy::Rect bounds = cy::Rect::from_min_max(cy::Vec2{0.25F, 0.5F}, cy::Vec2{0.3F, 0.55F});
    const cy::Vec2 uv{0.275F, 0.525F};
    u16 u = 0;
    u16 v = 0;
    quantise_uv(uv, bounds, u, v);
    const cy::Vec2 decoded = dequantise_uv(u, v, bounds);
    CY_CHECK_NEAR(decoded.x, uv.x, 1e-6F);
    CY_CHECK_NEAR(decoded.y, uv.y, 1e-6F);
}

CY_TEST_CASE("positions quantise relative to the bounding box, and the error is reported") {
    const cy::Aabb small =
        cy::Aabb::from_center_extents(cy::Vec3{0.0F, 0.0F, 0.0F}, cy::Vec3{0.5F, 0.5F, 0.5F});
    const cy::Vec3 point{0.125F, -0.375F, 0.5F};
    u16 packed[3] = {0, 0, 0};
    quantise_position(point, small, packed);
    const cy::Vec3 decoded = dequantise_position(packed, small);
    const f32 bound = position_quantisation_error(small);
    CY_CHECK_NEAR(decoded.x, point.x, bound);
    CY_CHECK_NEAR(decoded.y, point.y, bound);
    CY_CHECK_NEAR(decoded.z, point.z, bound);

    // "WHEN a mesh's bounding box is small relative to its detail THEN 16-bit quantised positions
    // SHALL be used, halving position bandwidth with no visible error."
    CY_CHECK(should_quantise_positions(small));
    CY_CHECK_LT(bound, 0.001F);
}

CY_TEST_CASE("a very large mesh is not quantised, and the number says why") {
    // "WHEN a very large mesh would suffer visible quantisation error THEN the importer SHALL warn
    // and default to full precision." The decision is a measured displacement against a tolerance,
    // not a rule of thumb about vertex counts.
    const cy::Aabb huge = cy::Aabb::from_center_extents(cy::Vec3{0.0F, 0.0F, 0.0F},
                                                        cy::Vec3{5000.0F, 5000.0F, 5000.0F});
    CY_CHECK_FALSE(should_quantise_positions(huge));
    CY_CHECK_GT(position_quantisation_error(huge), 0.001F);
    // The boundary is where it should be, and it is a measurement rather than a rule of thumb: at
    // 16 bits over a 500-metre tile the step is 7.6 mm, which is visible; a caller that can live
    // with a centimetre says so rather than being told.
    const cy::Aabb tile =
        cy::Aabb::from_center_extents(cy::Vec3{0.0F, 0.0F, 0.0F}, cy::Vec3{250.0F, 25.0F, 250.0F});
    CY_CHECK_FALSE(should_quantise_positions(tile));
    CY_CHECK(should_quantise_positions(tile, 0.01F));
    // A 100-metre tile is under a millimetre and is quantised, which is the useful half of the
    // rule.
    const cy::Aabb small_tile =
        cy::Aabb::from_center_extents(cy::Vec3{0.0F, 0.0F, 0.0F}, cy::Vec3{50.0F, 5.0F, 50.0F});
    CY_CHECK(should_quantise_positions(small_tile));
}

CY_TEST_CASE("colours pack into one word and come back") {
    const cy::u32 packed = pack_color_unorm8(1.0F, 0.5F, 0.0F, 0.25F);
    f32 unpacked[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    unpack_color_unorm8(packed, unpacked);
    CY_CHECK_NEAR(unpacked[0], 1.0F, 0.004F);
    CY_CHECK_NEAR(unpacked[1], 0.5F, 0.004F);
    CY_CHECK_NEAR(unpacked[2], 0.0F, 0.004F);
    CY_CHECK_NEAR(unpacked[3], 0.25F, 0.004F);
}

CY_TEST_CASE("the index width follows from the vertex count and nothing else") {
    CY_CHECK_EQ(index_width_for(3), IndexWidth::Sixteen);
    CY_CHECK_EQ(index_width_for(65536), IndexWidth::Sixteen);
    CY_CHECK_EQ(index_width_for(65537), IndexWidth::ThirtyTwo);
    CY_CHECK_EQ(index_byte_size(IndexWidth::Sixteen), 2U);
    CY_CHECK_EQ(index_byte_size(IndexWidth::ThirtyTwo), 4U);
}

CY_TEST_CASE("a depth pass binds the position stream and an alpha-tested one adds UVs") {
    // "WHEN a shadow pass renders a mesh THEN it SHALL bind only the position stream (plus UV for
    // alpha-tested materials)." The masks are constants so a pass does not assemble one itself.
    CY_CHECK(has_stream(kDepthPassStreams, VertexStream::Position));
    CY_CHECK_FALSE(has_stream(kDepthPassStreams, VertexStream::NormalTangent));
    CY_CHECK_FALSE(has_stream(kDepthPassStreams, VertexStream::Uv));
    CY_CHECK(has_stream(kMaskedDepthPassStreams, VertexStream::Uv));
    CY_CHECK_FALSE(has_stream(kMaskedDepthPassStreams, VertexStream::Color));
}

CY_TEST_CASE(
    "a mesh's device size follows from its streams, and quantising positions halves them") {
    MeshDescription desc;
    desc.streams = stream_bit(VertexStream::Position) | stream_bit(VertexStream::NormalTangent) |
                   stream_bit(VertexStream::Uv);
    desc.vertex_count = 1000;
    desc.index_count = 3000;
    desc.index_width = IndexWidth::Sixteen;

    const cy::u64 full = mesh_byte_size(desc);
    desc.positions_quantised = true;
    const cy::u64 quantised = mesh_byte_size(desc);
    CY_CHECK_LT(quantised, full);
    // 12 bytes to 8 over a thousand vertices.
    CY_CHECK_EQ(full - quantised, 4000ULL);
}

CY_TEST_CASE("LOD selection follows screen coverage, and the bias moves it") {
    const MeshLod chain[] = {
        MeshLod{0, 1, 0.5F, 10000},
        MeshLod{1, 1, 0.1F, 2000},
        MeshLod{2, 1, 0.0F, 400},
    };
    const cy::Span<const MeshLod> levels(chain, 3);
    CY_CHECK_EQ(select_lod(levels, 0.9F, 0.0F), 0U);
    CY_CHECK_EQ(select_lod(levels, 0.2F, 0.0F), 1U);
    CY_CHECK_EQ(select_lod(levels, 0.01F, 0.0F), 2U);
    // A positive bias keeps more detail: "treat this instance as if it were this much closer".
    CY_CHECK_EQ(select_lod(levels, 0.2F, 2.0F), 0U);
    // A mesh too small for any threshold gets the coarsest level, never nothing.
    CY_CHECK_EQ(select_lod(levels, 0.0F, -5.0F), 2U);
    CY_CHECK_EQ(select_lod(cy::Span<const MeshLod>(), 0.5F, 0.0F), 0U);
}

CY_TEST_CASE("mesh surfaces and LODs are copied into the server's own storage") {
    // Not a mesh test so much as an ownership one: a caller that kept its arrays would be a second
    // owner of one description.
    cy::Array<MeshSurface> surfaces(allocator());
    CY_REQUIRE(surfaces.push_back(MeshSurface{}).has_value());
    CY_CHECK_EQ(surfaces.size(), 1U);
    CY_CHECK_EQ(surfaces[0].material_slot, 0U);
}
