// SPDX-License-Identifier: MIT
#include <cy/ui/render/encode.h>

#include <algorithm>
#include <cmath>

namespace cy::ui::render {
namespace {

/// The first pixel whose centre is at or past `edge`: the centre of pixel `n` is `n + 0.5`.
[[nodiscard]] i64 first_pixel_at(f32 edge) noexcept {
    return static_cast<i64>(std::ceil(static_cast<f64>(edge) - 0.5));
}

[[nodiscard]] u32 clamp_to(i64 value, u32 limit) noexcept {
    return static_cast<u32>(std::clamp<i64>(value, 0, static_cast<i64>(limit)));
}

[[nodiscard]] f32 saturate(f32 value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

/// cy/ui.slang's `roundedBoxDistance`.
[[nodiscard]] f32 rounded_box_distance(f32 px, f32 py, f32 half_x, f32 half_y,
                                       f32 radius) noexcept {
    const f32 r = std::min({radius, half_x, half_y});
    const f32 qx = std::fabs(px) - half_x + r;
    const f32 qy = std::fabs(py) - half_y + r;
    const f32 mx = std::max(qx, 0.0F);
    const f32 my = std::max(qy, 0.0F);
    return std::sqrt((mx * mx) + (my * my)) + std::min(std::max(qx, qy), 0.0F) - r;
}

/// cy/ui.slang's `coverage`.
[[nodiscard]] f32 coverage(f32 px, f32 py, f32 half_x, f32 half_y, f32 radius) noexcept {
    if (half_x <= 0.0F || half_y <= 0.0F) {
        return 0.0F;
    }
    return saturate(0.5F - rounded_box_distance(px, py, half_x, half_y, radius));
}

/// cy/ui.slang's `unpackColour`: 0xAARRGGBB to (r, g, b, a).
void unpack_colour(u32 colour, f32 (&out)[4]) noexcept {
    out[0] = static_cast<f32>((colour >> 16U) & 0xFFU) / 255.0F;
    out[1] = static_cast<f32>((colour >> 8U) & 0xFFU) / 255.0F;
    out[2] = static_cast<f32>(colour & 0xFFU) / 255.0F;
    out[3] = static_cast<f32>(colour >> 24U) / 255.0F;
}

/// One texel of a page as (r, g, b, a) in [0, 1], clamped to the edge.
void texel(const ReferenceAtlas& atlas, i64 x, i64 y, f32 (&out)[4]) noexcept {
    const auto cx = static_cast<usize>(std::clamp<i64>(x, 0, static_cast<i64>(atlas.width) - 1));
    const auto cy = static_cast<usize>(std::clamp<i64>(y, 0, static_cast<i64>(atlas.height) - 1));
    const usize index = (cy * atlas.width) + cx;
    if (atlas.channels == 1U) {
        out[0] = static_cast<f32>(atlas.texels[index]) / 255.0F;
        out[1] = 0.0F;
        out[2] = 0.0F;
        out[3] = 1.0F;
        return;
    }
    for (usize channel = 0; channel < 4U; ++channel) {
        out[channel] = static_cast<f32>(atlas.texels[(index * 4U) + channel]) / 255.0F;
    }
}

/// `SampleLevel` with a point, clamped sampler.
void sample_point(const ReferenceAtlas& atlas, f32 u, f32 v, f32 (&out)[4]) noexcept {
    texel(atlas, static_cast<i64>(std::floor(u * static_cast<f32>(atlas.width))),
          static_cast<i64>(std::floor(v * static_cast<f32>(atlas.height))), out);
}

/// `SampleLevel` with a linear, clamped sampler: four texels around the sample, by their centres.
void sample_linear(const ReferenceAtlas& atlas, f32 u, f32 v, f32 (&out)[4]) noexcept {
    const f32 x = (u * static_cast<f32>(atlas.width)) - 0.5F;
    const f32 y = (v * static_cast<f32>(atlas.height)) - 0.5F;
    const f32 x0 = std::floor(x);
    const f32 y0 = std::floor(y);
    const f32 fx = x - x0;
    const f32 fy = y - y0;
    f32 corners[4][4] = {};
    const auto ix = static_cast<i64>(x0);
    const auto iy = static_cast<i64>(y0);
    texel(atlas, ix, iy, corners[0]);
    texel(atlas, ix + 1, iy, corners[1]);
    texel(atlas, ix, iy + 1, corners[2]);
    texel(atlas, ix + 1, iy + 1, corners[3]);
    for (usize channel = 0; channel < 4U; ++channel) {
        const f32 top = corners[0][channel] + ((corners[1][channel] - corners[0][channel]) * fx);
        const f32 bottom = corners[2][channel] + ((corners[3][channel] - corners[2][channel]) * fx);
        out[channel] = top + ((bottom - top) * fy);
    }
}

/// The middle one of three: what turns a multi-channel distance field's channels into one distance.
[[nodiscard]] f32 median_of(f32 a, f32 b, f32 c) noexcept {
    return std::max(std::min(a, b), std::min(std::max(a, b), c));
}

/// cy/ui.slang's `glyphField`: the signed distance in pixels, positive inside the outline, from a
/// distance-field page sampled linearly at the primitive's uv.
[[nodiscard]] f32 field_distance(const GpuUiPrimitive& row, f32 x, f32 y,
                                 const ReferenceAtlas& atlas) noexcept {
    const f32 u = row.uv[0] + (saturate((x - row.bounds[0]) / row.bounds[2]) * row.uv[2]);
    const f32 v = row.uv[1] + (saturate((y - row.bounds[1]) / row.bounds[3]) * row.uv[3]);
    f32 sampled[4] = {};
    sample_linear(atlas, u, v, sampled);
    return (median_of(sampled[0], sampled[1], sampled[2]) - 0.5F) * row.shape[2];
}

/// The material's texture term, multiplied into `fill`.
void apply_material(const GpuUiPrimitive& row, f32 x, f32 y, const ReferenceAtlas* atlas,
                    f32 (&fill)[4]) noexcept {
    const bool image = row.material == material_index(BuiltinMaterial::Image);
    const bool glyph = row.material == material_index(BuiltinMaterial::Glyph);
    if ((!image && !glyph) || atlas == nullptr || atlas->texels.empty()) {
        return;
    }
    const f32 u = row.uv[0] + (saturate((x - row.bounds[0]) / row.bounds[2]) * row.uv[2]);
    const f32 v = row.uv[1] + (saturate((y - row.bounds[1]) / row.bounds[3]) * row.uv[3]);
    f32 sampled[4] = {};
    if (glyph) {
        sample_point(*atlas, u, v, sampled);
        for (f32& channel : fill) {
            channel *= sampled[0];
        }
        return;
    }
    sample_linear(*atlas, u, v, sampled);
    for (usize channel = 0; channel < 4U; ++channel) {
        fill[channel] *= sampled[channel];
    }
}

[[nodiscard]] u32 to_byte(f32 value) noexcept {
    return static_cast<u32>(std::lround(saturate(value) * 255.0F));
}

/// `One, OneMinusSourceAlpha` over an RGBA8 texel, red in the low byte, rounded to the nearest
/// step.
[[nodiscard]] u32 blend(u32 destination, const f32 (&source)[4]) noexcept {
    const f32 keep = 1.0F - source[3];
    u32 out = 0;
    for (u32 channel = 0; channel < 4U; ++channel) {
        const f32 below = static_cast<f32>((destination >> (channel * 8U)) & 0xFFU) / 255.0F;
        out |= to_byte(source[channel] + (below * keep)) << (channel * 8U);
    }
    return out;
}

}  // namespace

bool shade_glyph_field(const GpuUiPrimitive& row, f32 x, f32 y, const ReferenceAtlas* atlas,
                       f32 (&out)[4]) noexcept {
    // cy/ui.slang's glyph-field branch: the glyph where the distance is past the edge by half a
    // pixel, the outline out to `border` pixels beyond it, both with a one-pixel ramp; nothing
    // outside the quad, which the box coverage of the primitive's own bounds decides.
    if (atlas == nullptr || atlas->texels.empty()) {
        return false;
    }
    const f32 half_x = row.bounds[2] * 0.5F;
    const f32 half_y = row.bounds[3] * 0.5F;
    const f32 box =
        coverage(x - (row.bounds[0] + half_x), y - (row.bounds[1] + half_y), half_x, half_y, 0.0F);
    const f32 distance = field_distance(row, x, y, *atlas);
    const f32 body = saturate(distance + 0.5F) * box;
    const f32 border = row.shape[1];
    const f32 outline =
        (border > 0.0F ? saturate(distance + border + 0.5F) : saturate(distance + 0.5F)) * box;
    f32 fill[4] = {};
    f32 edge[4] = {};
    unpack_colour(row.colour, fill);
    unpack_colour(row.border_colour, edge);
    bool drawn = false;
    for (usize channel = 0; channel < 4U; ++channel) {
        out[channel] = (fill[channel] * body) + (edge[channel] * (outline - body));
        drawn = drawn || out[channel] != 0.0F;
    }
    return drawn;
}

GpuUiPrimitive encode_primitive(const Primitive& primitive, f32 scale) noexcept {
    GpuUiPrimitive row;
    row.bounds[0] = primitive.bounds.x * scale;
    row.bounds[1] = primitive.bounds.y * scale;
    row.bounds[2] = primitive.bounds.width * scale;
    row.bounds[3] = primitive.bounds.height * scale;
    row.uv[0] = primitive.uv.x;
    row.uv[1] = primitive.uv.y;
    row.uv[2] = primitive.uv.width;
    row.uv[3] = primitive.uv.height;
    row.shape[0] = primitive.corner_radius * scale;
    row.shape[1] = primitive.border_width * scale;
    row.shape[2] = primitive.distance_range * scale;
    row.colour = primitive.colour;
    row.border_colour = primitive.border_colour;
    row.material = primitive.material;
    row.atlas = primitive.atlas;
    return row;
}

Primitive decode_primitive(const GpuUiPrimitive& row, f32 scale) noexcept {
    const f32 inverse = scale > 0.0F ? 1.0F / scale : 0.0F;
    Primitive primitive;
    primitive.bounds = Rect{row.bounds[0] * inverse, row.bounds[1] * inverse,
                            row.bounds[2] * inverse, row.bounds[3] * inverse};
    primitive.uv = Rect{row.uv[0], row.uv[1], row.uv[2], row.uv[3]};
    primitive.corner_radius = row.shape[0] * inverse;
    primitive.border_width = row.shape[1] * inverse;
    primitive.distance_range = row.shape[2] * inverse;
    primitive.colour = row.colour;
    primitive.border_colour = row.border_colour;
    primitive.material = static_cast<u16>(row.material);
    primitive.atlas = static_cast<u16>(row.atlas);
    primitive.clip = 0;
    primitive.transform = 0;
    return primitive;
}

PixelRect scissor_for(const Rect& clip, f32 scale, u32 width, u32 height) noexcept {
    const u32 left = clamp_to(first_pixel_at(clip.x * scale), width);
    const u32 top = clamp_to(first_pixel_at(clip.y * scale), height);
    const u32 right = clamp_to(first_pixel_at(clip.right() * scale), width);
    const u32 bottom = clamp_to(first_pixel_at(clip.bottom() * scale), height);
    PixelRect rect;
    rect.x = left;
    rect.y = top;
    rect.width = right > left ? right - left : 0U;
    rect.height = bottom > top ? bottom - top : 0U;
    return rect;
}

Status build_draws(const PrimitiveBuffer& buffer, f32 scale, u32 width, u32 height,
                   UiDrawList& out) noexcept {
    out.clear();
    const Span<const Primitive> primitives = buffer.primitives();
    if (Status reserved = out.rows.reserve(primitives.size()); !reserved) {
        return reserved;
    }
    for (const Primitive& primitive : primitives) {
        if (Status pushed = out.rows.push_back(encode_primitive(primitive, scale)); !pushed) {
            return pushed;
        }
    }
    const Span<const Rect> clips = buffer.clips();
    for (const Batch& batch : buffer.batches()) {
        if (batch.count == 0 || batch.first + batch.count > primitives.size()) {
            return fail(ErrorCode::InvalidArgument, "ui renderer: a batch is outside the stream");
        }
        const u16 clip = primitives[batch.first].clip;
        if (clip >= clips.size()) {
            return fail(ErrorCode::InvalidArgument, "ui renderer: a primitive names no clip");
        }
        UiDraw draw;
        draw.first = batch.first;
        draw.count = batch.count;
        draw.material = batch.material;
        draw.atlas = batch.atlas;
        draw.scissor = scissor_for(clips[clip], scale, width, height);
        if (draw.scissor.empty()) {
            ++out.clipped_batches;
            continue;
        }
        if (Status pushed = out.draws.push_back(draw); !pushed) {
            return pushed;
        }
    }
    return ok();
}

bool shade_reference(const GpuUiPrimitive& row, u32 px, u32 py, const ReferenceAtlas* atlas,
                     f32 (&out)[4]) noexcept {
    const f32 x = static_cast<f32>(px) + 0.5F;
    const f32 y = static_cast<f32>(py) + 0.5F;
    if (row.material == material_index(BuiltinMaterial::GlyphField)) {
        return shade_glyph_field(row, x, y, atlas, out);
    }
    const f32 half_x = row.bounds[2] * 0.5F;
    const f32 half_y = row.bounds[3] * 0.5F;
    const f32 local_x = x - (row.bounds[0] + half_x);
    const f32 local_y = y - (row.bounds[1] + half_y);
    const f32 radius = row.shape[0];
    const f32 border = row.shape[1];

    const f32 outer = coverage(local_x, local_y, half_x, half_y, radius);
    const f32 inner = border > 0.0F ? coverage(local_x, local_y, half_x - border, half_y - border,
                                               std::max(radius - border, 0.0F))
                                    : outer;
    f32 fill[4] = {};
    unpack_colour(row.colour, fill);
    apply_material(row, x, y, atlas, fill);
    f32 edge[4] = {};
    unpack_colour(row.border_colour, edge);
    bool drawn = false;
    for (usize channel = 0; channel < 4U; ++channel) {
        out[channel] = (fill[channel] * inner) + (edge[channel] * (outer - inner));
        drawn = drawn || out[channel] != 0.0F;
    }
    return drawn;
}

Status draw_reference(const UiDrawList& list, Span<const ReferenceAtlas> atlases, u32 width,
                      u32 height, Span<u32> target) noexcept {
    if (target.size() != static_cast<usize>(width) * height) {
        return fail(ErrorCode::InvalidArgument, "ui reference: the target is not the extent");
    }
    for (const UiDraw& draw : list.draws) {
        const ReferenceAtlas* atlas = draw.atlas < atlases.size() ? &atlases[draw.atlas] : nullptr;
        for (u32 index = draw.first; index < draw.first + draw.count; ++index) {
            const GpuUiPrimitive& row = list.rows[index];
            // The grown quad, clipped to the scissor: the pixels the device rasterises.
            const u32 x0 = clamp_to(first_pixel_at(row.bounds[0] - 1.0F), width);
            const u32 y0 = clamp_to(first_pixel_at(row.bounds[1] - 1.0F), height);
            const u32 x1 = clamp_to(first_pixel_at(row.bounds[0] + row.bounds[2] + 1.0F), width);
            const u32 y1 = clamp_to(first_pixel_at(row.bounds[1] + row.bounds[3] + 1.0F), height);
            for (u32 py = y0; py < y1; ++py) {
                for (u32 px = x0; px < x1; ++px) {
                    f32 shaded[4] = {};
                    if (!draw.scissor.contains(px, py) ||
                        !shade_reference(row, px, py, atlas, shaded)) {
                        continue;
                    }
                    u32& texel_out = target[(static_cast<usize>(py) * width) + px];
                    // The target is RGBA in memory and the shader returns (r, g, b, a).
                    texel_out = blend(texel_out, shaded);
                }
            }
        }
    }
    return ok();
}

}  // namespace cy::ui::render
