// BC7, BC5 and BC4 encoding. M11.c task 6.1b. See block_encode.h.

#include <cy/import/block_encode.h>

#include <cmath>
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

/// The two endpoints mode 6 fits its sixteen-entry palette between.
struct Endpoints {
    f32 low[4] = {};
    f32 high[4] = {};
};

/// The line through the block, found by the PRINCIPAL AXIS of its texels rather than by their
/// bounding box.
///
/// THE BOUNDING BOX IS THE WRONG LINE WHENEVER TWO CHANNELS ARE ANTI-CORRELATED, and that is not a
/// corner case — it is a red-to-green transition, which is half the masks in a material. The box's
/// corners are then `(min r, min g)` and `(max r, max g)`, and neither corner is a colour the block
/// contains: `unit.import`'s two-population case measured a peak error of 107 of 255 fitting that
/// line, against 19 fitting this one.
///
/// Power iteration on the 4x4 covariance, eight steps, from a fixed start. Deterministic — which is
/// a cook requirement and not a preference: `build-and-packaging` requires the same source to
/// produce the same bytes twice, and an iteration seeded from anything variable would not.
[[nodiscard]] Endpoints principal_axis(const u8 rgba[64]) noexcept {
    f32 mean[4] = {};
    for (u32 texel = 0; texel < 16; ++texel) {
        for (u32 channel = 0; channel < 4; ++channel) {
            mean[channel] += static_cast<f32>(rgba[(texel * 4U) + channel]);
        }
    }
    for (f32& value : mean) {
        value /= 16.0F;
    }

    f32 covariance[4][4] = {};
    for (u32 texel = 0; texel < 16; ++texel) {
        f32 centred[4];
        for (u32 channel = 0; channel < 4; ++channel) {
            centred[channel] = static_cast<f32>(rgba[(texel * 4U) + channel]) - mean[channel];
        }
        for (u32 row = 0; row < 4; ++row) {
            for (u32 column = 0; column < 4; ++column) {
                covariance[row][column] += centred[row] * centred[column];
            }
        }
    }

    // A fixed, non-degenerate start. Luminance-weighted so that a block whose only variation is in
    // one colour channel still has a non-zero projection on the first step.
    f32 axis[4] = {0.9F, 1.0F, 0.7F, 0.4F};
    for (u32 step = 0; step < 8; ++step) {
        f32 next[4] = {};
        for (u32 row = 0; row < 4; ++row) {
            for (u32 column = 0; column < 4; ++column) {
                next[row] += covariance[row][column] * axis[column];
            }
        }
        f32 length = 0.0F;
        for (const f32 value : next) {
            length += value * value;
        }
        if (length <= 1e-12F) {
            break;  // A flat block: every texel is the mean and the axis does not matter.
        }
        length = std::sqrt(length);
        for (u32 row = 0; row < 4; ++row) {
            axis[row] = next[row] / length;
        }
    }

    f32 lowest = 1e30F;
    f32 highest = -1e30F;
    for (u32 texel = 0; texel < 16; ++texel) {
        f32 projection = 0.0F;
        for (u32 channel = 0; channel < 4; ++channel) {
            projection +=
                (static_cast<f32>(rgba[(texel * 4U) + channel]) - mean[channel]) * axis[channel];
        }
        lowest = projection < lowest ? projection : lowest;
        highest = projection > highest ? projection : highest;
    }

    Endpoints endpoints;
    for (u32 channel = 0; channel < 4; ++channel) {
        endpoints.low[channel] = mean[channel] + (lowest * axis[channel]);
        endpoints.high[channel] = mean[channel] + (highest * axis[channel]);
    }
    return endpoints;
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

/// Least-squares endpoints for the indices already chosen.
///
/// One closed-form solve per channel over the sixteen texels: given each texel's weight, the pair of
/// endpoints minimising the squared error is the solution of a 2x2 normal system. It is what turns a
/// good line into the best palette ON that line, and it is where most of the remaining error goes.
void refine(const u8 rgba[64], const u32 indices[16], Endpoints& endpoints) noexcept {
    f32 a11 = 0.0F;
    f32 a12 = 0.0F;
    f32 a22 = 0.0F;
    for (const u32 index : Span<const u32>(indices, 16)) {
        const f32 weight = static_cast<f32>(kWeights4[index]) / 64.0F;
        a11 += (1.0F - weight) * (1.0F - weight);
        a12 += (1.0F - weight) * weight;
        a22 += weight * weight;
    }
    const f32 determinant = (a11 * a22) - (a12 * a12);
    if (determinant < 1e-6F) {
        return;  // Every texel took the same index: the system is singular and the line is fine.
    }
    for (u32 channel = 0; channel < 4; ++channel) {
        f32 b1 = 0.0F;
        f32 b2 = 0.0F;
        for (u32 texel = 0; texel < 16; ++texel) {
            const f32 weight = static_cast<f32>(kWeights4[indices[texel]]) / 64.0F;
            const auto value = static_cast<f32>(rgba[(texel * 4U) + channel]);
            b1 += (1.0F - weight) * value;
            b2 += weight * value;
        }
        endpoints.low[channel] = ((a22 * b1) - (a12 * b2)) / determinant;
        endpoints.high[channel] = ((a11 * b2) - (a12 * b1)) / determinant;
    }
}

[[nodiscard]] u32 clamp_to_byte(f32 value) noexcept {
    const f32 rounded = value + 0.5F;
    if (rounded <= 0.0F) {
        return 0;
    }
    return rounded >= 255.0F ? 255U : static_cast<u32>(rounded);
}

/// One candidate encoding: the two 7-bit fields, the two p-bits, the indices and the error.
struct Candidate {
    u32 field[2][4] = {};
    u32 p_bit[2] = {};
    Bc7Fit fit;
};

[[nodiscard]] Candidate evaluate(const u8 rgba[64], const Endpoints& endpoints, u32 p0,
                                 u32 p1) noexcept {
    Candidate candidate;
    candidate.p_bit[0] = p0;
    candidate.p_bit[1] = p1;
    u32 value[2][4];
    for (u32 channel = 0; channel < 4; ++channel) {
        candidate.field[0][channel] = quantise(clamp_to_byte(endpoints.low[channel]), p0);
        candidate.field[1][channel] = quantise(clamp_to_byte(endpoints.high[channel]), p1);
        value[0][channel] = (candidate.field[0][channel] << 1U) | p0;
        value[1][channel] = (candidate.field[1][channel] << 1U) | p1;
    }
    candidate.fit = fit_indices(rgba, value[0], value[1]);
    return candidate;
}

}  // namespace

