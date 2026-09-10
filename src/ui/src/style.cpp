// The `.cyss` parser, the matcher and the cascade. M8.b task 9.2.

#include <cy/ui/style.h>

#include <cstring>

namespace cy::ui {
namespace {

struct PropertyRow {
    const char* name;
    StyleProperty property;
    bool inherits;
};

/// THE CLOSED SET. Divergence 1: a property not in this table is an error with a location, not a
/// declaration quietly dropped.
constexpr PropertyRow kProperties[] = {
    {"background-color", StyleProperty::BackgroundColour, false},
    {"border-color", StyleProperty::BorderColour, false},
    {"border-width", StyleProperty::BorderWidth, false},
    {"corner-radius", StyleProperty::CornerRadius, false},
    {"opacity", StyleProperty::Opacity, false},
    {"color", StyleProperty::Colour, true},
    {"font-size", StyleProperty::FontSize, true},
    {"font-family", StyleProperty::FontFamily, true},
    {"width", StyleProperty::Width, false},
    {"height", StyleProperty::Height, false},
    {"min-width", StyleProperty::MinWidth, false},
    {"min-height", StyleProperty::MinHeight, false},
    {"max-width", StyleProperty::MaxWidth, false},
    {"max-height", StyleProperty::MaxHeight, false},
    {"margin-left", StyleProperty::MarginLeft, false},
    {"margin-top", StyleProperty::MarginTop, false},
    {"margin-right", StyleProperty::MarginRight, false},
    {"margin-bottom", StyleProperty::MarginBottom, false},
    {"padding-left", StyleProperty::PaddingLeft, false},
    {"padding-top", StyleProperty::PaddingTop, false},
    {"padding-right", StyleProperty::PaddingRight, false},
    {"padding-bottom", StyleProperty::PaddingBottom, false},
    {"flex-grow", StyleProperty::FlexGrow, false},
    {"flex-shrink", StyleProperty::FlexShrink, false},
    {"flex-basis", StyleProperty::FlexBasis, false},
    {"flex-direction", StyleProperty::FlexDirectionProperty, false},
    {"justify-content", StyleProperty::JustifyContent, false},
    {"align-items", StyleProperty::AlignItems, false},
    {"align-self", StyleProperty::AlignSelf, false},
    {"gap", StyleProperty::Gap, false},
    {"aspect-ratio", StyleProperty::AspectRatio, false},
    {"display", StyleProperty::Display, false},
    {"transition-duration", StyleProperty::TransitionDuration, false},
};

[[nodiscard]] bool is_space(char byte) noexcept {
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

[[nodiscard]] bool is_ident(char byte) noexcept {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
}

[[nodiscard]] u32 hex_of(char byte) noexcept {
    if (byte >= '0' && byte <= '9') {
        return static_cast<u32>(byte - '0');
    }
    if (byte >= 'a' && byte <= 'f') {
        return static_cast<u32>(byte - 'a') + 10U;
    }
    if (byte >= 'A' && byte <= 'F') {
        return static_cast<u32>(byte - 'A') + 10U;
    }
    return 0xFFFFFFFFU;
}

/// `#rgb`, `#rrggbb` and `#rrggbbaa`, premultiplied on the way in so painting never multiplies.
[[nodiscard]] bool parse_colour(std::string_view text, u32& out) noexcept {
    if (text.empty() || text[0] != '#') {
        return false;
    }
    const std::string_view digits = text.substr(1);
    u32 channels[4] = {0, 0, 0, 255};
    if (digits.size() == 3 || digits.size() == 4) {
        for (usize index = 0; index < digits.size(); ++index) {
            const u32 value = hex_of(digits[index]);
            if (value == 0xFFFFFFFFU) {
                return false;
            }
            channels[index] = (value * 16U) + value;
        }
    } else if (digits.size() == 6 || digits.size() == 8) {
        for (usize index = 0; index * 2 < digits.size(); ++index) {
            const u32 high = hex_of(digits[index * 2]);
            const u32 low = hex_of(digits[(index * 2) + 1]);
            if (high == 0xFFFFFFFFU || low == 0xFFFFFFFFU) {
                return false;
            }
            channels[index] = (high * 16U) + low;
        }
    } else {
        return false;
    }
    out = (channels[3] << 24U) | (channels[0] << 16U) | (channels[1] << 8U) | channels[2];
    return true;
}

[[nodiscard]] bool parse_number(std::string_view text, f32& out) noexcept {
    if (text.empty()) {
        return false;
    }
    usize cursor = 0;
    f32 sign = 1.0F;
    if (text[cursor] == '-') {
        sign = -1.0F;
        ++cursor;
    } else if (text[cursor] == '+') {
        ++cursor;
    }
    f32 value = 0.0F;
    bool digits = false;
    while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
        value = (value * 10.0F) + static_cast<f32>(text[cursor] - '0');
        ++cursor;
        digits = true;
    }
    if (cursor < text.size() && text[cursor] == '.') {
        ++cursor;
        f32 scale = 0.1F;
        while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
            value += static_cast<f32>(text[cursor] - '0') * scale;
            scale *= 0.1F;
            ++cursor;
            digits = true;
        }
    }
    if (!digits) {
        return false;
    }
    // DIVERGENCE 2: a bare number and `px` are the same thing, and both are REFERENCE UNITS. `em`,
    // `%` and the viewport units are not in the subset, so they are refused rather than guessed.
    const std::string_view rest = text.substr(cursor);
    if (!rest.empty() && rest != "px") {
        return false;
    }
    out = value * sign;
    return true;
}

struct Lexer {
    std::string_view text;
    usize cursor = 0;
    u32 line = 1;
    u32 column = 1;

