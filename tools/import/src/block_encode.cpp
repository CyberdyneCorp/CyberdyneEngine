// BC7, BC5 and BC4 encoding. M11.c task 6.1b. See block_encode.h.

#include <cy/import/block_encode.h>

#include <cstring>

namespace cy::import {
namespace {

/// A 128-bit little-endian bit writer. BC7 fills its block least-significant bit first, in the order
/// the mode's field list is written, which is why this is a cursor rather than a set of shifts.
class BlockBits {
public:
    void put(u32 value, u32 count) noexcept {
        for (u32 index = 0; index < count; ++index) {
            if (((value >> index) & 1U) != 0) {
                bytes_[cursor_ / 8U] |= static_cast<u8>(1U << (cursor_ % 8U));
            }
            ++cursor_;
        }
    }

    void copy_to(u8 out[16]) const noexcept { std::memcpy(out, bytes_, 16); }

private:
    u8 bytes_[16] = {};
    u32 cursor_ = 0;
};

// --- BC7, mode 6 ---------------------------------------------------------------------------------

/// The 4-bit index weights the BC7 specification tabulates. Not derived: the table is normative and
/// a decoder uses exactly these, so an encoder that computed `index * 64 / 15` would place every
/// texel slightly off the value the hardware will reconstruct.
constexpr u8 kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

[[nodiscard]] u32 interpolate(u32 low, u32 high, u32 weight) noexcept {
    return ((low * (64U - weight)) + (high * weight) + 32U) >> 6U;
}

struct Endpoints {
    u32 low[4] = {};
    u32 high[4] = {};
};

/// The bounding box of the block in RGBA, which is the line mode 6 fits its palette to.
[[nodiscard]] Endpoints bounding_box(const u8 rgba[64]) noexcept {
    Endpoints endpoints;
    for (u32 channel = 0; channel < 4; ++channel) {
        endpoints.low[channel] = 255;
        endpoints.high[channel] = 0;
    }
    for (u32 texel = 0; texel < 16; ++texel) {
        for (u32 channel = 0; channel < 4; ++channel) {
            const u32 value = rgba[(texel * 4U) + channel];
            endpoints.low[channel] = value < endpoints.low[channel] ? value : endpoints.low[channel];
            endpoints.high[channel] =
                value > endpoints.high[channel] ? value : endpoints.high[channel];
        }
    }
    return endpoints;
}

/// Split an 8-bit endpoint into mode 6's 7-bit value and its shared p-bit.
///
/// The p-bit is shared by all four channels of one endpoint, so it is chosen by majority over the
/// four low bits: a p-bit that disagreed with three channels to please one would cost more than it
/// saved.
[[nodiscard]] u32 choose_p_bit(const u32 values[4]) noexcept {
    u32 ones = 0;
    for (u32 channel = 0; channel < 4; ++channel) {
        ones += values[channel] & 1U;
    }
    return ones >= 2U ? 1U : 0U;
}

/// The 7-bit field whose reconstruction `(field << 1) | p_bit` is nearest to `value`.
///
/// For a fixed p-bit the representable 8-bit values are exactly the numbers whose low bit is the
/// p-bit, so the nearest is one rounded halving. Ties round up, which costs nothing and keeps the
/// function monotone in `value` — a non-monotone quantiser would invert two endpoints of a smooth
/// gradient and show as a band.
[[nodiscard]] u32 quantise(u32 value, u32 p_bit) noexcept {
    if (value <= p_bit) {
        return 0;
    }
    const u32 field = (value - p_bit + 1U) / 2U;
    return field > 127U ? 127U : field;
}

/// Squared error between two RGBA texels.
[[nodiscard]] u32 texel_error(const u8* texel, const u32 palette[4]) noexcept {
    u32 total = 0;
    for (u32 channel = 0; channel < 4; ++channel) {
        const auto difference = static_cast<i32>(texel[channel]) - static_cast<i32>(palette[channel]);
        total += static_cast<u32>(difference * difference);
    }
    return total;
}

struct Bc7Fit {
    u32 indices[16] = {};
    u64 error = 0;
};

/// Project every texel onto the sixteen-entry palette the two endpoints generate, taking the nearest.
[[nodiscard]] Bc7Fit fit_indices(const u8 rgba[64], const u32 low[4], const u32 high[4]) noexcept {
    u32 palette[16][4];
    for (u32 step = 0; step < 16; ++step) {
        for (u32 channel = 0; channel < 4; ++channel) {
            palette[step][channel] = interpolate(low[channel], high[channel], kWeights4[step]);
        }
    }
    Bc7Fit fit;
    for (u32 texel = 0; texel < 16; ++texel) {
        u32 best = 0;
        u32 best_error = texel_error(rgba + (texel * 4U), palette[0]);
        for (u32 step = 1; step < 16; ++step) {
            const u32 error = texel_error(rgba + (texel * 4U), palette[step]);
            if (error < best_error) {
                best_error = error;
                best = step;
            }
        }
        fit.indices[texel] = best;
        fit.error += best_error;
    }
    return fit;
}

}  // namespace

void encode_bc7_block(const u8 rgba[64], u8 out[16]) noexcept {
    const Endpoints box = bounding_box(rgba);

    // One p-bit per endpoint, chosen by majority over the four channels' low bits, and the
    // endpoints then quantised against it. A bounding-box fit with the normative weight table is
    // exact for any block whose texels lie on one line in RGBA — which is what mode 6 is for — and
    // this encoder does not attempt the partitioned modes; block_encode.h says so and says what it
    // costs.
    u32 p_bit[2] = {choose_p_bit(box.low), choose_p_bit(box.high)};
    u32 field[2][4];
    u32 value[2][4];
    for (u32 channel = 0; channel < 4; ++channel) {
        field[0][channel] = quantise(box.low[channel], p_bit[0]);
        field[1][channel] = quantise(box.high[channel], p_bit[1]);
        value[0][channel] = (field[0][channel] << 1U) | p_bit[0];
        value[1][channel] = (field[1][channel] << 1U) | p_bit[1];
    }

    Bc7Fit fit = fit_indices(rgba, value[0], value[1]);

    // ANCHOR. Mode 6's first index is stored in three bits, so its high bit must be zero. When it is
    // not, the endpoints are swapped and every index mirrored — which is the same palette read from
    // the other end and therefore the same picture.
    if (fit.indices[0] >= 8) {
        for (u32 channel = 0; channel < 4; ++channel) {
            const u32 held = field[0][channel];
            field[0][channel] = field[1][channel];
            field[1][channel] = held;
        }
        const u32 held = p_bit[0];
        p_bit[0] = p_bit[1];
        p_bit[1] = held;
        for (u32& index : fit.indices) {
            index = 15U - index;
        }
    }

    BlockBits bits;
    // Mode 6 is six zero bits then a one, so the seven-bit field holds 0x40.
    bits.put(1U << 6U, 7);
    for (u32 channel = 0; channel < 4; ++channel) {
        bits.put(field[0][channel], 7);
        bits.put(field[1][channel], 7);
    }
    bits.put(p_bit[0], 1);
    bits.put(p_bit[1], 1);
    bits.put(fit.indices[0], 3);
    for (u32 texel = 1; texel < 16; ++texel) {
        bits.put(fit.indices[texel], 4);
    }
    bits.copy_to(out);
}

void encode_bc4_block(const u8 values[16], u8 out[8]) noexcept {
    u32 minimum = 255;
    u32 maximum = 0;
    for (u32 texel = 0; texel < 16; ++texel) {
        minimum = values[texel] < minimum ? values[texel] : minimum;
        maximum = values[texel] > maximum ? values[texel] : maximum;
    }

    // The eight-value mode, which needs red0 > red1. A flat block makes them equal, and the
    // specification's other mode — six interpolated values plus 0 and 255 — is then identical for
    // every index, so the flat case is written as-is rather than special-cased.
    const u32 red0 = maximum;
    const u32 red1 = minimum;
    u32 palette[8];
    palette[0] = red0;
    palette[1] = red1;
    for (u32 step = 2; step < 8; ++step) {
        palette[step] = (((8U - step) * red0) + ((step - 1U) * red1)) / 7U;
    }

    u64 packed = 0;
    for (u32 texel = 0; texel < 16; ++texel) {
        u32 best = 0;
        u32 best_error = 256 * 256;
        for (u32 step = 0; step < 8; ++step) {
            const auto difference =
                static_cast<i32>(values[texel]) - static_cast<i32>(palette[step]);
            const auto error = static_cast<u32>(difference * difference);
            if (error < best_error) {
                best_error = error;
                best = step;
            }
        }
        packed |= static_cast<u64>(best) << (texel * 3U);
    }

    out[0] = static_cast<u8>(red0);
    out[1] = static_cast<u8>(red1);
    for (u32 index = 0; index < 6; ++index) {
        out[2 + index] = static_cast<u8>((packed >> (index * 8U)) & 0xFFU);
    }
}

namespace {

/// Gather one 4x4 block out of a level, clamping at the edge.
void gather(Span<const u8> level, u32 width, u32 height, u32 channels, u32 block_x, u32 block_y,
            u8 rgba[64]) noexcept {
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            u32 x = (block_x * 4U) + column;
            u32 y = (block_y * 4U) + row;
            x = x < width ? x : width - 1U;
            y = y < height ? y : height - 1U;
            const usize source = ((static_cast<usize>(y) * width) + x) * channels;
            u8* texel = rgba + (((row * 4U) + column) * 4U);
            texel[0] = level[source];
            texel[1] = channels > 1 ? level[source + 1] : level[source];
            texel[2] = channels > 2 ? level[source + 2] : level[source];
            texel[3] = channels > 3 ? level[source + 3] : 255U;
        }
    }
}

