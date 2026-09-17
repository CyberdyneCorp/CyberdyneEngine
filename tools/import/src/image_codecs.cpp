// PNG and baseline JPEG decoding. M11.c task 6.1b. See image_codecs.h.

#include <cy/import/image_codecs.h>

#include <cmath>
#include <cstring>
#include <numbers>

namespace cy::import {
namespace {

// ================================================================================================
// DEFLATE
// ================================================================================================

/// A bit reader over a DEFLATE stream. LSB-first within a byte, which is DEFLATE's own order and
/// the opposite of JPEG's — the two readers are separate types for exactly that reason.
class BitReader {
public:
    explicit BitReader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] usize byte_cursor() const noexcept { return cursor_; }

    /// Discard the partial byte, which is what a stored block starts on.
    void align() noexcept {
        held_ = 0;
        bits_ = 0;
    }

    [[nodiscard]] u32 read(u32 count) noexcept {
        while (bits_ < count) {
            if (cursor_ >= bytes_.size()) {
                ok_ = false;
                return 0;
            }
            held_ |= static_cast<u32>(bytes_[cursor_++]) << bits_;
            bits_ += 8;
        }
        const u32 value = held_ & ((1U << count) - 1U);
        held_ >>= count;
        bits_ -= count;
        return value;
    }

    /// Copy `count` whole bytes from the aligned position. Used by a stored block.
    [[nodiscard]] Span<const u8> take(usize count) noexcept {
        if (!ok_ || cursor_ + count > bytes_.size()) {
            ok_ = false;
            return {};
        }
        const Span<const u8> view(bytes_.data() + cursor_, count);
        cursor_ += count;
        return view;
    }

private:
    Span<const u8> bytes_;
    usize cursor_ = 0;
    u32 held_ = 0;
    u32 bits_ = 0;
    bool ok_ = true;
};

/// A canonical Huffman decoder, built from code lengths the way RFC 1951 §3.2.2 specifies.
///
/// Counts and offsets rather than a tree: the table is built in two linear passes and decoding
/// walks one bit at a time through at most fifteen lengths, which is the whole of DEFLATE's code
/// space.
class Huffman {
public:
    static constexpr u32 kMaxBits = 15;

    [[nodiscard]] bool build(const u8* lengths, u32 count) noexcept {
        for (u32& slot : counts_) {
            slot = 0;
        }
        for (u32 index = 0; index < count; ++index) {
            if (lengths[index] > kMaxBits) {
                return false;
            }
            ++counts_[lengths[index]];
        }
        counts_[0] = 0;
        u32 offsets[kMaxBits + 2] = {};
        for (u32 bits = 1; bits <= kMaxBits; ++bits) {
            offsets[bits + 1] = offsets[bits] + counts_[bits];
        }
        if (offsets[kMaxBits + 1] > kMaxSymbols) {
            return false;
        }
        // Indexed by the LENGTH, not by the length plus one: `offsets[len]` is the number of
        // symbols shorter than `len`, which is exactly where the first symbol of that length goes.
        for (u32 index = 0; index < count; ++index) {
            if (lengths[index] != 0) {
                symbols_[offsets[lengths[index]]++] = static_cast<u16>(index);
            }
        }
        return true;
    }

