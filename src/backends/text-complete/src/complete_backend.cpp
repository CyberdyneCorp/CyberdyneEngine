// SPDX-License-Identifier: MIT
#include <cy/backends/text/complete_backend.h>

#include "internal.h"

#include <utility>

namespace cy::text {

/// One opened face: the same bytes, at the same size and instance, in both libraries.
struct CompleteFace {
    detail::FreeTypeFace* freetype = nullptr;
    detail::HarfBuzzFace* harfbuzz = nullptr;
    /// A WOFF face's decompressed sfnt, which HarfBuzz shapes from; empty for every other format,
    /// whose bytes HarfBuzz reads where the caller keeps them.
    Array<u8> sfnt;
    FontDesc desc;
    FontMetrics metrics;
    bool live = false;
};

struct CompleteBackendState {
    explicit CompleteBackendState(Allocator& allocator) noexcept : faces(allocator) {}

    detail::FreeTypeLibrary* library = nullptr;
    /// Indexed by `BackendFace`. A closed slot is reused by the next `open_face`.
    Array<CompleteFace> faces;
};

namespace {

[[nodiscard]] CompleteFace* face_of(CompleteBackendState* state, BackendFace face) noexcept {
    if (state == nullptr || face >= state->faces.size() || !state->faces[face].live) {
        return nullptr;
    }
    return &state->faces[face];
}

/// Whether the bytes are a WOFF (version 1) file, which FreeType reads and HarfBuzz does not.
[[nodiscard]] bool is_woff(Span<const u8> bytes) noexcept {
    return bytes.size() >= 4 && bytes[0] == 'w' && bytes[1] == 'O' && bytes[2] == 'F' &&
           bytes[3] == 'F';
}

[[nodiscard]] Unexpected<Error> no_such_face() noexcept {
    return fail(ErrorCode::NotFound, "the complete text backend has no such face");
}

}  // namespace

CompleteTextBackend::CompleteTextBackend(Allocator& allocator) noexcept : allocator_(&allocator) {}

CompleteTextBackend::~CompleteTextBackend() {
    stop();
}

Status CompleteTextBackend::start() noexcept {
    if (state_ != nullptr) {
        return fail(ErrorCode::AlreadyExists, "the complete text backend is already running");
    }
    auto* state = detail::create<CompleteBackendState>(*allocator_, *allocator_);
    if (state == nullptr) {
        return fail(ErrorCode::OutOfMemory, "the complete text backend could not allocate itself");
    }
    Expected<detail::FreeTypeLibrary*, Error> library = detail::freetype_open_library(*allocator_);
    if (!library) {
        detail::destroy(*allocator_, state);
        return make_unexpected(library.error());
    }
    state->library = library.value();
    state_ = state;
    return ok();
}

void CompleteTextBackend::stop() noexcept {
    if (state_ == nullptr) {
        return;
    }
    for (BackendFace face = 0; face < state_->faces.size(); ++face) {
        close_face(face);
    }
    detail::freetype_close_library(*allocator_, state_->library);
    detail::destroy(*allocator_, state_);
    state_ = nullptr;
}

TextCapabilities CompleteTextBackend::capabilities() const noexcept {
    TextCapabilities capabilities;
    capabilities.complex_shaping = true;
    capabilities.bidirectional = true;
    capabilities.colour_glyphs = true;
    capabilities.variable_fonts = true;
    capabilities.signed_distance_fields = true;
    capabilities.outline_fonts = true;
    // Still false, each for a stated reason (README.md): dictionary breaking needs ICU's data,
    // which this build does not ship; subpixel positioning multiplies the atlas by four for an
    // effect a distance field already gives; vertical layout and font-table kashida are not built.
    capabilities.backend = name();
    return capabilities;
}

Expected<BackendFace, Error> CompleteTextBackend::open_face(const FontDesc& desc,
                                                            const FontSource& source) noexcept {
    if (state_ == nullptr) {
        return fail(ErrorCode::Unavailable, "the complete text backend has not been started");
    }
    if (desc.mode == RenderMode::SubpixelLcd) {
        return fail(ErrorCode::Unsupported,
                    "LCD subpixel rendering is not built: it needs a three-channel coverage atlas "
                    "and a known panel order, and a distance field covers the scaling case");
    }
    Expected<detail::FreeTypeFace*, Error> freetype =
        detail::freetype_open_face(*allocator_, *state_->library, desc, source);
    if (!freetype) {
        return make_unexpected(freetype.error());
    }
    CompleteFace face;
    FontSource shaped = source;
    if (is_woff(source.bytes)) {
        if (Status unpacked = detail::freetype_sfnt(*freetype.value(), face.sfnt); !unpacked) {
            detail::freetype_close_face(*allocator_, freetype.value());
            return make_unexpected(unpacked.error());
        }
        shaped = FontSource{Span<const u8>(face.sfnt.data(), face.sfnt.size()), 0};
    }
    Expected<detail::HarfBuzzFace*, Error> harfbuzz =
        detail::harfbuzz_open_face(*allocator_, desc, shaped);
    if (!harfbuzz) {
        detail::freetype_close_face(*allocator_, freetype.value());
        return make_unexpected(harfbuzz.error());
    }

    face.freetype = freetype.value();
    face.harfbuzz = harfbuzz.value();
    face.desc = desc;
    face.metrics = detail::freetype_metrics(*face.freetype);
    // The space is measured by the shaper rather than by the rasteriser, because it is the shaper's
    // advances that lay the text out: a hinted raster advance would justify by a different width.
    face.metrics.space_advance =
        detail::harfbuzz_advance(*face.harfbuzz, detail::freetype_glyph_for(*face.freetype, ' '));
    face.live = true;

    // The sfnt moves with the face, and the HarfBuzz blob points into its heap block, which a move
    // of the array does not relocate.
    for (BackendFace slot = 0; slot < state_->faces.size(); ++slot) {
        if (!state_->faces[slot].live) {
            state_->faces[slot] = std::move(face);
            return slot;
        }
    }
    if (Status pushed = state_->faces.push_back(std::move(face)); !pushed) {
        detail::harfbuzz_close_face(*allocator_, face.harfbuzz);
        detail::freetype_close_face(*allocator_, face.freetype);
        return make_unexpected(pushed.error());
    }
    return static_cast<BackendFace>(state_->faces.size() - 1);
}

void CompleteTextBackend::close_face(BackendFace face) noexcept {
    CompleteFace* found = face_of(state_, face);
    if (found == nullptr) {
        return;
    }
    detail::harfbuzz_close_face(*allocator_, found->harfbuzz);
    detail::freetype_close_face(*allocator_, found->freetype);
    found->harfbuzz = nullptr;
    found->freetype = nullptr;
    found->sfnt.clear();
    found->live = false;
}

FontMetrics CompleteTextBackend::face_metrics(BackendFace face) const noexcept {
    const CompleteFace* found = face_of(state_, face);
    return found != nullptr ? found->metrics : FontMetrics{};
}

GlyphIndex CompleteTextBackend::glyph_for(BackendFace face, Codepoint codepoint) const noexcept {
    const CompleteFace* found = face_of(state_, face);
    return found != nullptr ? detail::freetype_glyph_for(*found->freetype, codepoint) : kNotdef;
}

Status CompleteTextBackend::rasterise(BackendFace face, GlyphIndex glyph,
                                      GlyphRaster& out) noexcept {
    CompleteFace* found = face_of(state_, face);
    if (found == nullptr) {
        return no_such_face();
    }
    // A colour glyph is a colour raster whatever the face was asked for: an emoji in a
    // distance-field face is still an emoji, and a distance field of its outline would be a
    // silhouette.
    if (found->desc.mode != RenderMode::SignedDistanceField ||
        detail::freetype_has_colour(*found->freetype)) {
        Status rasterised = detail::freetype_rasterise(*found->freetype, glyph, out);
        if (!rasterised || out.format == PixelFormat::Colour ||
            found->desc.mode != RenderMode::SignedDistanceField) {
            return rasterised;
        }
    }
    detail::GlyphOutline outline;
    if (Status traced = detail::freetype_outline(*found->freetype, glyph, outline); !traced) {
        return traced;
    }
    return detail::msdf_generate(outline, found->desc.distance_range, out);
}

Status CompleteTextBackend::shape(BackendFace face, const ShapeRequest& request,
                                  Array<BackendGlyph>& out) noexcept {
    CompleteFace* found = face_of(state_, face);
    if (found == nullptr) {
        return no_such_face();
    }
    return detail::harfbuzz_shape(*found->harfbuzz, request, out);
}

Status CompleteTextBackend::glyph_closure(BackendFace face, Span<const Codepoint> codepoints,
                                          Array<GlyphIndex>& out) noexcept {
    CompleteFace* found = face_of(state_, face);
    if (found == nullptr) {
        return no_such_face();
    }
    return detail::harfbuzz_closure(*found->harfbuzz, codepoints, out);
}

Status CompleteTextBackend::resolve_bidi(std::string_view text, ParagraphDirection direction,
                                         BidiResult& out) noexcept {
    if (!detail::icu_available()) {
        return TextBackend::resolve_bidi(text, direction, out);
    }
    return detail::icu_resolve_bidi(text, direction, out);
}

}  // namespace cy::text