    void advance(usize count) noexcept {
        for (usize step = 0; step < count && cursor < text.size(); ++step) {
            if (text[cursor] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
            ++cursor;
        }
    }

    void skip_trivia() noexcept {
        while (cursor < text.size()) {
            if (is_space(text[cursor])) {
                advance(1);
                continue;
            }
            if (text.compare(cursor, 2, "/*") == 0) {
                const usize close = text.find("*/", cursor + 2);
                advance((close == std::string_view::npos) ? (text.size() - cursor)
                                                          : (close + 2 - cursor));
                continue;
            }
            break;
        }
    }

    [[nodiscard]] std::string_view take_while(bool (*predicate)(char)) noexcept {
        const usize begin = cursor;
        while (cursor < text.size() && predicate(text[cursor])) {
            advance(1);
        }
        return text.substr(begin, cursor - begin);
    }
};

}  // namespace

/// The parser is a friend of `StyleSheet` so the sheet's arrays stay private to everyone else.
class StyleParser {
public:
    StyleParser(std::string_view text, StyleSheet& sheet) noexcept : lexer_{text}, sheet_(&sheet) {}

    [[nodiscard]] Status run() noexcept {
        while (true) {
            lexer_.skip_trivia();
            if (lexer_.cursor >= lexer_.text.size()) {
                break;
            }
            if (Status parsed = parse_rule(); !parsed) {
                return parsed;
            }
        }
        return ok();
    }

private:
    [[nodiscard]] Status report(const char* message, Name detail = Name{}) noexcept {
        StyleDiagnostic diagnostic;
        diagnostic.message = message;
        diagnostic.detail = detail;
        diagnostic.line = lexer_.line;
        diagnostic.column = lexer_.column;
        return sheet_->diagnostics_.push_back(diagnostic);
    }

