#ifndef CY_UI_STYLE_H
#define CY_UI_STYLE_H
// `.cyss`: the style sheet, its cascade, and the divergences from CSS. M8.b task 9.2.
//
// `ui-system`: "Styling SHALL use `.cyss`, a deliberately CSS-compatible subset, supporting: type,
// class and id selectors; descendant and child combinators; pseudo-states `:hover`, `:focus`,
// `:active`, `:disabled`, `:checked`, and `:first-child`/`:last-child`; custom properties
// (`--name`) with `var()` resolution; and the cascade with specificity and inline-override
// precedence."
//
// And then the sentence that shapes this file: "**The supported subset and its divergences from CSS
// SHALL be documented explicitly, rather than approximating CSS and leaving differences to be
// discovered.**"
//
// --- THE DIVERGENCES, IN FULL --------------------------------------------------------------------
//
//  1. THE PROPERTY SET IS CLOSED. A property this module does not know is an ERROR with a line and
//  a
//     column, not a silently ignored declaration — "WHEN a style sheet uses a CSS property outside
//     the subset THEN cooking SHALL report it by name and location". CSS ignores what it does not
//     understand, and that is the right choice for a document that must survive a browser it was
//     not written for and the wrong one for an asset that is cooked.
//  2. LENGTHS ARE UNITLESS OR `px`, and both mean REFERENCE UNITS — not physical pixels. `em`,
//  `rem`,
//     `%`, `vh` and `vw` are not in the subset. Percentages of a parent are expressed through the
//     flex and anchor models, which is where a layout engine can resolve them in one pass.
//  3. THE CASCADE ORDER IS THE SPECIFICATION'S, and it is not CSS's: inline element overrides, then
//     matched rules by specificity, then inherited properties from ancestors, then the active
//     theme, then engine defaults. CSS puts author and user styles in a different relationship;
//     this order is the one `ui-system` states.
//  4. SPECIFICITY IS (ids, classes-and-pseudo-states, types) COMPARED LEXICOGRAPHICALLY, as CSS's
//     is, but there is no `!important` and no inline-style-versus-important interaction to get
//     wrong: an inline override always wins.
//  5. `var()` RESOLVES AT MATCH TIME against the cascade in force, with a cycle producing a
//     diagnostic rather than an infinite recursion. There is no computed-value inheritance of a
//     custom property's *fallback*; a `var()` with no definition and no fallback is an error.
//  6. NO `@media`, NO `@supports`, NO ANIMATIONS IN THIS FILE. Transitions and keyframes are the
//     animation model's, declared on the element rather than in the sheet.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/ui/store.h>

#include <string_view>

namespace cy::ui {

/// Every property `.cyss` understands. THE SET IS CLOSED, and that is divergence 1.
enum class StyleProperty : u16 {
    BackgroundColour = 0,
    BorderColour,
    BorderWidth,
    CornerRadius,
    Opacity,
    Colour,
    FontSize,
    FontFamily,
    Width,
    Height,
    MinWidth,
    MinHeight,
    MaxWidth,
    MaxHeight,
    MarginLeft,
    MarginTop,
    MarginRight,
    MarginBottom,
    PaddingLeft,
    PaddingTop,
    PaddingRight,
    PaddingBottom,
    FlexGrow,
    FlexShrink,
    FlexBasis,
    FlexDirectionProperty,
    JustifyContent,
    AlignItems,
    AlignSelf,
    Gap,
    AspectRatio,
    Display,
    TransitionDuration,
    Count,
};

/// The property's `.cyss` spelling. Never null.
[[nodiscard]] const char* style_property_name(StyleProperty property) noexcept;
/// The property a spelling names, or `Count` when the subset has none.
[[nodiscard]] StyleProperty style_property_from_name(std::string_view text) noexcept;
/// Whether a property is inherited from an ancestor when unset — CSS's own answer for each.
[[nodiscard]] bool style_property_inherits(StyleProperty property) noexcept;

/// A resolved value. Small and typed: a style system whose values were strings would parse colours
/// per frame.
struct StyleValue {
    enum class Kind : u8 { Number = 0, Colour, Keyword, Reference };

    Kind kind = Kind::Number;
    f32 number = 0.0F;
    /// Premultiplied RGBA.
    u32 colour = 0;
    /// A keyword (`row`, `centre`, `none`) or, for `Reference`, the custom property's name.
    Name keyword;
};

/// The pseudo-states a selector may require. A bit set, because a selector may name several.
enum class PseudoState : u16 {
    None = 0,
    Hover = 1U << 0U,
    Focus = 1U << 1U,
    Active = 1U << 2U,
    Disabled = 1U << 3U,
    Checked = 1U << 4U,
    FirstChild = 1U << 5U,
    LastChild = 1U << 6U,
};

[[nodiscard]] constexpr PseudoState operator|(PseudoState a, PseudoState b) noexcept {
    return static_cast<PseudoState>(static_cast<u16>(a) | static_cast<u16>(b));
}
[[nodiscard]] constexpr bool has_state(PseudoState set, PseudoState flag) noexcept {
    return (static_cast<u16>(set) & static_cast<u16>(flag)) != 0U;
}

/// One compound selector: a type, an id, classes and pseudo-states, all optional.
struct SelectorPart {
    Name type;
    Name id;
    /// Up to four classes; a selector needing more is a selector that should be a class.
    Name classes[4];
    u32 class_count = 0;
    PseudoState states = PseudoState::None;
    /// True when this part must be the PARENT of the next, rather than any ancestor: the `>`
    /// combinator.
    bool child_combinator = false;
};

/// A rule: a selector chain, its declarations, and where it came from.
struct StyleRule {
    /// The chain, outermost first. `panel > .row .label` is three parts.
    SelectorPart parts[4];
    u32 part_count = 0;
    /// Indices into the sheet's declaration array.
    u32 first_declaration = 0;
    u32 declaration_count = 0;
    /// (ids, classes and pseudo-states, types), compared lexicographically. Divergence 4.
    u32 specificity = 0;
    /// The line the rule started on, for the diagnostic that names it.
    u32 line = 0;
    /// The order the rule appeared in, which breaks a specificity tie the way CSS does.
    u32 order = 0;
};

struct StyleDeclaration {
    StyleProperty property = StyleProperty::Count;
    /// A custom property's name when `property` is `Count` and `custom` is set.
    Name custom;
    StyleValue value;
    u32 line = 0;
};

/// A parse or resolution diagnostic. Line- and column-precise, because "report it by name and
/// location" is the requirement and a diagnostic that names only the file sends an author hunting.
struct StyleDiagnostic {
    Name detail;
    const char* message = "";
    u32 line = 0;
    u32 column = 0;
};

/// A parsed sheet.
class StyleSheet {
public:
    explicit StyleSheet(Allocator& allocator) noexcept;

