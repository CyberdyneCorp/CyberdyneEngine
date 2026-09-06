#include <cy/servers/text/server.h>

#include <cstring>

namespace cy::text {
namespace {

/// The minimal backend's capabilities.
///
/// Every field is false, and that is the honest state at M5 rather than a placeholder: this backend
/// genuinely cannot shape Arabic, reorder a bidirectional paragraph or break Thai. A caller reads
/// this and decides; nothing here approximates any of them. See server.h.
[[nodiscard]] TextCapabilities minimal_capabilities() noexcept {
    TextCapabilities capabilities;
    capabilities.backend = "minimal";
    return capabilities;
}

/// A cheap, stable hash of a shaping request. Only used to find a candidate; the entry's own text
/// is compared before it is served, so a collision costs a comparison and never a wrong result.
[[nodiscard]] u64 hash_of(std::string_view text, const FallbackChain& chain,
                          Direction direction) noexcept {
    u64 hash = 0xCBF2'9CE4'8422'2325ULL;
    const auto mix = [&hash](u64 value) noexcept {
        hash ^= value;
        hash *= 0x0000'0100'0000'01B3ULL;
    };
    for (const char character : text) {
        mix(static_cast<u8>(character));
    }
    for (u32 index = 0; index < chain.count; ++index) {
        mix(chain.faces[index].bits());
    }
    mix(static_cast<u64>(direction));
    return hash;
}

}  // namespace

TextServer::~TextServer() {
    stop();
}

Status TextServer::start(const TextServerConfig& config) noexcept {
    if (running_) {
        return fail(ErrorCode::AlreadyExists, "the text server is already running");
    }
    if (config.backend == BackendKind::Complete) {
        // Named rather than silently downgraded. A caller that asked for shaping and got Latin has
        // a defect it cannot see; a caller told what is missing can decide.
        return fail(ErrorCode::NotImplemented,
                    "the complete backend needs HarfBuzz, ICU and FreeType, none of which is an "
                    "integrated dependency yet; select BackendKind::Minimal or integrate them");
    }
    if (Status started = atlas_.start(config.atlas); !started) {
        return started;
    }
    config_ = config;
    capabilities_ = minimal_capabilities();
    diagnostics_ = {};
    running_ = true;
    return ok();
}

void TextServer::stop() noexcept {
    atlas_.stop();
    faces_.clear();
    shaping_cache_.clear();
    running_ = false;
}

// --- Faces ---------------------------------------------------------------------------------------

const TextServer::Face* TextServer::find_face(FontHandle face) const noexcept {
    if (face.is_null() || face.index() >= faces_.size()) {
        return nullptr;
    }
    const Face& candidate = faces_[face.index()];
    if (!candidate.live || candidate.generation != face.generation()) {
        return nullptr;
    }
    return &candidate;
}

TextServer::Face* TextServer::find_face(FontHandle face) noexcept {
    return const_cast<Face*>(static_cast<const TextServer*>(this)->find_face(face));
}

Expected<FontHandle, Error> TextServer::create_face(const FontDesc& desc,
                                                    const ImageGridFont& grid) noexcept {
    if (!running_) {
        return fail(ErrorCode::Unavailable, "the text server has not been started");
    }
    if (Status valid = grid.validate(); !valid) {
        return make_unexpected(valid.error());
    }
    if (desc.size_pixels <= 0.0f) {
        return fail(ErrorCode::InvalidArgument, "a face size must be positive");
    }
    if (desc.axis_count > kMaxFontAxes) {
        return fail(ErrorCode::OutOfRange, "more variable-font axes than a face may pin");
    }

    Face face;
    face.desc = desc;
    face.grid = grid;
    face.live = true;
    // A grid font's metrics are the grid's: every glyph is a cell, the baseline is where the font
    // says, and the advance is the cell width unless the font gives one. There is no hinting and no
    // scaling — the face's `size_pixels` describes the source rather than resizing it, which is
    // what a bitmap font means and is why a caller that wants two sizes supplies two grids.
    face.metrics.ascent = grid.ascent;
    face.metrics.descent = static_cast<f32>(grid.cell_height) - grid.ascent;
    face.metrics.line_gap = 0.0f;
    face.metrics.x_height = grid.ascent * 0.5f;
    face.metrics.cap_height = grid.ascent;
    face.metrics.space_advance =
        grid.advance > 0.0f ? grid.advance : static_cast<f32>(grid.cell_width);
    face.metrics.monospace = true;

    for (usize index = 0; index < faces_.size(); ++index) {
        if (!faces_[index].live) {
            // The generation moves on REUSE, so a handle to the face that was here answers no.
            face.generation = faces_[index].generation + 1;
            faces_[index] = face;
            return FontHandle::from_slot(static_cast<u32>(index), face.generation);
        }
    }
    face.generation = 1;
    if (Status pushed = faces_.push_back(face); !pushed) {
        return make_unexpected(pushed.error());
    }
    return FontHandle::from_slot(static_cast<u32>(faces_.size() - 1), face.generation);
}

void TextServer::destroy_face(FontHandle face) noexcept {
    Face* found = find_face(face);
    if (found != nullptr) {
        found->live = false;
    }
}

bool TextServer::is_face(FontHandle face) const noexcept {
    return find_face(face) != nullptr;
}

Expected<FontMetrics, Error> TextServer::face_metrics(FontHandle face) const noexcept {
    const Face* found = find_face(face);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "no such face");
    }
    return found->metrics;
}

