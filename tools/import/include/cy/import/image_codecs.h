#ifndef CY_IMPORT_IMAGE_CODECS_H
#define CY_IMPORT_IMAGE_CODECS_H
// PNG and baseline JPEG decoding, and the DEFLATE decoder PNG needs. M11.c task 6.1b.
//
// ================================================================================================
// WHY THESE ARE WRITTEN HERE RATHER THAN LINKED
// ================================================================================================
//
// `texture.h`'s own note since M5 said PNG "needs a DEFLATE decoder and JPEG a DCT one, both are
// third-party dependencies ... and neither is integrated here", and `m11b:image-codecs` has been
// RED ever since. M11.c cannot ship content while the importer cannot read the two formats content
// arrives in, and `thirdparty-dependencies` makes every linked dependency a governed decision with
// a licence record and a vendoring cost.
//
// Both decoders below are **the read half only** and are deliberately small: DEFLATE with fixed and
// dynamic Huffman codes, PNG's five filters over 8- and 16-bit greyscale, palette, RGB and RGBA, and
// BASELINE sequential JPEG — huffman-coded, 8-bit, 4:4:4 through 4:2:0 chroma subsampling. What they
// do NOT read is stated where it is refused, in the error message, so a file this build cannot open
// says which feature it used rather than "unsupported".
//
// NO ENCODER. Nothing here writes PNG or JPEG. `tests/render/golden.cpp` already writes the stored
// PNG the project's captures use, and a second writer would be a second format.
//
// ================================================================================================
// WHAT REFUSES, AND WHY EVERY REFUSAL NAMES ITS FEATURE
// ================================================================================================
//
// An importer that answers "unsupported" teaches a project nothing. Every refusal below names the
// exact feature of the format that was used — an interlaced PNG, a progressive JPEG, an arithmetic
// entropy coder — because that is the sentence a person acts on.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/import/texture.h>

namespace cy::import {

/// Inflate a zlib stream (RFC 1950 framing over RFC 1951 DEFLATE) into `out`.
///
/// `out` is appended to, not cleared. The checksum is verified: a stream whose Adler-32 does not
/// match is a corrupt file, and a decoder that ignored it would hand the importer plausible noise.
[[nodiscard]] Status inflate_zlib(Span<const u8> bytes, Array<u8>& out) noexcept;

/// Decode a PNG image into the importer's 8-bit interleaved layout, top row first.
///
/// 16-bit samples are narrowed to 8; PNG stores them big-endian and the high byte is the value.
/// Palette images are expanded, with `tRNS` becoming an alpha channel where it is present.
[[nodiscard]] Expected<ImageData, Error> decode_png(Span<const u8> bytes) noexcept;

/// Decode a baseline sequential JPEG into the importer's layout.
///
/// Three components become RGB and one becomes a single channel; JPEG carries no alpha at all, so
/// an alpha channel is never fabricated here.
[[nodiscard]] Expected<ImageData, Error> decode_jpeg(Span<const u8> bytes) noexcept;

}  // namespace cy::import

#endif  // CY_IMPORT_IMAGE_CODECS_H
