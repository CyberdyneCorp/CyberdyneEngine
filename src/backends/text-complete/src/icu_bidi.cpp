// SPDX-License-Identifier: MIT
// ICU: the one translation unit that names it. See internal.h for what crosses out of here.
//
// Compiled in every build of this module; with CY_TEXT_ICU off it is the two-line stub at the
// bottom, and the server falls back to src/text/'s algorithm — which is what "the option drops ICU"
// means, rather than a configuration in which the backend's capability silently changes meaning.
#include "internal.h"

#include <cy/text/unicode.h>
#include <cy_features.h>

#if defined(CY_TEXT_ICU)
#    include <unicode/ubidi.h>
#endif

namespace cy::text::detail {

#if defined(CY_TEXT_ICU)

namespace {

/// Owns a `UBiDi` for the length of one call.
class BidiObject {
public:
    BidiObject() noexcept = default;
    ~BidiObject() {
        if (bidi_ != nullptr) {
            ubidi_close(bidi_);
        }
    }
    BidiObject(const BidiObject&) = delete;
    BidiObject& operator=(const BidiObject&) = delete;
    BidiObject(BidiObject&&) = delete;
    BidiObject& operator=(BidiObject&&) = delete;

    [[nodiscard]] UBiDi* open(i32 length, UErrorCode& error) noexcept {
        bidi_ = ubidi_openSized(length, 0, &error);
        return bidi_;
    }

private:
    UBiDi* bidi_ = nullptr;
};

[[nodiscard]] UBiDiLevel paragraph_level_of(ParagraphDirection direction) noexcept {
    switch (direction) {
        case ParagraphDirection::LeftToRight:
            return UBIDI_LTR;
        case ParagraphDirection::RightToLeft:
            return UBIDI_RTL;
        case ParagraphDirection::Auto:
            break;
    }
    // P2 and P3 — the first strong character decides, left to right when there is none — which is
    // what `cy::text::resolve_levels` does for `Auto`.
    return UBIDI_DEFAULT_LTR;
}

/// ICU works in UTF-16; the engine in UTF-8. Encode, remembering for every UTF-16 unit the byte the
/// codepoint it belongs to started at, so the levels can be mapped straight back.
[[nodiscard]] Status to_utf16(std::string_view text, Array<UChar>& units,
                              Array<u32>& origins) noexcept {
    usize cursor = 0;
    while (cursor < text.size()) {
        const auto begin = static_cast<u32>(cursor);
        const Codepoint codepoint = decode_utf8(text, cursor);
        Status status = ok();
        if (codepoint >= 0x10000) {
            const Codepoint value = codepoint - 0x10000;
            status = units.push_back(static_cast<UChar>(0xD800 + (value >> 10U)));
            status = status ? origins.push_back(begin) : status;
            status =
                status ? units.push_back(static_cast<UChar>(0xDC00 + (value & 0x3FFU))) : status;
        } else {
            status = units.push_back(static_cast<UChar>(codepoint));
        }
        status = status ? origins.push_back(begin) : status;
        if (!status) {
            return status;
        }
    }
    return origins.push_back(static_cast<u32>(text.size()));
}

}  // namespace

bool icu_available() noexcept {
    return true;
}

Status icu_resolve_bidi(std::string_view text, ParagraphDirection direction,
                        BidiResult& out) noexcept {
    out.runs.clear();
    out.approximated = false;
    out.overflowed = false;
    if (text.empty()) {
        // ICU refuses a null text even at length zero, and an empty array's data is null. An empty
        // paragraph has no runs and the level it was asked for — left to right when it was asked
        // to decide, since there is no strong character to decide by.
        out.paragraph_level = direction == ParagraphDirection::RightToLeft ? 1U : 0U;
        return ok();
    }
    Array<UChar> units;
    Array<u32> origins;
    if (Status encoded = to_utf16(text, units, origins); !encoded) {
        return encoded;
    }
    const auto length = static_cast<i32>(units.size());
    UErrorCode error = U_ZERO_ERROR;
    BidiObject object;
    UBiDi* bidi = object.open(length, error);
    if (U_SUCCESS(error) != 0) {
        ubidi_setPara(bidi, units.data(), length, paragraph_level_of(direction), nullptr, &error);
    }
    const UBiDiLevel* levels =
        U_SUCCESS(error) != 0 && length > 0 ? ubidi_getLevels(bidi, &error) : nullptr;
    if (U_FAILURE(error) != 0) {
        return fail(ErrorCode::Internal, u_errorName(error));
    }
    out.paragraph_level = bidi != nullptr ? ubidi_getParaLevel(bidi) : 0;

    // One run per maximal stretch of one level, in byte offsets, coalesced the way src/text/'s
    // algorithm coalesces them so the two can be compared run for run.
    for (i32 unit = 0; unit < length; ++unit) {
        const u32 begin = origins[static_cast<usize>(unit)];
        const u32 end = origins[static_cast<usize>(unit) + 1];
        if (begin == end) {
            continue;  // the first half of a surrogate pair; the second carries its bytes
        }
        BidiRun* last = out.runs.empty() ? nullptr : &out.runs[out.runs.size() - 1];
        if (last != nullptr && last->level == levels[unit] && last->end == begin) {
            last->end = end;
            continue;
        }
        BidiRun run;
        run.begin = begin;
        run.end = end;
        run.level = levels[unit];
        if (Status pushed = out.runs.push_back(run); !pushed) {
            return pushed;
        }
    }
    return ok();
}

#else

bool icu_available() noexcept {
    return false;
}

Status icu_resolve_bidi(std::string_view /*text*/, ParagraphDirection /*direction*/,
                        BidiResult& /*out*/) noexcept {
    return fail(ErrorCode::Unsupported, "ICU is not in this build (CY_TEXT_ICU is off)");
}

#endif

}  // namespace cy::text::detail
