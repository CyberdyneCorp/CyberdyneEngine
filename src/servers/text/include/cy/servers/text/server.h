#ifndef CY_SERVERS_TEXT_SERVER_H
#define CY_SERVERS_TEXT_SERVER_H
// `TextServer` — the engine's one interface to fonts and text. M5 task 5.3.
//
// `text-and-fonts` — "Engine-owned text interface": "`TextServer` SHALL be the engine-defined
// interface for fonts and text layout. All engine and game code SHALL use it; no HarfBuzz, ICU, or
// FreeType type SHALL appear outside the backend. The interface SHALL cover: font loading and
// querying, glyph rasterisation and atlas management, text shaping, line breaking, justification,
// cursor and hit-testing, and text measurement."
//
// --- WHAT IS HERE AT M5, AND WHAT THE INTERFACE PROMISES ANYWAY ---------------------------------
//
// Everything the specification lists is on this interface. What differs between M5 and M8 is what
// the BACKEND behind it can do, and a caller finds that out from `capabilities()` rather than from
// which functions exist — because an interface that grew functions as backends landed would make
// every caller a compile-time fork.
//
// The minimal backend, which is the only one at M5:
//   * shapes one glyph per codepoint, left to right, from an image-grid font;
//   * breaks lines on spaces, hyphens and newlines, without a dictionary;
//   * justifies by distributing space between words, without kashida;
//   * rasterises by copying a grid cell, so `RenderMode` is honoured only as `Monochrome` and
//     `Grayscale` — both of which a grid font already is.
//
// Everything it cannot do is `false` in its capabilities and is refused with a diagnosis rather
// than approximated. `shape` of Arabic returns the codepoints in logical order and says
// `complex_shaping` is false; it does not pretend to have joined them.
//
// --- THE SERVER OWNS ITS STATE, WHICH IS WHAT MAKES IT A SERVER ---------------------------------
//
// Faces, the glyph atlas and the shaping cache all live here, addressed by handle, and nothing
// above this interface holds a pointer into any of them. That is `engine-architecture`'s definition
// of a server, and it is what lets an editor, a game's interface and a debug overlay share one
// atlas rather than three.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/servers/text/atlas.h>
#include <cy/servers/text/font.h>
#include <cy/servers/text/layout.h>
#include <cy/servers/text/text.h>

#include <string_view>

namespace cy::text {

/// Which backend to build.
enum class BackendKind : u8 {
    /// Left-to-right, no shaping, no ICU. The only one at M5 and the one a size-constrained build
    /// keeps.
    Minimal = 0,
    /// HarfBuzz, ICU and FreeType. Selecting it before those dependencies are integrated fails at
    /// `start` with a message naming what is missing, rather than silently falling back — a caller
    /// that asked for Arabic and got Latin has a bug it cannot see.
    Complete = 1,
};

struct TextServerConfig {
    BackendKind backend = BackendKind::Minimal;
    GlyphAtlasConfig atlas;
    /// How many shaped runs to remember. `text-and-fonts`: "shaped results SHALL be cached keyed by
    /// the run's content and parameters". Zero disables the cache, which is what a measurement of
    /// the cache's own value does.
    u32 shaping_cache_entries = 256;
};

/// The engine's text server.
///
/// Not thread-safe: it owns a mutable atlas and a mutable cache, and layout happens on one thread
/// per frame. A caller that lays out text on several threads gives each one a server, or measures
/// on one and draws on many.
class TextServer {
public:
    TextServer() noexcept = default;
    ~TextServer();

    TextServer(const TextServer&) = delete;
    TextServer& operator=(const TextServer&) = delete;

    [[nodiscard]] Status start(const TextServerConfig& config) noexcept;
    void stop() noexcept;
    [[nodiscard]] bool is_running() const noexcept { return running_; }

    /// What this build's backend can do. Queried, never inferred; see text.h.
    [[nodiscard]] const TextCapabilities& capabilities() const noexcept { return capabilities_; }

    // --- Fonts -----------------------------------------------------------------------------------

    /// Create a face from an image-grid font.
    ///
    /// The face owns the description and NOT the pixels: the grid's `pixels` span must outlive the
    /// face, which a cooked font asset held by the asset system satisfies. See font.h.
    [[nodiscard]] Expected<FontHandle, Error> create_face(const FontDesc& desc,
                                                          const ImageGridFont& grid) noexcept;

    /// Destroy a face. Its glyphs leave the atlas at the next insertion that needs the room, rather
    /// than immediately: evicting them here would cost a pass over the atlas for a face that is
    /// usually being replaced by another with the same glyphs.
    void destroy_face(FontHandle face) noexcept;

    [[nodiscard]] bool is_face(FontHandle face) const noexcept;
    [[nodiscard]] Expected<FontMetrics, Error> face_metrics(FontHandle face) const noexcept;

    /// The glyph a face uses for a codepoint, or `kNotdef`.
    [[nodiscard]] GlyphIndex glyph_for(FontHandle face, Codepoint codepoint) const noexcept;

    /// Whether a face has a glyph for a codepoint. What the fallback chain walks.
    [[nodiscard]] bool has_glyph(FontHandle face, Codepoint codepoint) const noexcept;

