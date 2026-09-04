// Vertex compression, LOD selection and automatic instancing. See cy/servers/render/mesh.h.

#include <cy/servers/render/mesh.h>

#include <cy/core/base/assert.h>
#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::render {
namespace {

constexpr const char* kStreamNames[] = {"Position", "NormalTangent", "Uv", "Color", "Skin"};
static_assert(sizeof(kStreamNames) / sizeof(kStreamNames[0]) == kVertexStreamCount);

constexpr f32 kSnormScale = 32767.0F;
constexpr f32 kUnorm16Scale = 65535.0F;

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    if (!(value > 0.0F)) {  // false for NaN as well, which is the answer that keeps the encoding in
        return 0.0F;        // range rather than producing an arbitrary integer
    }
    return (value < 1.0F) ? value : 1.0F;
}

[[nodiscard]] i16 to_snorm16(f32 value) noexcept {
    return static_cast<i16>(std::round(math::clamp(value, -1.0F, 1.0F) * kSnormScale));
}

[[nodiscard]] f32 from_snorm16(i16 value) noexcept {
    const f32 scaled = static_cast<f32>(value) / kSnormScale;
    return (scaled < -1.0F) ? -1.0F : scaled;
}

[[nodiscard]] u16 to_unorm16(f32 value) noexcept {
    return static_cast<u16>(std::round(clamp01(value) * kUnorm16Scale));
}

[[nodiscard]] f32 sign_or_positive(f32 value) noexcept {
    return (value < 0.0F) ? -1.0F : 1.0F;
}

}  // namespace

const char* vertex_stream_name(VertexStream stream) noexcept {
    const auto index = static_cast<usize>(stream);
    return (index < kVertexStreamCount) ? kStreamNames[index] : "<invalid>";
}

// --- Octahedral normals ---------------------------------------------------------------------
//
// The octahedral mapping projects the unit sphere onto an octahedron and unfolds it into the
// [-1, 1] square. Two 16-bit components then carry a direction to well under a hundredth of a
// degree, which is finer than a normal map's own precision — so the encoding is not the limiting
// factor anywhere it is used.

void octahedral_encode(Vec3 unit_vector, i16& out_x, i16& out_y) noexcept {
    const f32 magnitude =
        std::fabs(unit_vector.x) + std::fabs(unit_vector.y) + std::fabs(unit_vector.z);
    if (magnitude <= 0.0F) {
        // A zero vector has no direction to encode. +Z is the answer a shading path can survive; a
        // NaN is not.
        out_x = 0;
        out_y = 0;
        return;
    }
    const f32 inv = 1.0F / magnitude;
    f32 x = unit_vector.x * inv;
    f32 y = unit_vector.y * inv;
    if (unit_vector.z < 0.0F) {
        // The lower hemisphere folds outward across the square's diagonals.
        const f32 folded_x = (1.0F - std::fabs(y)) * sign_or_positive(x);
        const f32 folded_y = (1.0F - std::fabs(x)) * sign_or_positive(y);
        x = folded_x;
        y = folded_y;
    }
    out_x = to_snorm16(x);
    out_y = to_snorm16(y);
}

Vec3 octahedral_decode(i16 x, i16 y) noexcept {
    const f32 fx = from_snorm16(x);
    const f32 fy = from_snorm16(y);
    Vec3 vector{fx, fy, 1.0F - std::fabs(fx) - std::fabs(fy)};
    if (vector.z < 0.0F) {
        const f32 unfolded_x = (1.0F - std::fabs(vector.y)) * sign_or_positive(vector.x);
        const f32 unfolded_y = (1.0F - std::fabs(vector.x)) * sign_or_positive(vector.y);
        vector.x = unfolded_x;
        vector.y = unfolded_y;
    }
    // `normalized_or` rather than `normalize`: the latter asserts on a zero length, and while the
    // octahedral inverse cannot produce one from a valid encoding, a decode of arbitrary bytes is
    // exactly what a corrupted vertex buffer looks like and it should not take the process down.
    return normalized_or(vector, Vec3{0.0F, 0.0F, 1.0F});
}

