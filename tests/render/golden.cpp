#include "golden.h"

#include <algorithm>
#include <cstdio>

namespace cy::render_test {
namespace {

constexpr u8 kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
/// Stored deflate blocks carry a 16-bit length, so this is the most one can hold.
constexpr usize kMaxStoredBlock = 0xFFFF;

// --- CRC-32 and Adler-32, which PNG and zlib respectively require
// ---------------------------------
//
// Written out rather than pulled from a dependency: `thirdparty-dependencies` asks what a
// dependency buys, and thirty lines of two published checksums buys nothing that a fetched
// library's build integration would not cost twice over.

u32 crc32_of(const u8* data, usize size, u32 seed) noexcept {
    static u32 table[256];
    static bool built = false;
    if (!built) {
        for (u32 index = 0; index < 256; ++index) {
            u32 value = index;
            for (u32 bit = 0; bit < 8; ++bit) {
                value = ((value & 1U) != 0U) ? (0xEDB88320U ^ (value >> 1U)) : (value >> 1U);
            }
            table[index] = value;
        }
        built = true;
    }
    u32 crc = seed ^ 0xFFFFFFFFU;
    for (usize index = 0; index < size; ++index) {
        crc = table[(crc ^ data[index]) & 0xFFU] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFFFFFFU;
}

u32 adler32_of(const u8* data, usize size) noexcept {
    u32 low = 1;
    u32 high = 0;
    for (usize index = 0; index < size; ++index) {
        low = (low + data[index]) % 65521U;
        high = (high + low) % 65521U;
    }
    return (high << 16U) | low;
}

void put_be32(Array<u8>& out, u32 value, Status& status) noexcept {
    const u8 bytes[4] = {static_cast<u8>(value >> 24U), static_cast<u8>(value >> 16U),
                         static_cast<u8>(value >> 8U), static_cast<u8>(value)};
    for (const u8 byte : bytes) {
        if (Status pushed = out.push_back(byte); !pushed) {
            status = pushed;
            return;
        }
    }
}

u32 read_be32(const u8* data) noexcept {
    return (static_cast<u32>(data[0]) << 24U) | (static_cast<u32>(data[1]) << 16U) |
           (static_cast<u32>(data[2]) << 8U) | static_cast<u32>(data[3]);
}

Status append(Array<u8>& out, const u8* data, usize size) noexcept {
    for (usize index = 0; index < size; ++index) {
        if (Status pushed = out.push_back(data[index]); !pushed) {
            return pushed;
        }
    }
    return ok();
}

/// One PNG chunk: length, then the type and the payload, then a CRC over BOTH of those as one run.
///
/// The chunk is assembled in `body` first because the CRC covers the type and the payload together
/// and `crc32_of` takes one contiguous range; running it twice from a zero seed would produce a
/// file every decoder rejects.
Status put_chunk(Array<u8>& out, const char type[5], const u8* payload, usize size) noexcept {
    Array<u8> body(out.allocator());
    if (Status reserved = body.reserve(size + 4); !reserved) {
        return reserved;
    }
    if (Status appended = append(body, reinterpret_cast<const u8*>(type), 4); !appended) {
        return appended;
    }
    if (Status appended = append(body, payload, size); !appended) {
        return appended;
    }

    Status status;
    put_be32(out, static_cast<u32>(size), status);
    if (!status) {
        return status;
    }
    if (Status appended = append(out, body.data(), body.size()); !appended) {
        return appended;
    }
    put_be32(out, crc32_of(body.data(), body.size(), 0), status);
    return status;
}

/// The raw scanlines a PNG's zlib stream carries: a filter byte then the row's bytes, per row.
Status build_scanlines(const Image& image, Array<u8>& out) noexcept {
    if (Status reserved = out.reserve(static_cast<usize>(image.height) * (1 + (image.width * 3U)));
        !reserved) {
        return reserved;
    }
    for (u32 y = 0; y < image.height; ++y) {
        // Filter 0, "None". Every other filter would need the decoder below to implement it, and a
        // reference image is not the place to spend bytes.
        if (Status pushed = out.push_back(0); !pushed) {
            return pushed;
        }
        for (u32 x = 0; x < image.width; ++x) {
            const u32 texel = image.at(x, y);
            const u8 rgb[3] = {static_cast<u8>(texel & 0xFFU),
                               static_cast<u8>((texel >> 8U) & 0xFFU),
                               static_cast<u8>((texel >> 16U) & 0xFFU)};
            if (Status appended = append(out, rgb, 3); !appended) {
                return appended;
            }
        }
    }
    return ok();
}

/// A zlib stream whose deflate blocks are all stored. See the header comment for the trade.
Status build_zlib(const Array<u8>& raw, Array<u8>& out) noexcept {
    // 0x78 0x01: deflate, 32 KiB window, no preset dictionary, "fastest" compression level. The
    // level is advisory and a decoder ignores it; the two bytes still have to make FCHECK come out
    // a multiple of 31, which 0x7801 does.
    const u8 header[2] = {0x78, 0x01};
    if (Status appended = append(out, header, 2); !appended) {
        return appended;
    }
    usize offset = 0;
    do {
        const usize block =
            raw.size() - offset < kMaxStoredBlock ? raw.size() - offset : kMaxStoredBlock;
        const bool final_block = offset + block >= raw.size();
        const u8 flag = final_block ? 1U : 0U;
        const auto length = static_cast<u16>(block);
        const u8 preamble[5] = {flag, static_cast<u8>(length & 0xFFU),
                                static_cast<u8>(length >> 8U), static_cast<u8>(~length & 0xFFU),
                                static_cast<u8>((~length >> 8U) & 0xFFU)};
        if (Status appended = append(out, preamble, 5); !appended) {
            return appended;
        }
        if (Status appended = append(out, raw.data() + offset, block); !appended) {
            return appended;
        }
        offset += block;
    } while (offset < raw.size());

    Status status;
    put_be32(out, adler32_of(raw.data(), raw.size()), status);
    return status;
}

Status read_whole_file(const char* path, Array<u8>& out) noexcept {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return fail(ErrorCode::NotFound, "golden: the reference image is not there");
    }
    // A SHORT READ ENDS THE LOOP, whether it was the end of the file or an error. Reading again
    // after one is what leaves a stream's position indeterminate, and `ferror` below is what tells
    // the two apart — a truncated reference and a complete one must not both look like success.
    u8 buffer[4096];
    Status status;
    for (;;) {
        const usize read = std::fread(buffer, 1, sizeof(buffer), file);
        if (read != 0) {
            status = append(out, buffer, read);
            if (!status) {
                break;
            }
        }
        if (read < sizeof(buffer)) {
            if (std::ferror(file) != 0) {
                status = fail(ErrorCode::Internal, "golden: the reference image could not be read");
            }
            break;
        }
    }
    (void)std::fclose(file);
    return status;
}

/// Inflate a stream this module wrote: stored blocks only.
Status inflate_stored(Span<const u8> zlib, Array<u8>& out) noexcept {
    if (zlib.size() < 6) {
        return fail(ErrorCode::InvalidArgument, "golden: the zlib stream is too short");
    }
    usize offset = 2;  // past the two-byte zlib header
    for (;;) {
        if (offset + 5 > zlib.size()) {
            return fail(ErrorCode::InvalidArgument, "golden: a truncated deflate block");
        }
        const u8 flags = zlib[offset];
        if (((flags >> 1U) & 0x03U) != 0U) {
            return fail(ErrorCode::Unsupported,
                        "golden: this reference was not written by write_png() — its deflate "
                        "blocks are compressed, and this decoder reads stored blocks only. "
                        "Regenerate it with CY_RENDER_UPDATE_GOLDEN=1 rather than re-encoding it "
                        "with another tool");
        }
        const auto length =
            static_cast<usize>(zlib[offset + 1]) | (static_cast<usize>(zlib[offset + 2]) << 8U);
        offset += 5;
        if (offset + length > zlib.size()) {
            return fail(ErrorCode::InvalidArgument, "golden: a stored block runs past the stream");
        }
        if (Status appended = append(out, zlib.data() + offset, length); !appended) {
            return appended;
        }
        offset += length;
        if ((flags & 1U) != 0U) {
            return ok();
        }
    }
}

}  // namespace

Status adopt(Image& out, Span<const u32> texels, u32 width, u32 height) noexcept {
    if (texels.size() != static_cast<usize>(width) * height) {
        return fail(ErrorCode::InvalidArgument, "golden: the texel count is not width x height");
    }
    out.width = width;
    out.height = height;
    if (Status sized = out.texels.resize(texels.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < texels.size(); ++index) {
        out.texels[index] = texels[index] | 0xFF000000U;
    }
    return ok();
}

Status write_png(const char* path, const Image& image) noexcept {
    if (image.width == 0 || image.height == 0) {
        return fail(ErrorCode::InvalidArgument, "golden: an empty image has nothing to write");
    }
    Allocator& allocator = image.texels.allocator();
    Array<u8> file(allocator);
    if (Status appended = append(file, kSignature, sizeof(kSignature)); !appended) {
        return appended;
    }

    Array<u8> header(allocator);
    Status status;
    put_be32(header, image.width, status);
    put_be32(header, image.height, status);
    if (!status) {
        return status;
    }
    // Bit depth 8, colour type 2 (truecolour), compression 0, filter 0, interlace 0.
    const u8 tail[5] = {8, 2, 0, 0, 0};
    if (Status appended = append(header, tail, sizeof(tail)); !appended) {
        return appended;
    }
    if (Status written = put_chunk(file, "IHDR", header.data(), header.size()); !written) {
        return written;
    }

    Array<u8> raw(allocator);
    if (Status built = build_scanlines(image, raw); !built) {
        return built;
    }
    Array<u8> compressed(allocator);
    if (Status built = build_zlib(raw, compressed); !built) {
        return built;
    }
    if (Status written = put_chunk(file, "IDAT", compressed.data(), compressed.size()); !written) {
        return written;
    }
    if (Status written = put_chunk(file, "IEND", nullptr, 0); !written) {
        return written;
    }

    std::FILE* handle = std::fopen(path, "wb");
    if (handle == nullptr) {
        return fail(ErrorCode::PermissionDenied, "golden: cannot open the image for writing");
    }
    const usize written = std::fwrite(file.data(), 1, file.size(), handle);
    const bool closed = std::fclose(handle) == 0;
    if (written != file.size() || !closed) {
        return fail(ErrorCode::Internal, "golden: the image was not written in full");
    }
    return ok();
}

Status read_png(const char* path, Image& out) noexcept {
    Array<u8> file(out.texels.allocator());
    if (Status read = read_whole_file(path, file); !read) {
        return read;
    }
    if (file.size() < 8 + 25) {
        return fail(ErrorCode::InvalidArgument, "golden: the file is too short to be a PNG");
    }
    for (usize index = 0; index < sizeof(kSignature); ++index) {
        if (file[index] != kSignature[index]) {
            return fail(ErrorCode::InvalidArgument, "golden: the file is not a PNG");
        }
    }

    Array<u8> idat(out.texels.allocator());
    usize offset = 8;
    bool have_header = false;
    while (offset + 12 <= file.size()) {
        const u32 length = read_be32(file.data() + offset);
        const u8* type = file.data() + offset + 4;
        const u8* payload = type + 4;
        if (offset + 12 + length > file.size()) {
            return fail(ErrorCode::InvalidArgument,
                        "golden: a chunk runs past the end of the file");
        }
        if (type[0] == 'I' && type[1] == 'H' && type[2] == 'D' && type[3] == 'R') {
            if (length != 13) {
                return fail(ErrorCode::InvalidArgument, "golden: a malformed IHDR");
            }
            out.width = read_be32(payload);
            out.height = read_be32(payload + 4);
            if (payload[8] != 8 || payload[9] != 2 || payload[12] != 0) {
                return fail(ErrorCode::Unsupported,
                            "golden: this decoder reads 8-bit truecolour, non-interlaced PNGs — "
                            "what write_png() produces. Regenerate the reference with "
                            "CY_RENDER_UPDATE_GOLDEN=1");
            }
            have_header = true;
        } else if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T') {
            if (Status appended = append(idat, payload, length); !appended) {
                return appended;
            }
        } else if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D') {
            break;
        }
        offset += 12 + length;
    }
    if (!have_header || out.width == 0 || out.height == 0) {
        return fail(ErrorCode::InvalidArgument, "golden: the PNG has no usable IHDR");
    }

