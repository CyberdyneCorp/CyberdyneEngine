// The property tables. M8.b task 9.4.
//
// RANGE TABLES, NOT A TRIE. The covered set is a few dozen ranges and a linear scan over them costs
// less than the indirection a two-level trie needs — and a trie built from ranges this sparse is
// mostly empty pages. If the coverage grows to the whole of Unicode this becomes the wrong shape;
// that day it is one function to replace and the callers do not change.

#include <cy/text/unicode.h>

#include <algorithm>

namespace cy::text {
namespace {

struct BidiRange {
    Codepoint first;
    Codepoint last;
    BidiClass value;
};

/// Ordered by `first`, and every entry is from the Unicode character database for the covered
/// scripts. What is NOT here answers `LeftToRight` with `Coverage::Assumed`.
constexpr BidiRange kBidiRanges[] = {
    {0x0000, 0x0008, BidiClass::BoundaryNeutral},
    {0x0009, 0x0009, BidiClass::SegmentSeparator},
    {0x000A, 0x000A, BidiClass::ParagraphSeparator},
    {0x000B, 0x000B, BidiClass::SegmentSeparator},
    {0x000C, 0x000C, BidiClass::WhiteSpace},
    {0x000D, 0x000D, BidiClass::ParagraphSeparator},
    {0x000E, 0x001B, BidiClass::BoundaryNeutral},
    {0x001C, 0x001E, BidiClass::ParagraphSeparator},
    {0x001F, 0x001F, BidiClass::SegmentSeparator},
    {0x0020, 0x0020, BidiClass::WhiteSpace},
    {0x0021, 0x0022, BidiClass::OtherNeutral},
    {0x0023, 0x0025, BidiClass::EuropeanTerminator},
    {0x0026, 0x002A, BidiClass::OtherNeutral},
    {0x002B, 0x002B, BidiClass::EuropeanSeparator},
    {0x002C, 0x002C, BidiClass::CommonSeparator},
    {0x002D, 0x002D, BidiClass::EuropeanSeparator},
    {0x002E, 0x002F, BidiClass::CommonSeparator},
    {0x0030, 0x0039, BidiClass::EuropeanNumber},
    {0x003A, 0x003A, BidiClass::CommonSeparator},
    {0x003B, 0x0040, BidiClass::OtherNeutral},
    {0x0041, 0x005A, BidiClass::LeftToRight},
    {0x005B, 0x0060, BidiClass::OtherNeutral},
    {0x0061, 0x007A, BidiClass::LeftToRight},
    {0x007B, 0x007E, BidiClass::OtherNeutral},
    {0x007F, 0x0084, BidiClass::BoundaryNeutral},
    {0x0085, 0x0085, BidiClass::ParagraphSeparator},
    {0x0086, 0x009F, BidiClass::BoundaryNeutral},
    {0x00A0, 0x00A0, BidiClass::CommonSeparator},
    {0x00A1, 0x00A1, BidiClass::OtherNeutral},
    {0x00A2, 0x00A5, BidiClass::EuropeanTerminator},
    {0x00A6, 0x00A9, BidiClass::OtherNeutral},
    {0x00AA, 0x00AA, BidiClass::LeftToRight},
    {0x00AB, 0x00AF, BidiClass::OtherNeutral},
    {0x00B0, 0x00B1, BidiClass::EuropeanTerminator},
    {0x00B2, 0x00B3, BidiClass::EuropeanNumber},
    {0x00B4, 0x00B4, BidiClass::OtherNeutral},
    {0x00B5, 0x00B5, BidiClass::LeftToRight},
    {0x00B6, 0x00B8, BidiClass::OtherNeutral},
    {0x00B9, 0x00B9, BidiClass::EuropeanNumber},
    {0x00BA, 0x00BA, BidiClass::LeftToRight},
    {0x00BB, 0x00BF, BidiClass::OtherNeutral},
    {0x00C0, 0x02B8, BidiClass::LeftToRight},
    {0x0300, 0x036F, BidiClass::NonSpacingMark},
    {0x0370, 0x03FF, BidiClass::LeftToRight},
    {0x0400, 0x058F, BidiClass::LeftToRight},
    {0x0590, 0x05BD, BidiClass::RightToLeft},
    {0x05BE, 0x05BE, BidiClass::RightToLeft},
    {0x05BF, 0x05C7, BidiClass::NonSpacingMark},
    {0x05C8, 0x05FF, BidiClass::RightToLeft},
    {0x0600, 0x0605, BidiClass::ArabicNumber},
    {0x0606, 0x060F, BidiClass::ArabicLetter},
    {0x0610, 0x061A, BidiClass::NonSpacingMark},
    {0x061B, 0x064A, BidiClass::ArabicLetter},
    {0x064B, 0x065F, BidiClass::NonSpacingMark},
    {0x0660, 0x0669, BidiClass::ArabicNumber},
    {0x066A, 0x066A, BidiClass::EuropeanTerminator},
    {0x066B, 0x066C, BidiClass::ArabicNumber},
    {0x066D, 0x066F, BidiClass::ArabicLetter},
    {0x0670, 0x0670, BidiClass::NonSpacingMark},
    {0x0671, 0x06D5, BidiClass::ArabicLetter},
    {0x06D6, 0x06DC, BidiClass::NonSpacingMark},
    {0x06DD, 0x06DE, BidiClass::ArabicNumber},
    {0x06DF, 0x06E4, BidiClass::NonSpacingMark},
    {0x06E5, 0x06E6, BidiClass::ArabicLetter},
    {0x06E7, 0x06E8, BidiClass::NonSpacingMark},
    {0x06E9, 0x06E9, BidiClass::OtherNeutral},
    {0x06EA, 0x06ED, BidiClass::NonSpacingMark},
    {0x06EE, 0x06EF, BidiClass::ArabicLetter},
    {0x06F0, 0x06F9, BidiClass::EuropeanNumber},
    {0x06FA, 0x08FF, BidiClass::ArabicLetter},
    {0x2000, 0x200A, BidiClass::WhiteSpace},
    {0x200B, 0x200D, BidiClass::BoundaryNeutral},
    {0x200E, 0x200E, BidiClass::LeftToRight},
    {0x200F, 0x200F, BidiClass::RightToLeft},
    {0x2010, 0x2027, BidiClass::OtherNeutral},
    {0x2028, 0x2028, BidiClass::WhiteSpace},
    {0x2029, 0x2029, BidiClass::ParagraphSeparator},
    {0x202A, 0x202A, BidiClass::LeftToRightEmbedding},
    {0x202B, 0x202B, BidiClass::RightToLeftEmbedding},
    {0x202C, 0x202C, BidiClass::PopDirectionalFormat},
    {0x202D, 0x202D, BidiClass::LeftToRightOverride},
    {0x202E, 0x202E, BidiClass::RightToLeftOverride},
    {0x202F, 0x202F, BidiClass::CommonSeparator},
    {0x2030, 0x2034, BidiClass::EuropeanTerminator},
    {0x2035, 0x2060, BidiClass::OtherNeutral},
    {0x2066, 0x2066, BidiClass::LeftToRightIsolate},
    {0x2067, 0x2067, BidiClass::RightToLeftIsolate},
    {0x2068, 0x2068, BidiClass::FirstStrongIsolate},
    {0x2069, 0x2069, BidiClass::PopDirectionalIsolate},
    {0x3000, 0x3000, BidiClass::WhiteSpace},
    {0x3001, 0x303F, BidiClass::OtherNeutral},
    {0x3040, 0x30FF, BidiClass::LeftToRight},
    {0x4E00, 0x9FFF, BidiClass::LeftToRight},
    {0xFB1D, 0xFDFF, BidiClass::ArabicLetter},
    {0xFE70, 0xFEFC, BidiClass::ArabicLetter},
};

struct ScriptRange {
    Codepoint first;
    Codepoint last;
    Script value;
};

constexpr ScriptRange kScriptRanges[] = {
    {0x0000, 0x0040, Script::Common},   {0x0041, 0x005A, Script::Latin},
    {0x005B, 0x0060, Script::Common},   {0x0061, 0x007A, Script::Latin},
    {0x007B, 0x00BF, Script::Common},   {0x00C0, 0x02B8, Script::Latin},
    {0x0300, 0x036F, Script::Common},   {0x0370, 0x03FF, Script::Greek},
    {0x0400, 0x052F, Script::Cyrillic}, {0x0590, 0x05FF, Script::Hebrew},
    {0x0600, 0x06FF, Script::Arabic},   {0x0750, 0x077F, Script::Arabic},
    {0x0E00, 0x0E7F, Script::Thai},     {0x2000, 0x206F, Script::Common},
    {0x3000, 0x303F, Script::Common},   {0x3040, 0x309F, Script::Hiragana},
    {0x30A0, 0x30FF, Script::Katakana}, {0x3400, 0x4DBF, Script::Han},
    {0x4E00, 0x9FFF, Script::Han},      {0xF900, 0xFAFF, Script::Han},
    {0xFB1D, 0xFDFF, Script::Arabic},   {0xFE70, 0xFEFF, Script::Arabic},
};

[[nodiscard]] bool in_covered_block(Codepoint codepoint) noexcept {
    return std::ranges::any_of(kScriptRanges, [codepoint](const ScriptRange& range) noexcept {
        return codepoint >= range.first && codepoint <= range.last;
    });
}

[[nodiscard]] bool is_continuation(char byte) noexcept {
    return (static_cast<u8>(byte) & 0xC0U) == 0x80U;
}

}  // namespace

const char* bidi_class_name(BidiClass value) noexcept {
    switch (value) {
        case BidiClass::LeftToRight:
            return "L";
        case BidiClass::RightToLeft:
            return "R";
        case BidiClass::ArabicLetter:
            return "AL";
        case BidiClass::EuropeanNumber:
            return "EN";
        case BidiClass::EuropeanSeparator:
            return "ES";
        case BidiClass::EuropeanTerminator:
            return "ET";
        case BidiClass::ArabicNumber:
            return "AN";
        case BidiClass::CommonSeparator:
            return "CS";
        case BidiClass::NonSpacingMark:
            return "NSM";
        case BidiClass::BoundaryNeutral:
            return "BN";
        case BidiClass::ParagraphSeparator:
            return "B";
        case BidiClass::SegmentSeparator:
            return "S";
        case BidiClass::WhiteSpace:
            return "WS";
        case BidiClass::OtherNeutral:
            return "ON";
        case BidiClass::LeftToRightEmbedding:
            return "LRE";
        case BidiClass::RightToLeftEmbedding:
            return "RLE";
        case BidiClass::PopDirectionalFormat:
            return "PDF";
        case BidiClass::LeftToRightOverride:
            return "LRO";
        case BidiClass::RightToLeftOverride:
            return "RLO";
        case BidiClass::LeftToRightIsolate:
            return "LRI";
        case BidiClass::RightToLeftIsolate:
            return "RLI";
        case BidiClass::FirstStrongIsolate:
            return "FSI";
        case BidiClass::PopDirectionalIsolate:
            return "PDI";
        case BidiClass::Count:
            break;
    }
    return "?";
}

const char* script_name(Script value) noexcept {
    switch (value) {
        case Script::Common:
            return "common";
        case Script::Latin:
            return "latin";
        case Script::Greek:
            return "greek";
        case Script::Cyrillic:
            return "cyrillic";
        case Script::Hebrew:
            return "hebrew";
        case Script::Arabic:
            return "arabic";
        case Script::Thai:
            return "thai";
        case Script::Han:
            return "han";
        case Script::Hiragana:
            return "hiragana";
        case Script::Katakana:
            return "katakana";
        case Script::Unknown:
        case Script::Count:
            break;
    }
    return "unknown";
}

BidiClass bidi_class_of(Codepoint codepoint) noexcept {
    for (const BidiRange& range : kBidiRanges) {
        if (codepoint >= range.first && codepoint <= range.last) {
            return range.value;
        }
    }
    return BidiClass::LeftToRight;
}

Script script_of(Codepoint codepoint) noexcept {
    for (const ScriptRange& range : kScriptRanges) {
        if (codepoint >= range.first && codepoint <= range.last) {
            return range.value;
        }
    }
    return Script::Unknown;
}

Coverage coverage_of(Codepoint codepoint) noexcept {
    return in_covered_block(codepoint) ? Coverage::Known : Coverage::Assumed;
}

LineBreakClass line_break_class_of(Codepoint codepoint) noexcept {
    switch (codepoint) {
        case 0x000A:
        case 0x000B:
        case 0x000C:
        case 0x0085:
        case 0x2028:
        case 0x2029:
            return LineBreakClass::Mandatory;
        case 0x000D:
            return LineBreakClass::CarriageReturn;
        case 0x0009:
        case 0x0020:
            return LineBreakClass::Space;
        case 0x200B:
            return LineBreakClass::ZeroWidthSpace;
        case 0x00A0:
        case 0x202F:
            // A no-break space is GL in UAX #14; folded to `Alphabetic` here, which produces the
            // same answer for every pair in the table below — no break either side.
            return LineBreakClass::Alphabetic;
        case 0x002D:
            return LineBreakClass::Hyphen;
        case 0x002F:
            return LineBreakClass::BreakAfter;
        default:
            break;
    }
    if (codepoint >= 0x0030 && codepoint <= 0x0039) {
        return LineBreakClass::Numeric;
    }
    if (codepoint == 0x0024 || codepoint == 0x00A3 || codepoint == 0x00A5) {
        return LineBreakClass::PrefixNumeric;
    }
    if (codepoint == 0x0025) {
        return LineBreakClass::PostfixNumeric;
    }
    if (codepoint == 0x0021 || codepoint == 0x003F) {
        return LineBreakClass::Exclamation;
    }
    if (codepoint == 0x002C || codepoint == 0x002E || codepoint == 0x003A || codepoint == 0x003B) {
        return LineBreakClass::InfixSeparator;
    }
    if (codepoint == 0x0028 || codepoint == 0x005B || codepoint == 0x007B) {
        return LineBreakClass::OpenPunctuation;
    }
    if (codepoint == 0x0029 || codepoint == 0x005D || codepoint == 0x007D) {
        return LineBreakClass::ClosePunctuation;
    }
    if (codepoint == 0x0022 || codepoint == 0x0027 ||
        (codepoint >= 0x2018 && codepoint <= 0x201F)) {
        return LineBreakClass::Quotation;
    }
    if (codepoint >= 0x2010 && codepoint <= 0x2014) {
        return LineBreakClass::BreakAfter;
    }
    if (codepoint >= 0x0300 && codepoint <= 0x036F) {
        return LineBreakClass::CombiningMark;
    }
    if (codepoint >= 0x064B && codepoint <= 0x065F) {
        return LineBreakClass::CombiningMark;
    }
    if (codepoint >= 0x0E00 && codepoint <= 0x0E7F) {
        // THE SCRIPTS THAT NEED A DICTIONARY. UAX #14 declines to break inside SA and defers to a
        // dictionary; `linebreak.h`'s `WordDictionary` is where that deferral lands.
        return LineBreakClass::ComplexContext;
    }
    if ((codepoint >= 0x3040 && codepoint <= 0x30FF) ||
        (codepoint >= 0x3400 && codepoint <= 0x4DBF) ||
        (codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||
        (codepoint >= 0xF900 && codepoint <= 0xFAFF)) {
        return LineBreakClass::Ideographic;
    }
    return LineBreakClass::Alphabetic;
}

GraphemeBreak grapheme_break_of(Codepoint codepoint) noexcept {
    if (codepoint == 0x000D) {
        return GraphemeBreak::CarriageReturn;
    }
    if (codepoint == 0x000A) {
        return GraphemeBreak::LineFeed;
    }
    if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint <= 0x9F)) {
        return GraphemeBreak::Control;
    }
    if (codepoint == 0x200D) {
        return GraphemeBreak::ZeroWidthJoiner;
    }
    if ((codepoint >= 0x0300 && codepoint <= 0x036F) ||
        (codepoint >= 0x0483 && codepoint <= 0x0489) ||
        (codepoint >= 0x0591 && codepoint <= 0x05BD) ||
        (codepoint >= 0x0610 && codepoint <= 0x061A) ||
        (codepoint >= 0x064B && codepoint <= 0x065F) || codepoint == 0x0670 ||
        (codepoint >= 0x06D6 && codepoint <= 0x06DC) ||
        (codepoint >= 0x0E31 && codepoint <= 0x0E3A) ||
        (codepoint >= 0x0E47 && codepoint <= 0x0E4E) || codepoint == 0xFE0F) {
        return GraphemeBreak::Extend;
    }
    if (codepoint >= 0x1F1E6 && codepoint <= 0x1F1FF) {
        return GraphemeBreak::RegionalIndicator;
    }
    return GraphemeBreak::Other;
}