    /// Decode one symbol. Returns -1 when the stream ran out or the code is not in the table.
    [[nodiscard]] i32 decode(BitReader& reader) const noexcept {
        i32 code = 0;
        i32 first = 0;
        i32 index = 0;
        for (u32 bits = 1; bits <= kMaxBits; ++bits) {
            code |= static_cast<i32>(reader.read(1));
            if (!reader.ok()) {
                return -1;
            }
            const auto count = static_cast<i32>(counts_[bits]);
            if (code - first < count) {
                return symbols_[index + (code - first)];
            }
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return -1;
    }

private:
    static constexpr u32 kMaxSymbols = 288;
    u32 counts_[kMaxBits + 1] = {};
    u16 symbols_[kMaxSymbols] = {};
};

constexpr u16 kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr u8 kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr u16 kDistanceBase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                   33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                   1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr u8 kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void build_fixed_tables(Huffman& literals, Huffman& distances) noexcept {
    u8 lengths[288] = {};
    for (u32 index = 0; index < 144; ++index) {
        lengths[index] = 8;
    }
    for (u32 index = 144; index < 256; ++index) {
        lengths[index] = 9;
    }
    for (u32 index = 256; index < 280; ++index) {
        lengths[index] = 7;
    }
    for (u32 index = 280; index < 288; ++index) {
        lengths[index] = 8;
    }
    (void)literals.build(lengths, 288);
    u8 distance_lengths[30] = {};
    for (u8& length : distance_lengths) {
        length = 5;
    }
    (void)distances.build(distance_lengths, 30);
}

/// RFC 1951 §3.2.7: the two tables of a dynamic block, themselves Huffman coded.
[[nodiscard]] Status read_dynamic_tables(BitReader& reader, Huffman& literals,
                                         Huffman& distances) noexcept {
    constexpr u8 kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const u32 literal_count = reader.read(5) + 257;
    const u32 distance_count = reader.read(5) + 1;
    const u32 code_length_count = reader.read(4) + 4;
    if (!reader.ok() || literal_count > 286 || distance_count > 30) {
        return fail(ErrorCode::InvalidArgument,
                    "a DEFLATE block declaring an impossible code count");
    }

    u8 code_lengths[19] = {};
    for (u32 index = 0; index < code_length_count; ++index) {
        code_lengths[kOrder[index]] = static_cast<u8>(reader.read(3));
    }
    Huffman code_length_table;
    if (!reader.ok() || !code_length_table.build(code_lengths, 19)) {
        return fail(ErrorCode::InvalidArgument,
                    "a DEFLATE code-length table this build cannot read");
    }

    u8 lengths[288 + 30] = {};
    const u32 total = literal_count + distance_count;
    u32 written = 0;
    while (written < total) {
        const i32 symbol = code_length_table.decode(reader);
        if (symbol < 0) {
            return fail(ErrorCode::InvalidArgument,
                        "a DEFLATE code length that is not in its table");
        }
        if (symbol < 16) {
            lengths[written++] = static_cast<u8>(symbol);
            continue;
        }
        u32 repeat = 0;
        u8 value = 0;
        if (symbol == 16) {
            if (written == 0) {
                return fail(ErrorCode::InvalidArgument, "a DEFLATE repeat with nothing to repeat");
            }
            value = lengths[written - 1];
            repeat = reader.read(2) + 3;
        } else if (symbol == 17) {
            repeat = reader.read(3) + 3;
        } else {
            repeat = reader.read(7) + 11;
        }
        if (!reader.ok() || written + repeat > total) {
            return fail(ErrorCode::InvalidArgument, "a DEFLATE repeat past the end of its table");
        }
        for (u32 index = 0; index < repeat; ++index) {
            lengths[written++] = value;
        }
    }
    if (!literals.build(lengths, literal_count) ||
        !distances.build(lengths + literal_count, distance_count)) {
        return fail(ErrorCode::InvalidArgument, "a DEFLATE Huffman table this build cannot read");
    }
    return ok();
}

[[nodiscard]] Status inflate_block(BitReader& reader, const Huffman& literals,
                                   const Huffman& distances, Array<u8>& out) noexcept {
    for (;;) {
        const i32 symbol = literals.decode(reader);
        if (symbol < 0) {
            return fail(ErrorCode::InvalidArgument, "a DEFLATE symbol that is not in its table");
        }
        if (symbol == 256) {
            return ok();
        }
        if (symbol < 256) {
            if (Status pushed = out.push_back(static_cast<u8>(symbol)); !pushed) {
                return pushed;
            }
            continue;
        }
        const u32 length_index = static_cast<u32>(symbol) - 257U;
        if (length_index >= 29) {
            return fail(ErrorCode::InvalidArgument, "a DEFLATE length code outside the table");
        }
        const u32 length = kLengthBase[length_index] + reader.read(kLengthExtra[length_index]);
        const i32 distance_symbol = distances.decode(reader);
        if (distance_symbol < 0 || distance_symbol >= 30) {
            return fail(ErrorCode::InvalidArgument, "a DEFLATE distance code outside the table");
        }
        const u32 distance =
            kDistanceBase[distance_symbol] + reader.read(kDistanceExtra[distance_symbol]);
        if (!reader.ok() || distance == 0 || distance > out.size()) {
            return fail(ErrorCode::InvalidArgument, "a DEFLATE back-reference before the output");
        }
        const usize start = out.size() - distance;
        for (u32 index = 0; index < length; ++index) {
            // Byte at a time and re-read from `out`: an overlapping copy is how DEFLATE spells a
            // run, and a memcpy would read the bytes this loop is still writing.
            const u8 byte = out[start + index];
            if (Status pushed = out.push_back(byte); !pushed) {
                return pushed;
            }
        }
    }
}

[[nodiscard]] Status inflate_stored(BitReader& reader, Array<u8>& out) noexcept {
    reader.align();
    const u32 low = reader.read(8);
    const u32 high = reader.read(8);
    const u32 length = low | (high << 8U);
    (void)reader.read(16);  // The one's complement of the length; the framing is checked by size.
    const Span<const u8> block = reader.take(length);
    if (!reader.ok()) {
        return fail(ErrorCode::InvalidArgument,
                    "a stored DEFLATE block past the end of the stream");
    }
    return out.append(block);
}

[[nodiscard]] u32 adler32(Span<const u8> bytes) noexcept {
    u32 low = 1;
    u32 high = 0;
    for (const u8 byte : bytes) {
        low = (low + byte) % 65521U;
        high = (high + low) % 65521U;
    }
    return (high << 16U) | low;
}

// ================================================================================================
// PNG
// ================================================================================================

struct PngHeader {
    u32 width = 0;
    u32 height = 0;
    u8 bit_depth = 0;
    u8 colour_type = 0;
    u8 interlace = 0;
};

[[nodiscard]] u32 read_be32(const u8* bytes) noexcept {
    return (static_cast<u32>(bytes[0]) << 24U) | (static_cast<u32>(bytes[1]) << 16U) |
           (static_cast<u32>(bytes[2]) << 8U) | static_cast<u32>(bytes[3]);
}

/// How many channels a PNG colour type carries, or zero for one this build does not read.
[[nodiscard]] u32 png_channels(u8 colour_type) noexcept {
    switch (colour_type) {
        case 0:
            return 1;  // greyscale
        case 2:
            return 3;  // truecolour
        case 3:
            return 1;  // palette index
        case 4:
            return 2;  // greyscale + alpha
        case 6:
            return 4;  // truecolour + alpha
        default:
            return 0;
    }
}

/// PNG's five per-row filters, undone in place over the already-reconstructed previous row.
void undo_filter(u8 filter, u8* row, const u8* previous, usize row_bytes, usize step) noexcept {
    for (usize index = 0; index < row_bytes; ++index) {
        const u32 left = index >= step ? row[index - step] : 0U;
        const u32 up = previous != nullptr ? previous[index] : 0U;
        const u32 up_left = (previous != nullptr && index >= step) ? previous[index - step] : 0U;
        u32 addend = 0;
        switch (filter) {
            case 1:
                addend = left;
                break;
            case 2:
                addend = up;
                break;
            case 3:
                addend = (left + up) / 2U;
                break;
            case 4: {
                const auto estimate = static_cast<i32>(left + up - up_left);
                const i32 distance_left = std::abs(estimate - static_cast<i32>(left));
                const i32 distance_up = std::abs(estimate - static_cast<i32>(up));
                const i32 distance_up_left = std::abs(estimate - static_cast<i32>(up_left));
                if (distance_left <= distance_up && distance_left <= distance_up_left) {
                    addend = left;
                } else if (distance_up <= distance_up_left) {
                    addend = up;
                } else {
                    addend = up_left;
                }
                break;
            }
            default:
                addend = 0;
                break;
        }
        row[index] = static_cast<u8>((static_cast<u32>(row[index]) + addend) & 0xFFU);
    }
}

/// Expand one reconstructed scanline into the importer's 8-bit interleaved layout.
///
/// Handles the four sub-byte depths PNG allows (1, 2, 4), the 8-bit case, and 16-bit by keeping the
/// high byte — which is the value, PNG being big-endian.
void expand_scanline(const u8* row, const PngHeader& header, u32 channels, u8* out) noexcept {
    const u32 width = header.width;
    if (header.bit_depth == 8) {
        std::memcpy(out, row, static_cast<usize>(width) * channels);
        return;
    }
    if (header.bit_depth == 16) {
        for (usize index = 0; index < static_cast<usize>(width) * channels; ++index) {
            out[index] = row[index * 2];
        }
        return;
    }
    const u32 depth = header.bit_depth;
    const u32 per_byte = 8U / depth;
    const u32 mask = (1U << depth) - 1U;
    // Sub-byte depths only ever occur with one channel, which PNG guarantees for colour types 0
    // and 3 and forbids for the rest.
    for (u32 x = 0; x < width; ++x) {
        const u32 byte = row[x / per_byte];
        const u32 shift = 8U - depth - ((x % per_byte) * depth);
        out[x] = static_cast<u8>((byte >> shift) & mask);
    }
}

/// Scale a sub-byte greyscale sample up to the full 0..255 range.
void scale_greyscale(Array<u8>& pixels, u32 depth) noexcept {
    if (depth >= 8) {
        return;
    }
    const u32 maximum = (1U << depth) - 1U;
    for (u8& sample : pixels) {
        sample = static_cast<u8>((static_cast<u32>(sample) * 255U) / maximum);
    }
}

struct PngPalette {
    u8 rgb[256][3] = {};
    u8 alpha[256] = {};
    u32 count = 0;
    bool has_alpha = false;
};

[[nodiscard]] Expected<ImageData, Error> expand_palette(const ImageData& indices,
                                                        const PngPalette& palette) noexcept {
    ImageData image;
    image.width = indices.width;
    image.height = indices.height;
    image.channels = palette.has_alpha ? 4U : 3U;
    const usize pixel_count = static_cast<usize>(image.width) * image.height;
    if (Status resized = image.pixels.resize(pixel_count * image.channels); !resized) {
        return make_unexpected(resized.error());
    }
    for (usize index = 0; index < pixel_count; ++index) {
        const u32 entry = indices.pixels[index];
        if (entry >= palette.count) {
            return fail(ErrorCode::InvalidArgument, "a PNG palette index past the end of PLTE");
        }
        u8* pixel = image.pixels.data() + (index * image.channels);
        pixel[0] = palette.rgb[entry][0];
        pixel[1] = palette.rgb[entry][1];
        pixel[2] = palette.rgb[entry][2];
        if (palette.has_alpha) {
            pixel[3] = palette.alpha[entry];
        }
    }
    return image;
}

/// Reconstruct every scanline of a non-interlaced PNG out of the inflated data stream.
[[nodiscard]] Expected<ImageData, Error> reconstruct(const PngHeader& header, u32 channels,
                                                     Array<u8>& raw) noexcept {
    const usize bits_per_pixel = static_cast<usize>(header.bit_depth) * channels;
    const usize row_bytes = ((static_cast<usize>(header.width) * bits_per_pixel) + 7U) / 8U;
    const usize step = (bits_per_pixel + 7U) / 8U;
    const usize expected = (row_bytes + 1U) * header.height;
    if (raw.size() < expected) {
        return fail(ErrorCode::InvalidArgument, "a PNG whose image data ends before its last row");
    }

    ImageData image;
    image.width = header.width;
    image.height = header.height;
    image.channels = channels;
    if (Status resized =
            image.pixels.resize(static_cast<usize>(header.width) * header.height * channels);
        !resized) {
        return make_unexpected(resized.error());
    }

    u8* previous = nullptr;
    for (u32 row = 0; row < header.height; ++row) {
        u8* line = raw.data() + (static_cast<usize>(row) * (row_bytes + 1U));
        const u8 filter = line[0];
        if (filter > 4) {
            return fail(ErrorCode::InvalidArgument, "a PNG row filter this build does not know");
        }
        undo_filter(filter, line + 1, previous, row_bytes, step);
        expand_scanline(line + 1, header, channels,
                        image.pixels.data() + (static_cast<usize>(row) * header.width * channels));
        previous = line + 1;
    }
    if (header.colour_type == 0 && header.bit_depth < 8) {
        scale_greyscale(image.pixels, header.bit_depth);
    }
    return image;
}

}  // namespace

Status inflate_zlib(Span<const u8> bytes, Array<u8>& out) noexcept {
    if (bytes.size() < 6) {
        return fail(ErrorCode::InvalidArgument, "shorter than a zlib stream");
    }
    const u8 method = bytes[0] & 0x0FU;
    if (method != 8) {
        return fail(ErrorCode::Unsupported, "a zlib stream that is not DEFLATE");
    }
    if ((bytes[1] & 0x20U) != 0) {
        return fail(ErrorCode::Unsupported,
                    "a zlib stream with a preset dictionary, which PNG forbids");
    }

    const usize produced_before = out.size();
    BitReader reader(Span<const u8>(bytes.data() + 2, bytes.size() - 2));
    for (;;) {
        const u32 final_block = reader.read(1);
        const u32 kind = reader.read(2);
        if (!reader.ok()) {
            return fail(ErrorCode::InvalidArgument, "a DEFLATE stream that ends inside a block");
        }
        Status block = ok();
        if (kind == 0) {
            block = inflate_stored(reader, out);
        } else if (kind == 1) {
            Huffman literals;
            Huffman distances;
            build_fixed_tables(literals, distances);
            block = inflate_block(reader, literals, distances, out);
        } else if (kind == 2) {
            Huffman literals;
            Huffman distances;
            if (Status tables = read_dynamic_tables(reader, literals, distances); !tables) {
                return tables;
            }
            block = inflate_block(reader, literals, distances, out);
        } else {
            return fail(ErrorCode::InvalidArgument, "a reserved DEFLATE block type");
        }
        if (!block) {
            return block;
        }
        if (final_block != 0) {
            break;
        }
    }

    // The trailing Adler-32 is four bytes after the last byte the reader consumed, aligned. A
    // corrupt stream that inflated anyway is the failure mode this catches.
    const usize trailer = 2U + reader.byte_cursor();
    if (trailer + 4 > bytes.size()) {
        return fail(ErrorCode::InvalidArgument, "a zlib stream with no checksum");
    }
    const u32 declared = read_be32(bytes.data() + trailer);
    const u32 measured =
        adler32(Span<const u8>(out.data() + produced_before, out.size() - produced_before));
    if (declared != measured) {
        return fail(ErrorCode::InvalidArgument,
                    "a zlib stream whose checksum does not match what it inflated to");
    }
    return ok();
}

Expected<ImageData, Error> decode_png(Span<const u8> bytes) noexcept {
    constexpr u8 kSignature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < 8 || std::memcmp(bytes.data(), kSignature, sizeof(kSignature)) != 0) {
        return fail(ErrorCode::InvalidArgument, "not a PNG: the eight-byte signature is wrong");
    }

