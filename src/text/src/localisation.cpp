// Locales, plural rules, message formatting, the string table and pseudo-localisation.
// M8.b task 9.4.

#include <cy/text/localisation.h>

#include <algorithm>
#include <cstring>

namespace cy::text {
namespace {

[[nodiscard]] bool is_lower_alpha(char byte) noexcept {
    return byte >= 'a' && byte <= 'z';
}

[[nodiscard]] bool is_digit(char byte) noexcept {
    return byte >= '0' && byte <= '9';
}

[[nodiscard]] bool is_upper_alpha(char byte) noexcept {
    return byte >= 'A' && byte <= 'Z';
}

[[nodiscard]] bool is_alpha(char byte) noexcept {
    return is_lower_alpha(byte) || is_upper_alpha(byte);
}

[[nodiscard]] char to_lower(char byte) noexcept {
    return (byte >= 'A' && byte <= 'Z') ? static_cast<char>(byte + ('a' - 'A')) : byte;
}

[[nodiscard]] char to_upper(char byte) noexcept {
    return is_lower_alpha(byte) ? static_cast<char>(byte - ('a' - 'A')) : byte;
}

[[nodiscard]] bool language_is(const Locale& locale, const char* code) noexcept {
    return std::strncmp(locale.language, code, sizeof(locale.language)) == 0;
}

/// The right-to-left languages. A list rather than a script lookup because a locale carries a
/// language and only sometimes a script, and `ar` without a script is still Arabic.
[[nodiscard]] bool language_is_rtl(const Locale& locale) noexcept {
    static constexpr const char* kRtl[] = {"ar", "he", "fa", "ur", "yi", "dv", "ps", "sd", "ug"};
    return std::ranges::any_of(
        kRtl, [&locale](const char* code) noexcept { return language_is(locale, code); });
}

[[nodiscard]] Status append(Array<char>& out, std::string_view piece) noexcept {
    for (const char byte : piece) {
        if (Status pushed = out.push_back(byte); !pushed) {
            return pushed;
        }
    }
    return ok();
}

/// The accented replacement for a Latin letter, in UTF-8. Chosen so the letter is still readable —
/// pseudo-localisation is meant to be legible and obviously not English.
[[nodiscard]] std::string_view accented(char byte) noexcept {
    switch (to_lower(byte)) {
        case 'a':
            return (byte == 'a') ? "\xC3\xA1" : "\xC3\x81";
        case 'e':
            return (byte == 'e') ? "\xC3\xA9" : "\xC3\x89";
        case 'i':
            return (byte == 'i') ? "\xC3\xAD" : "\xC3\x8D";
        case 'o':
            return (byte == 'o') ? "\xC3\xB3" : "\xC3\x93";
        case 'u':
            return (byte == 'u') ? "\xC3\xBA" : "\xC3\x9A";
        case 'n':
            return (byte == 'n') ? "\xC3\xB1" : "\xC3\x91";
        case 'c':
            return (byte == 'c') ? "\xC3\xA7" : "\xC3\x87";
        case 's':
            return (byte == 's') ? "\xC5\xA1" : "\xC5\xA0";
        default:
            return {};
    }
}

}  // namespace

bool Locale::operator==(const Locale& other) const noexcept {
    return std::strncmp(language, other.language, sizeof(language)) == 0 &&
           std::strncmp(script, other.script, sizeof(script)) == 0 &&
           std::strncmp(region, other.region, sizeof(region)) == 0;
}

u32 Locale::write(char* out, usize capacity) const noexcept {
    if (out == nullptr || capacity == 0) {
        return 0;
    }
    u32 written = 0;
    const auto put = [&](const char* piece, char separator) noexcept {
        if (piece[0] == '\0') {
            return;
        }
        if (written != 0 && written + 1 < capacity) {
            out[written++] = separator;
        }
        for (usize index = 0; piece[index] != '\0' && written + 1 < capacity; ++index) {
            out[written++] = piece[index];
        }
    };
    put(language, '-');
    put(script, '-');
    put(region, '-');
    out[written] = '\0';
    return written;
}

Name Locale::language_name() const noexcept {
    return Name::intern(std::string_view(language, std::strlen(language)));
}

Expected<Locale, Error> parse_locale(std::string_view tag) noexcept {
    Locale locale;
    if (tag.empty()) {
        return locale;  // the root locale
    }
    usize cursor = 0;
    usize part = 0;
    while (cursor <= tag.size()) {
        usize end = cursor;
        while (end < tag.size() && tag[end] != '-' && tag[end] != '_') {
            ++end;
        }
        const std::string_view piece = tag.substr(cursor, end - cursor);
        if (piece.empty()) {
            return make_unexpected(
                Error{ErrorCode::InvalidArgument, "a locale subtag may not be empty", 0});
        }
        for (const char byte : piece) {
            if (!is_alpha(byte) && !is_digit(byte)) {
                return make_unexpected(
                    Error{ErrorCode::InvalidArgument, "a locale subtag is letters and digits", 0});
            }
        }
        if (part == 0) {
            if (piece.size() < 2 || piece.size() > 3) {
                return make_unexpected(Error{ErrorCode::InvalidArgument,
                                             "a language subtag is two or three letters", 0});
            }
            for (usize index = 0; index < piece.size(); ++index) {
                locale.language[index] = to_lower(piece[index]);
            }
        } else if (piece.size() == 4 && locale.script[0] == '\0') {
            locale.script[0] = to_upper(piece[0]);
            for (usize index = 1; index < 4; ++index) {
                locale.script[index] = to_lower(piece[index]);
            }
        } else if (piece.size() == 2 && locale.region[0] == '\0') {
            locale.region[0] = to_upper(piece[0]);
            locale.region[1] = to_upper(piece[1]);
        } else {
            return make_unexpected(
                Error{ErrorCode::InvalidArgument, "this locale subtag is not understood", 0});
        }
        ++part;
        if (end >= tag.size()) {
            break;
        }
        cursor = end + 1;
    }
    return locale;
}

ParagraphDirection direction_of(const Locale& locale) noexcept {
    // The SCRIPT decides where one is given — `az-Arab` is right-to-left and `az-Latn` is not —
    // and the language decides otherwise.
    if (locale.script[0] != '\0') {
        if (std::strncmp(locale.script, "Arab", 4) == 0 ||
            std::strncmp(locale.script, "Hebr", 4) == 0 ||
            std::strncmp(locale.script, "Thaa", 4) == 0) {
            return ParagraphDirection::RightToLeft;
        }
        return ParagraphDirection::LeftToRight;
    }
    return language_is_rtl(locale) ? ParagraphDirection::RightToLeft
                                   : ParagraphDirection::LeftToRight;
}

const char* plural_category_name(PluralCategory category) noexcept {
    switch (category) {
        case PluralCategory::Zero:
            return "zero";
        case PluralCategory::One:
            return "one";
        case PluralCategory::Two:
            return "two";
        case PluralCategory::Few:
            return "few";
        case PluralCategory::Many:
            return "many";
        case PluralCategory::Other:
        case PluralCategory::Count:
            break;
    }
    return "other";
}

bool plural_rules_known(const Locale& locale) noexcept {
    static constexpr const char* kKnown[] = {"en", "de", "nl", "sv", "da", "no", "es", "it",
                                             "pt", "fr", "ru", "uk", "pl", "cs", "sk", "ar",
                                             "ja", "zh", "ko", "th", "vi", "id", "ms"};
    return std::ranges::any_of(
        kKnown, [&locale](const char* code) noexcept { return language_is(locale, code); });
}

PluralCategory plural_category(const Locale& locale, i64 count) noexcept {
    const i64 value = (count < 0) ? -count : count;
    const i64 last = value % 10;
    const i64 last_two = value % 100;

    // The one-form languages. A message with `one` and `other` in these gets `other`, which is what
    // a Japanese translator expects and what a naive `count == 1` gets wrong.
    if (language_is(locale, "ja") || language_is(locale, "zh") || language_is(locale, "ko") ||
        language_is(locale, "th") || language_is(locale, "vi") || language_is(locale, "id") ||
        language_is(locale, "ms")) {
        return PluralCategory::Other;
    }
    if (language_is(locale, "fr")) {
        // French puts zero and one together, which is the difference from English that catches
        // people out.
        return (value == 0 || value == 1) ? PluralCategory::One : PluralCategory::Other;
    }
    if (language_is(locale, "ru") || language_is(locale, "uk")) {
        if (last == 1 && last_two != 11) {
            return PluralCategory::One;
        }
        if (last >= 2 && last <= 4 && (last_two < 12 || last_two > 14)) {
            return PluralCategory::Few;
        }
        return PluralCategory::Many;
    }
    if (language_is(locale, "pl")) {
        if (value == 1) {
            return PluralCategory::One;
        }
        if (last >= 2 && last <= 4 && (last_two < 12 || last_two > 14)) {
            return PluralCategory::Few;
        }
        return PluralCategory::Many;
    }
    if (language_is(locale, "cs") || language_is(locale, "sk")) {
        if (value == 1) {
            return PluralCategory::One;
        }
        if (value >= 2 && value <= 4) {
            return PluralCategory::Few;
        }
        return PluralCategory::Other;
    }
    if (language_is(locale, "ar")) {
        // Arabic is the language that uses all six, and the reason `PluralCategory` has six
        // enumerators rather than two.
        if (value == 0) {
            return PluralCategory::Zero;
        }
        if (value == 1) {
            return PluralCategory::One;
        }
        if (value == 2) {
            return PluralCategory::Two;
        }
        if (last_two >= 3 && last_two <= 10) {
            return PluralCategory::Few;
        }
        if (last_two >= 11 && last_two <= 99) {
            return PluralCategory::Many;
        }
        return PluralCategory::Other;
    }
    // English and everything else this module does not know: one / other.
    return (value == 1) ? PluralCategory::One : PluralCategory::Other;
}

namespace {

/// Find the argument named `name`, or null.
[[nodiscard]] const MessageArgument* argument_of(Span<const MessageArgument> arguments,
                                                 std::string_view name) noexcept {
    const Name interned = Name::find(name);
    if (interned.is_empty()) {
        return nullptr;
    }
    for (const MessageArgument& argument : arguments) {
        if (argument.name == interned) {
            return &argument;
        }
    }
    return nullptr;
}

/// Parse `{name, plural, one {…} other {…}}` starting after the argument's name, returning the arm
/// for `category` and the offset just past the closing brace.
[[nodiscard]] std::string_view plural_arm(std::string_view pattern, usize& cursor,
                                          PluralCategory category, bool& found) noexcept {
    found = false;
    std::string_view chosen;
    std::string_view fallback;
    while (cursor < pattern.size()) {
        while (cursor < pattern.size() && (pattern[cursor] == ' ' || pattern[cursor] == ',')) {
            ++cursor;
        }
        if (cursor < pattern.size() && pattern[cursor] == '}') {
            ++cursor;
            break;
        }
        const usize name_begin = cursor;
        while (cursor < pattern.size() && pattern[cursor] != ' ' && pattern[cursor] != '{') {
            ++cursor;
        }
        const std::string_view arm_name = pattern.substr(name_begin, cursor - name_begin);
        while (cursor < pattern.size() && pattern[cursor] == ' ') {
            ++cursor;
        }
        if (cursor >= pattern.size() || pattern[cursor] != '{') {
            break;
        }
        ++cursor;
        const usize body_begin = cursor;
        u32 depth = 1;
        while (cursor < pattern.size() && depth != 0) {
            if (pattern[cursor] == '{') {
                ++depth;
            } else if (pattern[cursor] == '}') {
                --depth;
                if (depth == 0) {
                    break;
                }
            }
            ++cursor;
        }
        const std::string_view body = pattern.substr(body_begin, cursor - body_begin);
        if (cursor < pattern.size()) {
            ++cursor;  // past the arm's closing brace
        }
        if (arm_name == plural_category_name(category)) {
            chosen = body;
            found = true;
        } else if (arm_name == "other") {
            fallback = body;
        }
    }
    if (!found && !fallback.empty()) {
        found = true;
        return fallback;
    }
    return chosen;
}

}  // namespace

Status format_message(std::string_view pattern, Span<const MessageArgument> arguments,
                      const Locale& locale, Array<char>& out, FormatReport& report) noexcept {
    out.clear();
    report = FormatReport{};

    usize cursor = 0;
    while (cursor < pattern.size()) {
        const char byte = pattern[cursor];
        if (byte != '{') {
            if (Status written = out.push_back(byte); !written) {
                return written;
            }
            ++cursor;
            continue;
        }
        if (cursor + 1 < pattern.size() && pattern[cursor + 1] == '{') {
            if (Status written = out.push_back('{'); !written) {
                return written;
            }
            cursor += 2;
            continue;
        }
        const usize open = cursor;
        ++cursor;
        const usize name_begin = cursor;
        while (cursor < pattern.size() && pattern[cursor] != '}' && pattern[cursor] != ',') {
            ++cursor;
        }
        const std::string_view name = pattern.substr(name_begin, cursor - name_begin);
        const MessageArgument* argument = argument_of(arguments, name);

        if (cursor < pattern.size() && pattern[cursor] == ',') {
            // A SELECTOR. Only `plural` is understood; anything else is left in place rather than
            // guessed at.
            usize probe = cursor + 1;
            while (probe < pattern.size() && pattern[probe] == ' ') {
                ++probe;
            }
            const usize kind_begin = probe;
            while (probe < pattern.size() && pattern[probe] != ',' && pattern[probe] != '}') {
                ++probe;
            }
            const std::string_view kind = pattern.substr(kind_begin, probe - kind_begin);
            if (kind == "plural" && argument != nullptr && argument->has_count) {
                const PluralCategory category = plural_category(locale, argument->count);
                bool found = false;
                usize arm_cursor = probe;
                const std::string_view arm = plural_arm(pattern, arm_cursor, category, found);
                cursor = arm_cursor;
                report.used_plural = true;
                report.selected = category;
                if (found) {
                    // The arm may itself contain `{name}` placeholders, and the count is the usual
                    // one — so it goes through this same formatter rather than a second path.
                    Array<char> nested(out.allocator());
                    FormatReport nested_report;
                    if (Status formatted =
                            format_message(arm, arguments, locale, nested, nested_report);
                        !formatted) {
                        return formatted;
                    }
                    report.substitutions += nested_report.substitutions + 1U;
                    report.missing += nested_report.missing;
                    if (Status written =
                            append(out, std::string_view(nested.data(), nested.size()));
                        !written) {
                        return written;
                    }
                    continue;
                }
                ++report.missing;
                continue;
            }
            // Not a selector this formatter understands: copy the whole brace group out verbatim so
            // it is visible rather than silently dropped.
            usize depth = 1;
            usize scan = open + 1;
            while (scan < pattern.size() && depth != 0) {
                if (pattern[scan] == '{') {
                    ++depth;
                } else if (pattern[scan] == '}') {
                    --depth;
                }
                ++scan;
            }
            ++report.missing;
            if (Status written = append(out, pattern.substr(open, scan - open)); !written) {
                return written;
            }
            cursor = scan;
            continue;
        }

        if (cursor < pattern.size()) {
            ++cursor;  // past '}'
        }
        if (argument == nullptr) {
            // VISIBLE, NOT EMPTY. A missing argument that renders as nothing is a bug that ships;
            // one that renders as `{name}` is a bug somebody reports.
            ++report.missing;
            if (Status written = append(out, pattern.substr(open, cursor - open)); !written) {
                return written;
            }
            continue;
        }
        ++report.substitutions;
        if (Status written = append(out, argument->value); !written) {
            return written;
        }
    }
    return ok();
}

StringTable::StringTable(Allocator& allocator) noexcept
    : entries_(allocator), missing_(allocator) {}

Status StringTable::add(const Locale& locale, Name key, std::string_view value) noexcept {
    for (Entry& entry : entries_.span()) {
        if (entry.key == key && entry.locale == locale) {
            entry.value = value;
            return ok();
        }
    }
    return entries_.push_back(Entry{locale, key, value});
}

std::string_view StringTable::find(const Locale& locale, Name key) noexcept {
    // THE CHAIN: the full locale, then the language alone, then the root. Three lookups rather than
    // a precomputed chain, because a chain would have to be rebuilt whenever the locale changes and
    // the table is small.
    Locale language_only;
    std::memcpy(language_only.language, locale.language, sizeof(language_only.language));
    const Locale root;

    const Locale* chain[3] = {&locale, &language_only, &root};
    for (const Locale* candidate : chain) {
        for (const Entry& entry : entries_.span()) {
            if (entry.key == key && entry.locale == *candidate) {
                return entry.value;
            }
        }
    }
    ++misses_;
    for (const Name recorded : missing_.span()) {
        if (recorded == key) {
            return {};
        }
    }
    (void)missing_.push_back(key);
    return {};
}

void StringTable::clear_report() noexcept {
    missing_.clear();
    misses_ = 0;
}

Status pseudo_localise(std::string_view text, const PseudoOptions& options,
                       Array<char>& out) noexcept {
    out.clear();
    if (options.brackets) {
        if (Status written = append(out, "["); !written) {
            return written;
        }
    }
    for (usize index = 0; index < text.size(); ++index) {
        const char byte = text[index];
        // A PLACEHOLDER IS NOT TRANSFORMED. Accenting the inside of `{count}` would break the
        // formatter, and a pseudo-localisation that breaks the string it is testing tests nothing.
        if (byte == '{') {
            const usize close = text.find('}', index);
            const usize end = (close == std::string_view::npos) ? text.size() : close + 1;
            if (Status written = append(out, text.substr(index, end - index)); !written) {
                return written;
            }
            index = end - 1;
            continue;
        }
        if (options.accent) {
            const std::string_view replacement = accented(byte);
            if (!replacement.empty()) {
                if (Status written = append(out, replacement); !written) {
                    return written;
                }
                continue;
            }
        }
        if (Status written = out.push_back(byte); !written) {
            return written;
        }
    }
    // EXPANSION: most translations are longer than English, and a layout that only fits English is
    // a layout that breaks on the first one delivered.
    const f32 expansion = std::max(options.expansion, 0.0F);
    const auto padding = static_cast<usize>(static_cast<f32>(text.size()) * expansion);
    for (usize index = 0; index < padding; ++index) {
        if (Status written = out.push_back('\xC2'); !written) {
            return written;
        }
        if (Status written = out.push_back('\xB7'); !written) {  // U+00B7 MIDDLE DOT
            return written;
        }
    }
    if (options.brackets) {
        if (Status written = append(out, "]"); !written) {
            return written;
        }
    }
    return ok();
}

}  // namespace cy::text