    Array<u8> raw(out.texels.allocator());
    if (Status inflated = inflate_stored(idat.span(), raw); !inflated) {
        return inflated;
    }
    const usize stride = 1 + (static_cast<usize>(out.width) * 3U);
    if (raw.size() != stride * out.height) {
        return fail(ErrorCode::InvalidArgument, "golden: the decoded size is not the image's size");
    }
    if (Status sized = out.texels.resize(static_cast<usize>(out.width) * out.height); !sized) {
        return sized;
    }
    for (u32 y = 0; y < out.height; ++y) {
        const u8* row = raw.data() + (static_cast<usize>(y) * stride);
        if (row[0] != 0) {
            return fail(ErrorCode::Unsupported,
                        "golden: this decoder reads filter-0 rows, which is what write_png() "
                        "produces. Regenerate the reference with CY_RENDER_UPDATE_GOLDEN=1");
        }
        for (u32 x = 0; x < out.width; ++x) {
            const u8* rgb = row + 1 + (static_cast<usize>(x) * 3U);
            out.texels[(static_cast<usize>(y) * out.width) + x] =
                static_cast<u32>(rgb[0]) | (static_cast<u32>(rgb[1]) << 8U) |
                (static_cast<u32>(rgb[2]) << 16U) | 0xFF000000U;
        }
    }
    return ok();
}