PackedNormalTangent pack_normal_tangent(Vec3 normal, Vec3 tangent, f32 bitangent_sign) noexcept {
    PackedNormalTangent packed;
    octahedral_encode(normal, packed.normal[0], packed.normal[1]);
    octahedral_encode(tangent, packed.tangent[0], packed.tangent[1]);
    // The sign goes in the low bit of tangent.x. Clearing it first means the bit carries the sign
    // and nothing else, so decoding is a test rather than a comparison against a threshold.
    packed.tangent[0] = static_cast<i16>(packed.tangent[0] & ~1);
    if (bitangent_sign < 0.0F) {
        packed.tangent[0] = static_cast<i16>(packed.tangent[0] | 1);
    }
    return packed;
}

Vec3 unpack_normal(const PackedNormalTangent& packed) noexcept {
    return octahedral_decode(packed.normal[0], packed.normal[1]);
}

Vec3 unpack_tangent(const PackedNormalTangent& packed) noexcept {
    return octahedral_decode(static_cast<i16>(packed.tangent[0] & ~1), packed.tangent[1]);
}

f32 unpack_bitangent_sign(const PackedNormalTangent& packed) noexcept {
    return ((packed.tangent[0] & 1) != 0) ? -1.0F : 1.0F;
}

// --- UVs and positions ----------------------------------------------------------------------

void quantise_uv(Vec2 uv, const Rect& bounds, u16& out_u, u16& out_v) noexcept {
    const Vec2 size = bounds.size;
    const f32 u = (size.x > 0.0F) ? ((uv.x - bounds.position.x) / size.x) : 0.0F;
    const f32 v = (size.y > 0.0F) ? ((uv.y - bounds.position.y) / size.y) : 0.0F;
    out_u = to_unorm16(u);
    out_v = to_unorm16(v);
}

Vec2 dequantise_uv(u16 u, u16 v, const Rect& bounds) noexcept {
    return Vec2{bounds.position.x + ((static_cast<f32>(u) / kUnorm16Scale) * bounds.size.x),
                bounds.position.y + ((static_cast<f32>(v) / kUnorm16Scale) * bounds.size.y)};
}

void quantise_position(Vec3 position, const Aabb& bounds, u16 out[3]) noexcept {
    const Vec3 size = bounds.is_empty() ? Vec3{0.0F, 0.0F, 0.0F} : bounds.size();
    const f32 axes[3] = {position.x - bounds.min.x, position.y - bounds.min.y,
                         position.z - bounds.min.z};
    const f32 extents[3] = {size.x, size.y, size.z};
    for (u32 axis = 0; axis < 3; ++axis) {
        out[axis] = to_unorm16((extents[axis] > 0.0F) ? (axes[axis] / extents[axis]) : 0.0F);
    }
}

Vec3 dequantise_position(const u16 quantised[3], const Aabb& bounds) noexcept {
    const Vec3 size = bounds.is_empty() ? Vec3{0.0F, 0.0F, 0.0F} : bounds.size();
    return Vec3{bounds.min.x + ((static_cast<f32>(quantised[0]) / kUnorm16Scale) * size.x),
                bounds.min.y + ((static_cast<f32>(quantised[1]) / kUnorm16Scale) * size.y),
                bounds.min.z + ((static_cast<f32>(quantised[2]) / kUnorm16Scale) * size.z)};
}

f32 position_quantisation_error(const Aabb& bounds) noexcept {
    if (bounds.is_empty()) {
        return 0.0F;
    }
    const Vec3 size = bounds.size();
    const f32 longest = math::max(size.x, math::max(size.y, size.z));
    // Half a step: rounding to nearest, so the worst case is halfway between two representable
    // values. 65535 steps rather than 65536, because both endpoints are representable.
    return (longest * 0.5F) / kUnorm16Scale;
}

bool should_quantise_positions(const Aabb& bounds, f32 tolerance_world_units) noexcept {
    return position_quantisation_error(bounds) <= tolerance_world_units;
}

u32 pack_color_unorm8(f32 r, f32 g, f32 b, f32 a) noexcept {
    const auto channel = [](f32 value) noexcept {
        return static_cast<u32>(std::round(clamp01(value) * 255.0F));
    };
    return channel(r) | (channel(g) << 8U) | (channel(b) << 16U) | (channel(a) << 24U);
}

