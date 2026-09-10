// `.cyss`: the parser, the cascade, and the divergences from CSS. M8.b task 9.2.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/ui/style.h>

#include <string_view>

using namespace cy;
using namespace cy::ui;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

[[nodiscard]] StyleSubject subject_of(const char* type, const char* klass,
                                      PseudoState states = PseudoState::None) noexcept {
    StyleSubject subject;
    subject.type = Name::intern(type);
    if (klass != nullptr) {
        subject.classes[0] = Name::intern(klass);
        subject.class_count = 1;
    }
    subject.states = states;
    return subject;
}

}  // namespace

CY_TEST_CASE("ui_style: a class selector with a hover state behaves as CSS would") {
    // "WHEN a developer who knows CSS writes a `.cyss` rule with a class selector and a hover state
    // THEN it SHALL behave as CSS would within the documented subset."
    constexpr std::string_view sheet_text = R"(
        button { background-color: #202020; corner-radius: 4; }
        .primary { background-color: #3060c0; }
        .primary:hover { background-color: #4070e0; }
    )";
    StyleSheet sheet(allocator());
    CY_REQUIRE(parse_stylesheet(sheet_text, sheet).has_value());
    CY_CHECK_EQ(sheet.diagnostics().size(), 0U);
    CY_REQUIRE_EQ(sheet.rules().size(), 3U);

    Array<StyleDiagnostic> diagnostics(allocator());
    ResolvedStyle resolved(allocator());
    const StyleSubject resting = subject_of("button", "primary");
    CY_REQUIRE(resolve_style(sheet, nullptr, resting, Span<const StyleDeclaration>{}, nullptr,
                             resolved, diagnostics)
                   .has_value());
    const ResolvedProperty* background = resolved.find(StyleProperty::BackgroundColour);
    CY_REQUIRE(background != nullptr);
    CY_CHECK_EQ(background->value.colour, 0xFF3060C0U);
    // The type rule's corner radius survives: the class rule did not mention it.
    const ResolvedProperty* radius = resolved.find(StyleProperty::CornerRadius);
    CY_REQUIRE(radius != nullptr);
    CY_CHECK_EQ(radius->value.number, 4.0F);

    // HOVERED: the more specific rule wins.
    ResolvedStyle hovered(allocator());
    const StyleSubject over = subject_of("button", "primary", PseudoState::Hover);
    CY_REQUIRE(resolve_style(sheet, nullptr, over, Span<const StyleDeclaration>{}, nullptr, hovered,
                             diagnostics)
                   .has_value());
    CY_CHECK_EQ(hovered.find(StyleProperty::BackgroundColour)->value.colour, 0xFF4070E0U);
}

CY_TEST_CASE("ui_style: an unsupported property is reported by name and location") {
    // "WHEN a style sheet uses a CSS property outside the subset THEN cooking SHALL report it by
    // name and location, rather than silently ignoring it." DIVERGENCE 1, as a diagnostic.
    constexpr std::string_view sheet_text = R"(
        panel {
            background-color: #101010;
            box-shadow: 0 0 4px black;
        }
    )";
    StyleSheet sheet(allocator());
    CY_REQUIRE(parse_stylesheet(sheet_text, sheet).has_value());
    CY_REQUIRE_EQ(sheet.diagnostics().size(), 1U);
    CY_CHECK_EQ(sheet.diagnostics()[0].detail, Name::intern("box-shadow"));
    CY_CHECK_EQ(sheet.diagnostics()[0].line, 4U);
    // And the rest of the rule still parsed, so a hot reload shows the diagnostic and keeps going.
    CY_REQUIRE_EQ(sheet.rules().size(), 1U);
    CY_CHECK_EQ(sheet.rules()[0].declaration_count, 1U);
}

CY_TEST_CASE("ui_style: a unit outside the subset is refused rather than guessed") {
    // DIVERGENCE 2: unitless and `px` are reference units; `em`, `%` and the viewport units are not
    // in the subset. A value in them becomes a keyword rather than a silently wrong number.
    constexpr std::string_view sheet_text = "panel { width: 50%; height: 20px; gap: 8; }";
    StyleSheet sheet(allocator());
    CY_REQUIRE(parse_stylesheet(sheet_text, sheet).has_value());
    CY_REQUIRE_EQ(sheet.rules().size(), 1U);
    CY_REQUIRE_EQ(sheet.declarations().size(), 3U);
    CY_CHECK_EQ(sheet.declarations()[0].value.kind, StyleValue::Kind::Keyword);
    CY_CHECK_EQ(sheet.declarations()[1].value.kind, StyleValue::Kind::Number);
    CY_CHECK_EQ(sheet.declarations()[1].value.number, 20.0F);
    CY_CHECK_EQ(sheet.declarations()[2].value.number, 8.0F);
}

CY_TEST_CASE("ui_style: specificity decides, and an inline override wins over everything") {
    constexpr std::string_view sheet_text = R"(
        label { color: #ffffff; }
        .warning { color: #ffcc00; }
        #urgent { color: #ff0000; }
    )";
    StyleSheet sheet(allocator());
    CY_REQUIRE(parse_stylesheet(sheet_text, sheet).has_value());

    StyleSubject subject = subject_of("label", "warning");
    subject.id = Name::intern("urgent");
    Array<StyleDiagnostic> diagnostics(allocator());
    ResolvedStyle resolved(allocator());
    CY_REQUIRE(resolve_style(sheet, nullptr, subject, Span<const StyleDeclaration>{}, nullptr,
                             resolved, diagnostics)
                   .has_value());
    // The id rule is the most specific.
    CY_CHECK_EQ(resolved.find(StyleProperty::Colour)->value.colour, 0xFFFF0000U);

    StyleDeclaration inline_override;
    inline_override.property = StyleProperty::Colour;
    inline_override.value.kind = StyleValue::Kind::Colour;
    inline_override.value.colour = 0xFF00FF00U;
    ResolvedStyle overridden(allocator());
    CY_REQUIRE(resolve_style(sheet, nullptr, subject,
                             Span<const StyleDeclaration>(&inline_override, 1), nullptr, overridden,
                             diagnostics)
                   .has_value());
    CY_CHECK_EQ(overridden.find(StyleProperty::Colour)->value.colour, 0xFF00FF00U);
    // AND THE INSPECTOR CAN SAY IT OVERRODE SOMETHING. "the inspector SHALL show which rule in
    // which style sheet supplied it, and what it overrode."
    CY_CHECK(overridden.find(StyleProperty::Colour)->overrode);
}

CY_TEST_CASE("ui_style: a descendant combinator matches an ancestor and a child one does not") {
    constexpr std::string_view sheet_text = R"(
        panel label { color: #111111; }
        row > label { color: #222222; }
    )";
    StyleSheet sheet(allocator());
    CY_REQUIRE(parse_stylesheet(sheet_text, sheet).has_value());
    CY_REQUIRE_EQ(sheet.rules().size(), 2U);

    const StyleSubject panel = subject_of("panel", nullptr);
    const StyleSubject row = subject_of("row", nullptr);
    const StyleSubject* deep_chain[2] = {&row, &panel};

    StyleSubject label = subject_of("label", nullptr);
    label.ancestors = Span<const StyleSubject* const>(deep_chain, 2);
    // The descendant rule matches through an intervening element; the child rule matches its
    // immediate parent, which `row` is.
    CY_CHECK(rule_matches(sheet.rules()[0], label));
    CY_CHECK(rule_matches(sheet.rules()[1], label));

    const StyleSubject* far_chain[2] = {&panel, &row};
    StyleSubject far_label = subject_of("label", nullptr);
    far_label.ancestors = Span<const StyleSubject* const>(far_chain, 2);
    // `row > label` does NOT match when `row` is a grandparent: `>` gets one chance.
    CY_CHECK(rule_matches(sheet.rules()[0], far_label));
    CY_CHECK_FALSE(rule_matches(sheet.rules()[1], far_label));
}

CY_TEST_CASE("ui_style: a custom property resolves, and an undefined one is a diagnostic") {
    // DIVERGENCE 5: `var()` resolves at match time, and a reference with no definition is an error
    // rather than a silent zero.
    constexpr std::string_view sheet_text = R"(
        :root { --accent: #ff8800; }
        button { background-color: var(--accent); }
        panel { background-color: var(--missing); }
    )";
    StyleSheet sheet(allocator());
    CY_REQUIRE(parse_stylesheet(sheet_text, sheet).has_value());
    CY_REQUIRE(sheet.custom_property(Name::intern("accent")) != nullptr);

    Array<StyleDiagnostic> diagnostics(allocator());
    ResolvedStyle resolved(allocator());
    CY_REQUIRE(resolve_style(sheet, nullptr, subject_of("button", nullptr),
                             Span<const StyleDeclaration>{}, nullptr, resolved, diagnostics)
                   .has_value());
    CY_REQUIRE(resolved.find(StyleProperty::BackgroundColour) != nullptr);
    CY_CHECK_EQ(resolved.find(StyleProperty::BackgroundColour)->value.colour, 0xFFFF8800U);
    CY_CHECK_EQ(diagnostics.size(), 0U);

    ResolvedStyle broken(allocator());
    CY_REQUIRE(resolve_style(sheet, nullptr, subject_of("panel", nullptr),
                             Span<const StyleDeclaration>{}, nullptr, broken, diagnostics)
                   .has_value());
    CY_CHECK_EQ(broken.find(StyleProperty::BackgroundColour), nullptr);
    CY_REQUIRE_EQ(diagnostics.size(), 1U);
    CY_CHECK_EQ(diagnostics[0].detail, Name::intern("missing"));
}

CY_TEST_CASE("ui_style: a theme supplies the base and the sheet overrides it") {
    // "Themes SHALL be swappable at runtime, re-resolving affected styles."
    constexpr std::string_view theme_text = "button { background-color: #303030; color: #cccccc; }";
    constexpr std::string_view sheet_text = "button { background-color: #0000ff; }";
    StyleSheet theme_sheet(allocator());
    StyleSheet sheet(allocator());
    CY_REQUIRE(parse_stylesheet(theme_text, theme_sheet).has_value());
    CY_REQUIRE(parse_stylesheet(sheet_text, sheet).has_value());

    Theme theme;
    theme.name = Name::intern("dark");
    theme.sheet = &theme_sheet;

    Array<StyleDiagnostic> diagnostics(allocator());
    ResolvedStyle resolved(allocator());
    CY_REQUIRE(resolve_style(sheet, &theme, subject_of("button", nullptr),
                             Span<const StyleDeclaration>{}, nullptr, resolved, diagnostics)
                   .has_value());
    // The document's own sheet wins for what it declares; the theme supplies the rest.
    CY_CHECK_EQ(resolved.find(StyleProperty::BackgroundColour)->value.colour, 0xFF0000FFU);
    CY_CHECK_EQ(resolved.find(StyleProperty::Colour)->value.colour, 0xFFCCCCCCU);
}

CY_TEST_CASE("ui_style: an inheritable property reaches a child and a private one does not") {
    ResolvedStyle parent(allocator());
    ResolvedProperty colour;
    colour.property = StyleProperty::Colour;
    colour.value.kind = StyleValue::Kind::Colour;
    colour.value.colour = 0xFF123456U;
    CY_REQUIRE(parent.set(colour).has_value());
    ResolvedProperty background;
    background.property = StyleProperty::BackgroundColour;
    background.value.colour = 0xFF654321U;
    CY_REQUIRE(parent.set(background).has_value());

    StyleSheet empty(allocator());
    CY_REQUIRE(parse_stylesheet("", empty).has_value());
    Array<StyleDiagnostic> diagnostics(allocator());
    ResolvedStyle child(allocator());
    CY_REQUIRE(resolve_style(empty, nullptr, subject_of("label", nullptr),
                             Span<const StyleDeclaration>{}, &parent, child, diagnostics)
                   .has_value());
    // Colour inherits, as it does in CSS; a background does not.
    CY_REQUIRE(child.find(StyleProperty::Colour) != nullptr);
    CY_CHECK_EQ(child.find(StyleProperty::Colour)->value.colour, 0xFF123456U);
    CY_CHECK_EQ(child.find(StyleProperty::BackgroundColour), nullptr);
}

CY_TEST_CASE("ui_style: applying a style says whether layout has to run again") {
    // The granularity a binding needs: a colour is paint, a width is layout.
    LayoutInput layout;
    PaintData paint;

    ResolvedStyle colour_only(allocator());
    ResolvedProperty colour;
    colour.property = StyleProperty::BackgroundColour;
    colour.value.colour = 0xFF00FF00U;
    CY_REQUIRE(colour_only.set(colour).has_value());
    CY_CHECK_FALSE(apply_style(colour_only, layout, paint));
    CY_CHECK_EQ(paint.background, 0xFF00FF00U);

    ResolvedStyle sized(allocator());
    ResolvedProperty width;
    width.property = StyleProperty::Width;
    width.value.number = 120.0F;
    CY_REQUIRE(sized.set(width).has_value());
    CY_CHECK(apply_style(sized, layout, paint));
    CY_CHECK_EQ(layout.preferred.x, 120.0F);
}
