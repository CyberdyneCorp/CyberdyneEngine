// SPDX-License-Identifier: MIT
// `unit.ui_toolkit`: the `.cyui` text form — parsed, written back, edited one line at a time — and
// documents made into widgets, composed from components, and reloaded without losing state.

#include "fixture.h"

#include <cy/ui/toolkit/document.h>
#include <cy/ui/toolkit/document_host.h>

#include <algorithm>
#include <cstring>
#include <string_view>

using namespace cy;
using namespace cy::ui;
using namespace cy::ui::toolkit_test;

namespace {

constexpr std::string_view kSettings = R"(cyui 1
stylesheet "menu.cyss"
param heading "Settings"
column #settings .panel gap=8 width=320
  // the volume, bound to the game
  label #title .title text=$heading
  slider #volume min=0 max=1 step=0.05 value=0.8 bind=volume
  toggle #subtitles text="Subtitles \"on\"" checked=true
  dropdown #quality items=["Low", "Medium", "High"] selected=1
  use "stat.cyui" #health label="Health" value=0.7
  use "stat.cyui" #mana label="Mana"
  row justify-content=end
    button #cancel text="Cancel"
    button #apply .primary text="Apply"
)";

constexpr std::string_view kStat = R"(cyui 1
param label "Stat"
param value 0.5
row .stat gap=4
  label text=$label
  progress-bar #bar value=$value width=120
)";

constexpr std::string_view kSheet =
    "label.title { color: #ffd34d; }\n.stat { gap: 10; background-color: #102030; }\n";

/// A resolver over three strings.
class Files final : public DocumentResolver {
public:
    [[nodiscard]] Status read(std::string_view path, Array<char>& out) noexcept override {
        std::string_view text;
        if (path == "stat.cyui") {
            text = stat;
        } else if (path == "menu.cyss") {
            text = kSheet;
        }
        if (text.empty()) {
            return fail(ErrorCode::NotFound, "no such file");
        }
        return out.append(Span<const char>(text.data(), text.size()));
    }
    std::string_view stat = kStat;
};

class Values final : public DocumentBindings {
public:
    class Number final : public BindingSource {
    public:
        [[nodiscard]] BoundValue read() noexcept override {
            BoundValue value;
            value.kind = BoundValue::Kind::Number;
            value.number = number;
            dirty = false;
            return value;
        }
        [[nodiscard]] bool changed() const noexcept override { return dirty; }
        f32 number = 0.25F;
        bool dirty = true;
    };
    [[nodiscard]] BindingSource* find(std::string_view name) noexcept override {
        return name == "volume" ? &volume : nullptr;
    }
    Number volume;
};

[[nodiscard]] std::string_view text_of(const Array<char>& bytes) noexcept {
    return {bytes.data(), bytes.size()};
}

/// The lines that differ between two texts, counted.
[[nodiscard]] u32 differing_lines(std::string_view a, std::string_view b) noexcept {
    u32 count = 0;
    while (!a.empty() || !b.empty()) {
        const usize ea = std::min(a.find('\n'), a.size());
        const usize eb = std::min(b.find('\n'), b.size());
        count += a.substr(0, ea) != b.substr(0, eb) ? 1U : 0U;
        a = a.substr(std::min(ea + 1, a.size()));
        b = b.substr(std::min(eb + 1, b.size()));
    }
    return count;
}

}  // namespace

CY_TEST_CASE("ui_document: the text form parses, and writes back byte for byte") {
    Document document(allocator());
    CY_REQUIRE(parse_document(kSettings, document).has_value());
    CY_CHECK(document.diagnostics().empty());
    CY_REQUIRE_EQ(document.nodes().size(), 11U);
    const DocumentNode& column = document.nodes()[0];
    CY_CHECK_EQ(column.kind.text(), std::string_view("column"));
    CY_CHECK_EQ(column.id.text(), std::string_view("settings"));
    CY_CHECK_EQ(column.classes[0].text(), std::string_view("panel"));
    CY_CHECK(document.nodes()[1].is_comment());
    const DocumentProperty* items = document.find(5, Name::intern("items"));
    CY_REQUIRE(items != nullptr);
    CY_CHECK_EQ(document.items(items->value).size(), 3U);
    CY_CHECK_EQ(document.text(document.items(items->value)[2]), std::string_view("High"));
    const DocumentProperty* caption = document.find(4, Name::intern("text"));
    CY_CHECK_EQ(document.text(caption->value), std::string_view("Subtitles \"on\""));
    CY_CHECK_EQ(document.nodes()[9].parent, 8U);
    CY_CHECK_EQ(document.parameters().size(), 1U);
    Array<char> written(allocator());
    CY_REQUIRE(write_document(document, written).has_value());
    CY_CHECK_EQ(text_of(written), kSettings);
}

