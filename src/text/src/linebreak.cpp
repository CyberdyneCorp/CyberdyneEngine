// UAX #14's pair table, the dictionary deferral, justification and overflow. M8.b task 9.4.

#include <cy/text/linebreak.h>

#include <algorithm>

namespace cy::text {
namespace {

using LB = LineBreakClass;

/// What may happen between two classes.
enum class Action : u8 {
    /// A break is forbidden here.
    Prohibited = 0,
    /// A break is allowed.
    Direct = 1,
    /// A break is allowed only if a space intervenes — UAX #14's "indirect".
    Indirect = 2,
};

/// The pair rules, as rules rather than as a 19-by-19 literal.
///
/// UAX #14's table 2 is generated from its numbered rules, and a table transcribed by hand is a
/// table with a typo in it that nobody finds for a year. These are the rules the covered classes
/// can express, in the standard's own order — the order is most of their content, because a later
/// rule only applies where no earlier one did.
[[nodiscard]] Action pair_action(LB before, LB after) noexcept {
    // LB7: never break before a space or a zero-width space. (The caller also skips these, so this
    // is the belt to that pair of braces.)
    if (after == LB::Space || after == LB::ZeroWidthSpace) {
        return Action::Prohibited;
    }
    // LB8: a zero-width space allows a break after it, whatever follows.
    if (before == LB::ZeroWidthSpace) {
        return Action::Direct;
    }
    // LB9, LB10: a combining mark attaches to what precedes it and otherwise behaves as AL.
    if (after == LB::CombiningMark) {
        return Action::Prohibited;
    }
    if (before == LB::CombiningMark) {
        before = LB::Alphabetic;
    }
    // LB13: never break before a close, an exclamation, an infix separator or a symbol.
    if (after == LB::ClosePunctuation || after == LB::Exclamation || after == LB::InfixSeparator) {
        return Action::Prohibited;
    }
    // LB14: never break after an open, even across spaces.
    if (before == LB::OpenPunctuation) {
        return Action::Prohibited;
    }
    // LB19: never break before or after a quotation mark.
    if (after == LB::Quotation || before == LB::Quotation) {
        return Action::Prohibited;
    }
    // LB21: never break before a hyphen or a break-after character, and never after a
    // break-before one.
    if (after == LB::Hyphen || after == LB::BreakAfter) {
        return Action::Prohibited;
    }
    if (before == LB::BreakBefore) {
        return Action::Prohibited;
    }
    // LB18: a space is a break opportunity, and every rule that outranks it has been applied.
    if (before == LB::Space) {
        return Action::Direct;
    }
    // LB23, LB23a, LB24, LB25: letters and numbers do not break against each other, and prefixes
    // and postfixes do not break against numbers — directly. A space between them still breaks,
    // which is what `Indirect` means and what makes "3 000" and "hello world" wrap.
    const bool before_word = before == LB::Alphabetic || before == LB::Numeric ||
                             before == LB::Ideographic || before == LB::ComplexContext;
    const bool after_word = after == LB::Alphabetic || after == LB::Numeric ||
                            after == LB::Ideographic || after == LB::ComplexContext;
    if (before_word && after_word) {
        // An ideograph breaks against a letter or a number directly — that is LB23a's exception and
        // the reason CJK wraps anywhere.
        if (before == LB::Ideographic || after == LB::Ideographic) {
            return Action::Direct;
        }
        if (before == LB::ComplexContext || after == LB::ComplexContext) {
            return Action::Direct;
        }
        return Action::Indirect;
    }
    if ((before == LB::PrefixNumeric && after_word) ||
        (before_word && after == LB::PostfixNumeric)) {
        return Action::Indirect;
    }
    if (before == LB::PrefixNumeric && after == LB::PostfixNumeric) {
        return Action::Indirect;
    }
    // LB22 and the rest: everything not named above may break.
    return Action::Direct;
}

struct Character {
    u32 begin = 0;
    u32 end = 0;
    Codepoint codepoint = 0;
    LB klass = LB::Alphabetic;
};

[[nodiscard]] f32 clampf(f32 value, f32 low, f32 high) noexcept {
    return std::clamp(value, low, high);
}

}  // namespace

Status WordList::add(std::string_view word) noexcept {
    if (word.empty()) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "an empty word matches everywhere", 0});
    }
    return words_.push_back(word);
}

usize WordList::longest_match(std::string_view text) const noexcept {
    usize best = 0;
    for (const std::string_view word : words_.span()) {
        if (word.size() > best && text.starts_with(word)) {
            best = word.size();
        }
    }
    return best;
}