    PngHeader header;
    PngPalette palette;
    Array<u8> compressed;
    bool seen_header = false;
    usize cursor = 8;
    while (cursor + 8 <= bytes.size()) {
        const u32 length = read_be32(bytes.data() + cursor);
        const u8* type = bytes.data() + cursor + 4;
        const usize body = cursor + 8;
        if (body + length + 4 > bytes.size()) {
            return fail(ErrorCode::InvalidArgument,
                        "a PNG chunk that runs past the end of the file");
        }
        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (length < 13) {
                return fail(ErrorCode::InvalidArgument, "a PNG IHDR shorter than thirteen bytes");
            }
            header.width = read_be32(bytes.data() + body);
            header.height = read_be32(bytes.data() + body + 4);
            header.bit_depth = bytes[body + 8];
            header.colour_type = bytes[body + 9];
            header.interlace = bytes[body + 12];
            seen_header = true;
        } else if (std::memcmp(type, "PLTE", 4) == 0) {
            palette.count = length / 3U;
            if (palette.count > 256) {
                return fail(ErrorCode::InvalidArgument, "a PNG palette of more than 256 entries");
            }
            for (u32 entry = 0; entry < palette.count; ++entry) {
                std::memcpy(palette.rgb[entry],
                            bytes.data() + body + (static_cast<usize>(entry) * 3U), 3);
                palette.alpha[entry] = 255;
            }
        } else if (std::memcmp(type, "tRNS", 4) == 0) {
            if (header.colour_type != 3) {
                return fail(ErrorCode::Unsupported,
                            "a PNG with a transparent colour key; export it with an alpha channel");
            }
            palette.has_alpha = true;
            for (u32 entry = 0; entry < length && entry < 256U; ++entry) {
                palette.alpha[entry] = bytes[body + entry];
            }
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            if (Status appended = compressed.append(Span<const u8>(bytes.data() + body, length));
                !appended) {
                return make_unexpected(appended.error());
            }
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        cursor = body + length + 4;
    }

