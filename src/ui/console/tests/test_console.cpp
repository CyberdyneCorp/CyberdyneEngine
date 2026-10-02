// SPDX-License-Identifier: MIT
// `unit.ui_console`: the developer console with no device — its rows, its scrollback, its commands,
// and what it flattens to.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/ui/console/console.h>
#include <cy/ui/layout.h>
#include <cy/ui/paint.h>
#include <cy/ui/text/builtin_font.h>

#include <cstdio>
#include <string_view>

using namespace cy;
using namespace cy::ui;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

constexpr Vec2 kViewport{480.0F, 270.0F};

/// A store with an absolute root the size of the viewport, a text painter on the built-in font, and
/// a console along the root's bottom edge.
struct Fixture {
    Fixture() noexcept {
        cy::text::TextServerConfig config;
        started =
            server.start(config).has_value() && text.start(server, builtin_font(), 1).has_value();
        auto made = store.create(kNoElement, Name::intern("hud"));
        started = started && made.has_value();
        if (!started) {
            return;
        }
        root = *made;
        store.layout_input(root)->model = LayoutModel::Absolute;
        ConsoleStyle style;
        style.anchor_min = Vec2{0.0F, 1.0F};
        style.anchor_max = Vec2{1.0F, 1.0F};
        style.offset_min = Vec2{8.0F, -110.0F};
        style.offset_max = Vec2{-8.0F, -8.0F};
        style.visible_rows = 5;
        style.history = 64;
        started = console.create(store, text, root, style).has_value();
    }

    [[nodiscard]] bool lay_out() noexcept {
        ScaleSettings settings;
        settings.mode = ScaleMode::FixedPixel;
        LayoutReport report;
        return layout(store, settings, kViewport, &text, report).has_value();
    }

    cy::text::TextServer server;
    TextPainter text{allocator()};
    ElementStore store{allocator()};
    DevConsole console{allocator()};
    ElementId root;
    bool started = false;
};

ConsoleReply spawn(std::string_view arguments, void* user) noexcept {
    auto* count = static_cast<u32*>(user);
    ++*count;
    return ConsoleReply{
        arguments.empty() ? std::string_view("spawned nothing") : std::string_view("spawned"),
        kConsoleInfo};
}

}  // namespace

CY_TEST_CASE(
    "ui_console: the scrollback is virtualised — rows stay fixed however much is printed") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const usize elements = fixture.store.size();
    char line[32];
    for (u32 index = 0; index < 1000U; ++index) {
        const int length = std::snprintf(line, sizeof(line), "line %u", index);
        CY_REQUIRE(
            fixture.console.print(std::string_view(line, static_cast<usize>(length))).has_value());
    }
    // A thousand prints: the history keeps its 64, the store the same five rows it began with.
    CY_CHECK_EQ(fixture.store.size(), elements);
    CY_CHECK_EQ(fixture.console.rows().size(), 5U);
    CY_CHECK_EQ(fixture.console.line_count(), 64U);
    CY_CHECK(fixture.console.line(63) == std::string_view("line 999"));
    CY_CHECK(fixture.console.line(0) == std::string_view("line 936"));
    // The bottom row is the newest line, and the rows above count back.
    CY_CHECK(fixture.text.text_of(fixture.console.rows()[4]) == std::string_view("line 999"));
    CY_CHECK(fixture.text.text_of(fixture.console.rows()[0]) == std::string_view("line 995"));

    // Scrolling rebinds rows to older lines, and clamps at the oldest.
    CY_REQUIRE(fixture.console.scroll(3).has_value());
    CY_CHECK(fixture.text.text_of(fixture.console.rows()[4]) == std::string_view("line 996"));
    CY_REQUIRE(fixture.console.scroll(1000).has_value());
    CY_CHECK_EQ(fixture.console.scroll_offset(), 59U);
    CY_CHECK(fixture.text.text_of(fixture.console.rows()[0]) == std::string_view("line 936"));
    // A new print follows the output to the bottom again.
    CY_REQUIRE(fixture.console.print("tail").has_value());
    CY_CHECK_EQ(fixture.console.scroll_offset(), 0U);
    CY_CHECK(fixture.text.text_of(fixture.console.rows()[4]) == std::string_view("tail"));
}

