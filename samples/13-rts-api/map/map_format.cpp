// SPDX-License-Identifier: MIT
// map_format.cpp — the map's payloads. See map_format.h.

#include "map_format.h"

#include <cy/foliage/instance.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace sample::rts::map {
namespace {

static_assert(std::endian::native == std::endian::little,
              "the map's payloads are little-endian and are copied as such");

using cy::f64;
using cy::Status;
using cy::usize;

// --- Writing -------------------------------------------------------------------------------------

class Writer {
public:
    explicit Writer(std::vector<u8>& out) noexcept : out_(&out) {}

    template <class T>
    void put(T value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* bytes = reinterpret_cast<const u8*>(&value);
        out_->insert(out_->end(), bytes, bytes + sizeof(T));
    }
    void vec2(Vec2 value) {
        put(value.x);
        put(value.y);
    }
    void vec3(Vec3 value) {
        put(value.x);
        put(value.y);
        put(value.z);
    }
    void text(const std::string& value) {
        put(static_cast<u32>(value.size()));
        out_->insert(out_->end(), value.begin(), value.end());
    }
    template <class T>
    void array(const std::vector<T>& values) {
        static_assert(std::is_trivially_copyable_v<T>);
        put(static_cast<u32>(values.size()));
        const auto* bytes = reinterpret_cast<const u8*>(values.data());
        out_->insert(out_->end(), bytes, bytes + (values.size() * sizeof(T)));
    }
    void header(u32 magic) {
        put(magic);
        put(kFormatVersion);
    }

private:
    std::vector<u8>* out_;
};

// --- Reading -------------------------------------------------------------------------------------

/// A bounded cursor. Every read checks what is left, and the first failure sticks: a decoder reads
/// on and checks `ok()` once, which keeps it a list of fields rather than a ladder of ifs.
class Reader {
public:
    explicit Reader(cy::Span<const u8> bytes) noexcept : bytes_(bytes) {}

    template <class T>
    T get() noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        T value{};
        if (!take(sizeof(T))) {
            return value;
        }
        std::memcpy(&value, bytes_.data() + at_ - sizeof(T), sizeof(T));
        return value;
    }
    Vec2 vec2() noexcept {
        const f32 x = get<f32>();
        return Vec2{x, get<f32>()};
    }
    Vec3 vec3() noexcept {
        const f32 x = get<f32>();
        const f32 y = get<f32>();
        return Vec3{x, y, get<f32>()};
    }
    std::string text() {
        const u32 length = get<u32>();
        if (length > 256U || !take(length)) {
            fail("a name is longer than the payload");
            return {};
        }
        return {reinterpret_cast<const char*>(bytes_.data() + at_ - length), length};
    }
    template <class T>
    void array(std::vector<T>& out, u32 limit) {
        const u32 count = get<u32>();
        if (count > limit) {
            fail("a count is past its bound");
            return;
        }
        if (!take(static_cast<usize>(count) * sizeof(T))) {
            return;
        }
        out.resize(count);
        std::memcpy(out.data(), bytes_.data() + at_ - (static_cast<usize>(count) * sizeof(T)),
                    static_cast<usize>(count) * sizeof(T));
    }
    /// Read a count and check it against `limit`.
    u32 count(u32 limit) noexcept {
        const u32 value = get<u32>();
        if (value > limit) {
            fail("a count is past its bound");
            return 0;
        }
        return value;
    }
    void header(u32 magic) noexcept {
        if (get<u32>() != magic) {
            fail("the payload is not the kind of map asset it was loaded as");
        } else if (get<u32>() > kFormatVersion) {
            fail("the payload is a newer map format than this build reads");
        }
    }
    void fail(const char* why) noexcept {
        if (error_ == nullptr) {
            error_ = why;
        }
    }
    [[nodiscard]] Status finish() const noexcept {
        if (error_ != nullptr) {
            return cy::fail(cy::ErrorCode::InvalidArgument, error_);
        }
        if (at_ != bytes_.size()) {
            return cy::fail(cy::ErrorCode::InvalidArgument, "the payload has bytes after its end");
        }
        return cy::ok();
    }

private:
    bool take(usize count) noexcept {
        if (error_ != nullptr) {
            return false;
        }
        if (count > bytes_.size() - at_) {
            fail("the payload is truncated");
            return false;
        }
        at_ += count;
        return true;
    }