    if (!seen_header) {
        return fail(ErrorCode::InvalidArgument, "a PNG with no IHDR chunk");
    }
    if (header.width == 0 || header.height == 0) {
        return fail(ErrorCode::InvalidArgument, "a PNG with a zero dimension");
    }
    if (header.interlace != 0) {
        return fail(ErrorCode::Unsupported,
                    "an interlaced (Adam7) PNG; re-export it without interlacing");
    }
    const u32 channels = png_channels(header.colour_type);
    if (channels == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a PNG colour type that is not one of 0, 2, 3, 4, 6");
    }
    if (header.bit_depth != 1 && header.bit_depth != 2 && header.bit_depth != 4 &&
        header.bit_depth != 8 && header.bit_depth != 16) {
        return fail(ErrorCode::InvalidArgument, "a PNG bit depth PNG itself does not define");
    }
    if (compressed.empty()) {
        return fail(ErrorCode::InvalidArgument, "a PNG with no IDAT chunk");
    }

    Array<u8> raw;
    if (Status inflated = inflate_zlib(compressed.span(), raw); !inflated) {
        return make_unexpected(inflated.error());
    }
    auto image = reconstruct(header, channels, raw);
    if (!image) {
        return image;
    }
    if (header.colour_type == 3) {
        if (palette.count == 0) {
            return fail(ErrorCode::InvalidArgument, "a palette PNG with no PLTE chunk");
        }
        return expand_palette(image.value(), palette);
    }
    return image;
}

