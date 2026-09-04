#pragma once
// Golden images: the committed reference format, the difference metric, and the diff artefact.
// Task 6.3.
//
// ================================================================================================
// WHY PNG, AND WHY THIS PNG
// ================================================================================================
//
// `testing-and-quality` requires the difference to be "inspectable in review". A reviewer looks at
// a pull request in a browser, so the reference has to be a format the browser renders — which
// rules out the PPM `samples/03-first-light --capture` writes and rules in PNG.
//
// The encoder below writes PNG with **stored (uncompressed) deflate blocks**. That is a valid PNG
// by the specification and every viewer reads it; what it is not is a small one. The trade is
// deliberate: a real deflate encoder is several hundred lines of Huffman tables that would have to
// be correct forever for a gate to keep working, against a reference file that is about 1.3 times
// the size of the raw pixels. At 192x108 that is 62 KiB a reference, and there are two of them.
//
// THE DECODER ONLY READS WHAT THE ENCODER WRITES: 8-bit truecolour, no interlace, filter 0 on every
// row, and stored deflate blocks. A reference re-encoded by an external tool is REJECTED with a
// message saying so rather than being read wrongly — which is the honest failure, and which is also
// why regeneration goes through `CY_RENDER_UPDATE_GOLDEN` (see tests/render/README.md) rather than
// through whatever is on somebody's PATH.
//
// ================================================================================================
// THE TOLERANCE IS DERIVED FROM THE REFERENCE, NOT TUNED UNTIL IT PASSED
// ================================================================================================
//
// Two numbers, and neither of them was chosen by running the test until it went green.
//
// `kChannelTolerance` = 2, in 8-bit units. The colour target is `Rgba8Unorm` and the last thing the
// shader does is `pow(x, 1/2.2)`, after which the hardware quantises to 1/255. Two conformant
// implementations may land on either side of a quantisation boundary — that is one step — and the
// transcendental itself may differ in its last few f32 bits, which is far below a step everywhere
// except at a boundary it has already been counted at. So one step is the physical difference and
// two is one step of headroom over it. A difference of three is a different shading result, not a
// different rounding, and this metric says so.
//
// `edge_texels` is the budget for texels that CANNOT be held to that, and it is measured off the
// reference image rather than declared. A minified checkerboard has texels where the sampled
// coordinate sits exactly on the boundary between two texels of the source, and there the last bit
// of a UV computation selects one of two greys that differ by 0x88 — a rounding difference with a
// non-rounding consequence. Those texels are exactly the ones with a high-contrast neighbour, so
// `compare()` counts them in the REFERENCE (an implementation-independent property of the committed
// file) and hands back the count. The case then asserts `differing <= edge_texels`: a rounding
// difference can only appear where a rounding difference can matter.
//
// On the machine the reference was generated on, `differing` is 0 and the case says so separately.
// That is the stronger claim and it is the one that catches a regression; the budget above is what
// keeps the gate meaningful on a machine that is not this one.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::render_test {

/// An 8-bit RGBA image, row-major from the TOP-LEFT — the layout a Vulkan image copy produces and
/// the layout a PNG row wants, so nothing in this file flips anything.
struct Image {
    explicit Image(Allocator& allocator) noexcept : texels(allocator) {}

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    u32 width = 0;
    u32 height = 0;
    /// Little-endian byte order in a `u32`: red in the low byte, which is what `Rgba8Unorm` means
    /// on every platform this engine builds for.
    Array<u32> texels;

    [[nodiscard]] u32 at(u32 x, u32 y) const noexcept {
        return texels[(static_cast<usize>(y) * width) + x];
    }
};

/// One texel differs when any channel differs by more than this many 8-bit steps. See the header
/// comment for where the number comes from.
inline constexpr u32 kChannelTolerance = 2;

/// A neighbouring texel is a high-contrast one when any channel differs by more than this. The
/// checkerboard's own contrast is 0x88, and an anti-aliased geometric edge crosses this within a
/// texel or two of the silhouette.
inline constexpr u32 kEdgeContrast = 32;

/// What comparing two images found.
struct Comparison {
    /// Texels over `kChannelTolerance`.
    u32 differing = 0;
    /// Texels in the reference with a high-contrast four-neighbour. The derived budget.
    u32 edge_texels = 0;
    /// Of the differing texels, how many were NOT on such an edge. This is the number that must be
    /// zero: a difference away from an edge is a shading difference.
    u32 differing_off_edge = 0;
    u32 max_channel_delta = 0;
    u32 worst_x = 0;
    u32 worst_y = 0;
    /// False when the two images are not the same size, which is a different failure from a
    /// different picture and is reported as one.
    bool comparable = false;
};

/// Compare a candidate against a committed reference. Makes no assertions; the case does.
[[nodiscard]] Comparison compare(const Image& reference, const Image& candidate) noexcept;

/// Read a PNG this module wrote. Fails, naming what it found, on anything else.
[[nodiscard]] Status read_png(const char* path, Image& out) noexcept;

/// Write a PNG: 8-bit truecolour, filter 0, stored deflate blocks.
[[nodiscard]] Status write_png(const char* path, const Image& image) noexcept;

/// Write the difference artefact: the reference at half intensity, with every differing texel in
/// magenta. What a reviewer opens when a golden case fails.
[[nodiscard]] Status write_difference(const char* path, const Image& reference,
                                      const Image& candidate) noexcept;

/// Fill `out` from a span of `Rgba8Unorm` texels a render produced.
[[nodiscard]] Status adopt(Image& out, Span<const u32> texels, u32 width, u32 height) noexcept;

}  // namespace cy::render_test
