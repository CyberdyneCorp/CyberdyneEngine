// SPDX-License-Identifier: MIT
#include <cy/editor/terrain_service.h>

#include <cy/terrain/stack.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

namespace cy::editor {
namespace {

using terrain::BrushOp;
using terrain::BrushPoint;
using terrain::Modifier;
using terrain::ModifierKind;
using terrain::ModifierStack;
using terrain::TerrainBounds;

/// Bounds-checked little-endian reads over the request.
class Reader {
public:
    explicit Reader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool u8_(u8& out) noexcept {
        if (!take(1)) {
            return false;
        }
        out = bytes_[offset_ - 1];
        return true;
    }
    [[nodiscard]] bool u32_(u32& out) noexcept {
        if (!take(4)) {
            return false;
        }
        out = 0;
        for (usize byte = 0; byte < 4; ++byte) {
            out |= static_cast<u32>(bytes_[offset_ - 4 + byte]) << (byte * 8);
        }
        return true;
    }
    [[nodiscard]] bool u64_(u64& out) noexcept {
        u32 low = 0;
        u32 high = 0;
        if (!u32_(low) || !u32_(high)) {
            return false;
        }
        out = static_cast<u64>(low) | (static_cast<u64>(high) << 32U);
        return true;
    }
    [[nodiscard]] bool f32_(f32& out) noexcept {
        u32 bits = 0;
        if (!u32_(bits)) {
            return false;
        }
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }
    [[nodiscard]] usize offset() const noexcept { return offset_; }
    [[nodiscard]] bool finished() const noexcept { return offset_ == bytes_.size(); }

private:
    [[nodiscard]] bool take(usize count) noexcept {
        if (bytes_.size() - offset_ < count) {
            return false;
        }
        offset_ += count;
        return true;
    }

