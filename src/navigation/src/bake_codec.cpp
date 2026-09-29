// SPDX-License-Identifier: MIT
// The `.cynavmesh` codec. See cy/navigation/bake_codec.h for the layout.

#include <cy/navigation/bake_codec.h>
#include <cy/navigation/tile_identity.h>

#include <cstring>
#include <utility>

namespace cy::navigation {
namespace {

constexpr u8 kMagic[8] = {'C', 'Y', 'N', 'A', 'V', 'M', 'S', 'H'};
/// Bytes per element, used to refuse a count the remaining payload cannot hold before allocating.
constexpr usize kVertexBytes = 12;
constexpr usize kPolyBytes = 22;
constexpr usize kCornerBytes = 4;

[[nodiscard]] Error refused(ErrorCode code, const char* reason) noexcept {
    return Error{code, reason};
}

class Writer {
public:
    explicit Writer(Array<u8>& out) noexcept : out_(out) {}

    void bytes(const u8* data, usize count) noexcept {
        if (status_) {
            status_ = out_.append(Span<const u8>(data, count));
        }
    }
    void u8_value(u8 value) noexcept { bytes(&value, 1); }
    void u32_value(u32 value) noexcept {
        const u8 encoded[4] = {
            static_cast<u8>(value & 0xFFU), static_cast<u8>((value >> 8) & 0xFFU),
            static_cast<u8>((value >> 16) & 0xFFU), static_cast<u8>((value >> 24) & 0xFFU)};
        bytes(encoded, sizeof(encoded));
    }
    void u64_value(u64 value) noexcept {
        u32_value(static_cast<u32>(value & 0xFFFFFFFFULL));
        u32_value(static_cast<u32>(value >> 32));
    }
    void i32_value(i32 value) noexcept { u32_value(static_cast<u32>(value)); }
    void f32_value(f32 value) noexcept {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u32_value(bits);
    }
    void vec3(Vec3 value) noexcept {
        f32_value(value.x);
        f32_value(value.y);
        f32_value(value.z);
    }

