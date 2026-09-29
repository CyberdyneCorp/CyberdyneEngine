// SPDX-License-Identifier: MIT
// Tile digests, the source fingerprint and the bake identity. See cy/navigation/tile_identity.h.

#include <cy/navigation/tile_identity.h>

#include <algorithm>
#include <cstring>

namespace cy::navigation {
namespace {

/// 64-bit FNV-1a over values written little-endian, so a digest never depends on the host.
class Digest {
public:
    void u8_value(u8 value) noexcept {
        state_ ^= value;
        state_ *= kPrime;
    }
    void u32_value(u32 value) noexcept {
        for (u32 shift = 0; shift < 32; shift += 8) {
            u8_value(static_cast<u8>((value >> shift) & 0xFFU));
        }
    }
    void u64_value(u64 value) noexcept {
        for (u32 shift = 0; shift < 64; shift += 8) {
            u8_value(static_cast<u8>((value >> shift) & 0xFFU));
        }
    }
    void i32_value(i32 value) noexcept { u32_value(static_cast<u32>(value)); }
    /// By bits, with -0 folded into +0 so equal values digest equal.
    void f32_value(f32 value) noexcept {
        const f32 folded = (value == 0.0F) ? 0.0F : value;
        u32 bits = 0;
        std::memcpy(&bits, &folded, sizeof(bits));
        u32_value(bits);
    }
    void vec3(Vec3 value) noexcept {
        f32_value(value.x);
        f32_value(value.y);
        f32_value(value.z);
    }
    void aabb(const Aabb& value) noexcept {
        vec3(value.min);
        vec3(value.max);
    }
    void coord(TileCoord value) noexcept {
        i32_value(value.x);
        i32_value(value.z);
        i32_value(value.layer);
    }
    void poly(const NavPoly& value) noexcept {
        u32_value(value.first_corner);
        u8_value(value.corner_count);
        u8_value(value.area);
        f32_value(value.cost);
        vec3(value.centre);
    }
    void corners(Span<const u32> values) noexcept {
        u64_value(values.size());
        for (const u32 corner : values) {
            u32_value(corner);
        }
    }
    void settings(const NavBakeSettings& value) noexcept {
        f32_value(value.agent_radius);
        f32_value(value.agent_height);
        f32_value(value.max_slope_degrees);
        f32_value(value.step_height);
        f32_value(value.cell_size);
        f32_value(value.cell_height);
        f32_value(value.tile_size);
        u64_value(value.layers);
        u64_value(value.tags);
        u8_value(static_cast<u8>(value.backend));
    }

