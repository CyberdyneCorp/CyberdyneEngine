#ifndef CY_SERVERS_TEXT_ATLAS_H
#define CY_SERVERS_TEXT_ATLAS_H
// The glyph atlas: where a rasterised glyph lives, and what happens when it runs out. M5 task 5.3.
//
// `text-and-fonts` — "Glyph rasterisation and atlas": "Glyphs SHALL be rasterised on demand and
// cached in **glyph atlases**, keyed by font, size, variation axes, transform, and rendering mode
// ... Atlases SHALL be packed dynamically, grow up to a device limit, and evict least-recently-used
// glyphs under pressure."
//
// --- THE KEY IS THE WHOLE OF THE CORRECTNESS -----------------------------------------------------
//
// Two glyphs that differ in ANY of the things the key names must not share a slot, and the failure
// when they do is silent: text at 12 pixels renders the raster made for 11, or a bold face renders
// the regular one's coverage. So `GlyphKey` carries every one of them, `FontHandle` folds in the
// size, the axes and the render mode (a face is created per instance — see font.h), and what is
// left in the key is the face, the glyph and the subpixel offset.
//
// --- EVICTION IS LEAST-RECENTLY-USED, AND THRASHING IS COUNTED -----------------------------------
//
// "WHEN many fonts and sizes are used THEN least-recently-used glyphs SHALL be evicted, and
// thrashing SHALL be reported as a diagnostic."
//
// A glyph re-rasterised after having been evicted since its last use is counted as a thrash. That
// number, and not occupancy, is what tells a developer the atlas is too small: an atlas at 60%
// occupancy that thrashes every frame has a fragmentation problem, and one at 99% that never
// thrashes is exactly the right size.
//
// --- WHY GROWTH DOUBLES AND WHY IT REPACKS -------------------------------------------------------
//
// Growing means allocating a larger texture and re-packing every live glyph into it, which costs a
// copy of everything currently resident. It is done at a doubling so the cost is paid a logarithmic
// number of times, and it stops at a device limit the caller supplies — because the alternative to
// stopping is a texture the device refuses to create, reported as a failure to draw text.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/geometry.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/servers/text/font.h>
#include <cy/servers/text/text.h>

namespace cy::text {

/// Everything that distinguishes one rasterisation of one glyph from another.
///
/// The face folds in size, axes and render mode; see the header. What remains is the glyph itself
/// and where within the pixel it was positioned.
struct GlyphKey {
    FontHandle face;
    GlyphIndex glyph = kNotdef;
    /// The horizontal subpixel bucket, in [0, `kSubpixelPositions`). Always zero when the backend's
    /// `subpixel_positioning` capability is false, which keeps the cache one entry per glyph.
    u8 subpixel = 0;

    friend bool operator==(const GlyphKey& a, const GlyphKey& b) noexcept {
        return a.face == b.face && a.glyph == b.glyph && a.subpixel == b.subpixel;
    }
};

/// How finely a glyph may be positioned within a pixel when subpixel positioning is on.
///
/// Four is the usual choice: it removes almost all of the spacing error and multiplies the cache by
/// four rather than by the sixteen a finer split would.
inline constexpr u8 kSubpixelPositions = 4;

/// Where a glyph sits in the atlas, and what it costs to draw.
struct GlyphSlot {
    /// The rectangle in atlas pixels.
    IRect rect;
    GlyphMetrics metrics;
    /// The atlas page. Always zero at M5: the atlas grows rather than adding pages, and a caller
    /// that batches by texture batches by this.
    u32 page = 0;
    /// Whether the slot holds colour rather than coverage. Always false until a backend reports the
    /// `colour_glyphs` capability.
    bool colour = false;
};

/// What the atlas is allowed to do.
struct GlyphAtlasConfig {
    /// The size it starts at. Small on purpose: an interface that draws forty glyphs should not
    /// allocate four megabytes to hold them.
    u32 initial_extent = 256;
    /// The largest it may become, which is the device's texture limit or a budget below it.
    u32 maximum_extent = 4096;
    /// Transparent pixels between packed glyphs, so that bilinear sampling of one does not reach
    /// into another. One is enough for point sampling and two for a mip chain; the default is the
    /// safe one.
    u32 padding = 1;
};

/// A dynamically packed, least-recently-used glyph cache over one growing texture.
///
/// Not thread-safe. Text layout happens on one thread per frame in every design this engine has,
/// and a lock around a cache probe would cost more than the probe.
class GlyphAtlas {
public:
    GlyphAtlas() noexcept = default;

    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;

