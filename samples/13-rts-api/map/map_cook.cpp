// SPDX-License-Identifier: MIT
// map_cook.cpp — the RTS test map's cook. See map_cook.h.

#include "map_cook.h"

#include <cy/core/assets/cooked.h>
#include <cy/core/assets/package.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/foliage/placement.h>
#include <cy/foliage/species.h>
#include <cy/import/image_codecs.h>
#include <cy/terrain/region.h>
#include <cy/terrain/stack.h>
#include <cy/terrain/surface.h>
#include <cy/world/coordinates.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "map_meshes.h"
#include "map_source.h"

namespace sample::rts::map {
namespace {

using cy::i32;
using cy::Status;
using cy::usize;
namespace terrain = cy::terrain;
namespace foliage = cy::foliage;

/// The terrain identity the region's tiles carry, so they cannot collide with a runtime terrain's.
constexpr u32 kRegionTerrain = 0x13A1;
/// The build id the package's manifest names: the cook's own version, never a time.
constexpr const char* kBuildId = "rts-map-cook/1";
/// Foliage clusters a side: 32 m clusters over the 128 m map.
constexpr f32 kClusterMetres = 32.0F;

[[nodiscard]] f64 elapsed_ms(std::chrono::steady_clock::time_point since) noexcept {
    return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - since).count();
}

[[nodiscard]] Status read_file(const std::string& path, std::vector<u8>& out) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return cy::fail(cy::ErrorCode::NotFound, "the cook cannot open a file it was given");
    }
    // The size first, then one read of exactly that: a file that ends early is a failure.
    bool read = std::fseek(file, 0, SEEK_END) == 0;
    const long size = read ? std::ftell(file) : -1L;
    read = read && size >= 0 && std::fseek(file, 0, SEEK_SET) == 0;
    if (read) {
        out.resize(static_cast<usize>(size));
        read = out.empty() || std::fread(out.data(), 1, out.size(), file) == out.size();
    }
    (void)std::fclose(file);
    return read ? cy::ok()
                : cy::fail(cy::ErrorCode::Internal, "the cook could not read a file it opened");
}

// --- Noise ---------------------------------------------------------------------------------------