// ================================================================================================
// BASELINE JPEG
// ================================================================================================

namespace {

constexpr u8 kZigZag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                            12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                            35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                            58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/// A JPEG sample level, clamped to the byte range and rounded half away from zero. Written once
/// because both the inverse transform and the colour conversion need exactly this, and a nested
/// conditional written twice is the shape that drifts apart.
[[nodiscard]] u8 clamped_level(f32 value) noexcept {
    const f32 bounded = std::fmin(std::fmax(value, 0.0F), 255.0F);
    return static_cast<u8>(std::lround(bounded));
}

/// One JPEG Huffman table, in the DHT form: how many codes of each length, then the values.
struct JpegHuffman {
    u8 lengths[17] = {};
    u8 values[256] = {};
    i32 minimum_code[17] = {};
    i32 maximum_code[17] = {};
    i32 value_offset[17] = {};
    bool present = false;

    void prepare() noexcept {
        i32 code = 0;
        i32 index = 0;
        for (u32 bits = 1; bits <= 16; ++bits) {
            value_offset[bits] = index - code;
            minimum_code[bits] = code;
            code += lengths[bits];
            index += lengths[bits];
            maximum_code[bits] = lengths[bits] != 0 ? code - 1 : -1;
            code <<= 1;
        }
        present = true;
    }
};

struct JpegComponent {
    u8 id = 0;
    u8 horizontal = 1;
    u8 vertical = 1;
    u8 quantisation = 0;
    u8 dc_table = 0;
    u8 ac_table = 0;
    i32 dc_predictor = 0;
    Array<u8> plane;
    u32 plane_width = 0;
    u32 plane_height = 0;
};

/// A bit reader over JPEG entropy-coded data: MSB first, and 0xFF00 is a literal 0xFF.
class JpegBits {
public:
    JpegBits(Span<const u8> bytes, usize cursor) noexcept : bytes_(bytes), cursor_(cursor) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] usize cursor() const noexcept { return cursor_; }

    /// Step over a restart marker: discard the partial byte, then consume the two-byte
    /// `FFD0`..`FFD7` that separates two intervals. A reader that left the marker in the stream
    /// would read `FF` as data and refuse the rest of the scan.
    [[nodiscard]] bool restart() noexcept {
        held_ = 0;
        bits_ = 0;
        if (cursor_ + 2 > bytes_.size() || bytes_[cursor_] != 0xFF) {
            ok_ = false;
            return false;
        }
        const u8 marker = bytes_[cursor_ + 1];
        if (marker < 0xD0 || marker > 0xD7) {
            ok_ = false;
            return false;
        }
        cursor_ += 2;
        return true;
    }

    [[nodiscard]] u32 read(u32 count) noexcept {
        u32 value = 0;
        for (u32 index = 0; index < count; ++index) {
            value = (value << 1U) | read_bit();
        }
        return value;
    }

    [[nodiscard]] u32 read_bit() noexcept {
        if (bits_ == 0) {
            if (cursor_ >= bytes_.size()) {
                ok_ = false;
                return 0;
            }
            held_ = bytes_[cursor_++];
            if (held_ == 0xFF) {
                // A stuffed zero is data; any other marker ends the scan.
                if (cursor_ < bytes_.size() && bytes_[cursor_] == 0x00) {
                    ++cursor_;
                } else {
                    ok_ = false;
                    return 0;
                }
            }
            bits_ = 8;
        }
        --bits_;
        return (held_ >> bits_) & 1U;
    }

private:
    Span<const u8> bytes_;
    usize cursor_ = 0;
    u32 held_ = 0;
    u32 bits_ = 0;
    bool ok_ = true;
};

[[nodiscard]] i32 decode_huffman(JpegBits& bits, const JpegHuffman& table) noexcept {
    i32 code = 0;
    for (u32 length = 1; length <= 16; ++length) {
        code = (code << 1) | static_cast<i32>(bits.read_bit());
        if (!bits.ok()) {
            return -1;
        }
        if (table.maximum_code[length] >= 0 && code <= table.maximum_code[length]) {
            return table.values[code + table.value_offset[length]];
        }
    }
    return -1;
}