    [[nodiscard]] u64 value() const noexcept { return state_; }

private:
    static constexpr u64 kOffset = 0xcbf29ce484222325ULL;
    static constexpr u64 kPrime = 0x100000001b3ULL;
    u64 state_ = kOffset;
};

void source_triangles(Digest& digest, const NavSourceGeometry& geometry) noexcept {
    digest.u64_value(geometry.vertices.size());
    for (const Vec3& vertex : geometry.vertices) {
        digest.vec3(vertex);
    }
    digest.u64_value(geometry.indices.size());
    for (const u32 index : geometry.indices) {
        digest.u32_value(index);
    }
    // The classification `build_tile` reads, defaults filled in, so an omitted array and one of
    // defaults name the same source.
    const usize triangles = geometry.indices.size() / 3;
    for (usize triangle = 0; triangle < triangles; ++triangle) {
        digest.u8_value((triangle < geometry.layer.size()) ? geometry.layer[triangle] : u8{0});
        digest.u8_value((triangle < geometry.tag.size()) ? geometry.tag[triangle] : u8{0});
        digest.u8_value((triangle < geometry.area.size()) ? geometry.area[triangle] : kAreaGround);
    }
}

void source_volumes(Digest& digest, const NavBakeSource& source) noexcept {
    digest.u64_value(source.surfaces.size());
    for (const NavSurfaceVolume& surface : source.surfaces) {
        digest.u64_value(surface.node);
        digest.aabb(surface.bounds);
        digest.u8_value(surface.exclude ? u8{1} : u8{0});
    }
    digest.u64_value(source.areas.size());
    for (const NavAreaVolume& area : source.areas) {
        digest.u64_value(area.node);
        digest.aabb(area.bounds);
        digest.u8_value(area.area);
        digest.f32_value(area.cost);
    }
}

[[nodiscard]] bool coord_before(TileCoord a, TileCoord b) noexcept {
    if (a.layer != b.layer) {
        return a.layer < b.layer;
    }
    if (a.z != b.z) {
        return a.z < b.z;
    }
    return a.x < b.x;
}

}  // namespace

u64 tile_digest(const NavTileData& tile) noexcept {
    Digest digest;
    digest.coord(tile.coord);
    digest.aabb(tile.bounds);
    digest.u64_value(tile.vertices().size());
    for (const Vec3& vertex : tile.vertices()) {
        digest.vec3(vertex);
    }
    digest.u64_value(tile.polys().size());
    for (const NavPoly& poly : tile.polys()) {
        digest.poly(poly);
    }
    digest.corners(tile.corners());
    return digest.value();
}

u64 mesh_tile_digest(const NavMesh& mesh, u32 slot) noexcept {
    const TileCoord coord = mesh.tile_coord(slot);
    if (mesh.tile_slot(coord) != slot) {
        return 0;
    }
    Digest digest;
    digest.coord(coord);
    digest.aabb(mesh.tile_bounds(slot));
    const Span<const Vec3> vertices = mesh.tile_vertices(slot);
    digest.u64_value(vertices.size());
    for (const Vec3& vertex : vertices) {
        digest.vec3(vertex);
    }
    const u32 polys = mesh.tile_poly_count(slot);
    digest.u64_value(polys);
    for (u32 index = 0; index < polys; ++index) {
        digest.poly(*mesh.poly(mesh.tile_poly(slot, index)));
    }
    digest.corners(mesh.tile_corners(slot));
    return digest.value();
}

u64 source_fingerprint(const NavBakeSettings& settings, const NavBakeSource& source,
                       u32 producer_version) noexcept {
    Digest digest;
    source_triangles(digest, source.geometry);
    source_volumes(digest, source);
    digest.settings(settings);
    digest.u32_value(producer_version);
    return digest.value();
}

u64 bake_identity(u64 fingerprint, Span<const u64> ordered_digests) noexcept {
    Digest digest;
    digest.u64_value(fingerprint);
    digest.u64_value(ordered_digests.size());
    for (const u64 tile : ordered_digests) {
        digest.u64_value(tile);
    }
    return digest.value();
}

Status ordered_tile_slots(const NavMesh& mesh, Array<u32>& out) noexcept {
    out.clear();
    for (u32 slot = 0; slot < mesh.tile_capacity(); ++slot) {
        if (mesh.tile_slot(mesh.tile_coord(slot)) == slot) {
            if (Status pushed = out.push_back(slot); !pushed) {
                return pushed;
            }
        }
    }
    std::ranges::sort(out, [&mesh](u32 a, u32 b) noexcept {
        return coord_before(mesh.tile_coord(a), mesh.tile_coord(b));
    });
    return ok();
}

Expected<u64, Error> mesh_bake_identity(u64 fingerprint, const NavMesh& mesh) noexcept {
    Array<u32> slots(mesh.allocator());
    if (Status ordered = ordered_tile_slots(mesh, slots); !ordered) {
        return make_unexpected(ordered.error());
    }
    Array<u64> digests(mesh.allocator());
    for (const u32 slot : slots.span()) {
        if (Status pushed = digests.push_back(mesh_tile_digest(mesh, slot)); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    return bake_identity(fingerprint, digests.span());
}

}  // namespace cy::navigation