namespace {

/// The largest per-channel difference between two texels, in 8-bit steps. Alpha is not compared:
/// every image here is opaque and a difference in it would be a difference in something that is not
/// the picture.
u32 channel_delta(u32 a, u32 b) noexcept {
    u32 worst = 0;
    for (u32 shift = 0; shift < 24; shift += 8) {
        const u32 left = (a >> shift) & 0xFFU;
        const u32 right = (b >> shift) & 0xFFU;
        const u32 delta = left > right ? left - right : right - left;
        worst = delta > worst ? delta : worst;
    }
    return worst;
}

/// Whether a texel has a four-neighbour it differs sharply from. See golden.h for why this is the
/// budget rather than a number somebody picked.
///
/// The neighbours are computed UNSIGNED and bounds-checked by the same comparison that rejects a
/// coordinate past the far edge: `x - 1` at x = 0 wraps to a very large number, which is not less
/// than the width. That is one comparison per axis instead of two, and it removes the signed-to-
/// unsigned crossing a bounds check with negative intermediates would need.
bool on_high_contrast_edge(const Image& image, u32 x, u32 y) noexcept {
    const u32 here = image.at(x, y);
    const u32 neighbours[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
    return std::ranges::any_of(neighbours, [&](const u32(&neighbour)[2]) {
        return neighbour[0] < image.width && neighbour[1] < image.height &&
               channel_delta(here, image.at(neighbour[0], neighbour[1])) > kEdgeContrast;
    });
}

}  // namespace

Comparison compare(const Image& reference, const Image& candidate) noexcept {
    Comparison result;
    if (reference.width != candidate.width || reference.height != candidate.height) {
        return result;
    }
    result.comparable = true;
    for (u32 y = 0; y < reference.height; ++y) {
        for (u32 x = 0; x < reference.width; ++x) {
            const bool edge = on_high_contrast_edge(reference, x, y);
            if (edge) {
                ++result.edge_texels;
            }
            const u32 delta = channel_delta(reference.at(x, y), candidate.at(x, y));
            if (delta > result.max_channel_delta) {
                result.max_channel_delta = delta;
                result.worst_x = x;
                result.worst_y = y;
            }
            if (delta > kChannelTolerance) {
                ++result.differing;
                if (!edge) {
                    ++result.differing_off_edge;
                }
            }
        }
    }
    return result;
}

Status write_difference(const char* path, const Image& reference, const Image& candidate) noexcept {
    Image difference(reference.texels.allocator());
    difference.width = reference.width;
    difference.height = reference.height;
    if (Status sized =
            difference.texels.resize(static_cast<usize>(reference.width) * reference.height);
        !sized) {
        return sized;
    }
    for (u32 y = 0; y < reference.height; ++y) {
        for (u32 x = 0; x < reference.width; ++x) {
            const u32 left = reference.at(x, y);
            const bool same = candidate.width == reference.width &&
                              candidate.height == reference.height &&
                              channel_delta(left, candidate.at(x, y)) <= kChannelTolerance;
            u32 texel = 0xFFFF00FFU;  // magenta: nothing in this scene is magenta
            if (same) {
                // The reference at half intensity, so the magenta reads as an overlay rather than
                // as a second picture.
                texel = 0xFF000000U | ((left & 0x00FEFEFEU) >> 1U);
            }
            difference.texels[(static_cast<usize>(y) * difference.width) + x] = texel;
        }
    }
    return write_png(path, difference);
}

}  // namespace cy::render_test