Codepoint decode_utf8(std::string_view text, usize& cursor) noexcept {
    if (cursor >= text.size()) {
        return 0;
    }
    const auto lead = static_cast<u8>(text[cursor]);
    if (lead < 0x80U) {
        ++cursor;
        return lead;
    }
    u32 length = 0;
    Codepoint codepoint = 0;
    if ((lead & 0xE0U) == 0xC0U) {
        length = 2;
        codepoint = lead & 0x1FU;
    } else if ((lead & 0xF0U) == 0xE0U) {
        length = 3;
        codepoint = lead & 0x0FU;
    } else if ((lead & 0xF8U) == 0xF0U) {
        length = 4;
        codepoint = lead & 0x07U;
    } else {
        ++cursor;
        return kReplacement;
    }
    if (cursor + length > text.size()) {
        ++cursor;
        return kReplacement;
    }
    for (u32 index = 1; index < length; ++index) {
        if (!is_continuation(text[cursor + index])) {
            ++cursor;
            return kReplacement;
        }
        codepoint = (codepoint << 6U) | (static_cast<u8>(text[cursor + index]) & 0x3FU);
    }
    cursor += length;
    const bool overlong = (length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
                          (length == 4 && codepoint < 0x10000);
    const bool surrogate = codepoint >= 0xD800 && codepoint <= 0xDFFF;
    if (overlong || surrogate || codepoint > 0x10FFFF) {
        return kReplacement;
    }
    return codepoint;
}

u32 encode_utf8(Codepoint codepoint, char* out) noexcept {
    if (out == nullptr || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        return 0;
    }
    if (codepoint < 0x80) {
        out[0] = static_cast<char>(codepoint);
        return 1;
    }
    if (codepoint < 0x800) {
        out[0] = static_cast<char>(0xC0U | (codepoint >> 6U));
        out[1] = static_cast<char>(0x80U | (codepoint & 0x3FU));
        return 2;
    }
    if (codepoint < 0x10000) {
        out[0] = static_cast<char>(0xE0U | (codepoint >> 12U));
        out[1] = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
        out[2] = static_cast<char>(0x80U | (codepoint & 0x3FU));
        return 3;
    }
    out[0] = static_cast<char>(0xF0U | (codepoint >> 18U));
    out[1] = static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU));
    out[2] = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
    out[3] = static_cast<char>(0x80U | (codepoint & 0x3FU));
    return 4;
}

