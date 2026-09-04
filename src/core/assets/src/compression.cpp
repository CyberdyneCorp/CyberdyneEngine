// The engine's compression interface, over the pinned zstd. Task 3.3.6.
//
// The only file in the engine that names zstd. Everything above it speaks CompressionMethod.

#include <cy/core/assets/compression.h>

#include <cy/core/assets/diagnostics.h>
#include <cy/core/base/assert.h>

#include <zstd.h>

#include <cstring>

namespace cy::assets {
namespace {

/// zstd's level scale for the three the engine names. 1 is its fast end, 3 its default, 12 a dense
/// setting still fast enough for a cook step; the maximum, 22, costs minutes for a few percent.
int zstd_level(CompressionLevel level) noexcept {
    switch (level) {
        case CompressionLevel::Fast:
            return 1;
        case CompressionLevel::Balanced:
            return 3;
        case CompressionLevel::Dense:
            return 12;
    }
    return 3;
}

/// The one place a method that has no implementation is refused, so every entry point refuses it
/// the same way and with the same message.
Status require_available(CompressionMethod method) noexcept {
    switch (method) {
        case CompressionMethod::None:
        case CompressionMethod::Zstd:
            return ok();
        case CompressionMethod::Lz4:
            return fail(ErrorCode::Unsupported,
                        "compression method LZ4 is declared but no LZ4 is pinned in "
                        "deps/manifest.toml");
        case CompressionMethod::Deflate:
            return fail(ErrorCode::Unsupported,
                        "compression method Deflate is declared but no zlib is pinned in "
                        "deps/manifest.toml");
    }
    return fail(ErrorCode::InvalidArgument, "unknown compression method");
}

}  // namespace

const char* compression_method_name(CompressionMethod method) noexcept {
    switch (method) {
        case CompressionMethod::None:
            return "none";
        case CompressionMethod::Zstd:
            return "zstd";
        case CompressionMethod::Lz4:
            return "lz4";
        case CompressionMethod::Deflate:
            return "deflate";
    }
    return "unknown";
}

bool compression_method_available(CompressionMethod method) noexcept {
    return method == CompressionMethod::None || method == CompressionMethod::Zstd;
}

usize compress_bound(CompressionMethod method, usize input_size) noexcept {
    switch (method) {
        case CompressionMethod::None:
            return input_size;
        case CompressionMethod::Zstd:
            return ZSTD_compressBound(input_size);
        case CompressionMethod::Lz4:
        case CompressionMethod::Deflate:
            // No implementation, so no bound. The call that would use it refuses first.
            return 0;
    }
    return 0;
}

Expected<usize, Error> compress(CompressionMethod method, CompressionLevel level, const void* input,
                                usize input_size, void* output, usize output_capacity) noexcept {
    if (Status available = require_available(method); !available) {
        return make_unexpected(available.error());
    }
    if (input_size != 0 && input == nullptr) {
        return fail(ErrorCode::InvalidArgument, "compress was given a null input of non-zero size");
    }

    if (method == CompressionMethod::None) {
        if (output_capacity < input_size) {
            return fail(ErrorCode::BufferTooSmall, "the stored-method output buffer is too small");
        }
        if (input_size != 0) {
            std::memcpy(output, input, input_size);
        }
        return input_size;
    }

    const usize written =
        ZSTD_compress(output, output_capacity, input, input_size, zstd_level(level));
    counters::record_compression(input_size, ZSTD_isError(written) != 0u ? 0 : written);
    if (ZSTD_isError(written) != 0u) {
        // The codec's own message is not propagated: it is a third-party string, and Error carries
        // a pointer that must outlive the call. The classification is what a caller acts on.
        return fail(ErrorCode::BufferTooSmall, "the compressor refused the output buffer");
    }
    return written;
}

Status decompress(CompressionMethod method, const void* input, usize input_size, void* output,
                  usize output_size) noexcept {
    if (Status available = require_available(method); !available) {
        return available;
    }

    if (method == CompressionMethod::None) {
        if (input_size != output_size) {
            return fail(ErrorCode::Io, "a stored payload's size does not match its recorded size");
        }
        if (output_size != 0) {
            std::memcpy(output, input, output_size);
        }
        return ok();
    }

    const usize produced = ZSTD_decompress(output, output_size, input, input_size);
    if (ZSTD_isError(produced) != 0u) {
        return fail(ErrorCode::Io, "the compressed payload is malformed or truncated");
    }
    if (produced != output_size) {
        return fail(ErrorCode::Io, "the payload decompressed to a different size than recorded");
    }
    return ok();
}

Status compress_framed(CompressionMethod method, CompressionLevel level, const void* input,
                       usize input_size, u32 frame_bytes, Array<u8>& output,
                       Array<FrameIndexEntry>& index) noexcept {
    if (Status available = require_available(method); !available) {
        return available;
    }
    if (frame_bytes == 0) {
        return fail(ErrorCode::InvalidArgument, "a frame of zero bytes would never terminate");
    }

    output.clear();
    index.clear();

    const auto* cursor = static_cast<const u8*>(input);
    usize remaining = input_size;
    u64 compressed_offset = 0;

    while (remaining != 0) {
        const u32 span = remaining < frame_bytes ? static_cast<u32>(remaining) : frame_bytes;
        const usize bound = compress_bound(method, span);
        const usize base = output.size();
        if (Status grown = output.resize(base + bound); !grown) {
            return grown;
        }

        Expected<usize, Error> written =
            compress(method, level, cursor, span, output.data() + base, bound);
        if (!written) {
            return make_unexpected(written.error());
        }
        // A frame that compressed larger than it started is stored: the reader is told which by the
        // frame's own compressed size matching its uncompressed size, so no per-frame flag is
        // needed and a pathological payload never grows the package.
        usize frame_size = written.value();
        if (frame_size >= span) {
            std::memcpy(output.data() + base, cursor, span);
            frame_size = span;
        }
        if (Status shrunk = output.resize(base + frame_size); !shrunk) {
            return shrunk;
        }

        if (Status added = index.push_back(
                FrameIndexEntry{compressed_offset, static_cast<u32>(frame_size), span});
            !added) {
            return added;
        }

        compressed_offset += frame_size;
        cursor += span;
        remaining -= span;
    }
    return ok();
}

Status decompress_range(CompressionMethod method, Span<const FrameIndexEntry> index,
                        u32 frame_bytes, FrameReader reader, void* reader_user, u64 offset,
                        void* destination, usize size, u32* frames_touched) noexcept {
    if (frames_touched != nullptr) {
        *frames_touched = 0;
    }
    if (Status available = require_available(method); !available) {
        return available;
    }
    if (reader == nullptr) {
        return fail(ErrorCode::InvalidArgument, "decompress_range needs a frame reader");
    }
    if (frame_bytes == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a frame index with a zero frame size is malformed");
    }
    if (size == 0) {
        return ok();
    }
    const u64 total = framed_uncompressed_size(index);
    if (offset > total || size > total - offset) {
        return fail(ErrorCode::OutOfRange, "the requested range lies outside the payload");
    }

    // Only the frames covering the range. The first and last are decompressed whole and clipped;
    // everything before and after is never read.
    const auto first = static_cast<usize>(offset / frame_bytes);
    const auto last = static_cast<usize>((offset + size - 1) / frame_bytes);
    CY_ASSERT_MSG(last < index.size(), "the frame index is shorter than the size it describes");

    // One scratch frame, reused. `frame_bytes` bounds both the compressed read and the
    // decompressed result, because a frame that grew was stored instead (see compress_framed).
    Array<u8> compressed;
    Array<u8> plain;
    if (Status grown = compressed.resize(frame_bytes); !grown) {
        return grown;
    }
    if (Status grown = plain.resize(frame_bytes); !grown) {
        return grown;
    }

    auto* out = static_cast<u8*>(destination);
    usize produced = 0;
    for (usize frame = first; frame <= last; ++frame) {
        const FrameIndexEntry& entry = index[frame];
        if (entry.compressed_size > frame_bytes || entry.uncompressed_size > frame_bytes) {
            return fail(ErrorCode::Io, "a frame index entry is larger than the frame size");
        }
        if (Status read = reader(reader_user, entry.compressed_offset, compressed.data(),
                                 entry.compressed_size);
            !read) {
            return read;
        }

        const u8* frame_plain = nullptr;
        if (entry.compressed_size == entry.uncompressed_size) {
            frame_plain = compressed.data();  // stored, either by method or by growth
        } else {
            if (Status expanded = decompress(method, compressed.data(), entry.compressed_size,
                                             plain.data(), entry.uncompressed_size);
                !expanded) {
                return expanded;
            }
            frame_plain = plain.data();
        }

        const u64 frame_start = static_cast<u64>(frame) * frame_bytes;
        const u64 clip_start = offset > frame_start ? offset - frame_start : 0;
        const u64 frame_end = frame_start + entry.uncompressed_size;
        const u64 wanted_end = offset + size;
        const u64 clip_end = (wanted_end < frame_end ? wanted_end : frame_end) - frame_start;
        const auto span = static_cast<usize>(clip_end - clip_start);
        std::memcpy(out + produced, frame_plain + clip_start, span);
        produced += span;
        counters::record_decompression(span, 1);

        if (frames_touched != nullptr) {
            ++*frames_touched;
        }
    }

    if (produced != size) {
        return fail(ErrorCode::Io, "the frame index does not cover the requested range");
    }
    return ok();
}

u64 framed_uncompressed_size(Span<const FrameIndexEntry> index) noexcept {
    u64 total = 0;
    for (const FrameIndexEntry& entry : index) {
        total += entry.uncompressed_size;
    }
    return total;
}

CompressionMethod default_method_for(PayloadForm form) noexcept {
    return form == PayloadForm::AlreadyCompressed ? CompressionMethod::None
                                                  : CompressionMethod::Zstd;
}

}  // namespace cy::assets