    cy::Span<const u8> bytes_;
    usize at_ = 0;
    const char* error_ = nullptr;
};

void write_library(Writer& writer, const MeshLibrary& library) {
    writer.array(library.positions);
    writer.array(library.normals);
    writer.array(library.uvs);
    writer.array(library.indices);
    writer.array(library.parts);
    writer.put(static_cast<u32>(library.meshes.size()));
    for (const MeshRange& mesh : library.meshes) {
        writer.text(mesh.name);
        writer.put(mesh.first_vertex);
        writer.put(mesh.vertex_count);
        writer.put(mesh.first_part);
        writer.put(mesh.part_count);
        writer.vec3(mesh.min);
        writer.vec3(mesh.max);
    }
}

/// Every range a mesh or a part names lies inside the arrays it names.
[[nodiscard]] bool library_consistent(const MeshLibrary& library) noexcept {
    if (library.normals.size() != library.positions.size() ||
        library.uvs.size() != library.positions.size()) {
        return false;
    }
    for (const MeshRange& mesh : library.meshes) {
        if (static_cast<u64>(mesh.first_vertex) + mesh.vertex_count > library.positions.size() ||
            static_cast<u64>(mesh.first_part) + mesh.part_count > library.parts.size()) {
            return false;
        }
        for (u32 part = mesh.first_part; part < mesh.first_part + mesh.part_count; ++part) {
            const MeshPart& range = library.parts[part];
            if (static_cast<u64>(range.first_index) + range.index_count > library.indices.size()) {
                return false;
            }
            for (u32 index = range.first_index; index < range.first_index + range.index_count;
                 ++index) {
                if (library.indices[index] >= mesh.vertex_count) {
                    return false;
                }
            }
        }
    }
    return true;
}

[[nodiscard]] f64 dequantise_unit(u16 value) noexcept {
    return static_cast<f64>(value) * (1.0 / 65535.0);
}

/// `foliage::quantise_tilt`'s range: the record's signed byte over +-32 degrees.
constexpr f32 kTiltRadians = 0.5585F;

// --- The digest ----------------------------------------------------------------------------------

/// FNV-1a over bytes, 64 bits. A digest of what was loaded, compared and printed; not a hash table.
class Digest {
public:
    void bytes(const void* data, usize size) noexcept {
        const auto* at = static_cast<const u8*>(data);
        for (usize index = 0; index < size; ++index) {
            value_ = (value_ ^ at[index]) * 0x0000'0100'0000'01B3ULL;
        }
    }
    template <class T>
    void array(const std::vector<T>& values) noexcept {
        const u64 size = values.size();
        bytes(&size, sizeof(size));
        bytes(values.data(), values.size() * sizeof(T));
    }
    template <class T>
    void value(const T& item) noexcept {
        bytes(&item, sizeof(T));
    }
    void text(const std::string& item) noexcept { bytes(item.data(), item.size()); }
    [[nodiscard]] u64 result() const noexcept { return value_; }

private:
    u64 value_ = 0xCBF2'9CE4'8422'2325ULL;
};

void digest_library(Digest& digest, const MeshLibrary& library) noexcept {
    digest.array(library.positions);
    digest.array(library.normals);
    digest.array(library.uvs);
    digest.array(library.indices);
    digest.array(library.parts);
    for (const MeshRange& mesh : library.meshes) {
        digest.text(mesh.name);
        digest.value(mesh.first_vertex);
        digest.value(mesh.vertex_count);
    }
}

}  // namespace

// --- Lookups -------------------------------------------------------------------------------------

u32 MeshLibrary::find(const char* name) const noexcept {
    for (usize index = 0; index < meshes.size(); ++index) {
        if (meshes[index].name == name) {
            return static_cast<u32>(index);
        }
    }
    return kNoMesh;
}

u32 MapLayout::material(const char* wanted) const noexcept {
    for (usize index = 0; index < materials.size(); ++index) {
        if (materials[index].name == wanted) {
            return static_cast<u32>(index);
        }
    }
    return kNoMaterial;
}

f32 TerrainData::sample(u32 i, u32 j) const noexcept {
    const u16 stored = heights[(static_cast<usize>(j) * edge) + i];
    return height_min + (static_cast<f32>(stored) * ((height_max - height_min) / 65535.0F));
}

