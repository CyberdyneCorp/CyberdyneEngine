// Itemisation, Arabic joining, and the shaping cache. M8.b task 9.4.

#include <cy/text/shaping.h>

namespace cy::text {
namespace {

/// One Arabic letter's four presentation forms, from the Arabic Presentation Forms-B block.
/// Zero means the letter has no form of that kind, which is how a right-joining letter says it has
/// no initial or medial form.
struct ArabicForms {
    Codepoint letter;
    Codepoint isolated;
    Codepoint final_form;
    Codepoint initial;
    Codepoint medial;
    JoiningType joining;
};

/// The Arabic letters, in codepoint order. Complete for the basic Arabic block's letters
/// (U+0621..U+064A) — the set every Arabic string is written in.
constexpr ArabicForms kArabic[] = {
    {0x0621, 0xFE80, 0, 0, 0, JoiningType::NonJoining},           // hamza
    {0x0622, 0xFE81, 0xFE82, 0, 0, JoiningType::Right},           // alef madda
    {0x0623, 0xFE83, 0xFE84, 0, 0, JoiningType::Right},           // alef hamza above
    {0x0624, 0xFE85, 0xFE86, 0, 0, JoiningType::Right},           // waw hamza
    {0x0625, 0xFE87, 0xFE88, 0, 0, JoiningType::Right},           // alef hamza below
    {0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C, JoiningType::Dual},  // yeh hamza
    {0x0627, 0xFE8D, 0xFE8E, 0, 0, JoiningType::Right},           // alef
    {0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92, JoiningType::Dual},  // beh
    {0x0629, 0xFE93, 0xFE94, 0, 0, JoiningType::Right},           // teh marbuta
    {0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98, JoiningType::Dual},  // teh
    {0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C, JoiningType::Dual},  // theh
    {0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0, JoiningType::Dual},  // jeem
    {0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4, JoiningType::Dual},  // hah
    {0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8, JoiningType::Dual},  // khah
    {0x062F, 0xFEA9, 0xFEAA, 0, 0, JoiningType::Right},           // dal
    {0x0630, 0xFEAB, 0xFEAC, 0, 0, JoiningType::Right},           // thal
    {0x0631, 0xFEAD, 0xFEAE, 0, 0, JoiningType::Right},           // reh
    {0x0632, 0xFEAF, 0xFEB0, 0, 0, JoiningType::Right},           // zain
    {0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4, JoiningType::Dual},  // seen
    {0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8, JoiningType::Dual},  // sheen
    {0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC, JoiningType::Dual},  // sad
    {0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0, JoiningType::Dual},  // dad
    {0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4, JoiningType::Dual},  // tah
    {0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8, JoiningType::Dual},  // zah
    {0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC, JoiningType::Dual},  // ain
    {0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0, JoiningType::Dual},  // ghain
    {0x0640, 0x0640, 0x0640, 0x0640, 0x0640, JoiningType::Dual},  // tatweel (kashida)
    {0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4, JoiningType::Dual},  // feh
    {0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8, JoiningType::Dual},  // qaf
    {0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC, JoiningType::Dual},  // kaf
    {0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0, JoiningType::Dual},  // lam
    {0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4, JoiningType::Dual},  // meem
    {0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8, JoiningType::Dual},  // noon
    {0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC, JoiningType::Dual},  // heh
    {0x0648, 0xFEED, 0xFEEE, 0, 0, JoiningType::Right},           // waw
    {0x0649, 0xFEEF, 0xFEF0, 0, 0, JoiningType::Right},           // alef maksura
    {0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4, JoiningType::Dual},  // yeh
};

/// The lam-alef ligature, which is MANDATORY in Arabic: lam followed by any alef is one glyph, and
/// rendering them as two is not a stylistic choice but an error.
struct LamAlef {
    Codepoint alef;
    Codepoint isolated;
    Codepoint final_form;
};

constexpr LamAlef kLamAlef[] = {
    {0x0622, 0xFEF5, 0xFEF6},
    {0x0623, 0xFEF7, 0xFEF8},
    {0x0625, 0xFEF9, 0xFEFA},
    {0x0627, 0xFEFB, 0xFEFC},
};

[[nodiscard]] const ArabicForms* forms_of(Codepoint codepoint) noexcept {
    for (const ArabicForms& entry : kArabic) {
        if (entry.letter == codepoint) {
            return &entry;
        }
    }
    return nullptr;
}

[[nodiscard]] const LamAlef* lam_alef_of(Codepoint codepoint) noexcept {
    for (const LamAlef& entry : kLamAlef) {
        if (entry.alef == codepoint) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

ShapingCapabilities shaping_capabilities() noexcept {
    return ShapingCapabilities{};
}

JoiningType joining_type_of(Codepoint codepoint) noexcept {
    // A MARK IS TRANSPARENT, and this is the rule an implementation usually forgets: a vowelled
    // Arabic word still joins across its marks, and treating a mark as non-joining breaks every
    // word that carries one.
    if ((codepoint >= 0x064B && codepoint <= 0x065F) || codepoint == 0x0670 ||
        (codepoint >= 0x06D6 && codepoint <= 0x06DC) ||
        (codepoint >= 0x0610 && codepoint <= 0x061A) || codepoint == 0x200D) {
        return JoiningType::Transparent;
    }
    const ArabicForms* entry = forms_of(codepoint);
    return (entry != nullptr) ? entry->joining : JoiningType::NonJoining;
}

Status itemise(std::string_view text, Span<const BidiRun> levels, const FaceCoverage* coverage,
               Array<TextRun>& out) noexcept {
    out.clear();
    usize cursor = 0;
    while (cursor < text.size()) {
        const u32 begin = static_cast<u32>(cursor);
        const Codepoint codepoint = decode_utf8(text, cursor);
        const u32 end = static_cast<u32>(cursor);

        Script script = script_of(codepoint);
        u8 level = 0;
        for (const BidiRun& run : levels) {
            if (begin >= run.begin && begin < run.end) {
                level = run.level;
                break;
            }
        }
        u32 face = 0;
        if (coverage != nullptr) {
            const u32 found = coverage->face_for(codepoint);
            face = (found == FaceCoverage::kNoFace) ? 0U : found;
        }

        if (!out.empty()) {
            TextRun& last = out[out.size() - 1];
            // COMMON joins whatever it is beside — a space between two Hebrew words must not split
            // the run, or every space becomes a batch boundary.
            const bool script_matches =
                (script == last.script) || (script == Script::Common) ||
                (last.script == Script::Common && script != Script::Unknown);
            if (script_matches && last.level == level && last.face == face && last.end == begin) {
                last.end = end;
                if (last.script == Script::Common && script != Script::Common) {
                    last.script = script;
                }
                continue;
            }
        }
        TextRun run;
        run.begin = begin;
        run.end = end;
        run.script = script;
        run.level = level;
        run.face = face;
        if (Status pushed = out.push_back(run); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status join_arabic(std::string_view text, u32 begin, u32 end, Array<JoinedGlyph>& out) noexcept {
    if (begin > end || end > text.size()) {
        return make_unexpected(
            Error{ErrorCode::OutOfRange, "the run's range is outside the text", 0});
    }

    // Decode the run once. Joining needs to look both ways, and a second decode pass is a second
    // chance for the two to disagree about where a character starts.
    struct Item {
        Codepoint codepoint = 0;
        u32 offset = 0;
        JoiningType joining = JoiningType::NonJoining;
    };
    Array<Item> items(out.allocator());
    usize cursor = begin;
    while (cursor < end) {
        Item item;
        item.offset = static_cast<u32>(cursor);
        item.codepoint = decode_utf8(text, cursor);
        item.joining = joining_type_of(item.codepoint);
        if (Status pushed = items.push_back(item); !pushed) {
            return pushed;
        }
    }

    const auto previous_joining = [&items](usize index) noexcept {
        for (usize probe = index; probe > 0; --probe) {
            if (items[probe - 1].joining != JoiningType::Transparent) {
                return items[probe - 1].joining;
            }
        }
        return JoiningType::NonJoining;
    };
    const auto next_joining = [&items](usize index) noexcept {
        for (usize probe = index + 1; probe < items.size(); ++probe) {
            if (items[probe].joining != JoiningType::Transparent) {
                return items[probe].joining;
            }
        }
        return JoiningType::NonJoining;
    };

    for (usize index = 0; index < items.size(); ++index) {
        const Item& item = items[index];
        JoinedGlyph glyph;
        glyph.source = item.codepoint;
        glyph.presentation = item.codepoint;
        glyph.source_offset = item.offset;

        const ArabicForms* entry = forms_of(item.codepoint);
        if (entry == nullptr) {
            // Not an Arabic letter — a mark, a digit, a space, or another script. It passes through
            // unchanged, so a caller may hand this a mixed run.
            if (Status pushed = out.push_back(glyph); !pushed) {
                return pushed;
            }
            continue;
        }

        // Does the previous letter join forwards, and does the next join backwards?
        const JoiningType before = previous_joining(index);
        const JoiningType after = next_joining(index);
        const bool joins_before = (before == JoiningType::Dual);
        const bool joins_after = (after == JoiningType::Dual || after == JoiningType::Right) &&
                                 (entry->joining == JoiningType::Dual);

        // LAM-ALEF, checked before the forms: it consumes two characters and produces one glyph.
        if (item.codepoint == 0x0644 && index + 1 < items.size()) {
            const LamAlef* ligature = lam_alef_of(items[index + 1].codepoint);
            if (ligature != nullptr) {
                glyph.ligature = true;
                glyph.presentation = joins_before ? ligature->final_form : ligature->isolated;
                glyph.form = joins_before ? JoiningForm::Final : JoiningForm::Isolated;
                if (Status pushed = out.push_back(glyph); !pushed) {
                    return pushed;
                }
                ++index;  // the alef is part of the ligature
                continue;
            }
        }

        if (joins_before && joins_after && entry->medial != 0) {
            glyph.form = JoiningForm::Medial;
            glyph.presentation = entry->medial;
        } else if (joins_before && entry->final_form != 0) {
            glyph.form = JoiningForm::Final;
            glyph.presentation = entry->final_form;
        } else if (joins_after && entry->initial != 0) {
            glyph.form = JoiningForm::Initial;
            glyph.presentation = entry->initial;
        } else {
            glyph.form = JoiningForm::Isolated;
            glyph.presentation = entry->isolated;
        }
        if (Status pushed = out.push_back(glyph); !pushed) {
            return pushed;
        }
    }
    return ok();
}

u64 hash_content(std::string_view text) noexcept {
    // FNV-1a, 64-bit. Fixed constants, so a shaped run cached in one process is keyed identically
    // in the next — which matters the day this cache is warmed from a cook.
    u64 hash = 0xCBF29CE484222325ULL;
    for (const char byte : text) {
        hash ^= static_cast<u64>(static_cast<u8>(byte));
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

u64 ShapeKey::hash() const noexcept {
    u64 value = content;
    value ^= static_cast<u64>(face) * 0x9E3779B97F4A7C15ULL;
    value ^= static_cast<u64>(size_sixteenths) * 0xC2B2AE3D27D4EB4FULL;
    value ^= static_cast<u64>(level) * 0x165667B19E3779F9ULL;
    value ^= static_cast<u64>(script) * 0x27220A95ULL;
    value ^= static_cast<u64>(language) * 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 31U;
    return value;
}

bool operator==(const ShapeKey& a, const ShapeKey& b) noexcept {
    return a.content == b.content && a.face == b.face && a.size_sixteenths == b.size_sixteenths &&
           a.level == b.level && a.script == b.script && a.language == b.language;
}

ShapeCache::ShapeCache(Allocator& allocator, usize capacity) noexcept
    : entries_(allocator), capacity_((capacity == 0) ? 1U : capacity) {}

bool ShapeCache::find(const ShapeKey& key, u32& value) noexcept {
    for (Entry& entry : entries_.span()) {
        if (entry.key == key) {
            entry.used = ++clock_;
            value = entry.value;
            ++hits_;
            return true;
        }
    }
    ++misses_;
    return false;
}

Status ShapeCache::insert(const ShapeKey& key, u32 value) noexcept {
    for (Entry& entry : entries_.span()) {
        if (entry.key == key) {
            entry.value = value;
            entry.used = ++clock_;
            return ok();
        }
    }
    if (entries_.size() >= capacity_) {
        // LEAST RECENTLY USED. A cache that evicted the newest entry would thrash exactly when the
        // working set is the size of the cache, which is the case it exists for.
        usize victim = 0;
        for (usize index = 1; index < entries_.size(); ++index) {
            if (entries_[index].used < entries_[victim].used) {
                victim = index;
            }
        }
        entries_[victim] = Entry{key, value, ++clock_};
        ++evictions_;
        return ok();
    }
    return entries_.push_back(Entry{key, value, ++clock_});
}

void ShapeCache::clear() noexcept {
    entries_.clear();
}

void ShapeCache::invalidate_language(u32 language) noexcept {
    usize index = 0;
    while (index < entries_.size()) {
        if (entries_[index].key.language == language) {
            entries_.remove_unordered(index);
            continue;
        }
        ++index;
    }
}

f32 ShapeCache::hit_rate() const noexcept {
    const u64 total = hits_ + misses_;
    if (total == 0) {
        return 0.0F;
    }
    return static_cast<f32>(static_cast<f64>(hits_) / static_cast<f64>(total));
}

}  // namespace cy::text