/// JPEG's signed-magnitude coefficient encoding: `size` bits whose top bit says which half.
[[nodiscard]] i32 extend(u32 value, u32 size) noexcept {
    if (size == 0) {
        return 0;
    }
    const auto threshold = static_cast<i32>(1U << (size - 1U));
    auto result = static_cast<i32>(value);
    if (result < threshold) {
        // `(-1 << size) + 1`, written without shifting a negative: the tree builds with
        // -Wshift-negative-value and the behaviour is unspecified before C++20 regardless.
        result += static_cast<i32>(~0U << size) + 1;
    }
    return result;
}

/// The separable inverse DCT, as two passes of the eight-point transform in floating point.
///
/// Floating point rather than one of the integer approximations: this decoder runs at cook time on
/// a desktop and the arithmetic is not the cost, while an integer IDCT is a second set of rounding
/// rules to get right for no benefit a cooked texture can see.
void inverse_dct(const f32* input, u8* out, usize stride) noexcept {
    f32 intermediate[64];
    for (u32 column = 0; column < 8; ++column) {
        for (u32 row = 0; row < 8; ++row) {
            f32 sum = 0.0F;
            for (u32 u = 0; u < 8; ++u) {
                const f32 scale = u == 0 ? 0.70710678F : 1.0F;
                sum += scale * input[(u * 8U) + column] *
                       std::cos(((2.0F * static_cast<f32>(row)) + 1.0F) * static_cast<f32>(u) *
                                std::numbers::pi_v<f32> / 16.0F);
            }
            intermediate[(row * 8U) + column] = sum * 0.5F;
        }
    }
    for (u32 row = 0; row < 8; ++row) {
        for (u32 column = 0; column < 8; ++column) {
            f32 sum = 0.0F;
            for (u32 u = 0; u < 8; ++u) {
                const f32 scale = u == 0 ? 0.70710678F : 1.0F;
                sum += scale * intermediate[(row * 8U) + u] *
                       std::cos(((2.0F * static_cast<f32>(column)) + 1.0F) * static_cast<f32>(u) *
                                std::numbers::pi_v<f32> / 16.0F);
            }
            out[(row * stride) + column] = clamped_level((sum * 0.5F) + 128.0F);
        }
    }
}

struct JpegState {
    u16 quantisation[4][64] = {};
    JpegHuffman dc[4];
    JpegHuffman ac[4];
    JpegComponent components[4];
    u32 component_count = 0;
    u32 width = 0;
    u32 height = 0;
    u32 restart_interval = 0;
    u32 max_horizontal = 1;
    u32 max_vertical = 1;
};

[[nodiscard]] Status read_quantisation(JpegState& state, Span<const u8> segment) noexcept {
    usize cursor = 0;
    while (cursor < segment.size()) {
        const u8 spec = segment[cursor++];
        const u32 precision = spec >> 4U;
        const u32 slot = spec & 0x0FU;
        if (slot >= 4) {
            return fail(ErrorCode::InvalidArgument, "a JPEG quantisation table index above three");
        }
        for (const u8 zig_zag : kZigZag) {
            if (cursor >= segment.size()) {
                return fail(ErrorCode::InvalidArgument, "a truncated JPEG quantisation table");
            }
            u16 value = segment[cursor++];
            if (precision != 0) {
                value = static_cast<u16>((value << 8U) | segment[cursor++]);
            }
            state.quantisation[slot][zig_zag] = value;
        }
    }
    return ok();
}

[[nodiscard]] Status read_huffman_tables(JpegState& state, Span<const u8> segment) noexcept {
    usize cursor = 0;
    while (cursor < segment.size()) {
        const u8 spec = segment[cursor++];
        const u32 kind = spec >> 4U;
        const u32 slot = spec & 0x0FU;
        if (slot >= 4 || kind > 1) {
            return fail(ErrorCode::InvalidArgument,
                        "a JPEG Huffman table index this build refuses");
        }
        JpegHuffman& table = kind == 0 ? state.dc[slot] : state.ac[slot];
        u32 total = 0;
        for (u32 length = 1; length <= 16; ++length) {
            if (cursor >= segment.size()) {
                return fail(ErrorCode::InvalidArgument, "a truncated JPEG Huffman table");
            }
            table.lengths[length] = segment[cursor++];
            total += table.lengths[length];
        }
        if (total > 256 || cursor + total > segment.size()) {
            return fail(ErrorCode::InvalidArgument, "a JPEG Huffman table longer than its segment");
        }
        for (u32 index = 0; index < total; ++index) {
            table.values[index] = segment[cursor++];
        }
        table.prepare();
    }
    return ok();
}

[[nodiscard]] Status read_frame(JpegState& state, Span<const u8> segment) noexcept {
    if (segment.size() < 6) {
        return fail(ErrorCode::InvalidArgument, "a truncated JPEG frame header");
    }
    state.height = (static_cast<u32>(segment[1]) << 8U) | segment[2];
    state.width = (static_cast<u32>(segment[3]) << 8U) | segment[4];
    state.component_count = segment[5];
    if (state.component_count != 1 && state.component_count != 3) {
        return fail(ErrorCode::Unsupported,
                    "a JPEG with a component count other than one or three; CMYK is not read");
    }
    if (segment.size() < 6U + (state.component_count * 3U)) {
        return fail(ErrorCode::InvalidArgument, "a truncated JPEG component list");
    }
    for (u32 index = 0; index < state.component_count; ++index) {
        JpegComponent& component = state.components[index];
        component.id = segment[6U + (index * 3U)];
        component.horizontal = static_cast<u8>(segment[7U + (index * 3U)] >> 4U);
        component.vertical = static_cast<u8>(segment[7U + (index * 3U)] & 0x0FU);
        component.quantisation = segment[8U + (index * 3U)];
        if (component.horizontal == 0 || component.vertical == 0 || component.horizontal > 4 ||
            component.vertical > 4) {
            return fail(ErrorCode::InvalidArgument, "a JPEG sampling factor outside one to four");
        }
        state.max_horizontal = state.max_horizontal > component.horizontal ? state.max_horizontal
                                                                           : component.horizontal;
        state.max_vertical =
            state.max_vertical > component.vertical ? state.max_vertical : component.vertical;
    }
    return ok();
}