usize next_grapheme(std::string_view text, usize offset) noexcept {
    if (offset >= text.size()) {
        return text.size();
    }
    usize cursor = offset;
    const Codepoint first = decode_utf8(text, cursor);
    GraphemeBreak previous = grapheme_break_of(first);
    // GB3: CR × LF, and nothing else joins a control.
    if (previous == GraphemeBreak::CarriageReturn && cursor < text.size()) {
        usize probe = cursor;
        if (decode_utf8(text, probe) == 0x000A) {
            return probe;
        }
        return cursor;
    }
    if (previous == GraphemeBreak::Control || previous == GraphemeBreak::LineFeed) {
        return cursor;
    }

    u32 regional_run = (previous == GraphemeBreak::RegionalIndicator) ? 1U : 0U;
    while (cursor < text.size()) {
        usize probe = cursor;
        const Codepoint next = decode_utf8(text, probe);
        const GraphemeBreak kind = grapheme_break_of(next);
        if (kind == GraphemeBreak::Extend || kind == GraphemeBreak::ZeroWidthJoiner) {
            cursor = probe;  // GB9: × (Extend | ZWJ)
            previous = kind;
            continue;
        }
        if (kind == GraphemeBreak::RegionalIndicator && regional_run == 1U) {
            // GB12/GB13: a flag is exactly two regional indicators, not a run of them.
            cursor = probe;
            regional_run = 2U;
            previous = kind;
            continue;
        }
        if (previous == GraphemeBreak::ZeroWidthJoiner && kind == GraphemeBreak::Other) {
            cursor = probe;  // GB11, approximated: ZWJ joins what follows it.
            previous = kind;
            continue;
        }
        break;
    }
    return cursor;
}

usize previous_grapheme(std::string_view text, usize offset) noexcept {
    if (offset == 0) {
        return 0;
    }
    const usize limit = (offset > text.size()) ? text.size() : offset;
    usize best = 0;
    usize cursor = 0;
    while (cursor < limit) {
        const usize next = next_grapheme(text, cursor);
        if (next >= limit) {
            return cursor;
        }
        best = next;
        cursor = next;
    }
    return best;
}

}  // namespace cy::text