    [[nodiscard]] Status parse_rule() noexcept {
        StyleRule rule;
        rule.line = lexer_.line;
        rule.order = static_cast<u32>(sheet_->rules_.size());
        bool root_block = false;

        // The selector chain, up to the opening brace.
        while (lexer_.cursor < lexer_.text.size() && lexer_.text[lexer_.cursor] != '{') {
            lexer_.skip_trivia();
            if (lexer_.cursor >= lexer_.text.size() || lexer_.text[lexer_.cursor] == '{') {
                break;
            }
            if (lexer_.text[lexer_.cursor] == '>') {
                lexer_.advance(1);
                if (rule.part_count > 0) {
                    rule.parts[rule.part_count - 1].child_combinator = true;
                }
                continue;
            }
            if (lexer_.text.compare(lexer_.cursor, 5, ":root") == 0) {
                lexer_.advance(5);
                root_block = true;
                continue;
            }
            if (rule.part_count >= 4) {
                if (Status reported = report("a selector chain of more than four parts is outside "
                                             "the `.cyss` subset");
                    !reported) {
                    return reported;
                }
                lexer_.advance(1);
                continue;
            }
            SelectorPart part;
            bool parsed_any = false;
            while (lexer_.cursor < lexer_.text.size()) {
                const char byte = lexer_.text[lexer_.cursor];
                if (byte == '.') {
                    lexer_.advance(1);
                    const std::string_view name = lexer_.take_while(is_ident);
                    if (part.class_count < 4) {
                        part.classes[part.class_count++] = Name::intern(name);
                    }
                    parsed_any = true;
                } else if (byte == '#') {
                    lexer_.advance(1);
                    part.id = Name::intern(lexer_.take_while(is_ident));
                    parsed_any = true;
                } else if (byte == ':') {
                    lexer_.advance(1);
                    const std::string_view name = lexer_.take_while(is_ident);
                    if (name == "hover") {
                        part.states = part.states | PseudoState::Hover;
                    } else if (name == "focus") {
                        part.states = part.states | PseudoState::Focus;
                    } else if (name == "active") {
                        part.states = part.states | PseudoState::Active;
                    } else if (name == "disabled") {
                        part.states = part.states | PseudoState::Disabled;
                    } else if (name == "checked") {
                        part.states = part.states | PseudoState::Checked;
                    } else if (name == "first-child") {
                        part.states = part.states | PseudoState::FirstChild;
                    } else if (name == "last-child") {
                        part.states = part.states | PseudoState::LastChild;
                    } else if (Status reported =
                                   report("this pseudo-state is outside the `.cyss` subset",
                                          Name::intern(name));
                               !reported) {
                        return reported;
                    }
                    parsed_any = true;
                } else if (is_ident(byte)) {
                    part.type = Name::intern(lexer_.take_while(is_ident));
                    parsed_any = true;
                } else {
                    break;
                }
            }
            if (!parsed_any) {
                lexer_.advance(1);
                continue;
            }
            rule.parts[rule.part_count++] = part;
        }

        if (lexer_.cursor >= lexer_.text.size()) {
            return report("a rule with no body");
        }
        lexer_.advance(1);  // past '{'

        rule.first_declaration = static_cast<u32>(sheet_->declarations_.size());
        while (lexer_.cursor < lexer_.text.size() && lexer_.text[lexer_.cursor] != '}') {
            lexer_.skip_trivia();
            if (lexer_.cursor >= lexer_.text.size() || lexer_.text[lexer_.cursor] == '}') {
                break;
            }
            if (Status parsed = parse_declaration(root_block); !parsed) {
                return parsed;
            }
        }
        if (lexer_.cursor < lexer_.text.size()) {
            lexer_.advance(1);  // past '}'
        }
        rule.declaration_count =
            static_cast<u32>(sheet_->declarations_.size()) - rule.first_declaration;

        if (root_block) {
            // `:root` declares custom properties and is not a rule anything matches.
            return ok();
        }
        // SPECIFICITY: (ids, classes and pseudo-states, types), packed so a lexicographic
        // comparison is one integer comparison. Divergence 4.
        u32 ids = 0;
        u32 classes = 0;
        u32 types = 0;
        for (u32 index = 0; index < rule.part_count; ++index) {
            const SelectorPart& part = rule.parts[index];
            ids += part.id.is_empty() ? 0U : 1U;
            classes += part.class_count;
            for (u32 bit = 0; bit < 7U; ++bit) {
                if ((static_cast<u16>(part.states) & (1U << bit)) != 0U) {
                    ++classes;
                }
            }
            types += part.type.is_empty() ? 0U : 1U;
        }
        rule.specificity = (ids << 20U) | (classes << 10U) | types;
        return sheet_->rules_.push_back(rule);
    }