[[nodiscard]] Status encode_level(Span<const u8> level, u32 width, u32 height, u32 channels,
                                  TextureFormat format, Array<u8>& out) noexcept {
    const u32 blocks_wide = (width + 3U) / 4U;
    const u32 blocks_high = (height + 3U) / 4U;
    for (u32 block_y = 0; block_y < blocks_high; ++block_y) {
        for (u32 block_x = 0; block_x < blocks_wide; ++block_x) {
            u8 rgba[64];
            gather(level, width, height, channels, block_x, block_y, rgba);
            u8 encoded[16] = {};
            usize produced = 0;
            if (format == TextureFormat::BC7) {
                encode_bc7_block(rgba, encoded);
                produced = 16;
            } else {
                u8 red[16];
                for (u32 texel = 0; texel < 16; ++texel) {
                    red[texel] = rgba[texel * 4U];
                }
                encode_bc4_block(red, encoded);
                produced = 8;
                if (format == TextureFormat::BC5) {
                    u8 green[16];
                    for (u32 texel = 0; texel < 16; ++texel) {
                        green[texel] = rgba[(texel * 4U) + 1U];
                    }
                    encode_bc4_block(green, encoded + 8);
                    produced = 16;
                }
            }
            if (Status appended = out.append(Span<const u8>(encoded, produced)); !appended) {
                return appended;
            }
        }
    }
    return ok();
}

}  // namespace