CY_TEST_CASE("ui_document: one property changed is one line of the text changed") {
    // `ui-system`: "WHEN one property of one element changes THEN the text diff SHALL show that
    // single change."
    Document document(allocator());
    CY_REQUIRE(parse_document(kSettings, document).has_value());
    CY_REQUIRE(document.set_number(3, "value", 0.4F).has_value());
    Array<char> once(allocator());
    CY_REQUIRE(write_document(document, once).has_value());
    CY_CHECK_EQ(differing_lines(kSettings, text_of(once)), 1U);
    CY_CHECK_NE(text_of(once).find("value=0.4 bind=volume"), std::string_view::npos);
    // A property added to one element, and an element added under another: still one line each.
    CY_REQUIRE(document.set_string(10, "tooltip", "Apply the settings").has_value());
    Array<char> twice(allocator());
    CY_REQUIRE(write_document(document, twice).has_value());
    CY_CHECK_EQ(differing_lines(text_of(once), text_of(twice)), 1U);
    Expected<u32, Error> added = document.add(8, "button");
    CY_REQUIRE(added.has_value());
    CY_REQUIRE(document.set_id(*added, "reset").has_value());
    Array<char> thrice(allocator());
    CY_REQUIRE(write_document(document, thrice).has_value());
    CY_CHECK_NE(text_of(thrice).find("    button #reset\n"), std::string_view::npos);
    // And what it wrote reads back to what it holds.
    Document again(allocator());
    CY_REQUIRE(parse_document(text_of(thrice), again).has_value());
    Array<char> round(allocator());
    CY_REQUIRE(write_document(again, round).has_value());
    CY_CHECK_EQ(text_of(round), text_of(thrice));
}

CY_TEST_CASE("ui_document: mistakes are named with a line and a column, and parsing goes on") {
    Document document(allocator());
    CY_REQUIRE(
        parse_document("cyui 1\ncolumn\n   label\n  label text=\"open\n  slider oops\n", document)
            .has_value());
    CY_REQUIRE(document.diagnostics().size() >= 3U);
    CY_CHECK_EQ(document.diagnostics()[0].line, 3U);  // three spaces
    bool unclosed = false;
    bool token = false;
    for (const DocumentDiagnostic& diagnostic : document.diagnostics()) {
        unclosed = unclosed || std::strstr(diagnostic.message, "not closed") != nullptr;
        token = token || (diagnostic.line == 5U && diagnostic.detail.text() == "oops");
    }
    CY_CHECK(unclosed);
    CY_CHECK(token);
    Document headless(allocator());
    CY_REQUIRE(parse_document("column\n", headless).has_value());
    CY_CHECK_EQ(headless.diagnostics().size(), 1U);
}