CY_TEST_CASE("ui_console: a print repaints its rows and never relays out") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    CY_REQUIRE(fixture.lay_out());
    CY_REQUIRE(fixture.console.print("ready", kConsoleWarning).has_value());
    ScaleSettings settings;
    settings.mode = ScaleMode::FixedPixel;
    LayoutReport report;
    CY_REQUIRE(layout(fixture.store, settings, kViewport, &fixture.text, report).has_value());
    CY_CHECK_EQ(report.measured, 0U);
    CY_CHECK_EQ(report.arranged, 0U);
    CY_CHECK(has_dirty(fixture.store.dirty(fixture.console.rows()[4]), Dirty::Paint));
}

CY_TEST_CASE("ui_console: a command runs, its reply is printed, and an unknown one is an error") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    u32 spawned = 0;
    CY_REQUIRE(fixture.console.add_command("spawn", &spawn, &spawned).has_value());
    CY_REQUIRE(fixture.console.type("spawn tank 3").has_value());
    CY_CHECK(fixture.text.text_of(fixture.console.input_line()) ==
             std::string_view("> spawn tank 3_"));
    CY_REQUIRE(fixture.console.submit().has_value());
    CY_CHECK_EQ(spawned, 1U);
    CY_CHECK(fixture.console.input().empty());
    CY_CHECK(fixture.console.line(0) == std::string_view("> spawn tank 3"));
    CY_CHECK(fixture.console.line(1) == std::string_view("spawned"));

    CY_REQUIRE(fixture.console.type("echo hello there").has_value());
    CY_REQUIRE(fixture.console.submit().has_value());
    CY_CHECK(fixture.console.line(3) == std::string_view("hello there"));

    CY_REQUIRE(fixture.console.type("teleport").has_value());
    CY_REQUIRE(fixture.console.backspace().has_value());
    CY_REQUIRE(fixture.console.submit().has_value());
    CY_CHECK(fixture.console.line(5) == std::string_view("unknown command: telepor"));

    CY_REQUIRE(fixture.console.type("clear").has_value());
    CY_REQUIRE(fixture.console.submit().has_value());
    CY_CHECK_EQ(fixture.console.line_count(), 0U);
}

CY_TEST_CASE("ui_console: it lays out along its anchors and flattens to its panel, rows and text") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    CY_REQUIRE(fixture.console.print("ab").has_value());
    CY_REQUIRE(fixture.lay_out());
    const ui::Rect panel = fixture.store.layout_output(fixture.console.panel())->rect;
    CY_CHECK_EQ(panel.x, 8.0F);
    CY_CHECK_EQ(panel.right(), 472.0F);
    CY_CHECK_EQ(panel.y, 160.0F);
    CY_CHECK_EQ(panel.bottom(), 262.0F);
    // Rows are one line tall and stacked from the panel's padding down.
    const ui::Rect first = fixture.store.layout_output(fixture.console.rows()[0])->rect;
    CY_CHECK_EQ(first.y, 164.0F);
    CY_CHECK_EQ(first.height, 13.0F);

    PrimitiveBuffer buffer(allocator());
    FlattenReport report;
    CY_REQUIRE(flatten(fixture.store, ui::Rect{0.0F, 0.0F, kViewport.x, kViewport.y}, buffer,
                       report, &fixture.text)
                   .has_value());
    // The panel and the input line — the root and the rows draw nothing of their own — then the
    // two glyphs of "ab" and the two of "> _", whose space draws nothing either.
    u32 glyphs = 0;
    for (const Primitive& primitive : buffer.primitives()) {
        glyphs += primitive.material == material_index(BuiltinMaterial::Glyph) ? 1U : 0U;
    }
    CY_CHECK_EQ(glyphs, 2U + 2U);
    CY_CHECK_EQ(buffer.primitives().size(), 2U + glyphs);
    CY_CHECK_EQ(report.empty, 1U + 5U);

    // Closed, the panel and everything in it leave the stream.
    CY_REQUIRE(fixture.console.set_open(false).has_value());
    CY_REQUIRE(fixture.lay_out());
    PrimitiveBuffer closed(allocator());
    CY_REQUIRE(flatten(fixture.store, ui::Rect{0.0F, 0.0F, kViewport.x, kViewport.y}, closed,
                       report, &fixture.text)
                   .has_value());
    CY_CHECK(closed.primitives().empty());
}
