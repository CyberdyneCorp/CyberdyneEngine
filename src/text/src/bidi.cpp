// UAX #9, over `unicode.h`'s tables. M8.b task 9.4.
//
// The rule numbers in the comments are the specification's. Anybody changing this file should have
// UAX #9 open beside it; the rules are terse and their ORDER is most of their content.

#include <cy/text/bidi.h>

#include <algorithm>

namespace cy::text {
namespace {

constexpr u8 kMaxDepth = 125;

/// One character, as the algorithm works on it.
struct Entry {
    u32 begin = 0;
    u32 end = 0;
    Codepoint codepoint = 0;
    BidiClass original = BidiClass::LeftToRight;
    BidiClass current = BidiClass::LeftToRight;
    u8 level = 0;
    /// X9 removes embedding and override formatting characters and boundary neutrals. Removing them
    /// from an array would renumber every index, so they are MARKED and skipped instead — which is
    /// what UAX #9's own note recommends.
    bool removed = false;
};

struct StatusEntry {
    u8 level = 0;
    /// `OtherNeutral` means "neutral": no override in effect.
    BidiClass override_status = BidiClass::OtherNeutral;
    bool isolate = false;
};

[[nodiscard]] u8 next_even(u8 level) noexcept {
    return static_cast<u8>((level + 2U) & ~1U);
}

[[nodiscard]] u8 next_odd(u8 level) noexcept {
    return static_cast<u8>((level + 1U) | 1U);
}

[[nodiscard]] bool is_isolate_initiator(BidiClass value) noexcept {
    return value == BidiClass::LeftToRightIsolate || value == BidiClass::RightToLeftIsolate ||
           value == BidiClass::FirstStrongIsolate;
}

[[nodiscard]] bool is_removed_by_x9(BidiClass value) noexcept {
    return value == BidiClass::LeftToRightEmbedding || value == BidiClass::RightToLeftEmbedding ||
           value == BidiClass::LeftToRightOverride || value == BidiClass::RightToLeftOverride ||
           value == BidiClass::PopDirectionalFormat || value == BidiClass::BoundaryNeutral;
}

/// P2/P3, and the same scan X5c uses for a first-strong isolate: the first strong character, with
/// the contents of an isolate skipped.
[[nodiscard]] u8 first_strong_level(Span<const Entry> entries, usize from, usize to) noexcept {
    u32 depth = 0;
    for (usize index = from; index < to; ++index) {
        const BidiClass value = entries[index].original;
        if (is_isolate_initiator(value)) {
            ++depth;
            continue;
        }
        if (value == BidiClass::PopDirectionalIsolate) {
            if (depth > 0) {
                --depth;
            }
            continue;
        }
        if (depth != 0) {
            continue;
        }
        if (value == BidiClass::LeftToRight) {
            return 0;
        }
        if (value == BidiClass::RightToLeft || value == BidiClass::ArabicLetter) {
            return 1;
        }
    }
    return 0;
}

/// The matching PDI for an isolate initiator at `index`, or `to` when there is none. BD9.
[[nodiscard]] usize matching_pdi(Span<const Entry> entries, usize index, usize to) noexcept {
    u32 depth = 1;
    for (usize probe = index + 1; probe < to; ++probe) {
        const BidiClass value = entries[probe].original;
        if (is_isolate_initiator(value)) {
            ++depth;
        } else if (value == BidiClass::PopDirectionalIsolate) {
            --depth;
            if (depth == 0) {
                return probe;
            }
        }
    }
    return to;
}

/// X1 to X8: the explicit levels, the directional status stack, and the overflow counters.
void resolve_explicit(Array<Entry>& entries, u8 paragraph_level, bool& overflowed) noexcept {
    StatusEntry stack[kMaxDepth + 2];
    usize depth = 0;
    stack[depth] = StatusEntry{paragraph_level, BidiClass::OtherNeutral, false};

    u32 overflow_isolates = 0;
    u32 overflow_embeddings = 0;
    u32 valid_isolates = 0;

    for (usize index = 0; index < entries.size(); ++index) {
        Entry& entry = entries[index];
        const BidiClass value = entry.original;
        switch (value) {
            case BidiClass::LeftToRightEmbedding:
            case BidiClass::RightToLeftEmbedding:
            case BidiClass::LeftToRightOverride:
            case BidiClass::RightToLeftOverride: {
                // X2 to X5. The formatting character itself takes the level in effect BEFORE it.
                entry.level = stack[depth].level;
                const bool right = (value == BidiClass::RightToLeftEmbedding) ||
                                   (value == BidiClass::RightToLeftOverride);
                const u8 level =
                    right ? next_odd(stack[depth].level) : next_even(stack[depth].level);
                if (level <= kMaxDepth && overflow_isolates == 0 && overflow_embeddings == 0) {
                    ++depth;
                    stack[depth].level = level;
                    stack[depth].isolate = false;
                    // `OtherNeutral` is this implementation's spelling of "no override": an
                    // embedding sets a level and leaves the characters' own classes alone.
                    BidiClass override_status = BidiClass::OtherNeutral;
                    if (value == BidiClass::LeftToRightOverride) {
                        override_status = BidiClass::LeftToRight;
                    } else if (value == BidiClass::RightToLeftOverride) {
                        override_status = BidiClass::RightToLeft;
                    }
                    stack[depth].override_status = override_status;
                } else {
                    if (overflow_isolates == 0) {
                        ++overflow_embeddings;
                    }
                    overflowed = true;
                }
                break;
            }
            case BidiClass::LeftToRightIsolate:
            case BidiClass::RightToLeftIsolate:
            case BidiClass::FirstStrongIsolate: {
                // X5a, X5b, X5c. An isolate initiator takes the current level and the current
                // override, unlike an embedding.
                entry.level = stack[depth].level;
                if (stack[depth].override_status != BidiClass::OtherNeutral) {
                    entry.current = stack[depth].override_status;
                }
                bool right = (value == BidiClass::RightToLeftIsolate);
                if (value == BidiClass::FirstStrongIsolate) {
                    const usize end = matching_pdi(entries.span(), index, entries.size());
                    right = first_strong_level(entries.span(), index + 1, end) == 1;
                }
                const u8 level =
                    right ? next_odd(stack[depth].level) : next_even(stack[depth].level);
                if (level <= kMaxDepth && overflow_isolates == 0 && overflow_embeddings == 0) {
                    ++valid_isolates;
                    ++depth;
                    stack[depth].level = level;
                    stack[depth].isolate = true;
                    stack[depth].override_status = BidiClass::OtherNeutral;
                } else {
                    ++overflow_isolates;
                    overflowed = true;
                }
                break;
            }
            case BidiClass::PopDirectionalIsolate: {
                // X6a.
                if (overflow_isolates > 0) {
                    --overflow_isolates;
                } else if (valid_isolates != 0) {
                    overflow_embeddings = 0;
                    while (!stack[depth].isolate) {
                        --depth;
                    }
                    --depth;
                    --valid_isolates;
                }
                entry.level = stack[depth].level;
                if (stack[depth].override_status != BidiClass::OtherNeutral) {
                    entry.current = stack[depth].override_status;
                }
                break;
            }
            case BidiClass::PopDirectionalFormat: {
                // X7.
                entry.level = stack[depth].level;
                if (overflow_isolates > 0) {
                    break;
                }
                if (overflow_embeddings > 0) {
                    --overflow_embeddings;
                    break;
                }
                if (!stack[depth].isolate && depth > 0) {
                    --depth;
                }
                break;
            }
            case BidiClass::ParagraphSeparator: {
                // X8: a paragraph separator takes the paragraph level. This implementation treats
                // its input as one paragraph, so this is the terminator's own level and nothing is
                // reset after it.
                entry.level = paragraph_level;
                break;
            }
            default: {
                // X6.
                entry.level = stack[depth].level;
                if (stack[depth].override_status != BidiClass::OtherNeutral) {
                    entry.current = stack[depth].override_status;
                }
                break;
            }
        }
        if (is_removed_by_x9(value)) {
            entry.removed = true;  // X9
        }
    }
}

/// The class of the character before `index` in the same run, or the run's start-of-sequence type.
[[nodiscard]] BidiClass previous_class(Span<const Entry> entries, usize begin, usize index,
                                       BidiClass sos) noexcept {
    for (usize probe = index; probe > begin; --probe) {
        const Entry& entry = entries[probe - 1];
        if (!entry.removed) {
            return entry.current;
        }
    }
    return sos;
}

/// W1 to W7 over one level run.
void resolve_weak(Span<Entry> entries, usize begin, usize end, BidiClass sos,
                  BidiClass eos) noexcept {
    // W1: a non-spacing mark takes the class of the previous character, or sos.
    for (usize index = begin; index < end; ++index) {
        Entry& entry = entries[index];
        if (entry.removed || entry.current != BidiClass::NonSpacingMark) {
            continue;
        }
        const BidiClass previous = previous_class(entries, begin, index, sos);
        entry.current =
            is_isolate_initiator(previous) || previous == BidiClass::PopDirectionalIsolate
                ? BidiClass::OtherNeutral
                : previous;
    }
    // W2: EN after AL becomes AN.
    BidiClass strong = sos;
    for (usize index = begin; index < end; ++index) {
        Entry& entry = entries[index];
        if (entry.removed) {
            continue;
        }
        if (entry.current == BidiClass::LeftToRight || entry.current == BidiClass::RightToLeft ||
            entry.current == BidiClass::ArabicLetter) {
            strong = entry.current;
        } else if (entry.current == BidiClass::EuropeanNumber &&
                   strong == BidiClass::ArabicLetter) {
            entry.current = BidiClass::ArabicNumber;
        }
    }
    // W3: AL becomes R.
    for (usize index = begin; index < end; ++index) {
        if (!entries[index].removed && entries[index].current == BidiClass::ArabicLetter) {
            entries[index].current = BidiClass::RightToLeft;
        }
    }
    // W4: a single ES between two ENs becomes EN; a single CS between two numbers of the same type
    // becomes that type.
    for (usize index = begin; index < end; ++index) {
        Entry& entry = entries[index];
        if (entry.removed || (entry.current != BidiClass::EuropeanSeparator &&
                              entry.current != BidiClass::CommonSeparator)) {
            continue;
        }
        usize before = index;
        while (before > begin && entries[before - 1].removed) {
            --before;
        }
        if (before == begin) {
            continue;
        }
        usize after = index + 1;
        while (after < end && entries[after].removed) {
            ++after;
        }
        if (after >= end) {
            continue;
        }
        const BidiClass left = entries[before - 1].current;
        const BidiClass right = entries[after].current;
        if (left == BidiClass::EuropeanNumber && right == BidiClass::EuropeanNumber) {
            entry.current = BidiClass::EuropeanNumber;
        } else if (entry.current == BidiClass::CommonSeparator && left == BidiClass::ArabicNumber &&
                   right == BidiClass::ArabicNumber) {
            entry.current = BidiClass::ArabicNumber;
        }
    }
    // W5: a sequence of ETs adjacent to an EN becomes EN.
    for (usize index = begin; index < end; ++index) {
        if (entries[index].removed || entries[index].current != BidiClass::EuropeanTerminator) {
            continue;
        }
        usize run_end = index;
        while (run_end < end && (entries[run_end].removed ||
                                 entries[run_end].current == BidiClass::EuropeanTerminator)) {
            ++run_end;
        }
        const BidiClass before = previous_class(entries, begin, index, sos);
        const BidiClass after = (run_end < end) ? entries[run_end].current : eos;
        if (before == BidiClass::EuropeanNumber || after == BidiClass::EuropeanNumber) {
            for (usize probe = index; probe < run_end; ++probe) {
                if (!entries[probe].removed) {
                    entries[probe].current = BidiClass::EuropeanNumber;
                }
            }
        }
        index = run_end - 1;
    }
    // W6: any remaining separator or terminator becomes ON.
    for (usize index = begin; index < end; ++index) {
        Entry& entry = entries[index];
        if (entry.removed) {
            continue;
        }
        if (entry.current == BidiClass::EuropeanSeparator ||
            entry.current == BidiClass::EuropeanTerminator ||
            entry.current == BidiClass::CommonSeparator) {
            entry.current = BidiClass::OtherNeutral;
        }
    }
    // W7: EN after L becomes L.
    strong = sos;
    for (usize index = begin; index < end; ++index) {
        Entry& entry = entries[index];
        if (entry.removed) {
            continue;
        }
        if (entry.current == BidiClass::LeftToRight || entry.current == BidiClass::RightToLeft) {
            strong = entry.current;
        } else if (entry.current == BidiClass::EuropeanNumber && strong == BidiClass::LeftToRight) {
            entry.current = BidiClass::LeftToRight;
        }
    }
}

[[nodiscard]] bool is_neutral_or_isolate(BidiClass value) noexcept {
    return value == BidiClass::WhiteSpace || value == BidiClass::OtherNeutral ||
           value == BidiClass::SegmentSeparator || value == BidiClass::ParagraphSeparator ||
           is_isolate_initiator(value) || value == BidiClass::PopDirectionalIsolate;
}

/// The strong direction a resolved class contributes at N1: a number counts as R.
[[nodiscard]] BidiClass strong_of(BidiClass value) noexcept {
    if (value == BidiClass::EuropeanNumber || value == BidiClass::ArabicNumber ||
        value == BidiClass::RightToLeft) {
        return BidiClass::RightToLeft;
    }
    return value;
}

/// N1 and N2 over one level run. N0 — paired brackets — is not implemented; see the header.
void resolve_neutral(Span<Entry> entries, usize begin, usize end, u8 level, BidiClass sos,
                     BidiClass eos) noexcept {
    const BidiClass embedding =
        ((level & 1U) != 0U) ? BidiClass::RightToLeft : BidiClass::LeftToRight;
    for (usize index = begin; index < end; ++index) {
        if (entries[index].removed || !is_neutral_or_isolate(entries[index].current)) {
            continue;
        }
        usize run_end = index;
        while (run_end < end &&
               (entries[run_end].removed || is_neutral_or_isolate(entries[run_end].current))) {
            ++run_end;
        }
        BidiClass before = sos;
        for (usize probe = index; probe > begin; --probe) {
            if (!entries[probe - 1].removed) {
                before = strong_of(entries[probe - 1].current);
                break;
            }
        }
        BidiClass after = eos;
        for (usize probe = run_end; probe < end; ++probe) {
            if (!entries[probe].removed) {
                after = strong_of(entries[probe].current);
                break;
            }
        }
        // N1: a neutral run between two of the same direction takes that direction. N2: otherwise
        // it takes the embedding direction.
        const BidiClass resolved = (before == after && (before == BidiClass::LeftToRight ||
                                                        before == BidiClass::RightToLeft))
                                       ? before
                                       : embedding;
        for (usize probe = index; probe < run_end; ++probe) {
            if (!entries[probe].removed) {
                entries[probe].current = resolved;
            }
        }
        index = run_end - 1;
    }
}

/// I1 and I2 over one level run.
void resolve_implicit(Span<Entry> entries, usize begin, usize end, u8 level) noexcept {
    const bool odd = (level & 1U) != 0U;
    for (usize index = begin; index < end; ++index) {
        Entry& entry = entries[index];
        if (entry.removed) {
            continue;
        }
        if (!odd) {
            // I1: at an even level, R goes up one, AN and EN go up two.
            if (entry.current == BidiClass::RightToLeft) {
                entry.level = static_cast<u8>(entry.level + 1U);
            } else if (entry.current == BidiClass::ArabicNumber ||
                       entry.current == BidiClass::EuropeanNumber) {
                entry.level = static_cast<u8>(entry.level + 2U);
            }
        } else if (entry.current == BidiClass::LeftToRight ||
                   entry.current == BidiClass::ArabicNumber ||
                   entry.current == BidiClass::EuropeanNumber) {
            // I2: at an odd level, L, AN and EN go up one.
            entry.level = static_cast<u8>(entry.level + 1U);
        }
    }
}

}  // namespace

Status resolve_levels(std::string_view text, ParagraphDirection direction,
                      BidiResult& out) noexcept {
    out.runs.clear();
    out.approximated = false;
    out.overflowed = false;

    Array<Entry> entries(out.runs.allocator());
    usize cursor = 0;
    while (cursor < text.size()) {
        Entry entry;
        entry.begin = static_cast<u32>(cursor);
        entry.codepoint = decode_utf8(text, cursor);
        entry.end = static_cast<u32>(cursor);
        entry.original = bidi_class_of(entry.codepoint);
        entry.current = entry.original;
        if (is_isolate_initiator(entry.original)) {
            out.approximated = true;
        }
        if (Status pushed = entries.push_back(entry); !pushed) {
            return pushed;
        }
    }
    if (entries.empty()) {
        out.paragraph_level = (direction == ParagraphDirection::RightToLeft) ? 1U : 0U;
        return ok();
    }

    // P2, P3.
    u8 paragraph_level = 0;
    if (direction == ParagraphDirection::RightToLeft) {
        paragraph_level = 1;
    } else if (direction == ParagraphDirection::Auto) {
        paragraph_level = first_strong_level(entries.span(), 0, entries.size());
    }
    out.paragraph_level = paragraph_level;

    resolve_explicit(entries, paragraph_level, out.overflowed);

    // X10, approximated by level runs. Each run's sos and eos come from the higher of its own level
    // and its neighbour's, which is what the specification says for a sequence's boundaries.
    usize index = 0;
    while (index < entries.size()) {
        while (index < entries.size() && entries[index].removed) {
            ++index;
        }
        if (index >= entries.size()) {
            break;
        }
        const u8 level = entries[index].level;
        usize run_end = index;
        while (run_end < entries.size() &&
               (entries[run_end].removed || entries[run_end].level == level)) {
            ++run_end;
        }
        u8 before = paragraph_level;
        for (usize probe = index; probe > 0; --probe) {
            if (!entries[probe - 1].removed) {
                before = entries[probe - 1].level;
                break;
            }
        }
        u8 after = paragraph_level;
        for (usize probe = run_end; probe < entries.size(); ++probe) {
            if (!entries[probe].removed) {
                after = entries[probe].level;
                break;
            }
        }
        const u8 sos_level = (level > before) ? level : before;
        const u8 eos_level = (level > after) ? level : after;
        const BidiClass sos =
            ((sos_level & 1U) != 0U) ? BidiClass::RightToLeft : BidiClass::LeftToRight;
        const BidiClass eos =
            ((eos_level & 1U) != 0U) ? BidiClass::RightToLeft : BidiClass::LeftToRight;

        resolve_weak(entries.span(), index, run_end, sos, eos);
        resolve_neutral(entries.span(), index, run_end, level, sos, eos);
        resolve_implicit(entries.span(), index, run_end, level);
        index = run_end;
    }

    // L1: a segment or paragraph separator, and any trailing whitespace and isolate formatting
    // characters at the end of a line, take the paragraph level. This implementation is given a
    // paragraph rather than a line, so the trailing run at the end of the text is the one reset.
    for (usize probe = 0; probe < entries.size(); ++probe) {
        const BidiClass original = entries[probe].original;
        if (original == BidiClass::SegmentSeparator || original == BidiClass::ParagraphSeparator) {
            entries[probe].level = paragraph_level;
            for (usize back = probe; back > 0; --back) {
                const BidiClass value = entries[back - 1].original;
                if (value == BidiClass::WhiteSpace || is_isolate_initiator(value) ||
                    value == BidiClass::PopDirectionalIsolate || is_removed_by_x9(value)) {
                    entries[back - 1].level = paragraph_level;
                    continue;
                }
                break;
            }
        }
    }
    for (usize back = entries.size(); back > 0; --back) {
        const BidiClass value = entries[back - 1].original;
        if (value == BidiClass::WhiteSpace || is_isolate_initiator(value) ||
            value == BidiClass::PopDirectionalIsolate || is_removed_by_x9(value)) {
            entries[back - 1].level = paragraph_level;
            continue;
        }
        break;
    }

    // Coalesce into runs. A removed character joins the run around it rather than splitting it: it
    // has no glyph, and a run boundary where the text has none would batch worse for no reason.
    for (const Entry& entry : entries.span()) {
        if (!out.runs.empty() && out.runs[out.runs.size() - 1].level == entry.level &&
            out.runs[out.runs.size() - 1].end == entry.begin) {
            out.runs[out.runs.size() - 1].end = entry.end;
            continue;
        }
        BidiRun run;
        run.begin = entry.begin;
        run.end = entry.end;
        run.level = entry.level;
        if (Status pushed = out.runs.push_back(run); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status reorder_visual(Span<const BidiRun> runs, u8 paragraph_level, Array<u32>& out) noexcept {
    out.clear();
    for (u32 index = 0; index < static_cast<u32>(runs.size()); ++index) {
        if (Status pushed = out.push_back(index); !pushed) {
            return pushed;
        }
    }
    if (runs.empty()) {
        return ok();
    }
    // L2: from the highest level down to the lowest odd level, reverse each maximal run of runs at
    // or above that level.
    u8 highest = paragraph_level;
    u8 lowest_odd = 63;
    for (const BidiRun& run : runs) {
        highest = std::max(highest, run.level);
        if ((run.level & 1U) != 0U && run.level < lowest_odd) {
            lowest_odd = run.level;
        }
    }
    if ((paragraph_level & 1U) != 0U && paragraph_level < lowest_odd) {
        lowest_odd = paragraph_level;
    }
    for (u8 level = highest; level >= lowest_odd && level > 0; --level) {
        usize index = 0;
        while (index < out.size()) {
            if (runs[out[index]].level < level) {
                ++index;
                continue;
            }
            usize run_end = index;
            while (run_end < out.size() && runs[out[run_end]].level >= level) {
                ++run_end;
            }
            for (usize low = index, high = run_end - 1; low < high; ++low, --high) {
                const u32 held = out[low];
                out[low] = out[high];
                out[high] = held;
            }
            index = run_end;
        }
    }
    return ok();
}

Status apply_structured_text(std::string_view text, StructuredText kind,
                             Array<char>& out) noexcept {
    out.clear();
    const auto append = [&out](std::string_view piece) noexcept -> Status {
        for (const char byte : piece) {
            if (Status pushed = out.push_back(byte); !pushed) {
                return pushed;
            }
        }
        return ok();
    };
    // U+2066 LEFT-TO-RIGHT ISOLATE and U+2069 POP DIRECTIONAL ISOLATE, in UTF-8.
    constexpr std::string_view kLri = "\xE2\x81\xA6";
    constexpr std::string_view kPdi = "\xE2\x81\xA9";

    if (kind == StructuredText::None) {
        return append(text);
    }
    if (kind == StructuredText::Code) {
        // The whole string is one left-to-right island. Nothing inside it reorders.
        if (Status written = append(kLri); !written) {
            return written;
        }
        if (Status written = append(text); !written) {
            return written;
        }
        return append(kPdi);
    }

    // A PATH OR A URL. Each component is isolated and the separators are left outside, so the
    // components keep their own direction and the separators keep the path's order — which is what
    // makes a path readable in a right-to-left interface rather than merely correct.
    const auto is_separator = [kind](char byte) noexcept {
        if (kind == StructuredText::Url) {
            return byte == '/' || byte == ':' || byte == '?' || byte == '&' || byte == '=' ||
                   byte == '#' || byte == '.';
        }
        return byte == '/' || byte == '\\';
    };
    usize begin = 0;
    for (usize index = 0; index <= text.size(); ++index) {
        const bool at_end = index == text.size();
        if (!at_end && !is_separator(text[index])) {
            continue;
        }
        if (index > begin) {
            if (Status written = append(kLri); !written) {
                return written;
            }
            if (Status written = append(text.substr(begin, index - begin)); !written) {
                return written;
            }
            if (Status written = append(kPdi); !written) {
                return written;
            }
        }
        if (!at_end) {
            if (Status written = append(text.substr(index, 1)); !written) {
                return written;
            }
        }
        begin = index + 1;
    }
    return ok();
}

}  // namespace cy::text