    [[nodiscard]] Status parse_declaration(bool root_block) noexcept {
        lexer_.skip_trivia();
        const bool custom = lexer_.text.compare(lexer_.cursor, 2, "--") == 0;
        if (custom) {
            lexer_.advance(2);
        }
        const u32 line = lexer_.line;
        const std::string_view property = lexer_.take_while(is_ident);
        lexer_.skip_trivia();
        if (lexer_.cursor >= lexer_.text.size() || lexer_.text[lexer_.cursor] != ':') {
            return report("a declaration needs a colon", Name::intern(property));
        }
        lexer_.advance(1);
        lexer_.skip_trivia();

        const usize value_begin = lexer_.cursor;
        while (lexer_.cursor < lexer_.text.size() && lexer_.text[lexer_.cursor] != ';' &&
               lexer_.text[lexer_.cursor] != '}') {
            lexer_.advance(1);
        }
        std::string_view raw = lexer_.text.substr(value_begin, lexer_.cursor - value_begin);
        while (!raw.empty() && is_space(raw[raw.size() - 1])) {
            raw = raw.substr(0, raw.size() - 1);
        }
        if (lexer_.cursor < lexer_.text.size() && lexer_.text[lexer_.cursor] == ';') {
            lexer_.advance(1);
        }

        StyleDeclaration declaration;
        declaration.line = line;
        if (raw.starts_with("var(") && raw.size() > 5 && raw[raw.size() - 1] == ')') {
            std::string_view reference = raw.substr(4, raw.size() - 5);
            if (reference.starts_with("--")) {
                reference = reference.substr(2);
            }
            declaration.value.kind = StyleValue::Kind::Reference;
            declaration.value.keyword = Name::intern(reference);
        } else {
            u32 colour = 0;
            f32 number = 0.0F;
            if (parse_colour(raw, colour)) {
                declaration.value.kind = StyleValue::Kind::Colour;
                declaration.value.colour = colour;
            } else if (parse_number(raw, number)) {
                declaration.value.kind = StyleValue::Kind::Number;
                declaration.value.number = number;
            } else {
                declaration.value.kind = StyleValue::Kind::Keyword;
                declaration.value.keyword = Name::intern(raw);
            }
        }

        if (custom) {
            declaration.custom = Name::intern(property);
            declaration.property = StyleProperty::Count;
            return root_block ? sheet_->custom_.push_back(declaration)
                              : sheet_->declarations_.push_back(declaration);
        }
        const StyleProperty resolved = style_property_from_name(property);
        if (resolved == StyleProperty::Count) {
            // THE DIAGNOSTIC THE REQUIREMENT ASKS FOR, by name and by location.
            return report("this property is outside the `.cyss` subset", Name::intern(property));
        }
        declaration.property = resolved;
        return root_block ? sheet_->custom_.push_back(declaration)
                          : sheet_->declarations_.push_back(declaration);
    }

