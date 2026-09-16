#ifndef CY_IMPORT_BLOCK_ENCODE_H
#define CY_IMPORT_BLOCK_ENCODE_H
// Block compression: BC7, BC5 and BC4. M11.c task 6.1b.
//
// ================================================================================================
// WHAT THIS CLOSES, AND WHAT IT DELIBERATELY DOES NOT
// ================================================================================================
//
// `texture.h` has said since M5 that "BC7 and ASTC encoding is a third-party dependency ... and none
// is integrated at M5. `select_format` therefore names the format the cook WOULD produce, the
// payload carries uncompressed mips, and the cooked header records both". M11.c's beauty shot is the
// first content this project ships, and shipping it with `encoded = false` would mean the first
// textured picture in this engine's history costs four times the memory its own format table claims.
//
// So the two desktop formats the artefact needs are encoded here:
//
//   | format | what it stores | what is implemented |
//   |---|---|---|
//   | **BC4** | one channel, 8 bytes a block | the full 8-value interpolated mode |
//   | **BC5** | two channels, 16 bytes a block | two BC4 blocks, red then green |
//   | **BC7** | four channels, 16 bytes a block | **MODE 6 ONLY** — one partition, RGBA at 7 bits plus a p-bit, 4-bit indices |
//
// **BC7 mode 6 and not all eight modes, and that is a real limitation rather than a simplification.**
// Mode 6 is the single-partition, full-alpha mode: one line through RGBA and sixteen steps along it.
// The line is found by the block's PRINCIPAL AXIS and then least-squares refined against the indices
// it produced — a bounding box is the wrong line whenever two channels are anti-correlated, which a
// red-to-green transition is, and `unit.import` keeps that case with both numbers in it.
//
// What mode 6 cannot express is THREE OR MORE colour populations, which do not lie on any line;
// modes 0 through 3's partitions are the answer to those and this encoder has none. The cost is
// measured rather than asserted: `unit.import`'s BC7 cases report peak error against the source for
// a ramp (1 of 255), an anti-correlated pair (1) and a three-population block, and
// `docs/design/beauty-shot.md` publishes the artefact's own figure.
//
// **BC6H and ASTC are NOT here** and the gap is declared rather than hidden: `select_format` still
// names them for HDR and for mobile, and `encode_mip_chain` refuses them by name, which leaves
// `CookedTexture::encoded` false for exactly those two and keeps the header honest. See
// `m11c.toml`'s `texture-encoders-cover-the-formats-they-name`.
//
// ================================================================================================
// THE ENCODER IS A PURE FUNCTION AND THAT IS A COOK REQUIREMENT
// ================================================================================================
//
// `build-and-packaging` requires a cook to be deterministic — the same source produces the same
// bytes, twice, from empty. Nothing below is threaded, nothing reads a clock, nothing depends on
// iteration order over a container, and every arithmetic step is integer or f32 with no fused
// contraction that a different -march could reassociate into a different block.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/import/texture.h>

namespace cy::import {

/// Whether this build can actually encode the format, as opposed to naming it.
///
/// The separation is the point: `is_block_compressed` says what the format IS, and this says what
/// this build can PRODUCE. A cook writes `CookedTexture::encoded` from this and from nothing else.
[[nodiscard]] bool can_encode(TextureFormat format) noexcept;

/// Encode one 4x4 block of RGBA8 into BC7 mode 6. Writes 16 bytes.
void encode_bc7_block(const u8 rgba[64], u8 out[16]) noexcept;

/// Encode one 4x4 block of a single channel into BC4. Writes 8 bytes.
void encode_bc4_block(const u8 values[16], u8 out[8]) noexcept;

/// Encode a whole uncompressed mip chain into `format`.
///
/// `levels` is the chain `generate_mips` produced: level 0 at the base dimensions, each level
/// halved, `channels` interleaved bytes a texel. The output is the same chain in blocks, level by
/// level, four-by-four, with the edge of a non-multiple-of-four level clamped rather than wrapped —
/// which is the padding `import_texture`'s `block-size-mismatch` diagnostic warns about.
///
/// Refuses a format this build cannot encode, by name.
[[nodiscard]] Status encode_mip_chain(Span<const u8> levels, u32 width, u32 height, u32 mip_count,
                                      u32 channels, TextureFormat format, Array<u8>& out) noexcept;

}  // namespace cy::import

#endif  // CY_IMPORT_BLOCK_ENCODE_H