    [[nodiscard]] Status status() const noexcept { return status_; }

private:
    Array<u8>& out_;
    Status status_ = ok();
};

/// Reads little-endian values; once a read runs past the end every later read yields zero and
/// `truncated()` is true, so a decoder checks once per section rather than once per value.
class Reader {
public:
    explicit Reader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool take(u8* out, usize count) noexcept {
        if (truncated_ || bytes_.size() - offset_ < count) {
            truncated_ = true;
            std::memset(out, 0, count);
            return false;
        }
        std::memcpy(out, bytes_.data() + offset_, count);
        offset_ += count;
        return true;
    }
    [[nodiscard]] u8 u8_value() noexcept {
        u8 value = 0;
        (void)take(&value, 1);
        return value;
    }
    [[nodiscard]] u32 u32_value() noexcept {
        u8 encoded[4] = {};
        (void)take(encoded, sizeof(encoded));
        return static_cast<u32>(encoded[0]) | (static_cast<u32>(encoded[1]) << 8) |
               (static_cast<u32>(encoded[2]) << 16) | (static_cast<u32>(encoded[3]) << 24);
    }
    [[nodiscard]] u64 u64_value() noexcept {
        const u64 low = u32_value();
        const u64 high = u32_value();
        return low | (high << 32);
    }
    [[nodiscard]] i32 i32_value() noexcept { return static_cast<i32>(u32_value()); }
    [[nodiscard]] f32 f32_value() noexcept {
        const u32 bits = u32_value();
        f32 value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    [[nodiscard]] Vec3 vec3() noexcept {
        const f32 x = f32_value();
        const f32 y = f32_value();
        const f32 z = f32_value();
        return Vec3{x, y, z};
    }

    /// Whether `count` elements of `size` bytes can still be read.
    [[nodiscard]] bool holds(u32 count, usize size) const noexcept {
        return !truncated_ && usize{count} <= (bytes_.size() - offset_) / size;
    }
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }
    [[nodiscard]] bool finished() const noexcept { return offset_ == bytes_.size(); }

private:
    Span<const u8> bytes_;
    usize offset_ = 0;
    bool truncated_ = false;
};

void write_settings(Writer& writer, const NavBakeSettings& settings) noexcept {
    writer.f32_value(settings.agent_radius);
    writer.f32_value(settings.agent_height);
    writer.f32_value(settings.max_slope_degrees);
    writer.f32_value(settings.step_height);
    writer.f32_value(settings.cell_size);
    writer.f32_value(settings.cell_height);
    writer.f32_value(settings.tile_size);
    writer.u64_value(settings.layers);
    writer.u64_value(settings.tags);
    writer.u8_value(static_cast<u8>(settings.backend));
}

[[nodiscard]] NavBakeSettings read_settings(Reader& reader) noexcept {
    NavBakeSettings settings;
    settings.agent_radius = reader.f32_value();
    settings.agent_height = reader.f32_value();
    settings.max_slope_degrees = reader.f32_value();
    settings.step_height = reader.f32_value();
    settings.cell_size = reader.f32_value();
    settings.cell_height = reader.f32_value();
    settings.tile_size = reader.f32_value();
    settings.layers = reader.u64_value();
    settings.tags = reader.u64_value();
    settings.backend = static_cast<NavBuildBackend>(reader.u8_value());
    return settings;
}

void write_tile(Writer& writer, const NavMesh& mesh, u32 slot) noexcept {
    const TileCoord coord = mesh.tile_coord(slot);
    writer.i32_value(coord.x);
    writer.i32_value(coord.z);
    writer.i32_value(coord.layer);
    const Aabb bounds = mesh.tile_bounds(slot);
    writer.vec3(bounds.min);
    writer.vec3(bounds.max);

    const Span<const Vec3> vertices = mesh.tile_vertices(slot);
    writer.u32_value(static_cast<u32>(vertices.size()));
    for (const Vec3& vertex : vertices) {
        writer.vec3(vertex);
    }
    const u32 polys = mesh.tile_poly_count(slot);
    writer.u32_value(polys);
    for (u32 index = 0; index < polys; ++index) {
        const NavPoly& poly = *mesh.poly(mesh.tile_poly(slot, index));
        writer.u32_value(poly.first_corner);
        writer.u8_value(poly.corner_count);
        writer.u8_value(poly.area);
        writer.f32_value(poly.cost);
        writer.vec3(poly.centre);
    }
    const Span<const u32> corners = mesh.tile_corners(slot);
    writer.u32_value(static_cast<u32>(corners.size()));
    for (const u32 corner : corners) {
        writer.u32_value(corner);
    }
    writer.u64_value(mesh_tile_digest(mesh, slot));
}

[[nodiscard]] Status read_vertices(Reader& reader, NavTileData& tile) noexcept {
    const u32 count = reader.u32_value();
    if (!reader.holds(count, kVertexBytes)) {
        return make_unexpected(
            refused(ErrorCode::OutOfRange, "cynavmesh: truncated in a tile's vertices"));
    }
    if (Status sized = tile.vertices().resize(count); !sized) {
        return sized;
    }
    for (Vec3& vertex : tile.vertices().span()) {
        vertex = reader.vec3();
    }
    return ok();
}

[[nodiscard]] Status read_polys(Reader& reader, NavTileData& tile) noexcept {
    const u32 count = reader.u32_value();
    if (!reader.holds(count, kPolyBytes)) {
        return make_unexpected(
            refused(ErrorCode::OutOfRange, "cynavmesh: truncated in a tile's polygons"));
    }
    if (Status sized = tile.polys().resize(count); !sized) {
        return sized;
    }
    for (NavPoly& poly : tile.polys().span()) {
        poly.first_corner = reader.u32_value();
        poly.corner_count = reader.u8_value();
        poly.area = reader.u8_value();
        poly.cost = reader.f32_value();
        poly.centre = reader.vec3();
    }
    return ok();
}

[[nodiscard]] Status read_corners(Reader& reader, NavTileData& tile) noexcept {
    const u32 count = reader.u32_value();
    if (!reader.holds(count, kCornerBytes)) {
        return make_unexpected(
            refused(ErrorCode::OutOfRange, "cynavmesh: truncated in a tile's corners"));
    }
    if (Status sized = tile.corners().resize(count); !sized) {
        return sized;
    }
    for (u32& corner : tile.corners().span()) {
        corner = reader.u32_value();
    }
    return ok();
}

/// Every polygon names corners the tile has, and every corner a vertex it has, so a blob that
/// carries a matching digest over a malformed tile still cannot make a query read out of bounds.
[[nodiscard]] Status check_structure(const NavTileData& tile) noexcept {
    const usize corners = tile.corners().size();
    for (const NavPoly& poly : tile.polys()) {
        if (poly.corner_count < 3 || poly.corner_count > kMaxPolyVertices ||
            poly.first_corner > corners || corners - poly.first_corner < poly.corner_count) {
            return make_unexpected(refused(ErrorCode::InvalidArgument,
                                           "cynavmesh: a polygon names corners the tile has not"));
        }
    }
    for (const u32 corner : tile.corners()) {
        if (corner >= tile.vertices().size()) {
            return make_unexpected(refused(ErrorCode::InvalidArgument,
                                           "cynavmesh: a corner names a vertex the tile has not"));
        }
    }
    return ok();
}

[[nodiscard]] Status read_tile(Reader& reader, NavBakeAsset& asset) noexcept {
    NavTileData tile(asset.tiles.allocator());
    tile.coord.x = reader.i32_value();
    tile.coord.z = reader.i32_value();
    tile.coord.layer = reader.i32_value();
    const Vec3 low = reader.vec3();
    const Vec3 high = reader.vec3();
    tile.bounds = Aabb::from_min_max(low, high);
    if (Status read = read_vertices(reader, tile); !read) {
        return read;
    }
    if (Status read = read_polys(reader, tile); !read) {
        return read;
    }
    if (Status read = read_corners(reader, tile); !read) {
        return read;
    }
    const u64 stored = reader.u64_value();
    if (reader.truncated()) {
        return make_unexpected(refused(ErrorCode::OutOfRange, "cynavmesh: truncated in a tile"));
    }
    if (Status valid = check_structure(tile); !valid) {
        return valid;
    }
    const u64 derived = tile_digest(tile);
    if (derived != stored) {
        return make_unexpected(refused(ErrorCode::InvalidArgument,
                                       "cynavmesh: a tile's digest does not match its data"));
    }
    if (Status pushed = asset.digests.push_back(derived); !pushed) {
        return pushed;
    }
    return asset.tiles.push_back(std::move(tile));
}

[[nodiscard]] Status read_header(Reader& reader, NavBakeAsset& asset, u32& tiles) noexcept {
    u8 magic[sizeof(kMagic)] = {};
    if (!reader.take(magic, sizeof(magic))) {
        return make_unexpected(refused(ErrorCode::OutOfRange, "cynavmesh: truncated header"));
    }
    if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        return make_unexpected(
            refused(ErrorCode::InvalidArgument, "cynavmesh: not a .cynavmesh asset (bad magic)"));
    }
    const u32 version = reader.u32_value();
    if (!reader.truncated() && version != kNavBakeFormatVersion) {
        return make_unexpected(refused(ErrorCode::Unsupported, "cynavmesh: unknown version"));
    }
    asset.settings = read_settings(reader);
    asset.source_fingerprint = reader.u64_value();
    asset.bake_identity = reader.u64_value();
    tiles = reader.u32_value();
    if (reader.truncated()) {
        return make_unexpected(refused(ErrorCode::OutOfRange, "cynavmesh: truncated header"));
    }
    if (Status valid = validate_bake_settings(asset.settings); !valid) {
        return make_unexpected(
            refused(ErrorCode::InvalidArgument, "cynavmesh: the stored settings are invalid"));
    }
    return ok();
}

}  // namespace