[[nodiscard]] u32 hash2(i32 x, i32 z, u32 seed) noexcept {
    u32 h = (static_cast<u32>(x) * 0x8DA6'B343U) ^ (static_cast<u32>(z) * 0xD816'3841U) ^
            (seed * 0xCB1A'B31FU);
    h ^= h >> 13U;
    h *= 0x5bd1'e995U;
    h ^= h >> 15U;
    return h;
}

[[nodiscard]] f32 lattice(i32 x, i32 z, u32 seed) noexcept {
    return static_cast<f32>(hash2(x, z, seed) & 0xFFFFU) / 65535.0F;
}

/// Value noise in [0, 1] with a smoothstep between lattice points, `metres` a feature.
[[nodiscard]] f32 value_noise(f32 x, f32 z, f32 metres, u32 seed) noexcept {
    const f32 u = x / metres;
    const f32 v = z / metres;
    const f32 fu = std::floor(u);
    const f32 fv = std::floor(v);
    const auto i = static_cast<i32>(fu);
    const auto j = static_cast<i32>(fv);
    f32 tu = u - fu;
    f32 tv = v - fv;
    tu = tu * tu * (3.0F - (2.0F * tu));
    tv = tv * tv * (3.0F - (2.0F * tv));
    const f32 a = lattice(i, j, seed);
    const f32 b = lattice(i + 1, j, seed);
    const f32 c = lattice(i, j + 1, seed);
    const f32 d = lattice(i + 1, j + 1, seed);
    return (a + ((b - a) * tu)) + (((c + ((d - c) * tu)) - (a + ((b - a) * tu))) * tv);
}

/// Three octaves of `value_noise`, in [0, 1].
[[nodiscard]] f32 fractal(f32 x, f32 z, f32 metres, u32 seed) noexcept {
    return (value_noise(x, z, metres, seed) * 0.57F) +
           (value_noise(x, z, metres * 0.5F, seed + 1U) * 0.29F) +
           (value_noise(x, z, metres * 0.25F, seed + 2U) * 0.14F);
}

[[nodiscard]] f32 smoothstep(f32 edge0, f32 edge1, f32 value) noexcept {
    if (edge1 <= edge0) {
        return value >= edge1 ? 1.0F : 0.0F;
    }
    const f32 t = std::clamp((value - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - (2.0F * t));
}

// --- Colour --------------------------------------------------------------------------------------

[[nodiscard]] f32 to_linear(u8 encoded) noexcept {
    const f32 c = static_cast<f32>(encoded) / 255.0F;
    return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] u8 to_srgb(f32 linear) noexcept {
    const f32 c = std::clamp(linear, 0.0F, 1.0F);
    const f32 encoded = c <= 0.0031308F ? c * 12.92F : (1.055F * std::pow(c, 1.0F / 2.4F)) - 0.055F;
    return static_cast<u8>(std::lround(std::clamp(encoded, 0.0F, 1.0F) * 255.0F));
}

/// A decoded content texture as linear RGB, for the bake's detail and for the package.
struct Image {
    u32 width = 0;
    u32 height = 0;
    std::vector<Vec3> linear;
    f32 mean_luminance = 1.0F;

    /// Bilinear, wrapped, at `u, v` in repeats.
    [[nodiscard]] Vec3 sample(f32 u, f32 v) const noexcept {
        const f32 x = ((u - std::floor(u)) * static_cast<f32>(width)) - 0.5F;
        const f32 y = ((v - std::floor(v)) * static_cast<f32>(height)) - 0.5F;
        const f32 fx = std::floor(x);
        const f32 fy = std::floor(y);
        const f32 tx = x - fx;
        const f32 ty = y - fy;
        const auto wrap = [](i32 value, u32 extent) {
            const auto n = static_cast<i32>(extent);
            return static_cast<u32>(((value % n) + n) % n);
        };
        const u32 x0 = wrap(static_cast<i32>(fx), width);
        const u32 x1 = wrap(static_cast<i32>(fx) + 1, width);
        const u32 y0 = wrap(static_cast<i32>(fy), height);
        const u32 y1 = wrap(static_cast<i32>(fy) + 1, height);
        const auto at = [&](u32 px, u32 py) {
            return linear[(static_cast<usize>(py) * width) + px];
        };
        const auto mix = [](Vec3 a, Vec3 b, f32 t) {
            return Vec3{a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t), a.z + ((b.z - a.z) * t)};
        };
        return mix(mix(at(x0, y0), at(x1, y0), tx), mix(at(x0, y1), at(x1, y1), tx), ty);
    }
};

[[nodiscard]] f32 luminance(Vec3 c) noexcept {
    return (0.2126F * c.x) + (0.7152F * c.y) + (0.0722F * c.z);
}

[[nodiscard]] Status decode_image(const std::string& path, Image& out, std::string& detail) {
    std::vector<u8> bytes;
    if (Status read = read_file(path, bytes); !read) {
        detail = path;
        return read;
    }
    cy::Expected<cy::import::ImageData, cy::Error> decoded =
        cy::import::decode_png(cy::Span<const u8>(bytes.data(), bytes.size()));
    if (!decoded) {
        detail = path;
        return cy::make_unexpected(decoded.error());
    }
    const cy::import::ImageData& image = *decoded;
    if (image.channels < 3) {
        detail = path;
        return cy::fail(cy::ErrorCode::Unsupported, "a map texture is not an RGB image");
    }
    out.width = image.width;
    out.height = image.height;
    out.linear.resize(static_cast<usize>(image.width) * image.height);
    f64 total = 0.0;
    for (usize pixel = 0; pixel < out.linear.size(); ++pixel) {
        const u8* texel = image.pixels.data() + (pixel * image.channels);
        out.linear[pixel] = Vec3{to_linear(texel[0]), to_linear(texel[1]), to_linear(texel[2])};
        total += static_cast<f64>(luminance(out.linear[pixel]));
    }
    out.mean_luminance =
        std::max(static_cast<f32>(total / static_cast<f64>(out.linear.size())), 1e-3F);
    return cy::ok();
}

/// RGBA8 sRGB with a full box-filtered mip chain, averaged in linear light.
[[nodiscard]] TextureData encode_mips(u32 width, u32 height, const std::vector<Vec3>& linear) {
    TextureData texture;
    texture.width = width;
    texture.height = height;
    texture.srgb = true;
    std::vector<Vec3> level = linear;
    u32 w = width;
    u32 h = height;
    while (true) {
        for (const Vec3& c : level) {
            texture.pixels.push_back(to_srgb(c.x));
            texture.pixels.push_back(to_srgb(c.y));
            texture.pixels.push_back(to_srgb(c.z));
            texture.pixels.push_back(255U);
        }
        texture.mips += 1;
        if (w == 1 && h == 1) {
            break;
        }
        const u32 nw = std::max(w / 2U, 1U);
        const u32 nh = std::max(h / 2U, 1U);
        std::vector<Vec3> next(static_cast<usize>(nw) * nh);
        for (u32 y = 0; y < nh; ++y) {
            for (u32 x = 0; x < nw; ++x) {
                Vec3 sum{};
                for (u32 dy = 0; dy < 2U; ++dy) {
                    for (u32 dx = 0; dx < 2U; ++dx) {
                        const u32 sx = std::min((x * 2U) + dx, w - 1U);
                        const u32 sy = std::min((y * 2U) + dy, h - 1U);
                        const Vec3& c = level[(static_cast<usize>(sy) * w) + sx];
                        sum = Vec3{sum.x + c.x, sum.y + c.y, sum.z + c.z};
                    }
                }
                next[(static_cast<usize>(y) * nw) + x] =
                    Vec3{sum.x * 0.25F, sum.y * 0.25F, sum.z * 0.25F};
            }
        }
        level = std::move(next);
        w = nw;
        h = nh;
    }
    return texture;
}

// --- The terrain ---------------------------------------------------------------------------------

/// What the stack's evaluation left, kept for the bake and the foliage.
struct TerrainCook {
    explicit TerrainCook(cy::Allocator& allocator, const terrain::RegionDescription& region,
                         u64 seed)
        : layout(terrain::region_layout(region)),
          stack(allocator, layout, seed),
          store(allocator, layout),
          heights(store, nullptr),
          query(allocator) {}

    terrain::TileLayout layout;
    terrain::ModifierStack stack;
    terrain::TerrainStore store;
    terrain::HeightfieldSource heights;
    terrain::TerrainQuery query;
};

[[nodiscard]] terrain::TerrainBounds box_around(const std::vector<Vec2>& points, f32 ox,
                                                f32 oz) noexcept {
    terrain::TerrainBounds bounds{1e30, 1e30, -1e30, -1e30};
    for (const Vec2& point : points) {
        bounds.min_x = std::min(bounds.min_x, static_cast<f64>(point.x - ox));
        bounds.min_z = std::min(bounds.min_z, static_cast<f64>(point.y - oz));
        bounds.max_x = std::max(bounds.max_x, static_cast<f64>(point.x - ox));
        bounds.max_z = std::max(bounds.max_z, static_cast<f64>(point.y - oz));
    }
    return bounds;
}

/// One source step as the engine's modifier, in the region's own metres.
[[nodiscard]] Status add_modifier(const MapSource& source, const SourceModifier& step,
                                  terrain::ModifierStack& stack) {
    const f32 ox = source.origin_x;
    const f32 oz = source.origin_z;
    terrain::Modifier modifier;
    modifier.kind = step.kind;
    modifier.name = terrain::modifier_kind_name(step.kind);
    switch (step.kind) {
        case terrain::ModifierKind::Noise:
            modifier.period = step.numbers[0];
            modifier.amplitude = step.numbers[1];
            modifier.iterations = static_cast<u32>(step.numbers[2]);
            modifier.bounds = terrain::TerrainBounds{0.0, 0.0, static_cast<f64>(source.size),
                                                     static_cast<f64>(source.size)};
            break;
        case terrain::ModifierKind::Crater: {
            const f64 cx = static_cast<f64>(step.numbers[0] - ox);
            const f64 cz = static_cast<f64>(step.numbers[1] - oz);
            const auto r = static_cast<f64>(step.numbers[2]);
            modifier.bounds = terrain::TerrainBounds{cx - r, cz - r, cx + r, cz + r};
            modifier.amplitude = step.numbers[3];
            break;
        }
        case terrain::ModifierKind::Flatten:
            modifier.bounds = terrain::TerrainBounds{
                static_cast<f64>(step.numbers[0] - ox), static_cast<f64>(step.numbers[1] - oz),
                static_cast<f64>(step.numbers[2] - ox), static_cast<f64>(step.numbers[3] - oz)};
            modifier.height = step.numbers[4];
            break;
        case terrain::ModifierKind::RoadSpline:
            modifier.period = step.numbers[0];
            modifier.height = 0.0F;
            modifier.layer = step.layer;
            modifier.suppresses_foliage = true;
            modifier.bounds = box_around(step.points, ox, oz);
            modifier.radius = step.numbers[0];
            break;
        case terrain::ModifierKind::Brush:
            modifier.brush = step.brush;
            modifier.name = terrain::brush_op_name(step.brush);
            modifier.radius = step.numbers[0];
            modifier.amplitude = step.numbers[1];
            modifier.falloff = step.numbers[2];
            modifier.iterations = step.brush == terrain::BrushOp::Smooth
                                      ? static_cast<u32>(step.numbers[3])
                                      : terrain::kBrushSmoothPasses;
            modifier.height = step.brush == terrain::BrushOp::Flatten ? step.numbers[3] : 0.0F;
            modifier.layer = step.layer;
            break;
        default:
            return cy::fail(cy::ErrorCode::Unsupported, "a map modifier the cook does not convert");
    }
    cy::Expected<u32, cy::Error> added = stack.add(modifier);
    if (!added) {
        return cy::make_unexpected(added.error());
    }
    if (step.kind == terrain::ModifierKind::RoadSpline) {
        std::vector<terrain::TerrainPoint> points;
        points.reserve(step.points.size());
        for (const Vec2& point : step.points) {
            points.push_back(terrain::TerrainPoint{static_cast<f64>(point.x - ox),
                                                   static_cast<f64>(point.y - oz)});
        }
        return stack.add_spline(
            *added, cy::Span<const terrain::TerrainPoint>(points.data(), points.size()));
    }
    if (step.kind == terrain::ModifierKind::Brush) {
        std::vector<terrain::BrushPoint> dabs;
        dabs.reserve(step.points.size());
        for (const Vec2& point : step.points) {
            dabs.push_back(terrain::BrushPoint{static_cast<f64>(point.x - ox),
                                               static_cast<f64>(point.y - oz), 1.0F});
        }
        // A stroke's dabs a third of a radius apart, as the editor's stroke recorder spaces them:
        // the source lists the corners of a stroke, the brush paints the path between them.
        std::vector<terrain::BrushPoint> path;
        const f64 step_metres = std::max(0.5, static_cast<f64>(modifier.radius) / 3.0);
        for (usize index = 0; index < dabs.size(); ++index) {
            path.push_back(dabs[index]);
            if (index + 1 == dabs.size()) {
                break;
            }
            const f64 dx = dabs[index + 1].x - dabs[index].x;
            const f64 dz = dabs[index + 1].z - dabs[index].z;
            const f64 length = std::sqrt((dx * dx) + (dz * dz));
            const auto steps = static_cast<u32>(length / step_metres);
            for (u32 at = 1; at < steps; ++at) {
                const f64 t = static_cast<f64>(at) / static_cast<f64>(steps);
                path.push_back(
                    terrain::BrushPoint{dabs[index].x + (dx * t), dabs[index].z + (dz * t), 1.0F});
            }
        }
        return stack.add_brush(*added,
                               cy::Span<const terrain::BrushPoint>(path.data(), path.size()));
    }
    return cy::ok();
}

[[nodiscard]] Status cook_terrain(cy::Allocator& allocator, const MapSource& source,
                                  TerrainCook& cooked, MapContent& out, CookReport& report) {
    cooked.stack.set_generator(source.generator);
    for (const SourceModifier& step : source.stack) {
        if (step.kind == terrain::ModifierKind::RoadSpline) {
            if (Status declared = cooked.stack.declare_decoration(step.layer, true); !declared) {
                return declared;
            }
        }
        if (Status added = add_modifier(source, step, cooked.stack); !added) {
            return added;
        }
    }
    report.modifiers = static_cast<u32>(cooked.stack.size());

    terrain::RegionDescription region;
    region.terrain = kRegionTerrain;
    region.tiles = source.tiles;
    region.extent = source.size;
    cy::Expected<terrain::RegionSnapshot, cy::Error> snapshot =
        terrain::evaluate_region(allocator, cooked.stack, region);
    if (!snapshot) {
        return cy::make_unexpected(snapshot.error());
    }
    // The same tiles into a store, for the query foliage places against.
    const auto last = static_cast<i32>(source.tiles) - 1;
    if (Status flattened = cooked.stack.flatten(cooked.store, 0, 0, last, last, 0); !flattened) {
        return flattened;
    }
    if (Status added = cooked.query.add_source(cooked.heights); !added) {
        return added;
    }

    TerrainData& data = out.terrain;
    data.edge = snapshot->edge;
    data.origin_x = source.origin_x;
    data.origin_z = source.origin_z;
    data.spacing = source.size / static_cast<f32>(snapshot->edge - 1U);
    data.height_min = cooked.layout.height_min;
    data.height_max = cooked.layout.height_max;
    data.heights.assign(snapshot->heights.begin(), snapshot->heights.end());
    for (const terrain::MaterialTexel& texel : snapshot->texels.span()) {
        for (u32 slot = 0; slot < terrain::kMaxTexelLayers; ++slot) {
            data.texel_layers.push_back(texel.layer[slot]);
            data.texel_weights.push_back(texel.weight[slot]);
        }
    }
    report.terrain_edge = data.edge;

    // The ground: what `mesh_tile()` emitted, in world metres, its texture coordinate the baked
    // albedo's — the map's square, once.
    MeshLibrary& ground = out.ground;
    ground = MeshLibrary{};
    for (usize index = 0; index < snapshot->positions.size(); ++index) {
        const Vec3 p = snapshot->positions[index];
        const Vec3 world{p.x + source.origin_x, p.y, p.z + source.origin_z};
        ground.positions.push_back(world);
        ground.normals.push_back(snapshot->normals[index]);
        ground.uvs.push_back(Vec2{p.x / source.size, p.z / source.size});
    }
    ground.indices.assign(snapshot->indices.begin(), snapshot->indices.end());
    MeshPart part;
    part.first_index = 0;
    part.index_count = static_cast<u32>(ground.indices.size());
    part.material = out.layout.material("terrain");
    if (part.material == kNoMaterial) {
        return cy::fail(cy::ErrorCode::NotFound, "the map declares no 'terrain' material");
    }
    ground.parts.push_back(part);
    MeshRange mesh;
    mesh.name = "terrain";
    mesh.vertex_count = static_cast<u32>(ground.positions.size());
    mesh.part_count = 1;
    mesh.min = Vec3{source.origin_x, data.height_max, source.origin_z};
    mesh.max = Vec3{source.origin_x + source.size, data.height_min, source.origin_z + source.size};
    for (const Vec3& p : ground.positions) {
        mesh.min.y = std::min(mesh.min.y, p.y);
        mesh.max.y = std::max(mesh.max.y, p.y);
    }
    ground.meshes.push_back(mesh);
    report.ground_triangles = static_cast<u32>(ground.indices.size() / 3U);
    for (const u8 weight : data.texel_weights) {
        report.painted_texels += weight != 0 && weight != 255 ? 1U : 0U;
    }
    return cy::ok();
}

// --- The bake ------------------------------------------------------------------------------------

/// How a layer's detail is made, resolved once.
struct LayerBake {
    Vec3 colour;
    const Image* texture = nullptr;
    bool rows = false;
    f32 metres = 2.0F;
    f32 contrast = 0.3F;
};

/// One layer's colour at a point, with its detail.
[[nodiscard]] Vec3 layer_colour(const LayerBake& layer, u32 index, f32 x, f32 z,
                                u32 seed) noexcept {
    f32 factor = 1.0F;
    if (layer.texture != nullptr) {
        const Vec3 texel = layer.texture->sample(x / layer.metres, z / layer.metres);
        factor =
            1.0F + (layer.contrast * ((luminance(texel) / layer.texture->mean_luminance) - 1.0F));
    } else if (layer.rows) {
        const f32 furrow = std::sin((x / layer.metres) * 6.2831853F);
        factor = 1.0F + (layer.contrast * 0.5F * furrow) +
                 (0.15F * (value_noise(x, z, 0.6F, seed + index) - 0.5F));
    } else {
        factor = 1.0F + (layer.contrast * 2.0F *
                         (fractal(x, z, layer.metres, seed + (index * 17U)) - 0.5F));
    }
    return Vec3{layer.colour.x * factor, layer.colour.y * factor, layer.colour.z * factor};
}

/// The texel weights of quad (qi, qj), spread into one weight a layer.
void quad_weights(const TerrainData& terrain, u32 qi, u32 qj, std::vector<f32>& out) noexcept {
    const u32 quads = terrain.edge - 1U;
    qi = std::min(qi, quads - 1U);
    qj = std::min(qj, quads - 1U);
    const usize texel = ((static_cast<usize>(qj) * quads) + qi) * 4U;
    f32 total = 0.0F;
    for (u32 slot = 0; slot < 4U; ++slot) {
        total += static_cast<f32>(terrain.texel_weights[texel + slot]);
    }
    for (u32 slot = 0; slot < 4U; ++slot) {
        const u8 layer = terrain.texel_layers[texel + slot];
        const f32 weight = static_cast<f32>(terrain.texel_weights[texel + slot]);
        if (layer < out.size() && total > 0.0F) {
            out[layer] += weight / total;
        }
    }
    if (total <= 0.0F) {
        out[0] += 1.0F;
    }
}

[[nodiscard]] Status bake_albedo(const MapSource& source, const std::vector<Image>& images,
                                 u32 edge, MapContent& out) {
    const TerrainData& terrain = out.terrain;
    std::vector<LayerBake> layers;
    for (const SourceLayer& layer : source.layers) {
        LayerBake bake;
        bake.colour = layer.colour;
        bake.metres = std::max(layer.metres, 0.05F);
        bake.contrast = layer.contrast;
        bake.rows = layer.detail == "rows";
        for (usize index = 0; index < source.textures.size(); ++index) {
            if (source.textures[index].name == layer.detail) {
                bake.texture = &images[index];
            }
        }
        if (bake.texture == nullptr && !bake.rows && layer.detail != "noise") {
            return cy::fail(cy::ErrorCode::NotFound,
                            "a layer's detail is not a texture the map declares");
        }
        layers.push_back(bake);
    }
    const auto seed = static_cast<u32>(source.seed);
    const f32 texel_metres = source.size / static_cast<f32>(edge);
    const f32 spacing = terrain.spacing;
    std::vector<Vec3> linear(static_cast<usize>(edge) * edge);
    std::vector<f32> weights(layers.size());
    std::vector<f32> corner(layers.size());
    for (u32 v = 0; v < edge; ++v) {
        for (u32 u = 0; u < edge; ++u) {
            const f32 x = source.origin_x + ((static_cast<f32>(u) + 0.5F) * texel_metres);
            const f32 z = source.origin_z + ((static_cast<f32>(v) + 0.5F) * texel_metres);
            // A warped lookup, so a painted edge reads as a ragged boundary rather than a lattice.
            const f32 wx = x + (1.2F * (value_noise(x, z, 3.0F, seed + 91U) - 0.5F));
            const f32 wz = z + (1.2F * (value_noise(x, z, 3.0F, seed + 92U) - 0.5F));
            const f32 gu = std::clamp(((wx - terrain.origin_x) / spacing) - 0.5F, 0.0F,
                                      static_cast<f32>(terrain.edge - 2U));
            const f32 gv = std::clamp(((wz - terrain.origin_z) / spacing) - 0.5F, 0.0F,
                                      static_cast<f32>(terrain.edge - 2U));
            const auto qi = static_cast<u32>(gu);
            const auto qj = static_cast<u32>(gv);
            const f32 tu = gu - static_cast<f32>(qi);
            const f32 tv = gv - static_cast<f32>(qj);
            std::ranges::fill(weights, 0.0F);
            const f32 bilinear[4] = {(1.0F - tu) * (1.0F - tv), tu * (1.0F - tv), (1.0F - tu) * tv,
                                     tu * tv};
            for (u32 c = 0; c < 4U; ++c) {
                std::ranges::fill(corner, 0.0F);
                quad_weights(terrain, qi + (c & 1U), qj + (c >> 1U), corner);
                for (usize layer = 0; layer < weights.size(); ++layer) {
                    weights[layer] += corner[layer] * bilinear[c];
                }
            }
            // Steep ground is rock and the lake's rim is sand, whatever was painted.
            const f32 h = terrain.height_at(x, z);
            const f32 hx = terrain.height_at(x + spacing, z) - terrain.height_at(x - spacing, z);
            const f32 hz = terrain.height_at(x, z + spacing) - terrain.height_at(x, z - spacing);
            const f32 slope =
                std::atan(std::sqrt((hx * hx) + (hz * hz)) / (2.0F * spacing)) * 57.29578F;
            const f32 steep = smoothstep(source.slope_from, source.slope_full, slope);
            const f32 shore =
                source.shore_metres > 0.0F
                    ? 1.0F - smoothstep(source.water_level + (source.shore_metres * 0.3F),
                                        source.water_level + source.shore_metres, h)
                    : 0.0F;
            for (f32& weight : weights) {
                weight *= (1.0F - steep) * (1.0F - shore);
            }
            weights[source.shore_layer] += shore * (1.0F - steep);
            weights[source.slope_layer] += steep;
            Vec3 colour{};
            for (usize layer = 0; layer < layers.size(); ++layer) {
                if (weights[layer] <= 1e-4F) {
                    continue;
                }
                const Vec3 c = layer_colour(layers[layer], static_cast<u32>(layer), x, z, seed);
                colour = Vec3{colour.x + (c.x * weights[layer]), colour.y + (c.y * weights[layer]),
                              colour.z + (c.z * weights[layer])};
            }
            // The broad variation a field has across tens of metres, and a darker lake bed.
            const f32 broad = 0.9F + (0.2F * value_noise(x, z, 24.0F, seed + 7U));
            const f32 wet = h < source.water_level ? 0.6F : 1.0F;
            colour = Vec3{colour.x * broad * wet, colour.y * broad * wet, colour.z * broad * wet};
            linear[(static_cast<usize>(v) * edge) + u] = colour;
        }
    }
    out.albedo = encode_mips(edge, edge, linear);
    return cy::ok();
}

// --- Foliage -------------------------------------------------------------------------------------

[[nodiscard]] foliage::SpeciesClass species_class(const std::string& klass) noexcept {
    if (klass == "understory") {
        return foliage::SpeciesClass::Understory;
    }
    return klass == "scatter" ? foliage::SpeciesClass::Scatter : foliage::SpeciesClass::Canopy;
}

[[nodiscard]] foliage::RuleInput rule_input(const std::string& input) noexcept {
    if (input == "altitude") {
        return foliage::RuleInput::Altitude;
    }
    return input == "noise" ? foliage::RuleInput::Noise : foliage::RuleInput::Slope;
}

[[nodiscard]] Status cook_foliage(cy::Allocator& allocator, const MapSource& source,
                                  const TerrainCook& cooked, MapContent& out,
                                  std::vector<CookedCluster>& clusters, CookReport& report) {
    foliage::SpeciesLibrary library(allocator);
    std::vector<FoliageSpecies> species;
    for (const SourceSpecies& kind : source.species) {
        foliage::SpeciesDeclaration declaration;
        declaration.name = kind.name.c_str();
        declaration.klass = species_class(kind.klass);
        declaration.scale_min = kind.scale_min;
        declaration.scale_max = kind.scale_max;
        declaration.variations = 4;
        declaration.footprint_metres = kind.footprint;
        if (Status declared = library.declare(declaration); !declared) {
            return declared;
        }
        FoliageSpecies cooked_species;
        cooked_species.name = kind.name;
        cooked_species.mesh = out.props.find(kind.mesh.c_str());
        if (cooked_species.mesh == kNoMesh) {
            return cy::fail(cy::ErrorCode::NotFound, "a species names a mesh the kit has not");
        }
        cooked_species.scale_min = kind.scale_min;
        cooked_species.scale_max = kind.scale_max;
        cooked_species.tint_a = kind.tint_a;
        cooked_species.tint_b = kind.tint_b;
        species.push_back(cooked_species);
    }

    foliage::PlacementRuleSet rules(allocator);
    rules.sun_direction = source.sun_travel;
    for (const SourceRule& rule : source.rules) {
        foliage::PlacementRule placed;
        placed.species = foliage::species_id(rule.species.c_str());
        placed.density_per_hectare = rule.density;
        placed.spacing_metres = rule.spacing;
        placed.priority = rule.priority;
        placed.noise_metres = rule.noise_metres;
        placed.orientation = foliage::OrientationMode::RandomLean;
        placed.max_tilt_degrees = 4.0F;
        for (const SourceTest& test : rule.tests) {
            if (placed.test_count >= foliage::kMaxRuleTests) {
                return cy::fail(cy::ErrorCode::InvalidArgument, "a rule has too many tests");
            }
            foliage::RuleTest& made = placed.tests[placed.test_count++];
            made.input = rule_input(test.input);
            made.outer_min = test.corners[0];
            made.inner_min = test.corners[1];
            made.inner_max = test.corners[2];
            made.outer_max = test.corners[3];
        }
        if (Status pushed = rules.rules.push_back(placed); !pushed) {
            return pushed;
        }
    }
    for (const SourceExclusion& zone : source.exclusions) {
        foliage::ExclusionZone excluded;
        excluded.min_x = static_cast<f64>(zone.min_x - source.origin_x);
        excluded.min_z = static_cast<f64>(zone.min_z - source.origin_z);
        excluded.max_x = static_cast<f64>(zone.max_x - source.origin_x);
        excluded.max_z = static_cast<f64>(zone.max_z - source.origin_z);
        excluded.feather_metres = zone.feather;
        if (Status pushed = rules.exclusions.push_back(excluded); !pushed) {
            return pushed;
        }
    }
    if (const foliage::RuleProblem problem = foliage::validate_rules(rules, library);
        problem != foliage::RuleProblem::None) {
        return cy::fail(cy::ErrorCode::InvalidArgument, foliage::rule_problem_name(problem));
    }

    // NO FIELD IS READ: every rule tests slope, altitude and noise, which the sampler computes from
    // the terrain query. The store exists because the sampler's contract is a store.
    cy::world::PartitionConfig partition;
    partition.partition = kRegionTerrain;
    partition.base_cell_size = 128.0F;
    partition.levels = 1;
    cy::environment::FieldRegistry registry(allocator);
    cy::environment::FieldStore fields(allocator, registry, partition);
    const foliage::PlacementSampler sampler(
        cooked.query, fields, foliage::FieldBindings::standard(), cy::normalize(source.sun_travel));
    foliage::GenerationContext context;
    context.seed = source.seed;
    context.library = &library;
    context.rules = &rules;
    context.sampler = &sampler;
    context.policy.edge_metres = kClusterMetres;
    context.policy.target_instances = 2048;
    context.policy.max_instances = 8192;
    context.apply_ecosystem_state = false;

    const auto regions = static_cast<i32>(std::ceil(source.size / kClusterMetres));
    for (i32 rz = 0; rz < regions; ++rz) {
        for (i32 rx = 0; rx < regions; ++rx) {
            cy::Expected<foliage::FoliagePopulation, cy::Error> population =
                foliage::generate_region(allocator, context, foliage::ClusterCoord{rx, rz, 0});
            if (!population) {
                return cy::make_unexpected(population.error());
            }
            const foliage::PlacementDiagnostic& why = population->diagnostic;
            report.foliage_candidates += why.candidates;
            report.foliage_rejected_weight += why.rejected_weight;
            report.foliage_rejected_spacing += why.rejected_spacing;
            report.foliage_rejected_exclusion += why.rejected_exclusion_zone;
            const foliage::FoliageCluster& cluster = population->cluster;
            if (cluster.size() == 0) {
                continue;
            }
            CookedCluster cooked_cluster;
            const foliage::ClusterBounds& bounds = cluster.bounds();
            cooked_cluster.min_x = bounds.min_x + static_cast<f64>(source.origin_x);
            cooked_cluster.min_z = bounds.min_z + static_cast<f64>(source.origin_z);
            cooked_cluster.max_x = bounds.max_x + static_cast<f64>(source.origin_x);
            cooked_cluster.max_z = bounds.max_z + static_cast<f64>(source.origin_z);
            cooked_cluster.min_y = bounds.min_y;
            cooked_cluster.max_y = bounds.max_y;
            for (u32 slot = 0; slot < foliage::kMaxClusterSpecies; ++slot) {
                const foliage::SpeciesId id = cluster.species_at(static_cast<u8>(slot));
                if (id == foliage::kInvalidSpecies) {
                    break;
                }
                u8 index = 0xFFU;
                for (usize kind = 0; kind < source.species.size(); ++kind) {
                    if (foliage::species_id(source.species[kind].name.c_str()) == id) {
                        index = static_cast<u8>(kind);
                    }
                }
                cooked_cluster.slot_species.push_back(index);
            }
            // A ROAD SUPPRESSES FOLIAGE ALONG ITS WIDTH: the stack's declared decoration, asked of
            // the surface each instance stands on. The placement rules read no road field here, so
            // the declaration is applied to what generation produced.
            for (const foliage::FoliageInstance& instance : cluster.instances()) {
                const cy::world::WorldVec3d at = bounds.decode(instance);
                const terrain::SurfaceSample ground = cooked.query.sample(at.x, at.z);
                if (cooked.stack.suppresses_foliage(ground.layer)) {
                    report.foliage_on_roads += 1;
                    continue;
                }
                const auto* bytes = reinterpret_cast<const u8*>(&instance);
                cooked_cluster.records.insert(cooked_cluster.records.end(), bytes,
                                              bytes + sizeof(instance));
            }
            report.foliage_clusters += 1;
            report.foliage_bytes += cooked_cluster.records.size();
            report.foliage +=
                static_cast<u32>(cooked_cluster.records.size() / sizeof(foliage::FoliageInstance));
            clusters.push_back(std::move(cooked_cluster));
        }
    }
    std::vector<u8> payload;
    encode_foliage(species, clusters, payload);
    return decode_foliage(cy::Span<const u8>(payload.data(), payload.size()), out.foliage);
}

// --- Placements ----------------------------------------------------------------------------------

[[nodiscard]] Status place_all(const MapSource& source, MapContent& out) {
    for (const SourcePlacement& from : source.placements) {
        Placement placement;
        placement.mesh = out.props.find(from.mesh.c_str());
        if (placement.mesh == kNoMesh) {
            return cy::fail(cy::ErrorCode::NotFound, "a placement names a mesh the kit has not");
        }
        placement.kind = from.kind;
        placement.team = from.team;
        const f32 ground = from.kind == PlacementKind::Water
                               ? source.water_level
                               : out.terrain.height_at(from.x, from.z);
        placement.position = Vec3{from.x, ground, from.z};
        placement.yaw = from.yaw_degrees * 0.017453293F;
        placement.scale = Vec3{from.scale, from.scale, from.scale};
        placement.tag = from.tag;
        out.layout.placements.push_back(placement);
    }
    return cy::ok();
}

[[nodiscard]] Vec3 water_origin(const MapSource& source) noexcept {
    for (const SourcePlacement& placement : source.placements) {
        if (placement.kind == PlacementKind::Water) {
            return Vec3{placement.x, source.water_level, placement.z};
        }
    }
    return Vec3{0.0F, source.water_level, 0.0F};
}

// --- The package ---------------------------------------------------------------------------------

struct PackageEntryBytes {
    cy::AssetId id;
    cy::assets::AssetKind kind;
    std::vector<u8> payload;
};

[[nodiscard]] Status write_package(const std::string& path,
                                   const std::vector<PackageEntryBytes>& entries,
                                   cy::Allocator& allocator, u64& bytes) {
    cy::assets::PackageWriter writer;
    cy::assets::PackageManifest manifest;
    if (Status named = manifest.set_build_id(kBuildId); !named) {
        return named;
    }
    if (Status named = manifest.set_bundle("rts-map"); !named) {
        return named;
    }
    if (Status set = writer.set_manifest(manifest); !set) {
        return set;
    }
    // THE LAYOUT DEPENDS ON EVERYTHING ELSE, which is what a loader that asks for it by id
    // would preload; the others depend on nothing.
    std::vector<cy::AssetId> others;
    for (const PackageEntryBytes& entry : entries) {
        if (entry.id != kLayoutAsset) {
            others.push_back(entry.id);
        }
    }
    for (const PackageEntryBytes& entry : entries) {
        cy::Array<u8> cooked(allocator);
        if (Status wrapped = cy::assets::write_cooked_asset(
                entry.kind, cy::assets::VariantKey{},
                cy::Span<const u8>(entry.payload.data(), entry.payload.size()), cooked);
            !wrapped) {
            return wrapped;
        }
        cy::assets::PackageWriter::EntryOptions options;
        options.kind = entry.kind;
        const cy::Span<const cy::AssetId> dependencies =
            entry.id == kLayoutAsset ? cy::Span<const cy::AssetId>(others.data(), others.size())
                                     : cy::Span<const cy::AssetId>();
        if (Status added = writer.add(entry.id, cy::assets::VariantKey{}, cooked.span(), options,
                                      dependencies);
            !added) {
            return added;
        }
        bytes += cooked.size();
    }
    return writer.write(path.c_str());
}

}  // namespace

