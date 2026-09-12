// The GPU image, and the sampler that reads it the way a shader would — with its own decode, its
// own blend and its own layer rule. Task 1.1.
//
// EVERY FUNCTION BELOW THAT A SHADER WOULD HAVE TO WRITE IS WRITTEN HERE AND NOWHERE ELSE. Nothing
// in this file calls `decode_value()`, `combine_layers()` or the store. That duplication is
// deliberate and is the whole value of the file: two implementations that agree are evidence, and
// one implementation called twice is not. See gpu.h.

#include <cy/environment/gpu.h>

#include <cmath>
#include <cstring>

namespace cy::environment {
namespace {

[[nodiscard]] u32 bits_of(f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

[[nodiscard]] f32 float_of(u32 bits) noexcept {
    f32 value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

[[nodiscard]] u32 bits_of(i32 value) noexcept {
    return static_cast<u32>(value);
}

[[nodiscard]] i32 int_of(u32 bits) noexcept {
    return static_cast<i32>(bits);
}

[[nodiscard]] i32 floor_div_i32(i32 value, i32 divisor) noexcept {
    const i32 quotient = value / divisor;
    return ((value % divisor != 0) && ((value < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

[[nodiscard]] i32 ifloor32(f32 value) noexcept {
    return static_cast<i32>(std::floor(value));
}

[[nodiscard]] i32 clamp_i32(i32 value, i32 low, i32 high) noexcept {
    return (value < low) ? low : ((value > high) ? high : value);
}

/// The header, as the sampler reads it. A struct of locals rather than a cast over the buffer,
/// because a shader reads words out of a byte-address buffer and cannot cast either.
struct ImageHeader {
    u32 components = 1;
    u32 encoding = 0;
    u32 interpolation = 0;
    u32 rule = 0;
    u32 vertical_cells = 1;
    f32 cell_metres = 1.0F;
    f32 vertical_metres = 1.0F;
    f32 vertical_origin = 0.0F;
    f32 range_min = 0.0F;
    f32 range_max = 1.0F;
    f32 defaults[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    u32 entries = 0;
    i32 origin_tile_x = 0;
    i32 origin_tile_z = 0;
};

[[nodiscard]] ImageHeader read_header(Span<const u32> words) noexcept {
    ImageHeader header;
    const u32 packed = words[2];
    header.components = packed & 0xFFU;
    header.encoding = (packed >> 8U) & 0xFFU;
    header.interpolation = (packed >> 16U) & 0xFFU;
    header.rule = (packed >> 24U) & 0xFFU;
    header.vertical_cells = words[3];
    header.cell_metres = float_of(words[4]);
    header.vertical_metres = float_of(words[5]);
    header.vertical_origin = float_of(words[6]);
    header.range_min = float_of(words[7]);
    header.range_max = float_of(words[8]);
    for (u32 index = 0; index < 4; ++index) {
        header.defaults[index] = float_of(words[9 + index]);
    }
    header.entries = words[13];
    header.origin_tile_x = int_of(words[14]);
    header.origin_tile_z = int_of(words[15]);
    return header;
}

[[nodiscard]] u32 encoding_stride(u32 encoding) noexcept {
    switch (static_cast<FieldEncoding>(encoding)) {
        case FieldEncoding::F32:
            return 4;
        case FieldEncoding::UNorm16:
        case FieldEncoding::Uint16:
            return 2;
        case FieldEncoding::UNorm8:
        case FieldEncoding::Uint8:
            return 1;
    }
    return 4;
}

/// The shader's decode. One stored lattice point, from the image's own bytes, using the image's own
/// encoding and range — never the declaration's.
void decode_point(Span<const u32> words, const ImageHeader& header, u32 byte_offset,
                  f32 (&out)[4]) noexcept {
    const auto* bytes = reinterpret_cast<const u8*>(words.data());
    const u32 stride = encoding_stride(header.encoding);
    const f32 span = header.range_max - header.range_min;
    for (u32 index = 0; index < header.components; ++index) {
        const u32 offset = byte_offset + (index * stride);
        switch (static_cast<FieldEncoding>(header.encoding)) {
            case FieldEncoding::F32: {
                u32 raw = 0;
                std::memcpy(&raw, bytes + offset, sizeof(u32));
                out[index] = float_of(raw);
                break;
            }
            case FieldEncoding::UNorm8: {
                out[index] = header.range_min + ((static_cast<f32>(bytes[offset]) / 255.0F) * span);
                break;
            }
            case FieldEncoding::UNorm16: {
                u16 raw = 0;
                std::memcpy(&raw, bytes + offset, sizeof(u16));
                out[index] = header.range_min + ((static_cast<f32>(raw) / 65535.0F) * span);
                break;
            }
            case FieldEncoding::Uint8: {
                out[index] = static_cast<f32>(bytes[offset]);
                break;
            }
            case FieldEncoding::Uint16: {
                u16 raw = 0;
                std::memcpy(&raw, bytes + offset, sizeof(u16));
                out[index] = static_cast<f32>(raw);
                break;
            }
        }
    }
}

/// The tile table lookup: a binary search over entries sorted by (layer, z, x). What a shader does,
/// and why the builder sorts.
[[nodiscard]] bool find_entry(Span<const u32> words, const ImageHeader& header, u32 layer,
                              i32 tile_x, i32 tile_z, u32& payload_words) noexcept {
    u32 low = 0;
    u32 high = header.entries;
    while (low < high) {
        const u32 middle = low + ((high - low) / 2U);
        const u32 base = kFieldImageHeaderWords + (middle * kFieldImageEntryWords);
        const u32 entry_layer = words[base];
        const i32 entry_x = int_of(words[base + 1]);
        const i32 entry_z = int_of(words[base + 2]);
        const bool before = (entry_layer < layer) || (entry_layer == layer && entry_z < tile_z) ||
                            (entry_layer == layer && entry_z == tile_z && entry_x < tile_x);
        if (before) {
            low = middle + 1;
            continue;
        }
        if (entry_layer == layer && entry_z == tile_z && entry_x == tile_x) {
            payload_words = words[base + 3];
            return true;
        }
        high = middle;
    }
    return false;
}

/// Where a position lands on the image's lattice. The shader's own copy of `lattice_of()` in
/// store.cpp, and split out of the sampler for the same reason that one is: the arithmetic is the
/// half a reader has to check against its CPU counterpart line by line, and it is easier to do that
/// when it is not wrapped around a tile lookup and a blend.
struct ImageLattice {
    i32 i0 = 0;
    i32 k0 = 0;
    i32 j0 = 0;
    f32 fx = 0.0F;
    f32 fz = 0.0F;
    f32 fy = 0.0F;
    bool linear = false;
};

[[nodiscard]] ImageLattice image_lattice(const ImageHeader& header, f32 x, f32 y, f32 z) noexcept {
    ImageLattice lattice;
    lattice.linear =
        static_cast<FieldInterpolation>(header.interpolation) == FieldInterpolation::Linear;
    if (lattice.linear) {
        // Values sit at cell centres, so the continuous coordinate is offset by half a cell — the
        // same half-cell the CPU sampler applies, and the reason a lattice on cell corners would
        // make a tile's own edge values belong to its neighbour.
        const f32 u = (x / header.cell_metres) - 0.5F;
        const f32 w = (z / header.cell_metres) - 0.5F;
        lattice.i0 = ifloor32(u);
        lattice.k0 = ifloor32(w);
        lattice.fx = u - static_cast<f32>(lattice.i0);
        lattice.fz = w - static_cast<f32>(lattice.k0);
    } else {
        lattice.i0 = ifloor32(x / header.cell_metres);
        lattice.k0 = ifloor32(z / header.cell_metres);
    }

    const f32 vertical = (y - header.vertical_origin) / header.vertical_metres;
    if (lattice.linear && header.vertical_cells > 1) {
        const f32 v = vertical - 0.5F;
        lattice.j0 = ifloor32(v);
        lattice.fy = v - static_cast<f32>(lattice.j0);
    } else {
        lattice.j0 = ifloor32(vertical);
    }
    return lattice;
}

/// Which tile answers, and which tile a corner ends up reading from. The whole of the missing-
/// neighbour rule: a corner whose own tile is not in the table is clamped into the tile that
/// answered, so a sample never faults and never blends against a default it happens to sit next to.
struct ImageTile {
    u32 layer = 0;
    i32 x = 0;
    i32 z = 0;
    u32 payload = 0;
};

/// One lattice point, from whichever tile holds it.
void read_image_point(Span<const u32> words, const ImageHeader& header, const ImageTile& centre,
                      i32 gi, i32 gk, i32 gj, f32 (&value)[4]) noexcept {
    const auto cells = static_cast<i32>(kTileCells);
    const i32 j = clamp_i32(gj, 0, static_cast<i32>(header.vertical_cells) - 1);
    i32 tile_x = floor_div_i32(gi, cells);
    i32 tile_z = floor_div_i32(gk, cells);
    u32 payload = centre.payload;
    if ((tile_x != centre.x || tile_z != centre.z) &&
        !find_entry(words, header, centre.layer, tile_x, tile_z, payload)) {
        payload = centre.payload;
        tile_x = centre.x;
        tile_z = centre.z;
    }
    const i32 lx = clamp_i32(gi - (tile_x * cells), 0, cells - 1);
    const i32 lz = clamp_i32(gk - (tile_z * cells), 0, cells - 1);
    const u32 point = (static_cast<u32>(j) * kTileCells * kTileCells) +
                      (static_cast<u32>(lz) * kTileCells) + static_cast<u32>(lx);
    const u32 stride = encoding_stride(header.encoding) * header.components;
    decode_point(words, header, (payload * 4U) + (point * stride), value);
}

/// One layer of the image, sampled. The shader's own copy of `FieldStore::sample_layer()`.
[[nodiscard]] bool sample_image_layer(Span<const u32> words, const ImageHeader& header, u32 layer,
                                      f32 x, f32 y, f32 z, f32 (&out)[4]) noexcept {
    const auto cells = static_cast<i32>(kTileCells);
    ImageTile centre;
    centre.layer = layer;
    centre.x = floor_div_i32(ifloor32(x / header.cell_metres), cells);
    centre.z = floor_div_i32(ifloor32(z / header.cell_metres), cells);
    if (!find_entry(words, header, layer, centre.x, centre.z, centre.payload)) {
        return false;
    }

    const ImageLattice lattice = image_lattice(header, x, y, z);
    if (!lattice.linear) {
        read_image_point(words, header, centre, lattice.i0, lattice.k0, lattice.j0, out);
        return true;
    }

    f32 accumulated[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    const u32 vertical_taps = (header.vertical_cells > 1) ? 2U : 1U;
    for (u32 dy = 0; dy < vertical_taps; ++dy) {
        const f32 wy =
            (vertical_taps == 1U) ? 1.0F : ((dy == 0) ? (1.0F - lattice.fy) : lattice.fy);
        for (u32 dz = 0; dz < 2; ++dz) {
            const f32 wz = (dz == 0) ? (1.0F - lattice.fz) : lattice.fz;
            for (u32 dx = 0; dx < 2; ++dx) {
                const f32 wx = (dx == 0) ? (1.0F - lattice.fx) : lattice.fx;
                f32 corner[4] = {0.0F, 0.0F, 0.0F, 0.0F};
                read_image_point(words, header, centre, lattice.i0 + static_cast<i32>(dx),
                                 lattice.k0 + static_cast<i32>(dz),
                                 lattice.j0 + static_cast<i32>(dy), corner);
                const f32 weight = wx * wz * wy;
                for (u32 index = 0; index < header.components; ++index) {
                    accumulated[index] += corner[index] * weight;
                }
            }
        }
    }
    for (u32 index = 0; index < 4; ++index) {
        out[index] = accumulated[index];
    }
    return true;
}

/// The shader's own layer rule. `combine_layers()` is the CPU's; this one reads the rule byte out
/// of the header, which is what a shader has.
void combine_in_image(u32 rule, const f32 (&base)[4], const f32 (&delta)[4], u32 components,
                      f32 (&out)[4]) noexcept {
    for (u32 index = 0; index < 4; ++index) {
        out[index] = base[index];
    }
    for (u32 index = 0; index < components; ++index) {
        const f32 a = base[index];
        const f32 b = delta[index];
        switch (static_cast<FieldLayerRule>(rule)) {
            case FieldLayerRule::Replace:
                out[index] = b;
                break;
            case FieldLayerRule::Add:
                out[index] = a + b;
                break;
            case FieldLayerRule::Multiply:
                out[index] = a * b;
                break;
            case FieldLayerRule::Max:
                out[index] = (a > b) ? a : b;
                break;
            case FieldLayerRule::Min:
                out[index] = (a < b) ? a : b;
                break;
        }
    }
}

}  // namespace

bool is_field_image(Span<const u32> words) noexcept {
    return words.size() >= kFieldImageHeaderWords && words[0] == kFieldImageMagic &&
           words[1] == kFieldImageVersion;
}

Expected<FieldGpuImage, Error> build_field_image(const FieldStore& store, FieldId field,
                                                 FieldResidency level) noexcept {
    const FieldDeclaration* declaration = store.registry().declaration(field);
    if (declaration == nullptr) {
        return fail(ErrorCode::NotFound, "environment: no such field is declared");
    }
    if (!declaration->levels[static_cast<u32>(level)].declared()) {
        return fail(ErrorCode::InvalidArgument,
                    "environment: this field does not declare that residency level");
    }

    Array<TileAddress> addresses(store.allocator());
    if (Status listed = store.tiles_of(field, level, addresses); !listed) {
        return make_unexpected(listed.error());
    }

    FieldGpuImage image(store.allocator());
    image.field = field;
    image.level = level;
    image.tiles = static_cast<u32>(addresses.size());

    // The origin is the lowest tile in the table; an empty image still needs a defined one, and
    // zero is the only defensible choice when there are no tiles to take it from.
    i32 origin_x = 0;
    i32 origin_z = 0;
    if (!addresses.empty()) {
        origin_x = addresses[0].x;
        origin_z = addresses[0].z;
        for (const TileAddress& address : addresses.span()) {
            origin_x = (address.x < origin_x) ? address.x : origin_x;
            origin_z = (address.z < origin_z) ? address.z : origin_z;
        }
    }
    const f64 tile_metres =
        static_cast<f64>(declaration->levels[static_cast<u32>(level)].cell_metres) *
        static_cast<f64>(kTileCells);
    image.origin_x = static_cast<f64>(origin_x) * tile_metres;
    image.origin_z = static_cast<f64>(origin_z) * tile_metres;

    const u32 payload_bytes = FieldStore::tile_bytes(*declaration);
    const u32 payload_words = (payload_bytes + 3U) / 4U;
    const u32 table_words = static_cast<u32>(addresses.size()) * kFieldImageEntryWords;
    const u32 total =
        kFieldImageHeaderWords + table_words + (static_cast<u32>(addresses.size()) * payload_words);
    if (Status reserved = image.words.reserve(total); !reserved) {
        return make_unexpected(reserved.error());
    }
    for (u32 index = 0; index < total; ++index) {
        if (Status pushed = image.words.push_back(0U); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    u32* words = image.words.data();
    words[0] = kFieldImageMagic;
    words[1] = kFieldImageVersion;
    words[2] = declaration->components() | (static_cast<u32>(declaration->encoding) << 8U) |
               (static_cast<u32>(declaration->interpolation) << 16U) |
               (static_cast<u32>(declaration->layer_rule) << 24U);
    words[3] = declaration->vertical_cells;
    words[4] = bits_of(declaration->levels[static_cast<u32>(level)].cell_metres);
    words[5] = bits_of(declaration->vertical_metres);
    words[6] = bits_of(declaration->vertical_origin_metres);
    words[7] = bits_of(declaration->range_min);
    words[8] = bits_of(declaration->range_max);
    for (u32 index = 0; index < 4; ++index) {
        words[9 + index] = bits_of(declaration->default_value.components[index]);
    }
    words[13] = static_cast<u32>(addresses.size());
    words[14] = bits_of(origin_x);
    words[15] = bits_of(origin_z);

    u32 payload_cursor = kFieldImageHeaderWords + table_words;
    for (usize index = 0; index < addresses.size(); ++index) {
        const TileAddress& address = addresses[index];
        const u32 base = kFieldImageHeaderWords + (static_cast<u32>(index) * kFieldImageEntryWords);
        words[base] = address.layer;
        words[base + 1] = bits_of(address.x - origin_x);
        words[base + 2] = bits_of(address.z - origin_z);
        words[base + 3] = payload_cursor;

        const Span<const u8> data = store.tile_data(address);
        if (data.size() != payload_bytes) {
            return fail(ErrorCode::Internal,
                        "environment: a resident tile's size disagrees with its declaration");
        }
        std::memcpy(reinterpret_cast<u8*>(words + payload_cursor), data.data(), data.size());
        payload_cursor += payload_words;
    }
    return image;
}

FieldImageLocal image_local(const FieldGpuImage& image, const world::WorldVec3d& at) noexcept {
    FieldImageLocal local;
    local.x = static_cast<f32>(at.x - image.origin_x);
    local.y = static_cast<f32>(at.y);
    local.z = static_cast<f32>(at.z - image.origin_z);
    return local;
}

FieldValue sample_field_image(Span<const u32> words, f32 x, f32 y, f32 z) noexcept {
    FieldValue result;
    if (!is_field_image(words)) {
        return result;
    }
    const ImageHeader header = read_header(words);
    for (u32 index = 0; index < 4; ++index) {
        result.components[index] = header.defaults[index];
    }

    f32 base[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    f32 delta[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    const bool has_base =
        sample_image_layer(words, header, static_cast<u32>(FieldLayer::Base), x, y, z, base);
    const bool has_delta =
        sample_image_layer(words, header, static_cast<u32>(FieldLayer::Delta), x, y, z, delta);
    if (!has_base && !has_delta) {
        return result;
    }
    if (!has_base) {
        for (u32 index = 0; index < 4; ++index) {
            base[index] = header.defaults[index];
        }
    }
    if (!has_delta) {
        for (u32 index = 0; index < 4; ++index) {
            result.components[index] = base[index];
        }
        return result;
    }
    f32 combined[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    combine_in_image(header.rule, base, delta, header.components, combined);
    for (u32 index = 0; index < 4; ++index) {
        result.components[index] = combined[index];
    }
    return result;
}

}  // namespace cy::environment