f32 TerrainData::height_at(f32 x, f32 z) const noexcept {
    if (edge < 2 || heights.size() != static_cast<usize>(edge) * edge) {
        return 0.0F;
    }
    const f32 last = static_cast<f32>(edge - 1);
    const f32 u = std::clamp((x - origin_x) / spacing, 0.0F, last);
    const f32 v = std::clamp((z - origin_z) / spacing, 0.0F, last);
    const auto i = std::min(static_cast<u32>(u), edge - 2);
    const auto j = std::min(static_cast<u32>(v), edge - 2);
    const f32 fu = u - static_cast<f32>(i);
    const f32 fv = v - static_cast<f32>(j);
    const f32 top = sample(i, j) + ((sample(i + 1, j) - sample(i, j)) * fu);
    const f32 bottom = sample(i, j + 1) + ((sample(i + 1, j + 1) - sample(i, j + 1)) * fu);
    return top + ((bottom - top) * fv);
}

u8 TerrainData::layer_at(f32 x, f32 z) const noexcept {
    if (edge < 2) {
        return 0;
    }
    const u32 quads = edge - 1;
    const f32 u = std::clamp((x - origin_x) / spacing, 0.0F, static_cast<f32>(quads) - 0.001F);
    const f32 v = std::clamp((z - origin_z) / spacing, 0.0F, static_cast<f32>(quads) - 0.001F);
    const usize texel = (static_cast<usize>(v) * quads) + static_cast<usize>(u);
    return texel * 4U < texel_layers.size() ? texel_layers[texel * 4U] : 0;
}

// --- Encoders ------------------------------------------------------------------------------------

void encode_layout(const MapLayout& layout, std::vector<u8>& out) {
    Writer writer(out);
    writer.header(kLayoutMagic);
    writer.text(layout.name);
    writer.put(layout.origin_x);
    writer.put(layout.origin_z);
    writer.put(layout.size);
    writer.put(layout.seed);
    writer.put(layout.water_level);
    writer.vec3(layout.sun_travel);
    writer.put(layout.sun_lux);
    writer.put(layout.base_min_x);
    writer.put(layout.base_min_z);
    writer.put(layout.base_max_x);
    writer.put(layout.base_max_z);
    writer.put(layout.company_x);
    writer.put(layout.company_z);
    writer.put(static_cast<u32>(layout.textures.size()));
    for (const std::string& texture : layout.textures) {
        writer.text(texture);
    }
    writer.put(static_cast<u32>(layout.materials.size()));
    for (const MapMaterial& material : layout.materials) {
        writer.text(material.name);
        writer.vec3(material.base_colour);
        writer.put(material.roughness);
        writer.put(material.metallic);
        writer.vec3(material.emission);
        writer.put(material.texture);
        writer.put(static_cast<u8>(material.blend));
    }
    writer.put(static_cast<u32>(layout.placements.size()));
    for (const Placement& placement : layout.placements) {
        writer.put(placement.mesh);
        writer.put(static_cast<u8>(placement.kind));
        writer.put(placement.team);
        writer.vec3(placement.position);
        writer.put(placement.yaw);
        writer.vec3(placement.scale);
        writer.vec3(placement.tint);
        writer.text(placement.tag);
    }
}

void encode_terrain(const TerrainData& terrain, std::vector<u8>& out) {
    Writer writer(out);
    writer.header(kTerrainMagic);
    writer.put(terrain.edge);
    writer.put(terrain.origin_x);
    writer.put(terrain.origin_z);
    writer.put(terrain.spacing);
    writer.put(terrain.height_min);
    writer.put(terrain.height_max);
    writer.array(terrain.heights);
    writer.array(terrain.texel_layers);
    writer.array(terrain.texel_weights);
}

void encode_meshes(const MeshLibrary& library, std::vector<u8>& out) {
    Writer writer(out);
    writer.header(kMeshMagic);
    write_library(writer, library);
}

void encode_skinned(const SkinnedMesh& mesh, std::vector<u8>& out) {
    Writer writer(out);
    writer.header(kSkinMagic);
    writer.put(mesh.joint_count);
    writer.array(mesh.positions);
    writer.array(mesh.normals);
    writer.array(mesh.uvs);
    writer.array(mesh.joints);
    writer.array(mesh.indices);
    writer.array(mesh.parts);
}