    /// Allocate the initial texture. Fails with `InvalidArgument` on a configuration that cannot
    /// work — a maximum below the initial extent, an extent that is not a power of two.
    [[nodiscard]] Status start(const GlyphAtlasConfig& config) noexcept;
    void stop() noexcept;
    [[nodiscard]] bool is_running() const noexcept { return extent_ != 0; }

    /// The slot for a key, or null. Touching the entry, so the caller's read is what keeps it alive
    /// — which is the whole of what "least recently USED" means.
    [[nodiscard]] const GlyphSlot* find(const GlyphKey& key) noexcept;

    /// Insert a rasterised glyph, growing or evicting to make room.
    ///
    /// `coverage` is `metrics.width * metrics.height` bytes, one per pixel, top row first. It is
    /// copied into the atlas texture; the caller may free it on return.
    ///
    /// Fails with `OutOfRange` when the glyph is larger than the maximum extent — a 4096-pixel
    /// glyph is a font asking for something no atlas can hold, and enlarging the atlas would not
    /// help.
    [[nodiscard]] Expected<const GlyphSlot*, Error> insert(const GlyphKey& key,
                                                           const GlyphMetrics& metrics,
                                                           Span<const u8> coverage) noexcept;

    /// The atlas texture's coverage, `extent * extent` bytes. What an uploader hands to the RHI.
    [[nodiscard]] Span<const u8> pixels() const noexcept;
    [[nodiscard]] u32 extent() const noexcept { return extent_; }
    [[nodiscard]] usize live_glyphs() const noexcept { return entries_.size(); }

    /// The rectangle that has changed since `clear_dirty`, or an empty rectangle when nothing has.
    ///
    /// An uploader re-uploads this rather than the whole texture, which is the difference between
    /// a few hundred bytes per frame and four megabytes.
    [[nodiscard]] IRect dirty_region() const noexcept { return dirty_; }
    void clear_dirty() noexcept;

    [[nodiscard]] const TextDiagnostics& diagnostics() const noexcept { return diagnostics_; }
    void reset_diagnostics() noexcept;
    /// Occupancy, in [0, 1]. Read together with `diagnostics().thrashes`; see the header.
    [[nodiscard]] f32 occupancy() const noexcept;

private:
    struct Entry {
        GlyphKey key;
        GlyphSlot slot;
        /// The value of `clock_` when this entry was last found or inserted.
        u64 used_at = 0;
    };

    /// Repack every live glyph into a texture of `extent`. Used by growth and by the compaction
    /// that eviction triggers when the packer has fragmented.
    [[nodiscard]] Status repack(u32 extent) noexcept;
    /// Drop the least recently used entries until `count` have gone.
    void evict(usize count) noexcept;
    [[nodiscard]] usize find_index(const GlyphKey& key) const noexcept;
    void mark_dirty(const IRect& rect) noexcept;

    u32 extent_ = 0;
    GlyphAtlasConfig config_{};
    geom::AtlasPacker packer_;
    Array<u8> pixels_;
    Array<Entry> entries_;
    /// Keys evicted since the last reset, so that re-inserting one is recognisable as a thrash
    /// rather than as an ordinary first sighting. Bounded: it holds the last `kThrashMemory` keys,
    /// because the question it answers — "is this atlas too small for the working set" — is about
    /// the recent past.
    Array<GlyphKey> recently_evicted_;
    static constexpr usize kThrashMemory = 256;
    u64 clock_ = 0;
    IRect dirty_{};
    TextDiagnostics diagnostics_{};
};

}  // namespace cy::text

#endif  // CY_SERVERS_TEXT_ATLAS_H