Status encode_nav_bake(const NavBakeSettings& settings, u64 fingerprint, const NavMesh& mesh,
                       Array<u8>& out) noexcept {
    out.clear();
    Array<u32> slots(mesh.allocator());
    if (Status ordered = ordered_tile_slots(mesh, slots); !ordered) {
        return ordered;
    }
    Expected<u64, Error> identity = mesh_bake_identity(fingerprint, mesh);
    if (!identity) {
        return make_unexpected(identity.error());
    }
    Writer writer(out);
    writer.bytes(kMagic, sizeof(kMagic));
    writer.u32_value(kNavBakeFormatVersion);
    write_settings(writer, settings);
    writer.u64_value(fingerprint);
    writer.u64_value(*identity);
    writer.u32_value(static_cast<u32>(slots.size()));
    for (const u32 slot : slots.span()) {
        write_tile(writer, mesh, slot);
    }
    return writer.status();
}

Expected<NavBakeAsset, Error> decode_nav_bake(Allocator& allocator, Span<const u8> bytes) noexcept {
    NavBakeAsset asset(allocator);
    Reader reader(bytes);
    u32 tiles = 0;
    if (Status header = read_header(reader, asset, tiles); !header) {
        return make_unexpected(header.error());
    }
    for (u32 index = 0; index < tiles; ++index) {
        if (Status tile = read_tile(reader, asset); !tile) {
            return make_unexpected(tile.error());
        }
    }
    if (!reader.finished()) {
        return make_unexpected(
            refused(ErrorCode::InvalidArgument, "cynavmesh: trailing bytes after the last tile"));
    }
    if (bake_identity(asset.source_fingerprint, asset.digests.span()) != asset.bake_identity) {
        return make_unexpected(refused(ErrorCode::InvalidArgument,
                                       "cynavmesh: the bake identity does not match its tiles"));
    }
    return asset;
}

Status install_nav_bake(NavBakeAsset&& asset, NavMesh& mesh) noexcept {
    if (mesh.tile_size() != asset.settings.tile_size) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "the mesh's tile size differs from the saved bake's"});
    }
    for (NavTileData& tile : asset.tiles.span()) {
        Expected<TileChange, Error> published = mesh.add_tile(std::move(tile));
        if (!published) {
            return make_unexpected(published.error());
        }
    }
    return ok();
}

}  // namespace cy::navigation