/// Decode one 8x8 block into its component's plane.
[[nodiscard]] Status decode_block(JpegState& state, JpegComponent& component, JpegBits& bits,
                                  u32 block_x, u32 block_y) noexcept {
    const JpegHuffman& dc = state.dc[component.dc_table];
    const JpegHuffman& ac = state.ac[component.ac_table];
    if (!dc.present || !ac.present) {
        return fail(ErrorCode::InvalidArgument, "a JPEG scan naming a Huffman table it never sent");
    }
    f32 coefficients[64] = {};
    const i32 dc_size = decode_huffman(bits, dc);
    if (dc_size < 0 || dc_size > 15) {
        return fail(ErrorCode::InvalidArgument, "a JPEG DC code that is not in its table");
    }
    const i32 difference = extend(bits.read(static_cast<u32>(dc_size)), static_cast<u32>(dc_size));
    component.dc_predictor += difference;
    coefficients[0] = static_cast<f32>(component.dc_predictor) *
                      static_cast<f32>(state.quantisation[component.quantisation][0]);

    for (u32 index = 1; index < 64;) {
        const i32 symbol = decode_huffman(bits, ac);
        if (symbol < 0) {
            return fail(ErrorCode::InvalidArgument, "a JPEG AC code that is not in its table");
        }
        const u32 run = static_cast<u32>(symbol) >> 4U;
        const u32 size = static_cast<u32>(symbol) & 0x0FU;
        if (size == 0) {
            if (run != 15) {
                break;  // end of block
            }
            index += 16;
            continue;
        }
        index += run;
        if (index >= 64) {
            return fail(ErrorCode::InvalidArgument, "a JPEG run past the end of its block");
        }
        const i32 value = extend(bits.read(size), size);
        const u8 position = kZigZag[index];
        coefficients[position] =
            static_cast<f32>(value) *
            static_cast<f32>(state.quantisation[component.quantisation][position]);
        ++index;
    }

    u8* destination = component.plane.data() +
                      (static_cast<usize>(block_y) * 8U * component.plane_width) +
                      (static_cast<usize>(block_x) * 8U);
    inverse_dct(coefficients, destination, component.plane_width);
    return bits.ok() ? ok()
                     : fail(ErrorCode::InvalidArgument, "a JPEG scan that ends inside a block");
}

[[nodiscard]] Status allocate_planes(JpegState& state, u32 mcus_wide, u32 mcus_high) noexcept {
    for (u32 index = 0; index < state.component_count; ++index) {
        JpegComponent& component = state.components[index];
        component.plane_width = mcus_wide * component.horizontal * 8U;
        component.plane_height = mcus_high * component.vertical * 8U;
        if (Status resized = component.plane.resize(static_cast<usize>(component.plane_width) *
                                                    component.plane_height);
            !resized) {
            return resized;
        }
    }
    return ok();
}

/// One pass over the minimum coded units, in the interleaved order a baseline scan uses.
[[nodiscard]] Status decode_scan(JpegState& state, JpegBits& bits, u32 mcus_wide,
                                 u32 mcus_high) noexcept {
    u32 since_restart = 0;
    for (u32 mcu_y = 0; mcu_y < mcus_high; ++mcu_y) {
        for (u32 mcu_x = 0; mcu_x < mcus_wide; ++mcu_x) {
            if (state.restart_interval != 0 && since_restart == state.restart_interval) {
                // A restart marker is byte-aligned, resets every predictor, and the reader steps
                // over the two-byte marker itself.
                since_restart = 0;
                if (!bits.restart()) {
                    return fail(ErrorCode::InvalidArgument,
                                "a JPEG restart interval with no restart marker at its boundary");
                }
                for (u32 index = 0; index < state.component_count; ++index) {
                    state.components[index].dc_predictor = 0;
                }
            }
            for (u32 index = 0; index < state.component_count; ++index) {
                JpegComponent& component = state.components[index];
                for (u32 y = 0; y < component.vertical; ++y) {
                    for (u32 x = 0; x < component.horizontal; ++x) {
                        if (Status decoded = decode_block(state, component, bits,
                                                          (mcu_x * component.horizontal) + x,
                                                          (mcu_y * component.vertical) + y);
                            !decoded) {
                            return decoded;
                        }
                    }
                }
            }
            ++since_restart;
        }
    }
    return ok();
}