void encode_texture(const TextureData& texture, std::vector<u8>& out) {
    Writer writer(out);
    writer.header(kTextureMagic);
    writer.put(texture.width);
    writer.put(texture.height);
    writer.put(texture.mips);
    writer.put(static_cast<u8>(texture.srgb ? 1U : 0U));
    writer.put(static_cast<u32>(texture.pixels.size()));
    out.insert(out.end(), texture.pixels.begin(), texture.pixels.end());
}

void encode_foliage(const std::vector<FoliageSpecies>& species,
                    const std::vector<CookedCluster>& clusters, std::vector<u8>& out) {
    Writer writer(out);
    writer.header(kFoliageMagic);
    writer.put(static_cast<u32>(species.size()));
    for (const FoliageSpecies& kind : species) {
        writer.text(kind.name);
        writer.put(kind.mesh);
        writer.put(kind.scale_min);
        writer.put(kind.scale_max);
        writer.vec3(kind.tint_a);
        writer.vec3(kind.tint_b);
    }
    writer.put(static_cast<u32>(clusters.size()));
    for (const CookedCluster& cluster : clusters) {
        writer.put(cluster.min_x);
        writer.put(cluster.min_z);
        writer.put(cluster.max_x);
        writer.put(cluster.max_z);
        writer.put(cluster.min_y);
        writer.put(cluster.max_y);
        writer.array(cluster.slot_species);
        writer.array(cluster.records);
    }
}

// --- Decoders ------------------------------------------------------------------------------------

Status decode_layout(cy::Span<const u8> bytes, MapLayout& out) {
    Reader reader(bytes);
    reader.header(kLayoutMagic);
    out.name = reader.text();
    out.origin_x = reader.get<f32>();
    out.origin_z = reader.get<f32>();
    out.size = reader.get<f32>();
    out.seed = reader.get<u64>();
    out.water_level = reader.get<f32>();
    out.sun_travel = reader.vec3();
    out.sun_lux = reader.get<f32>();
    out.base_min_x = reader.get<f32>();
    out.base_min_z = reader.get<f32>();
    out.base_max_x = reader.get<f32>();
    out.base_max_z = reader.get<f32>();
    out.company_x = reader.get<f32>();
    out.company_z = reader.get<f32>();
    out.textures.resize(reader.count(kMaxContentTextures));
    for (std::string& texture : out.textures) {
        texture = reader.text();
    }
    out.materials.resize(reader.count(kMaxMaterials));
    for (MapMaterial& material : out.materials) {
        material.name = reader.text();
        material.base_colour = reader.vec3();
        material.roughness = reader.get<f32>();
        material.metallic = reader.get<f32>();
        material.emission = reader.vec3();
        material.texture = reader.get<u32>();
        material.blend = reader.get<u8>() == 0 ? Blend::Opaque : Blend::Transparent;
        if (material.texture != kNoTexture && material.texture != kAlbedoTexture &&
            material.texture >= out.textures.size()) {
            reader.fail("a material names a texture the map does not have");
        }
    }
    out.placements.resize(reader.count(kMaxPlacements));
    for (Placement& placement : out.placements) {
        placement.mesh = reader.get<u32>();
        const u8 kind = reader.get<u8>();
        if (kind > static_cast<u8>(PlacementKind::Water)) {
            reader.fail("a placement's kind is unknown");
        }
        placement.kind = static_cast<PlacementKind>(kind);
        placement.team = reader.get<u8>();
        placement.position = reader.vec3();
        placement.yaw = reader.get<f32>();
        placement.scale = reader.vec3();
        placement.tint = reader.vec3();
        placement.tag = reader.text();
    }
    if (!(out.size > 0.0F)) {
        reader.fail("the map's size is not positive");
    }
    return reader.finish();
}