GlyphIndex TextServer::glyph_for(FontHandle face, Codepoint codepoint) const noexcept {
    const Face* found = find_face(face);
    if (found == nullptr) {
        return kNotdef;
    }
    if (codepoint < found->grid.first_codepoint) {
        return kNotdef;
    }
    const auto cell = static_cast<u32>(codepoint - found->grid.first_codepoint);
    if (cell >= found->grid.glyph_count) {
        return kNotdef;
    }
    // Cell zero is a real glyph in a grid font, and `kNotdef` is zero, so the index is the cell
    // plus one. Without the shift, the space at the start of an ASCII grid would be
    // indistinguishable from "this font has no such character".
    return cell + 1;
}

bool TextServer::has_glyph(FontHandle face, Codepoint codepoint) const noexcept {
    return glyph_for(face, codepoint) != kNotdef;
}

// --- Rasterisation -------------------------------------------------------------------------------

Expected<const GlyphSlot*, Error> TextServer::glyph_slot(FontHandle face,
                                                         GlyphIndex glyph) noexcept {
    const Face* found = find_face(face);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, "no such face");
    }

    GlyphKey key;
    key.face = face;
    key.glyph = glyph;
    if (const GlyphSlot* resident = atlas_.find(key); resident != nullptr) {
        return resident;
    }

    const ImageGridFont& grid = found->grid;
    GlyphMetrics metrics;
    metrics.advance = grid.advance > 0.0f ? grid.advance : static_cast<f32>(grid.cell_width);
    metrics.bearing_x = 0.0f;
    // Y measured DOWN from the baseline, so a glyph whose top is `ascent` above it has a negative
    // bearing. font.h says so; it is repeated here because the sign is the thing that gets flipped.
    metrics.bearing_y = -grid.ascent;
    metrics.width = grid.cell_width;
    metrics.height = grid.cell_height;

    Array<u8> coverage;
    if (Status resized = coverage.resize(static_cast<usize>(grid.cell_width) * grid.cell_height);
        !resized) {
        return make_unexpected(resized.error());
    }

    if (glyph == kNotdef) {
        // The visible box the specification requires: a one-pixel outline of the cell, so a missing
        // character is obvious in a screenshot rather than an absence somebody has to notice.
        ++diagnostics_.notdef_served;
        for (u32 y = 0; y < grid.cell_height; ++y) {
            for (u32 x = 0; x < grid.cell_width; ++x) {
                const bool edge =
                    x == 0 || y == 0 || x + 1 == grid.cell_width || y + 1 == grid.cell_height;
                coverage[(static_cast<usize>(y) * grid.cell_width) + x] = edge ? 255U : 0U;
            }
        }
    } else {
        const u32 cell = glyph - 1;
        const u32 column = cell % grid.columns;
        const u32 row = cell / grid.columns;
        for (u32 y = 0; y < grid.cell_height; ++y) {
            const usize source =
                (static_cast<usize>((row * grid.cell_height) + y) * grid.image_width) +
                static_cast<usize>(column * grid.cell_width);
            std::memcpy(coverage.data() + (static_cast<usize>(y) * grid.cell_width),
                        grid.pixels.data() + source, grid.cell_width);
        }
    }

    return atlas_.insert(key, metrics, Span<const u8>(coverage.data(), coverage.size()));
}

