#ifndef CY_CORE_ASSETS_COMPRESSION_H
#define CY_CORE_ASSETS_COMPRESSION_H
// The engine's compression interface. Task 3.3.6.
//
// `core-assets-and-io` — "Package format": payload compression is per-entry and selectable, with
// **seekable framing so partial reads do not decompress the whole entry**, and already-compressed
// data such as block-compressed texture blocks is not recompressed by default.
//
// THE CODEC IS BEHIND THIS INTERFACE AND NOWHERE ELSE. `thirdparty-dependencies` requires an
// integrated dependency's types appear nowhere but its own adapter; zstd appears in
// src/compression.cpp and in no header. A caller names `CompressionMethod::Zstd`, never a zstd
// type, never a zstd constant, and never a zstd error.
//
// WHAT IS AVAILABLE AT M1. Zstd and None. The specification also names LZ4 and Deflate; neither is
// in deps/manifest.toml, and adding a dependency is a manifest decision rather than a module's, so
// both are declared enumerators that `compression_method_available()` reports as absent and that
// every entry point refuses with ErrorCode::Unsupported naming the method. That is the seam: the
// day a codec is pinned, this file gains a case and no caller changes.
//
// FRAMING. A framed payload is a sequence of independently compressed frames of a fixed
// UNCOMPRESSED size, plus an index of where each landed. Reading one mip level of a texture then
// touches only the frames covering it — `decompress_range` takes a reader callback rather than a
// buffer precisely so that the bytes outside those frames are never read from disk at all, which is
// the specification's scenario stated as something a test can count.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::assets {

enum class CompressionMethod : u8 {
    /// Stored. Also what an already-compressed payload uses; see `default_method_for`.
    None = 0,
    Zstd = 1,
    /// Declared, not implemented — no LZ4 is pinned. See the note at the top of this file.
    Lz4 = 2,
    /// Declared, not implemented — no zlib is pinned. See the note at the top of this file.
    Deflate = 3,
};

/// The enumerator's own spelling, for a diagnostic. Never null.
const char* compression_method_name(CompressionMethod method) noexcept;

/// True when this build can actually compress and decompress with the method.
[[nodiscard]] bool compression_method_available(CompressionMethod method) noexcept;

/// The three levels the engine names, so that a call site says what it wants rather than a number
/// whose meaning belongs to whichever codec is behind the interface.
enum class CompressionLevel : i8 {
    Fast = 1,
    Balanced = 2,
    Dense = 3,
};

/// The largest output `compress` can produce for `input_size` bytes. A compressor can grow
/// incompressible input, so a caller that sized its buffer to the input would be wrong exactly on
/// the data that is hardest to notice.
[[nodiscard]] usize compress_bound(CompressionMethod method, usize input_size) noexcept;

/// Compress one block. Returns the number of bytes written to `output`.
[[nodiscard]] Expected<usize, Error> compress(CompressionMethod method, CompressionLevel level,
                                              const void* input, usize input_size, void* output,
                                              usize output_capacity) noexcept;

/// Decompress one block into a buffer of exactly the expected size. `output_size` is the size the
/// payload had before compression, which every caller here knows from the directory entry — a
/// decompressor that discovered it from the stream would be one a corrupt stream could steer.
[[nodiscard]] Status decompress(CompressionMethod method, const void* input, usize input_size,
                                void* output, usize output_size) noexcept;

// --- Seekable framing -------------------------------------------------------------------------

/// The default uncompressed span of one frame. 64 KiB is small enough that a single mip level or
/// audio window touches few frames, and large enough that the per-frame ratio loss is slight.
inline constexpr u32 kDefaultFrameBytes = 64 * 1024;

/// Where one frame landed in the compressed payload. `uncompressed_size` is the frame size for
/// every frame but the last, which is recorded rather than derived so that a reader validates the
/// index instead of trusting arithmetic over a corrupt one.
struct FrameIndexEntry {
    u64 compressed_offset = 0;
    u32 compressed_size = 0;
    u32 uncompressed_size = 0;
};

/// Compress `input` as independently decompressible frames of `frame_bytes` uncompressed each.
///
/// `output` and `index` are appended to and are cleared first. A method of `None` still produces
/// frames — the framing is what makes a partial read possible, and an uncompressed entry that
/// wanted partial reads without them would need a second code path.
[[nodiscard]] Status compress_framed(CompressionMethod method, CompressionLevel level,
                                     const void* input, usize input_size, u32 frame_bytes,
                                     Array<u8>& output, Array<FrameIndexEntry>& index) noexcept;

/// Reads `size` bytes at `offset` within the compressed payload into `destination`.
///
/// This is how `decompress_range` reaches the bytes it needs: a package reader supplies one that
/// reads from the file at the entry's base offset, a test supplies one that counts. Returning an
/// error aborts the range read with that error.
using FrameReader = Status (*)(void* user, u64 offset, void* destination, usize size) noexcept;

/// Decompress the half-open uncompressed range `[offset, offset + size)`.
///
/// Only the frames covering the range are read through `reader` and decompressed; everything else
/// in the payload is never touched. This is the "partial read of a large asset" requirement, and
/// `frames_touched` is what a test asserts on.
[[nodiscard]] Status decompress_range(CompressionMethod method, Span<const FrameIndexEntry> index,
                                      u32 frame_bytes, FrameReader reader, void* reader_user,
                                      u64 offset, void* destination, usize size,
                                      u32* frames_touched = nullptr) noexcept;

/// The total uncompressed size an index describes.
[[nodiscard]] u64 framed_uncompressed_size(Span<const FrameIndexEntry> index) noexcept;

// --- Policy -----------------------------------------------------------------------------------

/// Whether a payload is already in a compressed form. Block-compressed texture blocks, Ogg and
/// Opus audio, and anything already framed by an importer are, and recompressing them costs load
/// time to save almost nothing.
enum class PayloadForm : u8 {
    /// Cooked bytes with ordinary redundancy: meshes, scenes, shader binaries, uncompressed audio.
    Compressible = 0,
    /// Already compressed by its own format. BC/ASTC texture blocks, Ogg, Opus, PNG.
    AlreadyCompressed = 1,
};

/// The method the package writer picks when the caller does not name one.
///
/// `AlreadyCompressed` yields `None` — the specification's "SHALL NOT be recompressed by default".
/// A caller that has measured otherwise for its own content passes an explicit method; the word in
/// the requirement is *default*, and this is that default in one place rather than in each writer.
[[nodiscard]] CompressionMethod default_method_for(PayloadForm form) noexcept;

}  // namespace cy::assets

#endif  // CY_CORE_ASSETS_COMPRESSION_H