Status find_breaks(std::string_view text, const WordDictionary* dictionary,
                   Array<BreakOpportunity>& out, BreakReport& report) noexcept {
    out.clear();
    report = BreakReport{};
    if (text.empty()) {
        return ok();
    }

    Array<Character> characters(out.allocator());
    usize cursor = 0;
    while (cursor < text.size()) {
        Character character;
        character.begin = static_cast<u32>(cursor);
        character.codepoint = decode_utf8(text, cursor);
        character.end = static_cast<u32>(cursor);
        character.klass = line_break_class_of(character.codepoint);
        if (coverage_of(character.codepoint) == Coverage::Assumed) {
            report.assumed_coverage = true;
        }
        if (Status pushed = characters.push_back(character); !pushed) {
            return pushed;
        }
    }

    const auto push = [&out, &report](u32 offset, bool mandatory, bool dictionary_break) noexcept {
        BreakOpportunity opportunity;
        opportunity.offset = offset;
        opportunity.mandatory = mandatory;
        opportunity.from_dictionary = dictionary_break;
        ++report.opportunities;
        if (mandatory) {
            ++report.mandatory;
        }
        if (dictionary_break) {
            report.used_dictionary = true;
        }
        return out.push_back(opportunity);
    };

    for (usize index = 0; index + 1 <= characters.size(); ++index) {
        const Character& current = characters[index];
        // LB4, LB5: a mandatory break after BK, CR (unless CRLF), LF and NL. The break is AFTER the
        // character, so the offset is where the next line starts.
        if (current.klass == LB::Mandatory) {
            if (Status pushed = push(current.end, true, false); !pushed) {
                return pushed;
            }
            continue;
        }
        if (current.klass == LB::CarriageReturn) {
            const bool crlf =
                (index + 1 < characters.size()) && characters[index + 1].klass == LB::Mandatory;
            if (!crlf) {
                if (Status pushed = push(current.end, true, false); !pushed) {
                    return pushed;
                }
            }
            continue;
        }
        if (index + 1 >= characters.size()) {
            break;  // LB3: the end of text is a break, and the caller does not need telling.
        }

        // THE DICTIONARY, for the scripts that have no spaces. Consulted before the pair table,
        // because the table's answer for SA × SA is "no break" and the dictionary's is the point.
        if (current.klass == LB::ComplexContext &&
            characters[index + 1].klass == LB::ComplexContext) {
            if (dictionary != nullptr) {
                const usize matched = dictionary->longest_match(text.substr(current.begin));
                if (matched > 0) {
                    const u32 boundary = current.begin + static_cast<u32>(matched);
                    // Skip to the end of the matched word and break there.
                    while (index + 1 < characters.size() && characters[index].end < boundary) {
                        ++index;
                    }
                    if (index + 1 < characters.size()) {
                        if (Status pushed = push(characters[index].end, false, true); !pushed) {
                            return pushed;
                        }
                    }
                    continue;
                }
                continue;  // Inside a word the dictionary knows: no break here.
            }
            // NO DICTIONARY. Breaking between every pair is wrong for Thai and better than a
            // paragraph that never wraps, and `used_dictionary` stays false so a caller can tell.
            if (Status pushed = push(current.end, false, false); !pushed) {
                return pushed;
            }
            continue;
        }

        // LB7: no break before a space or a zero-width space. Spaces are skipped so that the pair
        // table sees the characters either side of them, which is what "indirect" means.
        if (characters[index + 1].klass == LB::Space ||
            characters[index + 1].klass == LB::ZeroWidthSpace) {
            continue;
        }
        usize before = index;
        bool space_between = false;
        while (before > 0 && (characters[before].klass == LB::Space)) {
            space_between = true;
            --before;
        }
        if (characters[before].klass == LB::Space) {
            continue;  // Leading spaces: nothing to break after.
        }

        const Action action = pair_action(characters[before].klass, characters[index + 1].klass);
        const bool allowed =
            (action == Action::Direct) || (action == Action::Indirect && space_between);
        if (allowed) {
            if (Status pushed = push(current.end, false, false); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Status justify(Span<const JustifyPoint> points, Span<const JustifyPriority> priorities, f32 extra,
               JustifyResult& out) noexcept {
    out.distribution.clear();
    out.residual = 0.0F;
    if (Status sized = out.distribution.resize(points.size()); !sized) {
        return sized;
    }
    for (f32& share : out.distribution.span()) {
        share = 0.0F;
    }
    if (extra <= 0.0F || points.empty()) {
        out.residual = (extra > 0.0F) ? extra : 0.0F;
        return ok();
    }

    f32 remaining = extra;
    // THE PRIORITY ORDER IS THE CALLER'S. Latin justifies on spaces; Arabic reaches for kashida
    // before it stretches spaces, and putting that choice here rather than in a branch on the
    // script is what lets a project disagree with us about Japanese.
    for (const JustifyPriority priority : priorities) {
        if (remaining <= 0.0F) {
            break;
        }
        u32 count = 0;
        f32 capacity = 0.0F;
        bool unbounded = false;
        for (const JustifyPoint& point : points) {
            if (point.kind != priority) {
                continue;
            }
            ++count;
            if (point.limit <= 0.0F) {
                unbounded = true;
            } else {
                capacity += point.limit;
            }
        }
        if (count == 0) {
            continue;
        }
        const f32 share = remaining / static_cast<f32>(count);
        f32 taken = 0.0F;
        for (usize index = 0; index < points.size(); ++index) {
            if (points[index].kind != priority) {
                continue;
            }
            const f32 limit = (points[index].limit <= 0.0F) ? share : points[index].limit;
            const f32 given = (share < limit) ? share : limit;
            out.distribution[index] += given;
            taken += given;
        }
        remaining -= taken;
        if (!unbounded && capacity <= 0.0F) {
            continue;
        }
    }
    out.residual = (remaining > 1e-4F) ? remaining : 0.0F;
    return ok();
}

EllipsisPlan plan_ellipsis(std::string_view text, OverflowMode mode, f32 available,
                           f32 ellipsis_width,
                           f32 (*advance_of)(std::string_view, u32, void*) noexcept,
                           void* user) noexcept {
    EllipsisPlan plan;
    if (advance_of == nullptr || text.empty()) {
        return plan;
    }
    if (ellipsis_width > available) {
        // The ellipsis itself does not fit: the caller clips. An ellipsis wider than its box is not
        // an improvement on the text it replaced.
        return plan;
    }

    // Measure once, forward, recording each character's advance and the running width. Everything
    // below is arithmetic over that array — three passes over the text would be three chances for
    // the measurements to disagree.
    f32 total = 0.0F;
    u32 offset = 0;
    constexpr usize kMaxCharacters = 4096;
    f32 advances[kMaxCharacters];
    u32 offsets[kMaxCharacters];
    usize count = 0;
    while (offset < text.size() && count < kMaxCharacters) {
        const f32 advance = advance_of(text, offset, user);
        advances[count] = advance;
        offsets[count] = offset;
        total += advance;
        usize cursor = offset;
        (void)decode_utf8(text, cursor);
        offset = static_cast<u32>(cursor);
        ++count;
    }
    if (total <= available) {
        return plan;  // It fits; nothing to remove.
    }

    const f32 budget = available - ellipsis_width;
    switch (mode) {
        case OverflowMode::EllipsisEnd: {
            f32 width = 0.0F;
            usize keep = 0;
            while (keep < count && width + advances[keep] <= budget) {
                width += advances[keep];
                ++keep;
            }
            plan.remove_begin = (keep < count) ? offsets[keep] : static_cast<u32>(text.size());
            plan.remove_end = static_cast<u32>(text.size());
            plan.fits = true;
            return plan;
        }
        case OverflowMode::EllipsisStart: {
            f32 width = 0.0F;
            usize keep = count;
            while (keep > 0 && width + advances[keep - 1] <= budget) {
                width += advances[keep - 1];
                --keep;
            }
            plan.remove_begin = 0;
            plan.remove_end = (keep < count) ? offsets[keep] : static_cast<u32>(text.size());
            plan.fits = true;
            return plan;
        }
        case OverflowMode::EllipsisMiddle: {
            // Both ends kept, the middle removed — what a file path wants, and why the mode exists.
            f32 head = 0.0F;
            f32 tail = 0.0F;
            usize left = 0;
            usize right = count;
            while (left < right) {
                if (head <= tail) {
                    if (head + advances[left] + tail > budget) {
                        break;
                    }
                    head += advances[left];
                    ++left;
                } else {
                    if (head + advances[right - 1] + tail > budget) {
                        break;
                    }
                    tail += advances[right - 1];
                    --right;
                }
            }
            plan.remove_begin = (left < count) ? offsets[left] : static_cast<u32>(text.size());
            plan.remove_end = (right < count) ? offsets[right] : static_cast<u32>(text.size());
            plan.fits = true;
            return plan;
        }
        default:
            return plan;
    }
}

f32 shrink_to_fit(f32 natural, f32 available, f32 minimum) noexcept {
    if (natural <= 0.0F || available <= 0.0F) {
        return 1.0F;
    }
    if (natural <= available) {
        return 1.0F;
    }
    return clampf(available / natural, clampf(minimum, 0.05F, 1.0F), 1.0F);
}

}  // namespace cy::text