    Span<const u8> bytes_;
    usize offset_ = 0;
};

Status put_u8(Array<u8>& out, u8 value) noexcept {
    return out.push_back(value);
}

Status put_u32(Array<u8>& out, u32 value) noexcept {
    for (usize byte = 0; byte < 4; ++byte) {
        if (Status pushed = out.push_back(static_cast<u8>((value >> (byte * 8)) & 0xFFU));
            !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status put_u64(Array<u8>& out, u64 value) noexcept {
    if (Status low = put_u32(out, static_cast<u32>(value & 0xFFFFFFFFU)); !low) {
        return low;
    }
    return put_u32(out, static_cast<u32>(value >> 32U));
}

Status put_f32(Array<u8>& out, f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return put_u32(out, bits);
}

[[nodiscard]] Status refuse(const char* message) noexcept {
    return fail(ErrorCode::InvalidArgument, message);
}

[[nodiscard]] bool unit(f32 value) noexcept {
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}

/// FNV-1a over a byte range: a modifier's identity-free content, to notice that it changed.
[[nodiscard]] u64 digest(Span<const u8> bytes, u64 seed) noexcept {
    u64 hash = seed;
    for (const u8 byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

[[nodiscard]] TerrainBounds clip(const TerrainBounds& bounds, f64 extent) noexcept {
    TerrainBounds out = bounds;
    out.min_x = (out.min_x < 0.0) ? 0.0 : out.min_x;
    out.min_z = (out.min_z < 0.0) ? 0.0 : out.min_z;
    out.max_x = (out.max_x > extent) ? extent : out.max_x;
    out.max_z = (out.max_z > extent) ? extent : out.max_z;
    return out;
}

/// What one decoded modifier needs beyond its `Modifier` value.
struct Decoded {
    Modifier modifier;
    u64 identity_low = 0;
    u64 identity_high = 0;
    usize first_byte = 0;
    usize last_byte = 0;
};

[[nodiscard]] Status read_header(Reader& reader, u64& low, u64& high,
                                 terrain::RegionDescription& region, f32& base,
                                 u32& count) noexcept {
    u32 format = 0;
    if (!reader.u32_(format) || format != kTerrainEvaluateFormat) {
        return refuse("terrain.evaluate: the request format is not 1");
    }
    if (!reader.u64_(low) || !reader.u64_(high) || !reader.u32_(region.tiles) ||
        !reader.f32_(region.extent) || !reader.f32_(base) || !reader.u32_(count)) {
        return refuse("terrain.evaluate: the request header is truncated");
    }
    region.terrain = static_cast<u32>(low ^ (low >> 32U) ^ high ^ (high >> 32U));
    if (Status valid = terrain::validate_region(region); !valid) {
        return valid;
    }
    if (!std::isfinite(base) || base < -256.0F || base > 1024.0F) {
        return refuse("terrain.evaluate: the base height is outside [-256, 1024] metres");
    }
    if (count > kMaxTerrainModifiers) {
        return refuse("terrain.evaluate: the stack has more than 4096 modifiers");
    }
    return ok();
}

[[nodiscard]] Status read_modifier(Reader& reader, Decoded& out, u32& dabs) noexcept {
    out.first_byte = reader.offset();
    u8 op = 0;
    u8 enabled = 0;
    u8 layer = 0;
    Modifier& modifier = out.modifier;
    if (!reader.u64_(out.identity_low) || !reader.u64_(out.identity_high) || !reader.u8_(op) ||
        !reader.u8_(enabled) || !reader.u8_(layer) || !reader.f32_(modifier.radius) ||
        !reader.f32_(modifier.amplitude) || !reader.f32_(modifier.falloff) || !reader.u32_(dabs)) {
        return refuse("terrain.evaluate: a modifier is truncated");
    }
    if (op > static_cast<u8>(BrushOp::Hole) || enabled > 1) {
        return refuse("terrain.evaluate: a modifier names an unknown brush or enable state");
    }
    if (!std::isfinite(modifier.radius) || modifier.radius < 0.01F || modifier.radius > 10000.0F ||
        !unit(modifier.amplitude) || !unit(modifier.falloff)) {
        return refuse("terrain.evaluate: a brush radius, strength or falloff is out of range");
    }
    if (dabs == 0 || dabs > kMaxTerrainDabs) {
        return refuse("terrain.evaluate: a stroke has no dabs or too many");
    }
    modifier.kind = ModifierKind::Brush;
    modifier.name = terrain::brush_op_name(static_cast<BrushOp>(op));
    modifier.brush = static_cast<BrushOp>(op);
    modifier.enabled = enabled == 1;
    modifier.layer = layer;
    modifier.iterations = terrain::kBrushSmoothPasses;
    return ok();
}

[[nodiscard]] Status read_dabs(Reader& reader, u32 count, f64 extent,
                               Array<BrushPoint>& out) noexcept {
    out.clear();
    for (u32 index = 0; index < count; ++index) {
        f32 x = 0.0F;
        f32 z = 0.0F;
        f32 pressure = 0.0F;
        if (!reader.f32_(x) || !reader.f32_(z) || !reader.f32_(pressure)) {
            return refuse("terrain.evaluate: a stroke's dabs are truncated");
        }
        if (!unit(x) || !unit(z) || !unit(pressure)) {
            return refuse("terrain.evaluate: a dab lies outside the region or its pressure is");
        }
        if (Status pushed = out.push_back(
                BrushPoint{static_cast<f64>(x) * extent, static_cast<f64>(z) * extent, pressure});
            !pushed) {
            return pushed;
        }
    }
    return ok();
}

[[nodiscard]] Status encode(const terrain::RegionSnapshot& snapshot,
                            const terrain::RegionDescription& region,
                            Span<const TerrainBounds> stale, u64 generation,
                            Array<u8>& out) noexcept {
    const terrain::TileLayout layout = terrain::region_layout(region);
    out.clear();
    Status status = put_u32(out, kTerrainEvaluateFormat);
    status = status ? put_u64(out, generation) : status;
    status = status ? put_u32(out, snapshot.edge) : status;
    status = status ? put_f32(out, region.extent) : status;
    status = status ? put_f32(out, layout.height_min) : status;
    status = status ? put_f32(out, layout.height_max) : status;
    status = status ? put_u32(out, snapshot.rendered_triangles) : status;
    status = status ? put_u32(out, snapshot.rendered_hole_quads) : status;
    status = status ? put_u32(out, snapshot.collision_holes) : status;
    status = status ? put_u32(out, static_cast<u32>(stale.size())) : status;
    for (const TerrainBounds& bounds : stale) {
        status = status ? put_f32(out, static_cast<f32>(bounds.min_x)) : status;
        status = status ? put_f32(out, static_cast<f32>(bounds.min_z)) : status;
        status = status ? put_f32(out, static_cast<f32>(bounds.max_x)) : status;
        status = status ? put_f32(out, static_cast<f32>(bounds.max_z)) : status;
    }
    for (const u16 height : snapshot.heights.span()) {
        status = status ? put_u8(out, static_cast<u8>(height & 0xFFU)) : status;
        status = status ? put_u8(out, static_cast<u8>(height >> 8U)) : status;
    }
    for (const terrain::MaterialTexel& texel : snapshot.texels.span()) {
        status = status ? out.append({texel.layer, terrain::kMaxTexelLayers}) : status;
        status = status ? out.append({texel.weight, terrain::kMaxTexelLayers}) : status;
    }
    status = status ? out.append(snapshot.holes.span()) : status;
    return status;
}

}  // namespace

TerrainPreview::TerrainPreview(Allocator& allocator) noexcept
    : allocator_(&allocator), snapshot_(allocator), navigation_(allocator), seen_(allocator) {}

Status TerrainPreview::mark_changes(Span<const Seen> current) noexcept {
    const f64 extent = static_cast<f64>(region_.extent);
    Array<TerrainBounds> changed(*allocator_);
    const auto differs = [](const Seen& a, Span<const Seen> in) noexcept {
        for (const Seen& b : in) {
            if (a.identity_low == b.identity_low && a.identity_high == b.identity_high) {
                return a.digest != b.digest;
            }
        }
        return true;  // appeared or disappeared
    };
    for (const Seen& now : current) {
        if (differs(now, seen_.span())) {
            if (Status pushed = changed.push_back(clip(now.reach, extent)); !pushed) {
                return pushed;
            }
        }
    }
    for (const Seen& was : seen_.span()) {
        if (differs(was, current)) {
            if (Status pushed = changed.push_back(clip(was.reach, extent)); !pushed) {
                return pushed;
            }
        }
    }
    if (changed.empty()) {
        return ok();
    }
    // Accumulate, and keep the list bounded: past 64 regions the stale set is their union.
    Array<TerrainBounds> stale(*allocator_);
    if (Status copied = stale.append(navigation_.dirty()); !copied) {
        return copied;
    }
    for (const TerrainBounds& bounds : changed.span()) {
        bool known = false;
        for (const TerrainBounds& existing : stale.span()) {
            known = known || (existing.min_x == bounds.min_x && existing.min_z == bounds.min_z &&
                              existing.max_x == bounds.max_x && existing.max_z == bounds.max_z);
        }
        if (!known) {
            if (Status pushed = stale.push_back(bounds); !pushed) {
                return pushed;
            }
        }
    }
    if (stale.size() > 64) {
        TerrainBounds all = stale[0];
        for (const TerrainBounds& bounds : stale.span()) {
            all.min_x = (bounds.min_x < all.min_x) ? bounds.min_x : all.min_x;
            all.min_z = (bounds.min_z < all.min_z) ? bounds.min_z : all.min_z;
            all.max_x = (bounds.max_x > all.max_x) ? bounds.max_x : all.max_x;
            all.max_z = (bounds.max_z > all.max_z) ? bounds.max_z : all.max_z;
        }
        stale.clear();
        if (Status pushed = stale.push_back(all); !pushed) {
            return pushed;
        }
    }
    navigation_.clear_dirty();
    return navigation_.mark_dirty(stale.span());
}

Status TerrainPreview::evaluate(Span<const u8> request, Array<u8>& reply) noexcept {
    Reader reader(request);
    u64 low = 0;
    u64 high = 0;
    terrain::RegionDescription region;
    f32 base = 0.0F;
    u32 count = 0;
    if (Status header = read_header(reader, low, high, region, base, count); !header) {
        return header;
    }

    ModifierStack stack(*allocator_, terrain::region_layout(region), low ^ high);
    terrain::TerrainGenerator generator;
    generator.octaves = 0;
    generator.base_height = base;
    stack.set_generator(generator);

    Array<Seen> current(*allocator_);
    Array<BrushPoint> dabs(*allocator_);
    for (u32 index = 0; index < count; ++index) {
        Decoded decoded;
        u32 dab_count = 0;
        if (Status read = read_modifier(reader, decoded, dab_count); !read) {
            return read;
        }
        if (Status read = read_dabs(reader, dab_count, static_cast<f64>(region.extent), dabs);
            !read) {
            return read;
        }
        decoded.last_byte = reader.offset();
        if (decoded.modifier.brush == BrushOp::Flatten) {
            // The ground under the first dab as the modifiers beneath this one leave it.
            Expected<f32, Error> target =
                terrain::region_height_at(*allocator_, stack, region, dabs[0].x, dabs[0].z);
            if (!target) {
                return make_unexpected(target.error());
            }
            decoded.modifier.height = target.value();
        }
        Expected<u32, Error> added = stack.add(decoded.modifier);
        if (!added) {
            return make_unexpected(added.error());
        }
        if (Status attached = stack.add_brush(added.value(), dabs.span()); !attached) {
            return attached;
        }
        const Span<const u8> content =
            request.subspan(decoded.first_byte, decoded.last_byte - decoded.first_byte);
        // The position and the resolved flatten target are part of what the modifier does, so a
        // move or a change beneath a flatten marks its reach as well.
        const u64 content_digest = digest(content, 0xCBF29CE484222325ULL ^ index) ^
                                   std::bit_cast<u32>(decoded.modifier.height);
        Seen seen{decoded.identity_low, decoded.identity_high, content_digest,
                  terrain::modifier_reach(stack.modifiers()[added.value()])};
        if (Status pushed = current.push_back(seen); !pushed) {
            return pushed;
        }
    }
    if (!reader.finished()) {
        return refuse("terrain.evaluate: bytes follow the last modifier");
    }

    Expected<terrain::RegionSnapshot, Error> snapshot =
        terrain::evaluate_region(*allocator_, stack, region);
    if (!snapshot) {
        return make_unexpected(snapshot.error());
    }

    const bool same_terrain = has_snapshot_ && low == terrain_low_ && high == terrain_high_ &&
                              region.tiles == region_.tiles && region.extent == region_.extent;
    region_ = region;
    if (same_terrain) {
        if (Status marked = mark_changes(current.span()); !marked) {
            return marked;
        }
    } else {
        // A terrain seen for the first time has no baked navigation to be stale against.
        navigation_.clear_dirty();
    }
    seen_ = std::move(current);
    snapshot_ = std::move(snapshot.value());
    terrain_low_ = low;
    terrain_high_ = high;
    has_snapshot_ = true;
    ++generation_;
    return encode(snapshot_, region_, navigation_.dirty(), generation_, reply);
}

}  // namespace cy::editor