CY_TEST_CASE(
    "ui_document: a document becomes widgets, its components instantiated with parameters") {
    // `ui-system`: "WHEN a document instantiates another with parameters THEN the instance SHALL
    // reflect the source document's changes, with parameter overrides preserved."
    Kit kit;
    CY_REQUIRE(kit.started);
    Files files;
    Values values;
    DocumentInstance instance(allocator(), kit.toolkit);
    CY_REQUIRE(instance.load(kSettings, kNoElement, &files, &values).has_value());
    CY_REQUIRE(kit.frame());
    CY_REQUIRE_EQ(instance.roots().size(), 1U);
    const WidgetId title = instance.find("title");
    CY_CHECK_EQ(kit.toolkit.text(title), std::string_view("Settings"));  // the param's default
    // The sheet the document named is applied: the title is gold.
    CY_CHECK_EQ(kit.text.text_of(title), std::string_view("Settings"));
    const WidgetId volume = instance.find("volume");
    CY_CHECK_EQ(kit.toolkit.kind(volume), WidgetKind::Slider);
    // Bound: the game's 0.25, not the document's 0.8.
    CY_CHECK_NEAR(kit.toolkit.value(volume), 0.25F, 1e-6F);
    CY_CHECK(kit.toolkit.checked(instance.find("subtitles")));
    CY_CHECK_EQ(kit.toolkit.selected(instance.find("quality")), 1U);
    // Two instances of one component: each its own id, label and value; the second the default.
    const WidgetId health = instance.find("health");
    const WidgetId mana = instance.find("mana");
    CY_REQUIRE(health.is_valid());
    CY_REQUIRE(mana.is_valid());
    CY_CHECK_NEAR(kit.toolkit.value(kit.toolkit.find(health, "bar")), 0.7F, 1e-6F);
    CY_CHECK_NEAR(kit.toolkit.value(kit.toolkit.find(mana, "bar")), 0.5F, 1e-6F);
    Array<WidgetId> stat(allocator());
    CY_REQUIRE(kit.toolkit.children(health, stat).has_value());
    CY_CHECK_EQ(kit.toolkit.text(stat[0]), std::string_view("Health"));
    // A `.cyss` key is an inline declaration: the column is 320 wide.
    CY_CHECK_EQ(kit.rect(instance.roots()[0]).width, 320.0F);
    // The sheet's `.stat` rule reached the component's class — and the component's own inline
    // `gap=4` beat the sheet's `gap: 10`, as an inline declaration does.
    CY_CHECK_EQ(kit.store.paint(health)->background, 0xFF102030U);
    CY_CHECK_EQ(kit.store.layout_input(health)->gap, 4.0F);

    // The component changes: both instances follow, and the override stays.
    files.stat = R"(cyui 1
param label "Stat"
param value 0.5
row .stat gap=4
  label text=$label
  progress-bar #bar value=$value width=160
)";
    CY_REQUIRE(instance.reload(kSettings).has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(kit.rect(kit.toolkit.find(health, "bar")).width, 160.0F);
    CY_CHECK_EQ(kit.rect(kit.toolkit.find(mana, "bar")).width, 160.0F);
    CY_CHECK_NEAR(kit.toolkit.value(kit.toolkit.find(health, "bar")), 0.7F, 1e-6F);
}

CY_TEST_CASE("ui_document: a reload keeps every matched widget, its focus and the player's value") {
    Kit kit;
    CY_REQUIRE(kit.started);
    Files files;
    DocumentInstance instance(allocator(), kit.toolkit);
    CY_REQUIRE(instance.load(kSettings, kNoElement, &files, nullptr).has_value());
    CY_REQUIRE(kit.frame());
    const WidgetId volume = instance.find("volume");
    const WidgetId apply = instance.find("apply");
    CY_REQUIRE(kit.toolkit.focus(volume).has_value());
    CY_CHECK(kit.toolkit.action(UiAction::NavigateLeft));  // the player moves it: 0.75
    const f32 players = kit.toolkit.value(volume);
    const u32 widgets = kit.toolkit.widget_count();

    // The designer changes the title's text and the apply button's class, and adds a button.
    const std::string_view edited = R"(cyui 1
stylesheet "menu.cyss"
param heading "Options"
column #settings .panel gap=8 width=320
  // the volume, bound to the game
  label #title .title text=$heading
  slider #volume min=0 max=1 step=0.05 value=0.8 bind=volume
  toggle #subtitles text="Subtitles \"on\"" checked=true
  dropdown #quality items=["Low", "Medium", "High"] selected=1
  use "stat.cyui" #health label="Health" value=0.7
  use "stat.cyui" #mana label="Mana"
  row justify-content=end
    button #reset text="Reset"
    button #cancel text="Cancel"
    button #apply .danger text="Apply"
)";
    CY_REQUIRE(instance.reload(edited).has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK_EQ(instance.find("volume"), volume);
    CY_CHECK_EQ(instance.find("apply"), apply);
    CY_CHECK_EQ(kit.toolkit.focused(), volume);
    CY_CHECK_EQ(kit.toolkit.value(volume), players);  // `value=0.8` did not change: not reapplied
    CY_CHECK_EQ(kit.toolkit.text(instance.find("title")), std::string_view("Options"));
    CY_CHECK_EQ(kit.toolkit.widget_count(), widgets + 1U);
    CY_CHECK_EQ(instance.report().created, 1U);
    CY_CHECK_EQ(instance.report().removed, 0U);
    // The new button is first in its row, as its line is.
    Array<WidgetId> row(allocator());
    CY_REQUIRE(kit.toolkit.children(kit.toolkit.parent(apply), row).has_value());
    CY_REQUIRE_EQ(row.size(), 3U);
    CY_CHECK_EQ(kit.toolkit.id_of(row[0]).text(), std::string_view("reset"));
    CY_CHECK_EQ(kit.toolkit.id_of(row[2]).text(), std::string_view("apply"));

    // A reload that does not parse changes nothing and says why.
    CY_CHECK_FALSE(instance.reload("cyui 1\ncolumn\n   broken\n").has_value());
    CY_CHECK_FALSE(instance.diagnostics().empty());
    CY_CHECK_EQ(instance.find("volume"), volume);
    CY_CHECK_EQ(kit.toolkit.widget_count(), widgets + 1U);
}