bool can_encode(TextureFormat format) noexcept {
    return format == TextureFormat::BC7 || format == TextureFormat::BC5 ||
           format == TextureFormat::BC4;
}

Status encode_mip_chain(Span<const u8> levels, u32 width, u32 height, u32 mip_count, u32 channels,
                        TextureFormat format, Array<u8>& out) noexcept {
    if (!can_encode(format)) {
        return fail(ErrorCode::Unsupported,
                    "this build encodes BC7, BC5 and BC4; BC6H and ASTC are named by "
                    "`select_format` and produced by nobody, which is why the cooked header records "
                    "`encoded = false` for them rather than claiming otherwise");
    }
    if (channels == 0 || channels > 4) {
        return fail(ErrorCode::InvalidArgument, "an image with an impossible channel count");
    }

    usize cursor = 0;
    u32 level_width = width;
    u32 level_height = height;
    for (u32 level = 0; level < mip_count; ++level) {
        const usize level_bytes = static_cast<usize>(level_width) * level_height * channels;
        if (cursor + level_bytes > levels.size()) {
            return fail(ErrorCode::InvalidArgument,
                        "a mip chain shorter than the levels its header declares");
        }
        if (Status encoded =
                encode_level(Span<const u8>(levels.data() + cursor, level_bytes), level_width,
                             level_height, channels, format, out);
            !encoded) {
            return encoded;
        }
        cursor += level_bytes;
        level_width = level_width > 1U ? level_width / 2U : 1U;
        level_height = level_height > 1U ? level_height / 2U : 1U;
    }
    return ok();
}

}  // namespace cy::import
