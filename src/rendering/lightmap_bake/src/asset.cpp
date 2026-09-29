// SPDX-License-Identifier: MIT
// The cooked lightmap. See asset.h.

#include <cy/rendering/lightmap_bake/asset.h>
#include <cy/rendering/lightmap_bake/mips.h>

#include <cstring>

namespace cy::rendering::lightmap_bake {
namespace {

constexpr usize kHeaderWords = 8;

[[nodiscard]] Status put_u32(Array<u8>& out, u32 value) noexcept {
    for (u32 shift = 0; shift < 32U; shift += 8U) {
        if (Status pushed = out.push_back(static_cast<u8>((value >> shift) & 0xFFU)); !pushed) {
            return pushed;
        }
    }
    return ok();
}

[[nodiscard]] u32 get_u32(const u8* bytes) noexcept {
    return u32{bytes[0]} | (u32{bytes[1]} << 8U) | (u32{bytes[2]} << 16U) | (u32{bytes[3]} << 24U);
}

[[nodiscard]] Status put_halves(Array<u8>& out, Span<const Vec4> texels) noexcept {
    for (const Vec4& texel : texels) {
        const f32 channels[4] = {texel.x, texel.y, texel.z, texel.w};
        for (const f32 channel : channels) {
            const u16 half = half_from_float(channel);
            if (Status pushed = out.push_back(static_cast<u8>(half & 0xFFU)); !pushed) {
                return pushed;
            }
            if (Status pushed = out.push_back(static_cast<u8>(half >> 8U)); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

void get_halves(const u8*& cursor, Span<Vec4> texels) noexcept {
    const auto half_at = [&cursor]() {
        const auto value = static_cast<u16>(u32{cursor[0]} | (u32{cursor[1]} << 8U));
        cursor += 2;
        return float_from_half(value);
    };
    for (Vec4& texel : texels) {
        texel.x = half_at();
        texel.y = half_at();
        texel.z = half_at();
        texel.w = half_at();
    }
}

[[nodiscard]] Status put_shadow(Array<u8>& out, const BakedLightmap& lightmap) noexcept {
    const auto channels = static_cast<u32>(lightmap.shadow_lights.size());
    if (Status put = put_u32(out, channels); !put) {
        return put;
    }
    for (const u64 id : lightmap.shadow_lights) {
        if (Status put = put_u32(out, static_cast<u32>(id & 0xFFFFFFFFU)); !put) {
            return put;
        }
        if (Status put = put_u32(out, static_cast<u32>(id >> 32U)); !put) {
            return put;
        }
    }
    return channels == 0U ? ok() : put_halves(out, lightmap.shadow_mask.texels.span());
}

/// The version 3 section, after the shadow section: the directly baked lights and the mip chain.
[[nodiscard]] Status put_version3(Array<u8>& out, const BakedLightmap& lightmap) noexcept {
    if (Status put = put_u32(out, static_cast<u32>(lightmap.direct_lights.size())); !put) {
        return put;
    }
    for (const u64 id : lightmap.direct_lights) {
        if (Status put = put_u32(out, static_cast<u32>(id & 0xFFFFFFFFU)); !put) {
            return put;
        }
        if (Status put = put_u32(out, static_cast<u32>(id >> 32U)); !put) {
            return put;
        }
    }
    if (Status put = put_u32(out, lightmap.mip_levels); !put) {
        return put;
    }
    const bool masked = !lightmap.shadow_lights.empty();
    for (u32 level = 1; level <= lightmap.mip_levels; ++level) {
        if (Status put = put_halves(out, lightmap_level(lightmap, level).texels.span()); !put) {
            return put;
        }
        if (masked) {
            if (Status put = put_halves(out, shadow_mask_level(lightmap, level).texels.span());
                !put) {
                return put;
            }
        }
    }
    return ok();
}

/// What is left of a payload, read front to back. Every take fails rather than reading past the
/// end.
struct Reader {
    const u8* cursor = nullptr;
    usize remaining = 0;

    [[nodiscard]] bool take_u32(u32& out) noexcept {
        if (remaining < 4U) {
            return false;
        }
        out = get_u32(cursor);
        cursor += 4;
        remaining -= 4U;
        return true;
    }
    [[nodiscard]] bool take_ids(u32 count, Array<u64>& out) noexcept {
        out.clear();
        for (u32 at = 0; at < count; ++at) {
            u32 low = 0;
            u32 high = 0;
            if (!take_u32(low) || !take_u32(high) ||
                !out.push_back(u64{low} | (u64{high} << 32U)).has_value()) {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] bool take_halves(LightmapTexels& texels) noexcept {
        const usize count = usize{texels.width} * texels.height * texels.planes;
        if (remaining / 8U < count || !texels.texels.resize(count).has_value()) {
            return false;
        }
        get_halves(cursor, texels.texels.span());
        remaining -= count * 8U;
        return true;
    }
};

[[nodiscard]] Status malformed() noexcept {
    return fail(ErrorCode::InvalidArgument,
                "a lightmap payload whose sections disagree with its length");
}

/// The version 2 shadow section, after the texels.
[[nodiscard]] Status get_shadow(Reader& reader, BakedLightmap& out) noexcept {
    out.shadow_lights.clear();
    out.shadow_mask = LightmapTexels();
    u32 channels = 0;
    if (!reader.take_u32(channels) || channels > kMaxShadowMaskLights ||
        !reader.take_ids(channels, out.shadow_lights)) {
        return malformed();
    }
    if (channels == 0U) {
        return ok();
    }
    out.shadow_mask.width = out.texels.width;
    out.shadow_mask.height = out.texels.height;
    out.shadow_mask.planes = 1;
    return reader.take_halves(out.shadow_mask) ? ok() : malformed();
}

/// The version 3 section: the directly baked lights and the mip chain.
[[nodiscard]] Status get_version3(Reader& reader, BakedLightmap& out) noexcept {
    u32 direct = 0;
    if (!reader.take_u32(direct) || direct > 0xFFFFU || !reader.take_ids(direct, out.direct_lights) ||
        !reader.take_u32(out.mip_levels) || out.mip_levels > kMaxLightmapMipLevels) {
        return malformed();
    }
    for (u32 level = 1; level <= out.mip_levels; ++level) {
        LightmapTexels& texels = out.mip_texels[level - 1U];
        texels.width = out.texels.width >> level;
        texels.height = out.texels.height >> level;
        texels.planes = out.texels.planes;
        if (!reader.take_halves(texels)) {
            return malformed();
        }
        if (out.shadow_lights.empty()) {
            continue;
        }
        LightmapTexels& mask = out.mip_shadow_mask[level - 1U];
        mask.width = texels.width;
        mask.height = texels.height;
        mask.planes = 1;
        if (!reader.take_halves(mask)) {
            return malformed();
        }
    }
    return ok();
}

/// Everything after the texels, by version, which must account for every remaining byte.
[[nodiscard]] Status get_sections(u32 version, Reader reader, BakedLightmap& out) noexcept {
    if (version >= 2U) {
        if (Status read = get_shadow(reader, out); !read) {
            return read;
        }
    }
    if (version >= 3U) {
        if (Status read = get_version3(reader, out); !read) {
            return read;
        }
    }
    return reader.remaining == 0U ? ok() : malformed();
}

}  // namespace

Status encode_lightmap_asset(const BakedLightmap& lightmap, Array<u8>& out) noexcept {
    out.clear();
    const LightmapTexels& texels = lightmap.texels;
    const usize values = texels.texels.size() * 4U;
    if (Status reserved =
            out.reserve(((kHeaderWords + lightmap.addresses.size()) * 4U) + (values * 2U));
        !reserved) {
        return reserved;
    }
    const u32 header[kHeaderWords] = {kLightmapAssetMagic,
                                      kLightmapAssetVersion,
                                      static_cast<u32>(lightmap.mode),
                                      lightmap.page_size,
                                      lightmap.pages,
                                      lightmap.gutter_texels,
                                      texels.planes,
                                      static_cast<u32>(lightmap.addresses.size())};
    for (const u32 word : header) {
        if (Status put = put_u32(out, word); !put) {
            return put;
        }
    }
    for (const u32 address : lightmap.addresses) {
        if (Status put = put_u32(out, address); !put) {
            return put;
        }
    }
    if (Status put = put_halves(out, texels.texels.span()); !put) {
        return put;
    }
    if (Status put = put_shadow(out, lightmap); !put) {
        return put;
    }
    return put_version3(out, lightmap);
}

Status decode_lightmap_asset(Span<const u8> payload, BakedLightmap& out) noexcept {
    if (payload.size() < kHeaderWords * 4U) {
        return fail(ErrorCode::InvalidArgument, "a lightmap payload shorter than its header");
    }
    u32 header[kHeaderWords] = {};
    for (usize word = 0; word < kHeaderWords; ++word) {
        header[word] = get_u32(payload.data() + (word * 4U));
    }
    if (header[0] != kLightmapAssetMagic || header[1] < kLightmapAssetOldestVersion ||
        header[1] > kLightmapAssetVersion) {
        return fail(ErrorCode::InvalidArgument, "not a lightmap payload of this version");
    }
    if (header[2] >= static_cast<u32>(LightmapMode::Count) ||
        header[6] != lightmap_planes(static_cast<LightmapMode>(header[2]))) {
        return fail(ErrorCode::InvalidArgument, "a lightmap payload with an unknown encoding");
    }
    const u64 width = header[3];
    const u64 height = u64{header[3]} * header[4];
    const u64 texel_count = width * height * header[6];
    const u64 expected = ((u64{kHeaderWords} + header[7]) * 4U) + (texel_count * 8U);
    if (width == 0 || height == 0 || expected > payload.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "a lightmap payload whose length disagrees with its header");
    }
    out.mode = static_cast<LightmapMode>(header[2]);
    out.page_size = header[3];
    out.pages = header[4];
    out.gutter_texels = header[5];
    out.texels.width = header[3];
    out.texels.height = static_cast<u32>(height);
    out.texels.planes = header[6];
    if (Status sized = out.addresses.resize(header[7]); !sized) {
        return sized;
    }
    const u8* cursor = payload.data() + (kHeaderWords * 4U);
    for (u32& address : out.addresses) {
        address = get_u32(cursor);
        cursor += 4;
    }
    if (Status sized = out.texels.texels.resize(static_cast<usize>(texel_count)); !sized) {
        return sized;
    }
    get_halves(cursor, out.texels.texels.span());
    out.coverage.clear();
    out.shadow_lights.clear();
    out.shadow_mask = LightmapTexels();
    out.direct_lights.clear();
    out.mip_levels = 0;
    for (u32 level = 0; level < kMaxLightmapMipLevels; ++level) {
        out.mip_texels[level] = LightmapTexels();
        out.mip_shadow_mask[level] = LightmapTexels();
    }
    return get_sections(header[1], Reader{cursor, payload.size() - static_cast<usize>(expected)},
                        out);
}

u16 half_from_float(f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const auto sign = static_cast<u16>((bits >> 16U) & 0x8000U);
    const u32 magnitude = bits & 0x7FFFFFFFU;
    if (magnitude >= 0x7F800000U) {
        return static_cast<u16>(sign | 0x7C00U | (magnitude > 0x7F800000U ? 0x0200U : 0U));
    }
    if (magnitude >= 0x477FF000U) {
        return static_cast<u16>(sign | 0x7C00U);
    }
    if (magnitude < 0x38800000U) {
        if (magnitude < 0x33000000U) {
            return sign;
        }
        const u32 exponent = magnitude >> 23U;
        const u32 mantissa = (magnitude & 0x7FFFFFU) | 0x800000U;
        const u32 shift = 126U - exponent;
        u32 half = mantissa >> shift;
        const u32 remainder = mantissa & ((1U << shift) - 1U);
        const u32 halfway = 1U << (shift - 1U);
        if (remainder > halfway || (remainder == halfway && (half & 1U) != 0U)) {
            half += 1U;
        }
        return static_cast<u16>(sign | half);
    }
    u32 half = (magnitude - 0x38000000U) >> 13U;
    const u32 remainder = magnitude & 0x1FFFU;
    if (remainder > 0x1000U || (remainder == 0x1000U && (half & 1U) != 0U)) {
        half += 1U;
    }
    return static_cast<u16>(sign | half);
}

f32 float_from_half(u16 half) noexcept {
    const u32 sign = (u32{half} & 0x8000U) << 16U;
    const u32 exponent = (u32{half} >> 10U) & 0x1FU;
    u32 mantissa = u32{half} & 0x3FFU;
    u32 bits = 0;
    if (exponent == 0x1FU) {
        bits = sign | 0x7F800000U | (mantissa << 13U);
    } else if (exponent != 0U) {
        bits = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
    } else if (mantissa != 0U) {
        // A subnormal half is a normal float: shift the mantissa up until its leading bit is the
        // implicit one.
        u32 shift = 0;
        while ((mantissa & 0x400U) == 0U) {
            mantissa <<= 1U;
            shift += 1U;
        }
        bits = sign | ((113U - shift) << 23U) | ((mantissa & 0x3FFU) << 13U);
    } else {
        bits = sign;
    }
    f32 value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

}  // namespace cy::rendering::lightmap_bake