CY_TEST_CASE("ui_document: a cooked bundle carries a document and everything it reaches") {
    Array<char> bundle(allocator());
    CY_REQUIRE(append_bundle_entry(bundle, "document", "settings.cyui", kSettings).has_value());
    CY_REQUIRE(append_bundle_entry(bundle, "document", "stat.cyui", kStat).has_value());
    CY_REQUIRE(append_bundle_entry(bundle, "stylesheet", "menu.cyss", kSheet).has_value());
    BundleResolver resolver(allocator(), text_of(bundle));
    CY_REQUIRE(resolver.status().has_value());
    CY_CHECK_EQ(resolver.main_document(), kSettings);
    Kit kit;
    CY_REQUIRE(kit.started);
    DocumentInstance instance(allocator(), kit.toolkit);
    CY_REQUIRE(instance.load(resolver.main_document(), kNoElement, &resolver, nullptr).has_value());
    CY_CHECK(instance.find("mana").is_valid());
    CY_CHECK_EQ(instance.dependencies().size(), 3U);  // the sheet, and the component twice
    Array<BundleEntry> entries(allocator());
    CY_CHECK_FALSE(parse_bundle("cyuicook 1\ndocument a.cyui 99\nshort", entries).has_value());
}

CY_TEST_CASE("ui_document: every line of the document names the widget it made") {
    // What the editor's layout canvas draws its handles from: the engine's rectangle for each line
    // the author sees, a `use` line being the top of the component it instantiated.
    Kit kit;
    CY_REQUIRE(kit.started);
    Files files;
    DocumentInstance instance(allocator(), kit.toolkit);
    CY_REQUIRE(instance.load(kSettings, kNoElement, &files, nullptr).has_value());
    CY_CHECK_EQ(instance.widget_at_line(4), instance.roots()[0]);       // column #settings
    CY_CHECK(!instance.widget_at_line(5).is_valid());                   // a comment
    CY_CHECK_EQ(instance.widget_at_line(6), instance.find("title"));    // label #title
    CY_CHECK_EQ(instance.widget_at_line(10), instance.find("health"));  // use "stat.cyui" #health
    CY_CHECK_EQ(instance.widget_at_line(14), instance.find("apply"));
    CY_CHECK(!instance.widget_at_line(0).is_valid());
    CY_CHECK(!instance.widget_at_line(1).is_valid());   // the header
    CY_CHECK(!instance.widget_at_line(99).is_valid());  // past the end
    // A used document's own elements are not the root document's lines.
    const WidgetId bar = kit.toolkit.find(instance.find("health"), "bar");
    for (u32 line = 0; line < 20; ++line) {
        CY_CHECK(instance.widget_at_line(line) != bar);
    }

    // A reload that moves a line moves the answer with it.
    constexpr std::string_view kMoved = R"(cyui 1
stylesheet "menu.cyss"
param heading "Settings"
column #settings .panel gap=8 width=320
  label #title .title text=$heading
  slider #volume min=0 max=1 step=0.05 value=0.8
)";
    CY_REQUIRE(instance.reload(kMoved).has_value());
    CY_CHECK_EQ(instance.widget_at_line(5), instance.find("title"));
    CY_CHECK_EQ(instance.widget_at_line(6), instance.find("volume"));
}

