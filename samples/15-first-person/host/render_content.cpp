// SPDX-License-Identifier: MIT
// render_content.cpp — the arena's streams and textures. See render_content.h.

#include "render_content.h"

#include <cy/rendering/pipeline/frame_pipelines.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#if defined(CY_FIRST_PERSON_TEXTURES)
#    include <cy/import/image_codecs.h>
#endif

namespace sample::fps {
namespace {

using cy::f32;
using cy::u32;
using cy::u8;
using cy::usize;
using cy::Vec2;
using cy::Vec3;

/// Metres of a surface one texture spans: the floor's tiles are a metre, four to the image.
[[nodiscard]] f32 tile_metres(Surface surface) noexcept {
    return surface == Surface::Concrete ? 4.0F : 3.0F;
}

/// Twenty-four vertices of the box round `centre`, `half` either side: four a face, so each face
/// has its own normal. `metres` zero gives every face the whole texture; otherwise a face's
/// coordinates are its world position across it, a texture every `metres`.
void append_box(ArenaStreams& out, Vec3 centre, Vec3 half, f32 metres) {
    static constexpr Vec3 kAxes[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                                      Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
    for (const Vec3 normal : kAxes) {
        const Vec3 tangent =
            std::fabs(normal.y) > 0.5F ? Vec3{1.0F, 0.0F, 0.0F} : Vec3{0.0F, 1.0F, 0.0F};
        const Vec3 bitangent = cy::cross(normal, tangent);
        for (u32 corner = 0; corner < 4U; ++corner) {
            const f32 u = (corner == 1U || corner == 2U) ? 1.0F : -1.0F;
            const f32 v = (corner >= 2U) ? 1.0F : -1.0F;
            const Vec3 unit = (normal * 0.5F) + (tangent * (u * 0.5F)) + (bitangent * (v * 0.5F));
            const Vec3 at{centre.x + (unit.x * half.x * 2.0F), centre.y + (unit.y * half.y * 2.0F),
                          centre.z + (unit.z * half.z * 2.0F)};
            out.positions.push_back(at);
            cy::u16 packed[4];
            cy::rendering::pipeline::pack_normal_stream(normal, tangent, packed);
            out.normals.insert(out.normals.end(), packed, packed + 4);
            if (metres > 0.0F) {
                out.uvs.push_back(
                    Vec2{cy::dot(at, tangent) / metres, cy::dot(at, bitangent) / metres});
            } else {
                out.uvs.push_back(Vec2{(u * 0.5F) + 0.5F, (v * 0.5F) + 0.5F});
            }
        }
    }
}

#if defined(CY_FIRST_PERSON_TEXTURES)
[[nodiscard]] f32 to_linear(u8 encoded) noexcept {
    const f32 c = static_cast<f32>(encoded) / 255.0F;
    return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] u8 to_srgb(f32 linear) noexcept {
    const f32 c = std::clamp(linear, 0.0F, 1.0F);
    const f32 encoded = c <= 0.0031308F ? c * 12.92F : (1.055F * std::pow(c, 1.0F / 2.4F)) - 0.055F;
    return static_cast<u8>(std::lround(std::clamp(encoded, 0.0F, 1.0F) * 255.0F));
}

/// RGBA8 sRGB with a full box-filtered mip chain, averaged in linear light — the RTS map cook's.
void encode_mips(u32 width, u32 height, std::vector<Vec3> level, DetailTexture& out) {
    out.width = width;
    out.height = height;
    u32 w = width;
    u32 h = height;
    while (true) {
        for (const Vec3& c : level) {
            out.pixels.push_back(to_srgb(c.x));
            out.pixels.push_back(to_srgb(c.y));
            out.pixels.push_back(to_srgb(c.z));
            out.pixels.push_back(255U);
        }
        out.mips += 1;
        if (w == 1 && h == 1) {
            break;
        }
        const u32 nw = std::max(w / 2U, 1U);
        const u32 nh = std::max(h / 2U, 1U);
        std::vector<Vec3> next(static_cast<usize>(nw) * nh);
        for (u32 y = 0; y < nh; ++y) {
            for (u32 x = 0; x < nw; ++x) {
                Vec3 sum{};
                for (u32 corner = 0; corner < 4U; ++corner) {
                    const u32 sx = std::min((x * 2U) + (corner & 1U), w - 1U);
                    const u32 sy = std::min((y * 2U) + (corner >> 1U), h - 1U);
                    sum = sum + level[(static_cast<usize>(sy) * w) + sx];
                }
                next[(static_cast<usize>(y) * nw) + x] = sum * 0.25F;
            }
        }
        level = std::move(next);
        w = nw;
        h = nh;
    }
}
#endif

}  // namespace

void build_streams(cy::Span<const ArenaBox> boxes, ArenaStreams& out,
                   std::vector<BakedBox>& baked) {
    append_box(out, Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.5F, 0.5F, 0.5F}, 0.0F);
    out.indices.clear();
    for (u32 face = 0; face < 6U; ++face) {
        for (const u32 step : {0U, 1U, 2U, 0U, 2U, 3U}) {
            out.indices.push_back((face * 4U) + step);
        }
    }
    for (const ArenaBox& box : boxes) {
        BakedBox made;
        made.vertex_offset = static_cast<cy::i32>(out.positions.size());
        made.bounds = cy::Aabb::from_min_max(box.centre - box.half, box.centre + box.half);
        made.surface = box.surface;
        append_box(out, box.centre, box.half, tile_metres(box.surface));
        baked.push_back(made);
    }
}

Detail detail_of(Surface surface) noexcept {
    switch (surface) {
        case Surface::Concrete:
            return Detail::Stone;
        case Surface::PaintRed:
        case Surface::PaintTeal:
        case Surface::Plaster:
        case Surface::Wood:
            return Detail::Gravel;
        case Surface::Barrel:
            return Detail::Copper;
        default:
            break;
    }
    return Detail::None;
}

cy::Expected<DetailTexture, cy::Error> load_detail(Detail detail,
                                                   const char* content_root) noexcept {
#if defined(CY_FIRST_PERSON_TEXTURES)
    static constexpr const char* kFiles[] = {"", "stone_albedo.png", "gravel_albedo.png",
                                             "copper_albedo.png"};
    if (detail == Detail::None || detail == Detail::Count) {
        return cy::fail(cy::ErrorCode::InvalidArgument, "a surface without a texture");
    }
    const std::string path = std::string(content_root) + "/content/beauty/textures/" +
                             kFiles[static_cast<usize>(detail)];
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return cy::fail(cy::ErrorCode::NotFound, "an arena texture could not be opened");
    }
    bool read = std::fseek(file, 0, SEEK_END) == 0;
    const long length = read ? std::ftell(file) : -1;
    read = length > 0 && std::fseek(file, 0, SEEK_SET) == 0;
    std::vector<u8> bytes(read ? static_cast<usize>(length) : 0U);
    read = read && std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    (void)std::fclose(file);
    if (!read) {
        return cy::fail(cy::ErrorCode::Io, "an arena texture could not be read");
    }
    auto decoded = cy::import::decode_png(cy::Span<const u8>(bytes.data(), bytes.size()));
    if (!decoded) {
        return cy::make_unexpected(decoded.error());
    }
    const cy::import::ImageData& image = *decoded;
    if (image.channels < 3U) {
        return cy::fail(cy::ErrorCode::Unsupported, "an arena texture is not an RGB image");
    }
    std::vector<Vec3> linear(static_cast<usize>(image.width) * image.height);
    Vec3 total{};
    for (usize pixel = 0; pixel < linear.size(); ++pixel) {
        const u8* texel = image.pixels.data() + (pixel * image.channels);
        linear[pixel] = Vec3{to_linear(texel[0]), to_linear(texel[1]), to_linear(texel[2])};
        total = total + linear[pixel];
    }
    DetailTexture out;
    out.mean = total * (1.0F / static_cast<f32>(linear.size()));
    encode_mips(image.width, image.height, std::move(linear), out);
    return out;
#else
    (void)detail;
    (void)content_root;
    return cy::fail(cy::ErrorCode::Unsupported, "this build has no image importer");
#endif
}

}  // namespace sample::fps