// --- Shaping -------------------------------------------------------------------------------------

FontHandle TextServer::resolve_face(const FallbackChain& chain, Codepoint codepoint) noexcept {
    for (u32 index = 0; index < chain.count; ++index) {
        if (has_glyph(chain.faces[index], codepoint)) {
            if (index != 0) {
                // Counted, because "fonts that trigger fallback frequently" is one of the
                // diagnostics `text-and-fonts` asks for: a primary font that falls back on every
                // second character is the wrong primary font.
                ++diagnostics_.fallbacks_taken;
            }
            return chain.faces[index];
        }
    }
    return chain.count != 0 ? chain.faces[0] : FontHandle{};
}

Status TextServer::shape_uncached(std::string_view text, const FallbackChain& chain,
                                  Direction direction, ShapedRun& out) noexcept {
    out.glyphs.clear();
    out.face = chain.count != 0 ? chain.faces[0] : FontHandle{};
    out.direction = direction;
    out.source_begin = 0;
    out.source_end = static_cast<u32>(text.size());
    out.width = 0.0f;

    if (chain.count == 0) {
        return fail(ErrorCode::InvalidArgument, "shaping needs at least one face");
    }
    if (direction != Direction::LeftToRight) {
        // Refused rather than approximated. `capabilities().bidirectional` and `vertical_layout`
        // are both false, and a backend that produced left-to-right glyphs for a right-to-left
        // request would be lying in a way the caller cannot detect.
        return fail(ErrorCode::Unsupported,
                    "the minimal backend lays out left to right only; query capabilities() first");
    }

    usize cursor = 0;
    while (cursor < text.size()) {
        const auto offset = static_cast<u32>(cursor);
        const Codepoint codepoint = decode_utf8(text, cursor);
        if (codepoint == '\n') {
            // A newline occupies no width and produces no glyph. It is a break opportunity, and the
            // paragraph layout is what acts on it.
            continue;
        }
        const FontHandle face = resolve_face(chain, codepoint);
        const Face* found = find_face(face);
        if (found == nullptr) {
            return fail(ErrorCode::NotFound, "the fallback chain names a face that is not there");
        }

        ShapedGlyph glyph;
        glyph.face = face;
        glyph.glyph = glyph_for(face, codepoint);
        glyph.missing = glyph.glyph == kNotdef;
        glyph.source_offset = offset;
        glyph.advance = found->grid.advance > 0.0f ? found->grid.advance
                                                   : static_cast<f32>(found->grid.cell_width);
        glyph.offset = Vec2{out.width, 0.0f};
        out.width += glyph.advance;
        if (Status pushed = out.glyphs.push_back(glyph); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status TextServer::shape(std::string_view text, const FallbackChain& chain, Direction direction,
                         ShapedRun& out) noexcept {
    if (!running_) {
        return fail(ErrorCode::Unavailable, "the text server has not been started");
    }
    if (config_.shaping_cache_entries == 0) {
        ++diagnostics_.shaping_cache_misses;
        return shape_uncached(text, chain, direction, out);
    }

    const u64 hash = hash_of(text, chain, direction);
    for (ShapingCacheEntry& entry : shaping_cache_) {
        if (entry.hash != hash || entry.direction != direction ||
            entry.text.size() != text.size()) {
            continue;
        }
        if (std::memcmp(entry.text.data(), text.data(), text.size()) != 0) {
            continue;
        }
        if (entry.chain.count != chain.count) {
            continue;
        }
        bool same_chain = true;
        for (u32 index = 0; index < chain.count; ++index) {
            same_chain = same_chain && entry.chain.faces[index] == chain.faces[index];
        }
        if (!same_chain) {
            continue;
        }
        // A hit is a copy of the glyph array, which is what a caller gets anyway; the work saved is
        // the decoding, the fallback walk and the measurement.
        out.glyphs.clear();
        if (Status appended = out.glyphs.append(
                Span<const ShapedGlyph>(entry.run.glyphs.data(), entry.run.glyphs.size()));
            !appended) {
            return appended;
        }
        out.face = entry.run.face;
        out.direction = entry.run.direction;
        out.source_begin = entry.run.source_begin;
        out.source_end = entry.run.source_end;
        out.width = entry.run.width;
        ++diagnostics_.shaping_cache_hits;
        entry.used_at = ++shaping_clock_;
        return ok();
    }

    ++diagnostics_.shaping_cache_misses;
    if (Status shaped = shape_uncached(text, chain, direction, out); !shaped) {
        return shaped;
    }

    ShapingCacheEntry entry;
    entry.used_at = ++shaping_clock_;
    entry.hash = hash;
    entry.chain = chain;
    entry.direction = direction;
    if (Status appended = entry.text.append(Span<const char>(text.data(), text.size()));
        !appended) {
        // A cache that could not remember is not a failure to shape: the run is already produced.
        return ok();
    }
    if (Status appended =
            entry.run.glyphs.append(Span<const ShapedGlyph>(out.glyphs.data(), out.glyphs.size()));
        !appended) {
        return ok();
    }
    entry.run.face = out.face;
    entry.run.direction = out.direction;
    entry.run.source_begin = out.source_begin;
    entry.run.source_end = out.source_end;
    entry.run.width = out.width;

    if (shaping_cache_.size() >= config_.shaping_cache_entries) {
        // Least recently used, like the atlas. A shaping cache that evicted the newest entry would
        // throw away the string being typed.
        usize oldest = 0;
        for (usize index = 1; index < shaping_cache_.size(); ++index) {
            if (shaping_cache_[index].used_at < shaping_cache_[oldest].used_at) {
                oldest = index;
            }
        }
        shaping_cache_[oldest] = std::move(entry);
        return ok();
    }
    (void)shaping_cache_.push_back(std::move(entry));
    return ok();
}

Expected<Vec2, Error> TextServer::measure(std::string_view text,
                                          const FallbackChain& chain) noexcept {
    ShapedRun run;
    if (Status shaped = shape(text, chain, Direction::LeftToRight, run); !shaped) {
        return make_unexpected(shaped.error());
    }
    Expected<FontMetrics, Error> metrics = face_metrics(run.face);
    if (!metrics) {
        return make_unexpected(metrics.error());
    }
    return Vec2{run.width, metrics.value().ascent + metrics.value().descent};
}

// --- Layout --------------------------------------------------------------------------------------

Status TextServer::build_line(std::string_view text, u32 begin, u32 end, const FallbackChain& chain,
                              Direction direction, TextLine& out) noexcept {
    if (Status shaped = shape(text.substr(begin, end - begin), chain, direction, out.run());
        !shaped) {
        return shaped;
    }
    // The run's own offsets are relative to the slice it was shaped from; the line's are the
    // paragraph's. Rebasing here rather than shaping the whole text and slicing the glyphs is what
    // lets the shaping cache hit on a line that has not changed while the ones above it have.
    for (ShapedGlyph& glyph : out.run().glyphs) {
        glyph.source_offset += begin;
    }
    out.run().source_begin = begin;
    out.run().source_end = end;

    Expected<FontMetrics, Error> metrics = face_metrics(out.run().face);
    if (!metrics) {
        return make_unexpected(metrics.error());
    }
    out.set_metrics(metrics.value());
    return ok();
}

Status TextServer::layout_line(std::string_view text, const FallbackChain& chain,
                               TextLine& out) noexcept {
    if (!running_) {
        return fail(ErrorCode::Unavailable, "the text server has not been started");
    }
    return build_line(text, 0, static_cast<u32>(text.size()), chain, Direction::LeftToRight, out);
}

Status TextServer::layout_paragraph(std::string_view text, const FallbackChain& chain,
                                    const ParagraphOptions& options, TextParagraph& out) noexcept {
    return layout_paragraph_with_objects(text, chain, options, {}, out);
}

Status TextServer::layout_paragraph_with_objects(std::string_view text, const FallbackChain& chain,
                                                 const ParagraphOptions& options,
                                                 Span<const InlineObject> objects,
                                                 TextParagraph& out) noexcept {
    if (!running_) {
        return fail(ErrorCode::Unavailable, "the text server has not been started");
    }
    if (options.direction != Direction::LeftToRight) {
        return fail(ErrorCode::Unsupported,
                    "the minimal backend lays out left to right only; query capabilities() first");
    }
    for (usize index = 1; index < objects.size(); ++index) {
        if (objects[index].source_offset < objects[index - 1].source_offset) {
            // Refused rather than sorted. The caller's order is the one its own model holds, and
            // silently reordering it would make the placements it gets back refer to something
            // else.
            return fail(ErrorCode::InvalidArgument,
                        "inline objects must be in ascending source order");
        }
    }
    out.clear();

    Array<BreakOpportunity> breaks;
    if (Status found = find_break_opportunities(text, breaks); !found) {
        return found;
    }

    // Greedy line breaking: take the last opportunity that still fits. It is what every interface
    // layout does, and the alternative — Knuth-Plass over the whole paragraph — buys even spacing
    // that only justified prose benefits from and costs a quadratic pass.
    u32 line_begin = 0;
    usize next_break = 0;
    while (line_begin <= static_cast<u32>(text.size())) {
        u32 line_end = static_cast<u32>(text.size());
        bool mandatory = false;

        if (options.width > 0.0f && options.overflow != Overflow::Clip) {
            u32 candidate = line_begin;
            f32 width = 0.0f;
            for (usize index = next_break; index < breaks.size(); ++index) {
                if (breaks[index].source_offset <= line_begin) {
                    continue;
                }
                ShapedRun probe;
                if (Status shaped =
                        shape(text.substr(line_begin, breaks[index].source_offset - line_begin),
                              chain, Direction::LeftToRight, probe);
                    !shaped) {
                    return shaped;
                }
                if (probe.width > options.width && candidate != line_begin) {
                    break;
                }
                candidate = breaks[index].source_offset;
                width = probe.width;
                if (breaks[index].mandatory) {
                    mandatory = true;
                    break;
                }
                if (width > options.width) {
                    break;
                }
            }
            if (candidate != line_begin) {
                line_end = candidate;
            } else if (options.overflow == Overflow::CharacterWrap) {
                // No opportunity fits, so break between characters. A word longer than the box has
                // to go somewhere, and clipping it would lose the rest of the paragraph with it.
                usize cursor = line_begin;
                f32 width_so_far = 0.0f;
                while (cursor < text.size()) {
                    const usize character = cursor;
                    (void)decode_utf8(text, cursor);
                    ShapedRun probe;
                    if (Status shaped = shape(text.substr(line_begin, cursor - line_begin), chain,
                                              Direction::LeftToRight, probe);
                        !shaped) {
                        return shaped;
                    }
                    if (probe.width > options.width && character != line_begin) {
                        cursor = character;
                        break;
                    }
                    width_so_far = probe.width;
                }
                (void)width_so_far;
                line_end = static_cast<u32>(cursor);
            }
        }

        TextLine line;
        if (Status built =
                build_line(text, line_begin, line_end, chain, Direction::LeftToRight, line);
            !built) {
            return built;
        }
        if (Status pushed = out.lines_.push_back(std::move(line)); !pushed) {
            return pushed;
        }

        if (line_end >= static_cast<u32>(text.size())) {
            break;
        }
        line_begin = line_end;
        while (next_break < breaks.size() && breaks[next_break].source_offset <= line_begin) {
            ++next_break;
        }
        (void)mandatory;

        if (options.max_lines != 0 && out.lines_.size() >= options.max_lines) {
            out.truncated_ = true;
            break;
        }
    }

    // Alignment is applied to the ORIGINS rather than baked into glyph offsets, so re-aligning a
    // paragraph does not reshape it. See layout.h.
    f32 y = 0.0f;
    f32 widest = 0.0f;
    for (usize index = 0; index < out.lines_.size(); ++index) {
        const TextLine& line = out.lines_[index];
        f32 x = 0.0f;
        if (options.width > 0.0f) {
            switch (options.alignment) {
                case Alignment::Start:
                case Alignment::Justify:
                    x = 0.0f;
                    break;
                case Alignment::Centre:
                    x = (options.width - line.width()) * 0.5f;
                    break;
                case Alignment::End:
                    x = options.width - line.width();
                    break;
            }
        }
        if (Status pushed = out.origins_.push_back(Vec2{x, y}); !pushed) {
            return pushed;
        }
        widest = line.width() > widest ? line.width() : widest;
        y += line.height() * options.line_spacing;
    }
    out.size_ = Vec2{options.width > 0.0f ? options.width : widest, y};

    // Inline objects, placed on the line their source offset falls in. Their advance is not fed
    // back into the break decision, which is the honest limitation of doing this in one pass: an
    // object wider than the space left on its line overhangs rather than pushing the following text
    // down. A caller that needs the other behaviour reserves the width in its own text.
    for (const InlineObject& object : objects) {
        InlinePlacement placement;
        placement.id = object.id;
        placement.size = object.size;
        for (usize index = 0; index < out.lines_.size(); ++index) {
            const ShapedRun& run = out.lines_[index].run();
            const bool last = index + 1 == out.lines_.size();
            if (object.source_offset < run.source_end || last) {
                placement.line = static_cast<u32>(index);
                const f32 along = out.lines_[index].offset_of(object.source_offset);
                placement.position = Vec2{out.origins_[index].x + along,
                                          out.origins_[index].y + out.lines_[index].baseline() +
                                              object.baseline_offset - object.size.y};
                break;
            }
        }
        if (Status pushed = out.placements_.push_back(placement); !pushed) {
            return pushed;
        }
    }
    return ok();
}

// --- Diagnostics ---------------------------------------------------------------------------------

TextDiagnostics TextServer::diagnostics() const noexcept {
    TextDiagnostics combined = atlas_.diagnostics();
    combined.shaping_cache_hits = diagnostics_.shaping_cache_hits;
    combined.shaping_cache_misses = diagnostics_.shaping_cache_misses;
    combined.fallbacks_taken = diagnostics_.fallbacks_taken;
    combined.notdef_served = diagnostics_.notdef_served;
    combined.atlas_occupancy = atlas_.occupancy();
    return combined;
}

void TextServer::reset_diagnostics() noexcept {
    diagnostics_ = {};
    atlas_.reset_diagnostics();
}

}  // namespace cy::text