Status cook_map(const CookRequest& request, MapContent& out, CookReport& report,
                std::string& detail) {
    const auto started = std::chrono::steady_clock::now();
    out = MapContent{};
    report = CookReport{};
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Assets);

    std::vector<u8> text;
    if (Status read = read_file(request.source, text); !read) {
        detail = request.source;
        return read;
    }
    MapSource source;
    u32 line = 0;
    if (Status parsed = parse_map_source(
            std::string_view(reinterpret_cast<const char*>(text.data()), text.size()), source,
            line);
        !parsed) {
        detail = request.source + ":" + std::to_string(line);
        return parsed;
    }
    if (request.albedo_edge < 64 || request.albedo_edge > kMaxTextureEdge ||
        (request.albedo_edge & (request.albedo_edge - 1U)) != 0) {
        return cy::fail(cy::ErrorCode::InvalidArgument,
                        "the albedo's edge must be a power of two from 64 to 4096");
    }

    MapLayout& layout = out.layout;
    layout.name = source.name;
    layout.origin_x = source.origin_x;
    layout.origin_z = source.origin_z;
    layout.size = source.size;
    layout.seed = source.seed;
    layout.water_level = source.water_level;
    layout.sun_travel = source.sun_travel;
    layout.sun_lux = source.sun_lux;
    layout.base_min_x = source.base[0];
    layout.base_min_z = source.base[1];
    layout.base_max_x = source.base[2];
    layout.base_max_z = source.base[3];
    layout.company_x = source.company[0];
    layout.company_z = source.company[1];
    layout.materials = source.materials;
    for (const SourceTexture& texture : source.textures) {
        layout.textures.push_back(texture.name);
    }

    // THE TERRAIN: the stack, evaluated, meshed and stitched by the engine.
    auto phase = std::chrono::steady_clock::now();
    terrain::RegionDescription region;
    region.terrain = kRegionTerrain;
    region.tiles = source.tiles;
    region.extent = source.size;
    if (Status valid = terrain::validate_region(region); !valid) {
        return valid;
    }
    auto cooked = std::make_unique<TerrainCook>(allocator, region, source.seed);
    if (Status made = cook_terrain(allocator, source, *cooked, out, report); !made) {
        return made;
    }
    report.terrain_ms = elapsed_ms(phase);

    // THE CONTENT TEXTURES, decoded once: the bake's detail and the package's own.
    std::vector<Image> images(source.textures.size());
    for (usize index = 0; index < source.textures.size(); ++index) {
        const std::string path = request.content_root + "/" + source.textures[index].path;
        if (Status decoded = decode_image(path, images[index], detail); !decoded) {
            return decoded;
        }
        out.textures.push_back(
            encode_mips(images[index].width, images[index].height, images[index].linear));
    }

    phase = std::chrono::steady_clock::now();
    if (Status baked = bake_albedo(source, images, request.albedo_edge, out); !baked) {
        return baked;
    }
    report.albedo_edge = request.albedo_edge;
    report.bake_ms = elapsed_ms(phase);

    // THE KIT, then what is placed from it: the placements stand on the cooked terrain.
    if (Status built = build_mesh_library(layout, out.terrain, water_origin(source), out.props);
        !built) {
        return built;
    }
    if (Status built = build_worker(layout, out.worker); !built) {
        return built;
    }
    report.meshes = static_cast<u32>(out.props.meshes.size());
    report.worker_vertices = static_cast<u32>(out.worker.positions.size());
    if (Status placed = place_all(source, out); !placed) {
        return placed;
    }
    report.placements = static_cast<u32>(layout.placements.size());

    phase = std::chrono::steady_clock::now();
    std::vector<CookedCluster> clusters;
    if (Status planted = cook_foliage(allocator, source, *cooked, out, clusters, report);
        !planted) {
        return planted;
    }
    report.foliage_ms = elapsed_ms(phase);

    // THE PACKAGE: each asset's payload, in the cooked-asset envelope, under its fixed id.
    std::vector<PackageEntryBytes> entries;
    const auto add = [&entries](cy::AssetId id, cy::assets::AssetKind kind) -> std::vector<u8>& {
        entries.push_back(PackageEntryBytes{id, kind, {}});
        return entries.back().payload;
    };
    encode_layout(layout, add(kLayoutAsset, cy::assets::AssetKind::Binary));
    encode_terrain(out.terrain, add(kTerrainAsset, cy::assets::AssetKind::Terrain));
    encode_meshes(out.ground, add(kGroundAsset, cy::assets::AssetKind::Mesh));
    encode_texture(out.albedo, add(kAlbedoAsset, cy::assets::AssetKind::Texture));
    encode_meshes(out.props, add(kPropsAsset, cy::assets::AssetKind::Mesh));
    encode_skinned(out.worker, add(kWorkerAsset, cy::assets::AssetKind::Mesh));
    std::vector<FoliageSpecies> species = out.foliage.species;
    encode_foliage(species, clusters, add(kFoliageAsset, cy::assets::AssetKind::Binary));
    for (usize index = 0; index < out.textures.size(); ++index) {
        encode_texture(out.textures[index],
                       add(cy::AssetId{kMapNamespace, kFirstContentTexture + index},
                           cy::assets::AssetKind::Texture));
    }
    if (!request.package.empty()) {
        if (Status written =
                write_package(request.package, entries, allocator, report.package_bytes);
            !written) {
            detail = request.package;
            return written;
        }
    }
    report.total_ms = elapsed_ms(started);
    return cy::ok();
}

}  // namespace sample::rts::map