Status decode_terrain(cy::Span<const u8> bytes, TerrainData& out) {
    Reader reader(bytes);
    reader.header(kTerrainMagic);
    out.edge = reader.get<u32>();
    out.origin_x = reader.get<f32>();
    out.origin_z = reader.get<f32>();
    out.spacing = reader.get<f32>();
    out.height_min = reader.get<f32>();
    out.height_max = reader.get<f32>();
    reader.array(out.heights, kMaxTerrainEdge * kMaxTerrainEdge);
    reader.array(out.texel_layers, kMaxTerrainEdge * kMaxTerrainEdge * 4U);
    reader.array(out.texel_weights, kMaxTerrainEdge * kMaxTerrainEdge * 4U);
    const usize quads = out.edge > 1 ? static_cast<usize>(out.edge - 1) * (out.edge - 1) : 0;
    if (out.edge < 2 || out.edge > kMaxTerrainEdge ||
        out.heights.size() != static_cast<usize>(out.edge) * out.edge ||
        out.texel_layers.size() != quads * 4U || out.texel_weights.size() != quads * 4U) {
        reader.fail("the terrain's arrays disagree with its edge");
    }
    if (!(out.spacing > 0.0F) || !(out.height_max > out.height_min)) {
        reader.fail("the terrain's spacing or height range is empty");
    }
    return reader.finish();
}

Status decode_meshes(cy::Span<const u8> bytes, MeshLibrary& out) {
    Reader reader(bytes);
    reader.header(kMeshMagic);
    reader.array(out.positions, kMaxVertices);
    reader.array(out.normals, kMaxVertices);
    reader.array(out.uvs, kMaxVertices);
    reader.array(out.indices, kMaxIndices);
    reader.array(out.parts, kMaxParts);
    out.meshes.resize(reader.count(kMaxMeshes));
    for (MeshRange& mesh : out.meshes) {
        mesh.name = reader.text();
        mesh.first_vertex = reader.get<u32>();
        mesh.vertex_count = reader.get<u32>();
        mesh.first_part = reader.get<u32>();
        mesh.part_count = reader.get<u32>();
        mesh.min = reader.vec3();
        mesh.max = reader.vec3();
    }
    if (Status read = reader.finish(); !read) {
        return read;
    }
    if (!library_consistent(out)) {
        return cy::fail(cy::ErrorCode::InvalidArgument,
                        "a mesh or a part names a range outside the library");
    }
    return cy::ok();
}

Status decode_skinned(cy::Span<const u8> bytes, SkinnedMesh& out) {
    Reader reader(bytes);
    reader.header(kSkinMagic);
    out.joint_count = reader.get<u32>();
    reader.array(out.positions, kMaxVertices);
    reader.array(out.normals, kMaxVertices);
    reader.array(out.uvs, kMaxVertices);
    reader.array(out.joints, kMaxVertices);
    reader.array(out.indices, kMaxIndices);
    reader.array(out.parts, kMaxParts);
    if (Status read = reader.finish(); !read) {
        return read;
    }
    const usize vertices = out.positions.size();
    bool consistent = out.normals.size() == vertices && out.uvs.size() == vertices &&
                      out.joints.size() == vertices && out.joint_count > 0 &&
                      out.joint_count <= 255U;
    for (const u8 joint : out.joints) {
        consistent = consistent && joint < out.joint_count;
    }
    for (const u32 index : out.indices) {
        consistent = consistent && index < vertices;
    }
    for (const MeshPart& part : out.parts) {
        consistent = consistent &&
                     static_cast<u64>(part.first_index) + part.index_count <= out.indices.size();
    }
    return consistent ? cy::ok()
                      : cy::fail(cy::ErrorCode::InvalidArgument,
                                 "the skinned mesh's streams, joints or parts disagree");
}

Status decode_texture(cy::Span<const u8> bytes, TextureData& out) {
    Reader reader(bytes);
    reader.header(kTextureMagic);
    out.width = reader.get<u32>();
    out.height = reader.get<u32>();
    out.mips = reader.get<u32>();
    out.srgb = reader.get<u8>() != 0;
    if (out.width == 0 || out.height == 0 || out.width > kMaxTextureEdge ||
        out.height > kMaxTextureEdge || out.mips == 0 || out.mips > 13U) {
        reader.fail("the texture's extent or mip count is out of range");
        return reader.finish();
    }
    u64 expected = 0;
    for (u32 level = 0; level < out.mips; ++level) {
        expected += static_cast<u64>(std::max(out.width >> level, 1U)) *
                    std::max(out.height >> level, 1U) * 4U;
    }
    reader.array(out.pixels, static_cast<u32>(expected));
    if (out.pixels.size() != expected) {
        reader.fail("the texture's pixels disagree with its mip chain");
    }
    return reader.finish();
}