void unpack_color_unorm8(u32 packed, f32 out[4]) noexcept {
    for (u32 channel = 0; channel < 4; ++channel) {
        out[channel] = static_cast<f32>((packed >> (channel * 8U)) & 0xFFU) / 255.0F;
    }
}

// --- Sizes ----------------------------------------------------------------------------------

u32 vertex_stream_byte_size(VertexStream stream, bool positions_quantised) noexcept {
    switch (stream) {
        case VertexStream::Position:
            // Three 16-bit unorms plus two bytes of padding, or three floats. The pad keeps the
            // stride 4-byte aligned, which every vertex fetch path wants.
            return positions_quantised ? 8U : 12U;
        case VertexStream::NormalTangent:
            return sizeof(PackedNormalTangent);
        case VertexStream::Uv:
            // UV0 and UV1, each two 16-bit unorms.
            return 8U;
        case VertexStream::Color:
            return 4U;
        case VertexStream::Skin:
            // Four 8-bit bone indices and four 8-bit weights. Eight influences double it, which is
            // a per-mesh option the importer records; the four-influence figure is the default.
            return 8U;
        case VertexStream::Count:
            break;
    }
    return 0U;
}

u64 mesh_byte_size(const MeshDescription& desc) noexcept {
    u64 per_vertex = 0;
    for (u32 index = 0; index < kVertexStreamCount; ++index) {
        const auto stream = static_cast<VertexStream>(index);
        if (has_stream(desc.streams, stream)) {
            per_vertex += vertex_stream_byte_size(stream, desc.positions_quantised);
        }
    }
    return (per_vertex * desc.vertex_count) +
           (static_cast<u64>(index_byte_size(desc.index_width)) * desc.index_count);
}

// --- LOD ------------------------------------------------------------------------------------

u32 select_lod(Span<const MeshLod> chain, f32 screen_coverage, f32 bias) noexcept {
    if (chain.empty()) {
        return 0;
    }
    // The bias scales coverage rather than shifting the level index: a level index shift jumps a
    // variable amount of detail depending on how the chain was authored, and a coverage scale means
    // "treat this instance as if it were this much closer", which is what a designer means by it.
    const f32 scale = std::exp2(bias);
    const f32 biased = screen_coverage * ((scale > 0.0F) ? scale : 1.0F);
    for (usize level = 0; level < chain.size(); ++level) {
        if (biased >= chain[level].screen_coverage_threshold) {
            return static_cast<u32>(level);
        }
    }
    return static_cast<u32>(chain.size() - 1);
}

// --- Automatic instancing -------------------------------------------------------------------

Expected<u32, Error> build_instanced_batches(Span<const DrawItem> sorted_draws,
                                             Array<InstancedBatch>& out) noexcept {
    out.clear();
    if (sorted_draws.empty()) {
        return 0U;
    }
    CY_ASSERT_MSG(draws_are_ordered(sorted_draws),
                  "automatic instancing merges adjacent draws, so the list must be sorted; over an "
                  "unsorted list the batches would depend on publication order");

    const auto batch_of = [](const DrawItem& draw) noexcept {
        InstancedBatch batch;
        batch.pipeline = sort_key_pipeline(draw.key);
        batch.material = sort_key_material(draw.key);
        batch.mesh = sort_key_mesh(draw.key);
        batch.surface = draw.surface;
        return batch;
    };
    const auto mergeable = [](const InstancedBatch& a, const InstancedBatch& b) noexcept {
        return a.pipeline == b.pipeline && a.material == b.material && a.mesh == b.mesh &&
               a.surface == b.surface;
    };

    InstancedBatch current = batch_of(sorted_draws[0]);
    current.first_draw = 0;
    current.draw_count = 1;
    for (usize index = 1; index < sorted_draws.size(); ++index) {
        const InstancedBatch candidate = batch_of(sorted_draws[index]);
        if (mergeable(current, candidate)) {
            ++current.draw_count;
            continue;
        }
        if (Status pushed = out.push_back(current); !pushed) {
            return make_unexpected(pushed.error());
        }
        current = candidate;
        current.first_draw = static_cast<u32>(index);
        current.draw_count = 1;
    }
    if (Status pushed = out.push_back(current); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<u32>(out.size());
}

}  // namespace cy::render