CY_TEST_CASE("ui_document: a reload that drops the stylesheet line is no longer styled by it") {
    // Regression: the instance applied the sheets a reload collected only when there were some, so
    // a document whose `stylesheet` line was deleted kept being drawn with the sheet it no longer
    // names until the game restarted.
    Kit kit;
    CY_REQUIRE(kit.started);
    Files files;
    DocumentInstance instance(allocator(), kit.toolkit);
    constexpr std::string_view kStyled = R"(cyui 1
stylesheet "menu.cyss"
row .stat
)";
    constexpr std::string_view kBare = R"(cyui 1
row .stat
)";
    CY_REQUIRE(instance.load(kStyled, kNoElement, &files, nullptr).has_value());
    CY_REQUIRE(kit.frame());
    const WidgetId row = instance.roots()[0];
    CY_CHECK_EQ(kit.store.paint(row)->background, 0xFF102030U);
    CY_CHECK(!kit.toolkit.stylesheet().rules().empty());
    CY_REQUIRE(instance.reload(kBare).has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK(kit.toolkit.stylesheet().rules().empty());
    CY_CHECK(kit.store.paint(instance.roots()[0])->background != 0xFF102030U);
}

CY_TEST_CASE("ui_document: unloading takes an open drop-down's list and the document's dialogs") {
    // Regression: a drop-down's list and a dialog live in the toolkit's overlay, not under the
    // widgets they were made in. Destroying the document's root destroyed the drop-down and left
    // its open list on screen with no owner, and left every dialog the document held — open or
    // closed — alive and drawn, after the document that made it was gone.
    Kit kit;
    CY_REQUIRE(kit.started);
    constexpr std::string_view kMenu = R"(cyui 1
panel #menu
  dropdown #quality items=["Low", "High"] selected=0
  dialog #confirm title="Sure?"
    label text="Really?"
)";
    DocumentInstance instance(allocator(), kit.toolkit);
    CY_REQUIRE(instance.load(kMenu, kNoElement, nullptr, nullptr).has_value());
    CY_REQUIRE(kit.frame());
    const WidgetId quality = instance.find("quality");
    const WidgetId confirm = instance.find("confirm");
    CY_REQUIRE(kit.toolkit.alive(confirm));
    CY_REQUIRE(kit.toolkit.open(quality).has_value());
    CY_REQUIRE(kit.frame());
    CY_REQUIRE(kit.toolkit.is_open(quality));
    const u32 with_list = kit.toolkit.widget_count();
    CY_REQUIRE(with_list > 4U);  // the options are widgets while the list is open

    CY_REQUIRE(instance.unload().has_value());
    CY_REQUIRE(kit.frame());
    CY_CHECK_FALSE(kit.toolkit.alive(confirm));
    CY_CHECK_EQ(kit.toolkit.widget_count(), 0U);
    CY_CHECK_FALSE(kit.toolkit.captures_input());

    // And an open dialog goes the same way, giving the input back.
    CY_REQUIRE(instance.load(kMenu, kNoElement, nullptr, nullptr).has_value());
    CY_REQUIRE(kit.frame());
    CY_REQUIRE(kit.toolkit.open(instance.find("confirm")).has_value());
    CY_REQUIRE(kit.toolkit.captures_input());
    CY_REQUIRE(instance.unload().has_value());
    CY_CHECK_EQ(kit.toolkit.widget_count(), 0U);
    CY_CHECK_FALSE(kit.toolkit.captures_input());
}