    // --- Rasterisation -------------------------------------------------------------------------

    /// The atlas slot for a glyph, rasterising it if it is not resident.
    ///
    /// This is the one entry point that puts a glyph in the atlas, so it is the one place the
    /// rasterisation and thrash counters move.
    [[nodiscard]] Expected<const GlyphSlot*, Error> glyph_slot(FontHandle face,
                                                               GlyphIndex glyph) noexcept;

    [[nodiscard]] const GlyphAtlas& atlas() const noexcept { return atlas_; }
    [[nodiscard]] GlyphAtlas& atlas() noexcept { return atlas_; }

    // --- Shaping and layout ---------------------------------------------------------------------

    /// Shape one run of text with one face and its fallbacks.
    ///
    /// The result is cached, keyed by the text's content and every parameter that affects it, and a
    /// hit costs a hash and a comparison. `TextDiagnostics::shaping_cache_hits` is how a caller
    /// finds out whether its own call pattern is benefiting.
    [[nodiscard]] Status shape(std::string_view text, const FallbackChain& chain,
                               Direction direction, ShapedRun& out) noexcept;

    /// Measure text without producing glyph positions.
    ///
    /// Faster than shaping only when the cache misses; a caller that is about to draw the text
    /// should shape it and read the run's width instead of calling both.
    [[nodiscard]] Expected<Vec2, Error> measure(std::string_view text,
                                                const FallbackChain& chain) noexcept;

    /// Lay out one line, with no wrapping and no alignment.
    [[nodiscard]] Status layout_line(std::string_view text, const FallbackChain& chain,
                                     TextLine& out) noexcept;

    /// Lay out a wrapped, aligned block.
    [[nodiscard]] Status layout_paragraph(std::string_view text, const FallbackChain& chain,
                                          const ParagraphOptions& options,
                                          TextParagraph& out) noexcept;

    /// Lay out a block with objects flowed into it.
    ///
    /// The objects must be in ascending `source_offset` order; an out-of-order list is refused
    /// rather than silently reordered, because the caller's order is the one its own model holds.
    [[nodiscard]] Status layout_paragraph_with_objects(std::string_view text,
                                                       const FallbackChain& chain,
                                                       const ParagraphOptions& options,
                                                       Span<const InlineObject> objects,
                                                       TextParagraph& out) noexcept;

    // --- Diagnostics ----------------------------------------------------------------------------

    /// The atlas's counters plus this server's own. See `text-and-fonts`, "Font import and
    /// diagnostics".
    [[nodiscard]] TextDiagnostics diagnostics() const noexcept;
    void reset_diagnostics() noexcept;

private:
    /// One created face.
    ///
    /// Defined here rather than hidden behind a pointer: `Array<T>` needs a complete `T` to
    /// construct, so a nested type declared and not defined would mean either a pimpl indirection
    /// on every face lookup or an allocation this server does not otherwise need. What it costs is
    /// that a reader of this header sees the state; what it buys is that the state is one struct
    /// rather than an opaque handle to one.
    struct Face {
        FontDesc desc;
        /// Held by value; its pixels are held by reference and must outlive the face. See font.h.
        ImageGridFont grid;
        FontMetrics metrics;
        u32 generation = 0;
        bool live = false;
    };

    /// One remembered shaping result.
    struct ShapingCacheEntry {
        u64 hash = 0;
        /// The text itself, copied, so a hit is decided by comparison rather than by trusting the
        /// hash. A collision then costs a comparison and never a wrong result.
        Array<char> text;
        FallbackChain chain;
        Direction direction = Direction::LeftToRight;
        ShapedRun run;
        u64 used_at = 0;
    };

    [[nodiscard]] const Face* find_face(FontHandle face) const noexcept;
    [[nodiscard]] Face* find_face(FontHandle face) noexcept;
    /// The face in `chain` that has `codepoint`, or the primary when none does.
    [[nodiscard]] FontHandle resolve_face(const FallbackChain& chain, Codepoint codepoint) noexcept;
    [[nodiscard]] Status shape_uncached(std::string_view text, const FallbackChain& chain,
                                        Direction direction, ShapedRun& out) noexcept;
    [[nodiscard]] Status build_line(std::string_view text, u32 begin, u32 end,
                                    const FallbackChain& chain, Direction direction,
                                    TextLine& out) noexcept;

    bool running_ = false;
    TextServerConfig config_{};
    TextCapabilities capabilities_{};
    GlyphAtlas atlas_;
    /// Faces, addressed by handle. A plain array with a generation per slot rather than
    /// `HandlePool`: a face is a description and two spans, the count is in the tens, and the
    /// pool's chunked allocation would be machinery for nothing.
    Array<Face> faces_;
    Array<ShapingCacheEntry> shaping_cache_;
    /// A monotonic tick, bumped by every shaping request. It is NOT one of the diagnostics
    /// counters: using the hit count as the clock would leave every entry at zero in a workload
    /// that only ever misses, and the least-recently-used eviction would then always pick the first
    /// slot.
    u64 shaping_clock_ = 0;
    TextDiagnostics diagnostics_{};
};

}  // namespace cy::text

#endif  // CY_SERVERS_TEXT_SERVER_H