    StyleSheet(const StyleSheet&) = delete;
    StyleSheet& operator=(const StyleSheet&) = delete;
    StyleSheet(StyleSheet&&) noexcept = default;
    StyleSheet& operator=(StyleSheet&&) noexcept = default;

    [[nodiscard]] Span<const StyleRule> rules() const noexcept { return rules_.span(); }
    [[nodiscard]] Span<const StyleDeclaration> declarations() const noexcept {
        return declarations_.span();
    }
    [[nodiscard]] Span<const StyleDiagnostic> diagnostics() const noexcept {
        return diagnostics_.span();
    }
    /// The custom properties declared at the sheet's root (`:root { --accent: #ff0000; }`).
    [[nodiscard]] const StyleValue* custom_property(Name name) const noexcept;

    [[nodiscard]] Allocator& allocator() const noexcept { return rules_.allocator(); }

    friend class StyleParser;

private:
    Array<StyleRule> rules_;
    Array<StyleDeclaration> declarations_;
    Array<StyleDiagnostic> diagnostics_;
    Array<StyleDeclaration> custom_;
};

/// Parse `.cyss`. A sheet with diagnostics still parses — a caller cooking an asset fails on
/// `diagnostics()` being non-empty, and one hot-reloading a sheet shows them and keeps going.
[[nodiscard]] Status parse_stylesheet(std::string_view text, StyleSheet& out) noexcept;

// --- Matching and the cascade
// ---------------------------------------------------------------------

/// What an element presents to the matcher. Filled by the caller from the store and the element's
/// authored classes, because the store deliberately does not know what a class is.
struct StyleSubject {
    Name type;
    Name id;
    Name classes[4];
    u32 class_count = 0;
    PseudoState states = PseudoState::None;
    /// The ancestors, nearest first. Matching a descendant combinator walks this.
    Span<const StyleSubject* const> ancestors;
};

/// One property's resolved value and where it came from. `ui-system`'s inspector requirement:
/// "WHEN an element has an unexpected colour THEN the inspector SHALL show which rule in which
/// style sheet supplied it, and what it overrode."
struct ResolvedProperty {
    StyleProperty property = StyleProperty::Count;
    StyleValue value;
    /// The rule that won, or `kNoRule` for an inline override, a theme value or a default.
    u32 rule = 0xFFFFFFFFU;
    u32 specificity = 0;
    /// What this value replaced, when it replaced something. The "and what it overrode" half.
    bool overrode = false;

    static constexpr u32 kNoRule = 0xFFFFFFFFU;
};

/// The resolved style of one element.
class ResolvedStyle {
public:
    explicit ResolvedStyle(Allocator& allocator) noexcept : properties_(allocator) {}

    [[nodiscard]] const ResolvedProperty* find(StyleProperty property) const noexcept;
    [[nodiscard]] Span<const ResolvedProperty> properties() const noexcept {
        return properties_.span();
    }
    [[nodiscard]] Status set(const ResolvedProperty& property) noexcept;
    void clear() noexcept { properties_.clear(); }

private:
    Array<ResolvedProperty> properties_;
};

/// A theme: a base rule set and its custom property values, swappable at runtime.
struct Theme {
    Name name;
    const StyleSheet* sheet = nullptr;
};

/// Resolve one element's style, in the order divergence 3 states.
///
/// `inline_overrides` are the element's own declarations, which always win. `inherited` is the
/// parent's resolved style, which supplies the inheritable properties this element did not set.
[[nodiscard]] Status resolve_style(const StyleSheet& sheet, const Theme* theme,
                                   const StyleSubject& subject,
                                   Span<const StyleDeclaration> inline_overrides,
                                   const ResolvedStyle* inherited, ResolvedStyle& out,
                                   Array<StyleDiagnostic>& diagnostics) noexcept;

/// Whether a rule matches a subject. Exposed because the inspector wants to show near-misses.
[[nodiscard]] bool rule_matches(const StyleRule& rule, const StyleSubject& subject) noexcept;

/// Apply a resolved style to an element's layout and paint arrays.
///
/// This is where a style becomes layout input, and it is one function so that "which properties
/// affect layout" is answerable by reading it. It returns whether anything LAYOUT-AFFECTING
/// changed, so a caller can dirty paint alone when only a colour moved — the granularity the
/// binding requirement asks for.
[[nodiscard]] bool apply_style(const ResolvedStyle& style, LayoutInput& layout,
                               PaintData& paint) noexcept;

}  // namespace cy::ui

#endif  // CY_UI_STYLE_H