void encode_bc7_block(const u8 rgba[64], u8 out[16]) noexcept {
    // 1. THE LINE. The principal axis of the block rather than its bounding box; see above.
    Endpoints endpoints = principal_axis(rgba);

    // 2. THE PALETTE ON THAT LINE. Two rounds of "fit the indices, then solve for the endpoints
    // those indices imply". The first round is what moves the endpoints off the extremes of the
    // projection and onto the least-squares positions; the second is worth about a further unit of
    // peak error and the third is worth nothing measurable, which is why there are two.
    for (u32 round = 0; round < 2; ++round) {
        const Candidate probe = evaluate(rgba, endpoints, 0, 0);
        refine(rgba, probe.fit.indices, endpoints);
    }

    // 3. THE P-BITS. Mode 6 gives each endpoint one shared low bit across all four channels, so
    // there are four assignments and the cheapest thing that is not a guess is to try them. Ties
    // keep the FIRST, which makes the choice a pure function of the block — a cook requirement.
    Candidate best = evaluate(rgba, endpoints, 0, 0);
    for (u32 pair = 1; pair < 4; ++pair) {
        const Candidate candidate = evaluate(rgba, endpoints, pair & 1U, (pair >> 1U) & 1U);
        if (candidate.fit.error < best.fit.error) {
            best = candidate;
        }
    }

    // 4. THE ANCHOR. Mode 6's first index is stored in three bits, so its high bit must be zero.
    // When it is not, the endpoints are swapped and every index mirrored — the same palette read
    // from the other end, and therefore the same picture.
    if (best.fit.indices[0] >= 8) {
        for (u32 channel = 0; channel < 4; ++channel) {
            const u32 held = best.field[0][channel];
            best.field[0][channel] = best.field[1][channel];
            best.field[1][channel] = held;
        }
        const u32 held = best.p_bit[0];
        best.p_bit[0] = best.p_bit[1];
        best.p_bit[1] = held;
        for (u32& index : best.fit.indices) {
            index = 15U - index;
        }
    }

    BlockBits bits;
    // Mode 6 is six zero bits then a one, so the seven-bit field holds 0x40.
    bits.put(1U << 6U, 7);
    for (u32 channel = 0; channel < 4; ++channel) {
        bits.put(best.field[0][channel], 7);
        bits.put(best.field[1][channel], 7);
    }
    bits.put(best.p_bit[0], 1);
    bits.put(best.p_bit[1], 1);
    bits.put(best.fit.indices[0], 3);
    for (u32 texel = 1; texel < 16; ++texel) {
        bits.put(best.fit.indices[texel], 4);
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