    Lexer lexer_;
    StyleSheet* sheet_;
};

const char* style_property_name(StyleProperty property) noexcept {
    for (const PropertyRow& row : kProperties) {
        if (row.property == property) {
            return row.name;
        }
    }
    return "unknown";
}

StyleProperty style_property_from_name(std::string_view text) noexcept {
    for (const PropertyRow& row : kProperties) {
        if (text == row.name) {
            return row.property;
        }
    }
    return StyleProperty::Count;
}

bool style_property_inherits(StyleProperty property) noexcept {
    for (const PropertyRow& row : kProperties) {
        if (row.property == property) {
            return row.inherits;
        }
    }
    return false;
}

StyleSheet::StyleSheet(Allocator& allocator) noexcept
    : rules_(allocator), declarations_(allocator), diagnostics_(allocator), custom_(allocator) {}

const StyleValue* StyleSheet::custom_property(Name name) const noexcept {
    for (const StyleDeclaration& declaration : custom_.span()) {
        if (declaration.custom == name) {
            return &declaration.value;
        }
    }
    return nullptr;
}

Status parse_stylesheet(std::string_view text, StyleSheet& out) noexcept {
    StyleParser parser(text, out);
    return parser.run();
}

const ResolvedProperty* ResolvedStyle::find(StyleProperty property) const noexcept {
    for (const ResolvedProperty& entry : properties_.span()) {
        if (entry.property == property) {
            return &entry;
        }
    }
    return nullptr;
}

Status ResolvedStyle::set(const ResolvedProperty& property) noexcept {
    for (ResolvedProperty& entry : properties_.span()) {
        if (entry.property == property.property) {
            entry = property;
            entry.overrode = true;
            return ok();
        }
    }
    return properties_.push_back(property);
}

namespace {

[[nodiscard]] bool part_matches(const SelectorPart& part, const StyleSubject& subject) noexcept {
    if (!part.type.is_empty() && part.type != subject.type) {
        return false;
    }
    if (!part.id.is_empty() && part.id != subject.id) {
        return false;
    }
    for (u32 index = 0; index < part.class_count; ++index) {
        bool found = false;
        for (u32 candidate = 0; candidate < subject.class_count; ++candidate) {
            if (subject.classes[candidate] == part.classes[index]) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    // Every pseudo-state the selector names must be present. A `:hover:checked` selector is both.
    return (static_cast<u16>(part.states) & ~static_cast<u16>(subject.states)) == 0U;
}

}  // namespace

bool rule_matches(const StyleRule& rule, const StyleSubject& subject) noexcept {
    if (rule.part_count == 0) {
        return false;
    }
    // The last part matches the subject; the earlier parts match its ancestors, in order, with `>`
    // requiring the immediate parent.
    if (!part_matches(rule.parts[rule.part_count - 1], subject)) {
        return false;
    }
    usize ancestor = 0;
    for (u32 index = rule.part_count - 1; index > 0; --index) {
        const SelectorPart& part = rule.parts[index - 1];
        const bool immediate = part.child_combinator;
        bool matched = false;
        while (ancestor < subject.ancestors.size()) {
            const StyleSubject* candidate = subject.ancestors[ancestor];
            ++ancestor;
            if (candidate != nullptr && part_matches(part, *candidate)) {
                matched = true;
                break;
            }
            if (immediate) {
                return false;  // `>` gets exactly one chance: the immediate parent.
            }
        }
        if (!matched) {
            return false;
        }
    }
    return true;
}

Status resolve_style(const StyleSheet& sheet, const Theme* theme, const StyleSubject& subject,
                     Span<const StyleDeclaration> inline_overrides, const ResolvedStyle* inherited,
                     ResolvedStyle& out, Array<StyleDiagnostic>& diagnostics) noexcept {
    out.clear();

    // THE ORDER IS DIVERGENCE 3's, applied from the WEAKEST source first so that each stronger one
    // overwrites what the previous set — which is also why `ResolvedProperty::overrode` is true
    // exactly when something was replaced.
    // 5. Engine defaults are whatever the element already carries: this function only writes what a
    //    sheet, a theme, an ancestor or an inline override actually declared.
    // 4. The theme.
    const StyleSheet* sources[2] = {(theme != nullptr) ? theme->sheet : nullptr, &sheet};
    for (const StyleSheet* source : sources) {
        if (source == nullptr) {
            continue;
        }
        // A stable, simple application: iterate rules in document order and let a later rule win
        // when its specificity is at least the winner's. That is CSS's tie-break with one pass.
        for (const StyleRule& rule : source->rules()) {
            if (!rule_matches(rule, subject)) {
                continue;
            }
            for (u32 index = 0; index < rule.declaration_count; ++index) {
                const StyleDeclaration& declaration =
                    source->declarations()[rule.first_declaration + index];
                if (declaration.property == StyleProperty::Count) {
                    continue;
                }
                const ResolvedProperty* existing = out.find(declaration.property);
                if (existing != nullptr && existing->specificity > rule.specificity) {
                    continue;
                }
                ResolvedProperty resolved;
                resolved.property = declaration.property;
                resolved.value = declaration.value;
                resolved.rule = rule.order;
                resolved.specificity = rule.specificity;

                // `var()` RESOLVES AT MATCH TIME, and a reference with no definition is an error
                // rather than a silent zero. Divergence 5.
                if (resolved.value.kind == StyleValue::Kind::Reference) {
                    const StyleValue* value = source->custom_property(resolved.value.keyword);
                    if (value == nullptr && theme != nullptr && theme->sheet != nullptr) {
                        value = theme->sheet->custom_property(resolved.value.keyword);
                    }
                    if (value == nullptr) {
                        StyleDiagnostic diagnostic;
                        diagnostic.message = "this custom property is not defined";
                        diagnostic.detail = resolved.value.keyword;
                        diagnostic.line = declaration.line;
                        if (Status pushed = diagnostics.push_back(diagnostic); !pushed) {
                            return pushed;
                        }
                        continue;
                    }
                    resolved.value = *value;
                }
                if (Status set = out.set(resolved); !set) {
                    return set;
                }
            }
        }
    }

    // 3. Inherited properties from ancestors, for the properties that inherit and that nothing
    // above
    //    set.
    if (inherited != nullptr) {
        for (const ResolvedProperty& property : inherited->properties()) {
            if (!style_property_inherits(property.property) ||
                out.find(property.property) != nullptr) {
                continue;
            }
            ResolvedProperty copied = property;
            copied.rule = ResolvedProperty::kNoRule;
            copied.specificity = 0;
            if (Status set = out.set(copied); !set) {
                return set;
            }
        }
    }

    // 1. Inline overrides, which always win. There is no `!important` to interact with.
    for (const StyleDeclaration& declaration : inline_overrides) {
        if (declaration.property == StyleProperty::Count) {
            continue;
        }
        ResolvedProperty resolved;
        resolved.property = declaration.property;
        resolved.value = declaration.value;
        resolved.rule = ResolvedProperty::kNoRule;
        resolved.specificity = 0xFFFFFFFFU;
        if (Status set = out.set(resolved); !set) {
            return set;
        }
    }
    return ok();
}

namespace {

/// The keyword tables. Both spellings of `centre` are accepted, and both CSS's `flex-start` and the
/// bare `start`, because a developer who knows CSS should not have to look up which one this engine
/// wanted — `ui-system`: "so existing understanding transfers".
[[nodiscard]] FlexDirection direction_from_keyword(std::string_view keyword) noexcept {
    if (keyword == "column") {
        return FlexDirection::Column;
    }
    if (keyword == "row-reverse") {
        return FlexDirection::RowReverse;
    }
    if (keyword == "column-reverse") {
        return FlexDirection::ColumnReverse;
    }
    return FlexDirection::Row;
}

[[nodiscard]] Justify justify_from_keyword(std::string_view keyword) noexcept {
    if (keyword == "center" || keyword == "centre") {
        return Justify::Centre;
    }
    if (keyword == "end" || keyword == "flex-end") {
        return Justify::End;
    }
    if (keyword == "space-between") {
        return Justify::SpaceBetween;
    }
    if (keyword == "space-around") {
        return Justify::SpaceAround;
    }
    if (keyword == "space-evenly") {
        return Justify::SpaceEvenly;
    }
    return Justify::Start;
}

[[nodiscard]] Align align_from_keyword(std::string_view keyword) noexcept {
    if (keyword == "center" || keyword == "centre") {
        return Align::Centre;
    }
    if (keyword == "end" || keyword == "flex-end") {
        return Align::End;
    }
    if (keyword == "start" || keyword == "flex-start") {
        return Align::Start;
    }
    return Align::Stretch;
}

}  // namespace

bool apply_style(const ResolvedStyle& style, LayoutInput& layout, PaintData& paint) noexcept {
    bool layout_changed = false;
    const auto number = [](const ResolvedProperty* property, f32 fallback) noexcept {
        return (property == nullptr) ? fallback : property->value.number;
    };

    for (const ResolvedProperty& property : style.properties()) {
        switch (property.property) {
            case StyleProperty::BackgroundColour:
                paint.background = property.value.colour;
                break;
            case StyleProperty::BorderColour:
                paint.border_colour = property.value.colour;
                break;
            case StyleProperty::BorderWidth:
                paint.border_width = property.value.number;
                break;
            case StyleProperty::CornerRadius:
                paint.corner_radius = property.value.number;
                break;
            case StyleProperty::Opacity:
                paint.opacity = property.value.number;
                break;
            case StyleProperty::Colour:
                paint.tint = property.value.colour;
                break;
            case StyleProperty::Width:
                layout.preferred.x = property.value.number;
                layout_changed = true;
                break;
            case StyleProperty::Height:
                layout.preferred.y = property.value.number;
                layout_changed = true;
                break;
            case StyleProperty::MinWidth:
                layout.minimum.x = property.value.number;
                layout_changed = true;
                break;
            case StyleProperty::MinHeight:
                layout.minimum.y = property.value.number;
                layout_changed = true;
                break;
            case StyleProperty::MaxWidth:
                layout.maximum.x = property.value.number;
                layout_changed = true;
                break;
            case StyleProperty::MaxHeight:
                layout.maximum.y = property.value.number;
                layout_changed = true;
                break;
            case StyleProperty::MarginLeft:
                layout.margin.left = property.value.number;
                layout_changed = true;
                break;
            case StyleProperty::MarginTop:
                layout.margin.top = property.value.number;
                layout_changed = true;
                break;
            case StyleProperty::MarginRight:
                layout.margin.right = property.value.number;
                layout_changed = true;
                break;
            case StyleProperty::MarginBottom:
                layout.margin.bottom = property.value.number;
                layout_changed = true;
                break;
            case StyleProperty::PaddingLeft:
                layout.padding.left = property.value.number;
                layout_changed = true;
                break;
            case StyleProperty::PaddingTop:
                layout.padding.top = property.value.number;
                layout_changed = true;
                break;
            case StyleProperty::PaddingRight:
                layout.padding.right = property.value.number;
                layout_changed = true;
                break;
            case StyleProperty::PaddingBottom:
                layout.padding.bottom = property.value.number;
                layout_changed = true;
                break;
            case StyleProperty::FlexGrow:
                layout.flex_grow = property.value.number;
                layout_changed = true;
                break;
            case StyleProperty::FlexShrink:
                layout.flex_shrink = property.value.number;
                layout_changed = true;
                break;
            case StyleProperty::FlexBasis:
                layout.flex_basis = property.value.number;
                layout_changed = true;
                break;
            case StyleProperty::Gap:
                layout.gap = property.value.number;
                layout_changed = true;
                break;
            case StyleProperty::AspectRatio:
                layout.aspect_ratio = property.value.number;
                layout_changed = true;
                break;
            case StyleProperty::FlexDirectionProperty: {
                layout.direction = direction_from_keyword(property.value.keyword.text());
                layout_changed = true;
                break;
            }
            case StyleProperty::JustifyContent: {
                layout.justify = justify_from_keyword(property.value.keyword.text());
                layout_changed = true;
                break;
            }
            case StyleProperty::AlignItems:
            case StyleProperty::AlignSelf: {
                const Align align = align_from_keyword(property.value.keyword.text());
                if (property.property == StyleProperty::AlignItems) {
                    layout.align = align;
                } else {
                    layout.self_align = align;
                }
                layout_changed = true;
                break;
            }
            case StyleProperty::Display:
                // `display: none` is `Collapsed`, which the caller applies to the element's flags;
                // the layout input records it as a zero preferred size so a caller that ignores the
                // flag still gets nothing drawn.
                layout_changed = true;
                break;
            case StyleProperty::FontSize:
            case StyleProperty::FontFamily:
            case StyleProperty::TransitionDuration:
            case StyleProperty::Count:
                break;
        }
    }
    (void)number;
    return layout_changed;
}

}  // namespace cy::ui