/// Upsample every plane to full resolution and convert YCbCr to RGB.
[[nodiscard]] Expected<ImageData, Error> compose(JpegState& state) noexcept {
    ImageData image;
    image.width = state.width;
    image.height = state.height;
    image.channels = state.component_count == 1 ? 1U : 3U;
    if (Status resized =
            image.pixels.resize(static_cast<usize>(image.width) * image.height * image.channels);
        !resized) {
        return make_unexpected(resized.error());
    }

    for (u32 y = 0; y < image.height; ++y) {
        for (u32 x = 0; x < image.width; ++x) {
            u8 samples[3] = {};
            for (u32 index = 0; index < state.component_count; ++index) {
                const JpegComponent& component = state.components[index];
                const u32 source_x = (x * component.horizontal) / state.max_horizontal;
                const u32 source_y = (y * component.vertical) / state.max_vertical;
                samples[index] =
                    component
                        .plane[(static_cast<usize>(source_y) * component.plane_width) + source_x];
            }
            u8* pixel = image.pixels.data() +
                        (((static_cast<usize>(y) * image.width) + x) * image.channels);
            if (state.component_count == 1) {
                pixel[0] = samples[0];
                continue;
            }
            const f32 luma = static_cast<f32>(samples[0]);
            const f32 blue = static_cast<f32>(samples[1]) - 128.0F;
            const f32 red = static_cast<f32>(samples[2]) - 128.0F;
            const f32 channels[3] = {luma + (1.402F * red),
                                     luma - (0.344136F * blue) - (0.714136F * red),
                                     luma + (1.772F * blue)};
            for (u32 channel = 0; channel < 3; ++channel) {
                pixel[channel] = clamped_level(channels[channel]);
            }
        }
    }
    return image;
}

[[nodiscard]] Status read_scan_header(JpegState& state, Span<const u8> segment) noexcept {
    if (segment.empty() || segment[0] != state.component_count) {
        return fail(ErrorCode::Unsupported,
                    "a JPEG scan over a subset of the frame's components, which is progressive");
    }
    if (segment.size() < 1U + (static_cast<usize>(state.component_count) * 2U)) {
        return fail(ErrorCode::InvalidArgument, "a truncated JPEG scan header");
    }
    for (u32 index = 0; index < state.component_count; ++index) {
        const u8 id = segment[1U + (index * 2U)];
        const u8 tables = segment[2U + (index * 2U)];
        for (u32 slot = 0; slot < state.component_count; ++slot) {
            if (state.components[slot].id == id) {
                state.components[slot].dc_table = static_cast<u8>(tables >> 4U);
                state.components[slot].ac_table = static_cast<u8>(tables & 0x0FU);
            }
        }
    }
    return ok();
}

}  // namespace

Expected<ImageData, Error> decode_jpeg(Span<const u8> bytes) noexcept {
    if (bytes.size() < 4 || bytes[0] != 0xFF || bytes[1] != 0xD8) {
        return fail(ErrorCode::InvalidArgument, "not a JPEG: the start-of-image marker is missing");
    }
    JpegState state;
    usize cursor = 2;
    while (cursor + 4 <= bytes.size()) {
        if (bytes[cursor] != 0xFF) {
            return fail(ErrorCode::InvalidArgument, "a JPEG byte where a marker was expected");
        }
        const u8 marker = bytes[cursor + 1];
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            cursor += 2;
            continue;
        }
        const usize length = (static_cast<usize>(bytes[cursor + 2]) << 8U) | bytes[cursor + 3];
        if (length < 2 || cursor + 2 + length > bytes.size()) {
            return fail(ErrorCode::InvalidArgument, "a JPEG segment longer than the file");
        }
        const Span<const u8> segment(bytes.data() + cursor + 4, length - 2);
        Status step = ok();
        switch (marker) {
            case 0xC0:
            case 0xC1:
                step = read_frame(state, segment);
                break;
            case 0xC2:
                return fail(ErrorCode::Unsupported,
                            "a progressive JPEG; this build reads baseline sequential only");
            case 0xC9:
            case 0xCA:
            case 0xCB:
                return fail(ErrorCode::Unsupported,
                            "an arithmetic-coded JPEG; this build reads Huffman coding only");
            case 0xC4:
                step = read_huffman_tables(state, segment);
                break;
            case 0xDB:
                step = read_quantisation(state, segment);
                break;
            case 0xDD:
                if (segment.size() < 2) {
                    return fail(ErrorCode::InvalidArgument, "a truncated JPEG restart interval");
                }
                state.restart_interval =
                    (static_cast<u32>(segment[0]) << 8U) | static_cast<u32>(segment[1]);
                break;
            case 0xDA:
                step = read_scan_header(state, segment);
                break;
            default:
                break;
        }
        if (!step) {
            return make_unexpected(step.error());
        }
        cursor += 2 + length;
        if (marker != 0xDA) {
            continue;
        }

        if (state.width == 0 || state.height == 0) {
            return fail(ErrorCode::InvalidArgument, "a JPEG scan before its frame header");
        }
        const u32 mcu_width = state.max_horizontal * 8U;
        const u32 mcu_height = state.max_vertical * 8U;
        const u32 mcus_wide = (state.width + mcu_width - 1U) / mcu_width;
        const u32 mcus_high = (state.height + mcu_height - 1U) / mcu_height;
        if (Status allocated = allocate_planes(state, mcus_wide, mcus_high); !allocated) {
            return make_unexpected(allocated.error());
        }
        JpegBits bits(bytes, cursor);
        if (Status decoded = decode_scan(state, bits, mcus_wide, mcus_high); !decoded) {
            return make_unexpected(decoded.error());
        }
        return compose(state);
    }
    return fail(ErrorCode::InvalidArgument, "a JPEG with no scan in it");
}

}  // namespace cy::import