Status decode_foliage(cy::Span<const u8> bytes, FoliageData& out) {
    Reader reader(bytes);
    reader.header(kFoliageMagic);
    out.species.resize(reader.count(kMaxSpecies));
    for (FoliageSpecies& kind : out.species) {
        kind.name = reader.text();
        kind.mesh = reader.get<u32>();
        kind.scale_min = reader.get<f32>();
        kind.scale_max = reader.get<f32>();
        kind.tint_a = reader.vec3();
        kind.tint_b = reader.vec3();
    }
    out.clusters = reader.count(1024);
    out.cluster_bytes = 0;
    out.plants.clear();
    for (u32 index = 0; index < out.clusters; ++index) {
        cy::foliage::ClusterBounds bounds;
        bounds.min_x = reader.get<f64>();
        bounds.min_z = reader.get<f64>();
        bounds.max_x = reader.get<f64>();
        bounds.max_z = reader.get<f64>();
        bounds.min_y = reader.get<f32>();
        bounds.max_y = reader.get<f32>();
        std::vector<u8> slots;
        std::vector<u8> records;
        reader.array(slots, cy::foliage::kMaxClusterSpecies);
        reader.array(records, kMaxFoliage * sizeof(cy::foliage::FoliageInstance));
        if (records.size() % sizeof(cy::foliage::FoliageInstance) != 0) {
            reader.fail("a cluster's records are not whole instances");
            break;
        }
        out.cluster_bytes += records.size();
        // THE ENGINE'S OWN DECODE: the record's position is cluster-relative and quantised, and
        // only its cluster's bounds turn it back into metres.
        for (usize at = 0; at < records.size(); at += sizeof(cy::foliage::FoliageInstance)) {
            cy::foliage::FoliageInstance record;
            std::memcpy(&record, records.data() + at, sizeof(record));
            if (record.species_slot >= slots.size() ||
                slots[record.species_slot] >= out.species.size()) {
                reader.fail("a foliage record names a species the map does not have");
                break;
            }
            const FoliageSpecies& kind = out.species[slots[record.species_slot]];
            const cy::world::WorldVec3d position = bounds.decode(record);
            FoliagePlant plant;
            plant.species = slots[record.species_slot];
            plant.position = Vec3{static_cast<f32>(position.x), static_cast<f32>(position.y),
                                  static_cast<f32>(position.z)};
            plant.yaw = (static_cast<f32>(record.yaw) / 65536.0F) * 6.2831853F;
            plant.scale = kind.scale_min + (static_cast<f32>(dequantise_unit(record.scale)) *
                                            (kind.scale_max - kind.scale_min));
            plant.tilt_x = (static_cast<f32>(record.tilt_x) / 127.0F) * kTiltRadians;
            plant.tilt_z = (static_cast<f32>(record.tilt_z) / 127.0F) * kTiltRadians;
            plant.variation = record.variation;
            out.plants.push_back(plant);
        }
    }
    return reader.finish();
}

// --- The digest ----------------------------------------------------------------------------------

u64 content_digest(const MapContent& content) noexcept {
    Digest digest;
    const MapLayout& layout = content.layout;
    digest.text(layout.name);
    digest.value(layout.origin_x);
    digest.value(layout.origin_z);
    digest.value(layout.size);
    digest.value(layout.water_level);
    for (const MapMaterial& material : layout.materials) {
        digest.text(material.name);
        digest.value(material.base_colour);
        digest.value(material.texture);
    }
    for (const Placement& placement : layout.placements) {
        digest.value(placement.mesh);
        digest.value(placement.position);
        digest.value(placement.yaw);
        digest.value(placement.scale);
        digest.text(placement.tag);
    }
    digest.array(content.terrain.heights);
    digest.array(content.terrain.texel_layers);
    digest.array(content.terrain.texel_weights);
    digest_library(digest, content.ground);
    digest_library(digest, content.props);
    digest.array(content.worker.positions);
    digest.array(content.worker.joints);
    digest.array(content.worker.indices);
    digest.array(content.albedo.pixels);
    for (const FoliagePlant& plant : content.foliage.plants) {
        digest.value(plant.species);
        digest.value(plant.position);
        digest.value(plant.yaw);
        digest.value(plant.scale);
    }
    for (const TextureData& texture : content.textures) {
        digest.array(texture.pixels);
    }
    return digest.result();
}

}  // namespace sample::rts::map
