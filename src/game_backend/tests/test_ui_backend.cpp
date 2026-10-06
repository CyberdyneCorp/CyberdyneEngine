// SPDX-License-Identifier: MIT
// `integration.game_backend_ui`: ABI 1.6's interface entries over the real CyberUI store, through
// the table, the way a Swift HUD reaches them. No device: what is checked is the store the entries
// build, the layout it gets, the stream it flattens to, and the events the pointer raises.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/ui_backend.h>
#include <cy/test/test.h>
#include <cy/ui/paint.h>
#include <cy/ui/text/builtin_font.h>

#include <cstring>

using namespace cy;
using cy::game_backend::element_of;
using cy::game_backend::to_abi;
using cy::game_backend::UiAdapter;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

constexpr Vec2 kViewport{480.0F, 270.0F};
constexpr CyEntity kOwner = 0x0000000100000005ULL;

/// An element under `parent`, laid out absolutely; none when the store refuses.
ui::ElementId absolute(ui::ElementStore& store, ui::ElementId parent, const char* name) noexcept {
    auto made = store.create(parent, Name::intern(name));
    if (!made.has_value()) {
        return ui::kNoElement;
    }
    store.layout_input(*made)->model = ui::LayoutModel::Absolute;
    return *made;
}

/// A screen root in a store, a text painter on the built-in font, an element the embedder made
/// itself, and the adapter bound on an ABI host.
struct Fixture {
    Fixture() noexcept {
        started = server.start(cy::text::TextServerConfig{}).has_value() &&
                  text.start(server, ui::builtin_font(), 1).has_value() && root.is_valid() &&
                  embedder.is_valid() && adapter.start().has_value();
        host.game.ui = &adapter;
    }

    [[nodiscard]] bool lay_out() noexcept {
        ui::ScaleSettings settings;
        settings.mode = ui::ScaleMode::FixedPixel;
        return adapter.layout(settings, kViewport).has_value();
    }

    [[nodiscard]] CyUiElement make(CyUiElement parent, CyUiKind kind, const char* name) noexcept {
        CyUiElementDesc desc{};
        desc.kind = kind;
        desc.name = name;
        desc.owner = kOwner;
        CyUiElement out = CY_UI_ELEMENT_NULL;
        CY_CHECK_EQ(table().ui_create(&host, parent, &desc, &out), CY_RESULT_OK);
        return out;
    }

    [[nodiscard]] CyResult place(CyUiElement element, f32 x, f32 y, f32 w, f32 h) noexcept {
        CyUiLayout layout{};
        layout.offset_min[0] = x;
        layout.offset_min[1] = y;
        layout.offset_max[0] = x + w;
        layout.offset_max[1] = y + h;
        return table().ui_set_layout(&host, element, &layout);
    }

    [[nodiscard]] CyUiElement screen() noexcept {
        CyUiElement out = CY_UI_ELEMENT_NULL;
        CY_CHECK_EQ(table().ui_root(&host, &out), CY_RESULT_OK);
        return out;
    }

    cy::text::TextServer server;
    ui::TextPainter text{allocator()};
    ui::ElementStore store{allocator()};
    ui::ElementId root = absolute(store, ui::kNoElement, "screen");
    ui::ElementId embedder = absolute(store, root, "console");
    UiAdapter adapter{allocator(), store, text, root};
    abi::Host host{allocator()};
    bool started = false;
};

}  // namespace

CY_TEST_CASE("the root is the embedder's and every module element is made under it") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const CyUiElement screen = fixture.screen();
    CY_CHECK_EQ(element_of(screen), fixture.root);

    const CyUiElement panel = fixture.make(screen, CY_UI_PANEL, "selection");
    const CyUiElement label = fixture.make(panel, CY_UI_LABEL, nullptr);
    CY_CHECK(fixture.store.alive(element_of(panel)));
    CY_CHECK_EQ(fixture.store.hierarchy(element_of(panel))->parent, fixture.root);
    CY_CHECK_EQ(fixture.store.hierarchy(element_of(label))->parent, element_of(panel));
    // A null name is the kind's.
    CY_CHECK(fixture.store.type_of(element_of(label)).text() == "label");
    CY_CHECK_EQ(fixture.adapter.elements(), 2U);
    CY_CHECK_EQ(fixture.adapter.find("selection"), panel);

    // The root takes no writes, and an element the embedder made is not the module's.
    const CyInterface& iface = table();
    CY_CHECK_EQ(fixture.place(screen, 0, 0, 10, 10), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(iface.ui_destroy(&fixture.host, screen), CY_RESULT_NOT_FOUND);
    const CyUiElement console = to_abi(fixture.embedder);
    CY_CHECK_EQ(iface.ui_set_opacity(&fixture.host, console, 0.5F), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(iface.ui_destroy(&fixture.host, console), CY_RESULT_NOT_FOUND);
    CyUiElementDesc desc{};
    CyUiElement out = CY_UI_ELEMENT_NULL;
    CY_CHECK_EQ(iface.ui_create(&fixture.host, console, &desc, &out), CY_RESULT_NOT_FOUND);
    CY_CHECK(fixture.store.alive(fixture.embedder));
}

CY_TEST_CASE(
    "anchors and offsets place an element as the C++ store does, and the rect reads back") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const CyInterface& iface = table();
    const CyUiElement map = fixture.make(fixture.screen(), CY_UI_PANEL, "minimap");
    // Bottom-right, 102 by 74, six units in: samples/13-rts-selection's minimap.
    CyUiLayout layout{};
    layout.model = CY_UI_LAYOUT_ABSOLUTE;
    layout.anchor_min[0] = layout.anchor_min[1] = 1.0F;
    layout.anchor_max[0] = layout.anchor_max[1] = 1.0F;
    layout.offset_min[0] = -108.0F;
    layout.offset_min[1] = -80.0F;
    layout.offset_max[0] = layout.offset_max[1] = -6.0F;
    CY_CHECK_EQ(iface.ui_set_layout(&fixture.host, map, &layout), CY_RESULT_OK);
    // A zero preferred size is the content's, a zero maximum unbounded, a zero span one.
    const ui::LayoutInput& input = *fixture.store.layout_input(element_of(map));
    CY_CHECK_EQ(input.model, ui::LayoutModel::Absolute);
    CY_CHECK_EQ(input.preferred.x, -1.0F);
    CY_CHECK_EQ(input.maximum.y, 1e9F);
    CY_CHECK_EQ(input.flex_shrink, 1.0F);
    CY_CHECK_EQ(input.align, ui::Align::Stretch);
    CY_CHECK_EQ(input.grid_column_span, 1U);
    CY_REQUIRE(fixture.lay_out());

    f32 rect[4] = {};
    CY_CHECK_EQ(iface.ui_element_rect(&fixture.host, map, rect), CY_RESULT_OK);
    CY_CHECK_EQ(rect[0], 372.0F);
    CY_CHECK_EQ(rect[1], 190.0F);
    CY_CHECK_EQ(rect[2], 102.0F);
    CY_CHECK_EQ(rect[3], 74.0F);

    // Moved after it was laid out: the next layout places it anew. An element is placed by its
    // parent's arrange, so a write that dirtied only the element would leave it where it was.
    layout.offset_min[0] = -208.0F;
    CY_CHECK_EQ(iface.ui_set_layout(&fixture.host, map, &layout), CY_RESULT_OK);
    CY_REQUIRE(fixture.lay_out());
    CY_CHECK_EQ(iface.ui_element_rect(&fixture.host, map, rect), CY_RESULT_OK);
    CY_CHECK_EQ(rect[0], 272.0F);
    CY_CHECK_EQ(rect[2], 202.0F);

    // The two explicit spellings of a non-default: no shrink, and an alignment.
    layout.flags = CY_UI_LAYOUT_NO_SHRINK;
    layout.align = CY_UI_ALIGN_CENTRE;
    CY_CHECK_EQ(iface.ui_set_layout(&fixture.host, map, &layout), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.store.layout_input(element_of(map))->flex_shrink, 0.0F);
    CY_CHECK_EQ(fixture.store.layout_input(element_of(map))->align, ui::Align::Centre);
}

CY_TEST_CASE("a label is measured from its text, and a new number repaints without relayout") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const CyInterface& iface = table();
    const CyUiElement gold = fixture.make(fixture.screen(), CY_UI_LABEL, "gold");
    CY_CHECK_EQ(iface.ui_set_text(&fixture.host, gold, "1250", 0xFFFFD34DU, 1), CY_RESULT_OK);
    CY_CHECK(fixture.text.text_of(element_of(gold)) == "1250");
    CY_REQUIRE(fixture.lay_out());
    // The built-in font is 6 by 13: four glyphs.
    CY_CHECK_EQ(fixture.store.layout_output(element_of(gold))->desired.x, 24.0F);
    CY_CHECK_EQ(fixture.store.layout_output(element_of(gold))->desired.y, 13.0F);

    // Text on a panel, an image or a progress bar is a mistake.
    const CyUiElement panel = fixture.make(fixture.screen(), CY_UI_PANEL, "panel");
    CY_CHECK_EQ(iface.ui_set_text(&fixture.host, panel, "no", 0xFFFFFFFFU, 1),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.ui_set_progress(&fixture.host, gold, 0.5F), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.ui_set_image(&fixture.host, gold, 2, nullptr), CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("a progress bar's fill covers its fraction of the track, in the accent colour") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const CyInterface& iface = table();
    const CyUiElement bar = fixture.make(fixture.screen(), CY_UI_PROGRESS, "health-bar");
    CY_CHECK_EQ(fixture.place(bar, 10, 20, 50, 5), CY_RESULT_OK);
    CyUiStyle style{};
    style.background = 0xFF3A1E1EU;
    style.accent = 0xFF4CC34CU;
    style.corner_radius = 1.0F;
    CY_CHECK_EQ(iface.ui_set_style(&fixture.host, bar, &style), CY_RESULT_OK);
    CY_CHECK_EQ(iface.ui_set_progress(&fixture.host, bar, 0.64F), CY_RESULT_OK);
    CY_REQUIRE(fixture.lay_out());

    // The track keeps its absolute model whatever layout the module gave it.
    CY_CHECK_EQ(fixture.store.layout_input(element_of(bar))->model, ui::LayoutModel::Absolute);
    const ui::Hierarchy* node = fixture.store.hierarchy(element_of(bar));
    CY_REQUIRE_EQ(node->child_count, 1U);
    const ui::ElementId fill = node->first_child;
    CY_CHECK_EQ(fixture.store.paint(fill)->background, 0xFF4CC34CU);
    CY_CHECK_EQ(fixture.store.paint(fill)->corner_radius, 1.0F);
    CY_CHECK_NEAR(fixture.store.layout_output(fill)->rect.width, 32.0F, 1e-4F);
    CY_CHECK_EQ(fixture.store.layout_output(fill)->rect.height, 5.0F);

    // The fill is the adapter's, not the module's.
    CY_CHECK_EQ(iface.ui_set_opacity(&fixture.host, to_abi(fill), 0.5F), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(iface.ui_set_progress(&fixture.host, bar, 0.0F), CY_RESULT_OK);
    CY_REQUIRE(fixture.lay_out());
    CY_CHECK_EQ(fixture.store.layout_output(fill)->rect.width, 0.0F);
}

CY_TEST_CASE("visibility, opacity, clipping and an image reach the store's flags and paint") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const CyInterface& iface = table();
    const CyUiElement row = fixture.make(fixture.screen(), CY_UI_PANEL, "row");
    const ui::ElementId id = element_of(row);
    CyUiStyle style{};
    style.flags = CY_UI_STYLE_CLIP_CHILDREN;
    CY_CHECK_EQ(iface.ui_set_style(&fixture.host, row, &style), CY_RESULT_OK);
    CY_CHECK(ui::has_flag(fixture.store.flags(id), ui::ElementFlags::ClipsChildren));

    // Collapsed is the C++ HUD's hidden row: out of layout, other flags kept.
    CY_CHECK_EQ(iface.ui_set_visibility(&fixture.host, row, CY_UI_COLLAPSED), CY_RESULT_OK);
    CY_CHECK(ui::has_flag(fixture.store.flags(id), ui::ElementFlags::Collapsed));
    CY_CHECK_FALSE(ui::has_flag(fixture.store.flags(id), ui::ElementFlags::Visible));
    CY_CHECK(ui::has_flag(fixture.store.flags(id), ui::ElementFlags::ClipsChildren));
    CY_CHECK_EQ(iface.ui_set_visibility(&fixture.host, row, CY_UI_HIDDEN), CY_RESULT_OK);
    CY_CHECK_FALSE(ui::has_flag(fixture.store.flags(id), ui::ElementFlags::Collapsed));
    CY_CHECK_FALSE(ui::has_flag(fixture.store.flags(id), ui::ElementFlags::Visible));
    CY_CHECK_EQ(iface.ui_set_visibility(&fixture.host, row, CY_UI_VISIBLE), CY_RESULT_OK);
    CY_CHECK(ui::has_flag(fixture.store.flags(id), ui::ElementFlags::Visible));

    CY_CHECK_EQ(iface.ui_set_opacity(&fixture.host, row, 0.25F), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.store.paint(id)->opacity, 0.25F);

    const CyUiElement image = fixture.make(fixture.screen(), CY_UI_IMAGE, "portrait");
    const f32 uv[4] = {0.25F, 0.5F, 0.25F, 0.5F};
    CY_CHECK_EQ(iface.ui_set_image(&fixture.host, image, 2, uv), CY_RESULT_OK);
    const ui::PaintData& paint = *fixture.store.paint(element_of(image));
    CY_CHECK_EQ(paint.material, ui::material_index(ui::BuiltinMaterial::Image));
    CY_CHECK_EQ(paint.atlas, 2U);
    CY_CHECK_EQ(paint.uv.x, 0.25F);
    CY_CHECK_EQ(paint.uv.height, 0.5F);
    CY_CHECK_EQ(iface.ui_set_image(&fixture.host, image, 0x10000U, uv), CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("a press and a release over one button is one click, to the button's owner") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const CyUiElement button = fixture.make(fixture.screen(), CY_UI_BUTTON, "build");
    CY_CHECK_EQ(fixture.place(button, 100, 100, 60, 20), CY_RESULT_OK);
    CY_REQUIRE(fixture.lay_out());
    UiAdapter& adapter = fixture.adapter;
    const Vec2 on{120.0F, 110.0F};
    const Vec2 off{10.0F, 10.0F};

    CY_REQUIRE(adapter.route_pointer(on, CY_INPUT_BUTTON_LEFT, 0).has_value());
    CY_CHECK(adapter.pointer_over());
    // The press focused the button: a FOCUS, and no click yet.
    CY_REQUIRE_EQ(adapter.events().size(), 1U);
    CY_CHECK_EQ(adapter.events()[0].kind, static_cast<u32>(CY_UI_EVENT_FOCUS));
    adapter.clear_events();
    CY_REQUIRE(adapter.route_pointer(on, 0, CY_INPUT_BUTTON_LEFT).has_value());
    CY_REQUIRE_EQ(adapter.events().size(), 1U);
    const CyUiEvent& click = adapter.events()[0];
    CY_CHECK_EQ(click.kind, static_cast<u32>(CY_UI_EVENT_CLICK));
    CY_CHECK_EQ(click.element, button);
    CY_CHECK_EQ(click.owner, kOwner);
    CY_CHECK_EQ(click.button, CY_INPUT_BUTTON_LEFT);
    CY_CHECK_EQ(click.position[0], 120.0F);
    CY_CHECK_EQ(click.struct_size, sizeof(CyUiEvent));
    adapter.clear_events();

    // Pressed on the button and released off it is not a click, and neither is the reverse.
    CY_REQUIRE(adapter.route_pointer(on, CY_INPUT_BUTTON_LEFT, 0).has_value());
    CY_REQUIRE(adapter.route_pointer(off, 0, CY_INPUT_BUTTON_LEFT).has_value());
    CY_CHECK_FALSE(adapter.pointer_over());
    CY_REQUIRE(adapter.route_pointer(off, CY_INPUT_BUTTON_LEFT, 0).has_value());
    CY_REQUIRE(adapter.route_pointer(on, 0, CY_INPUT_BUTTON_LEFT).has_value());
    // Only the right button: nothing.
    CY_REQUIRE(adapter.route_pointer(on, CY_INPUT_BUTTON_RIGHT, 0).has_value());
    CY_REQUIRE(adapter.route_pointer(on, 0, CY_INPUT_BUTTON_RIGHT).has_value());
    for (const CyUiEvent& event : adapter.events()) {
        CY_CHECK_NE(event.kind, static_cast<u32>(CY_UI_EVENT_CLICK));
    }
    CY_CHECK_EQ(adapter.clicks(), 1U);
}

CY_TEST_CASE("a hit is the module's element under the point, and the world is null") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const CyInterface& iface = table();
    const CyUiElement panel = fixture.make(fixture.screen(), CY_UI_PANEL, "selection");
    CY_CHECK_EQ(fixture.place(panel, 6, 200, 180, 64), CY_RESULT_OK);
    const CyUiElement label = fixture.make(panel, CY_UI_LABEL, "title");
    CY_CHECK_EQ(iface.ui_set_text(&fixture.host, label, "Selected", 0xFFFFFFFFU, 1), CY_RESULT_OK);
    // The embedder's own element, at the top left.
    ui::LayoutInput& own = *fixture.store.layout_input(fixture.embedder);
    own.offset_max = Vec2{40.0F, 40.0F};
    CY_REQUIRE(fixture.lay_out());

    CyUiElement hit = 99;
    const f32 on_panel[2] = {150.0F, 250.0F};
    CY_CHECK_EQ(iface.ui_hit_test(&fixture.host, on_panel, &hit), CY_RESULT_OK);
    CY_CHECK_EQ(hit, panel);
    // Over the label inside it: the label, the topmost module element.
    const f32 on_label[2] = {10.0F, 205.0F};
    CY_CHECK_EQ(iface.ui_hit_test(&fixture.host, on_label, &hit), CY_RESULT_OK);
    CY_CHECK_EQ(hit, label);
    // The root is transparent: the middle of the screen is the world.
    const f32 world[2] = {240.0F, 100.0F};
    CY_CHECK_EQ(iface.ui_hit_test(&fixture.host, world, &hit), CY_RESULT_OK);
    CY_CHECK_EQ(hit, CY_UI_ELEMENT_NULL);
    // Over the embedder's element: not the module's, so null — and still over the interface.
    const f32 console[2] = {20.0F, 20.0F};
    CY_CHECK_EQ(iface.ui_hit_test(&fixture.host, console, &hit), CY_RESULT_OK);
    CY_CHECK_EQ(hit, CY_UI_ELEMENT_NULL);
    CY_REQUIRE(fixture.adapter.route_pointer(Vec2{20.0F, 20.0F}, 0, 0).has_value());
    CY_CHECK(fixture.adapter.pointer_over());
    // A collapsed element is not hit.
    CY_CHECK_EQ(iface.ui_set_visibility(&fixture.host, panel, CY_UI_COLLAPSED), CY_RESULT_OK);
    CY_REQUIRE(fixture.lay_out());
    CY_CHECK_EQ(iface.ui_hit_test(&fixture.host, on_panel, &hit), CY_RESULT_OK);
    CY_CHECK_EQ(hit, CY_UI_ELEMENT_NULL);
}

CY_TEST_CASE("focus moves between buttons with a blur and a focus, and a panel refuses it") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const CyInterface& iface = table();
    const CyUiElement first = fixture.make(fixture.screen(), CY_UI_BUTTON, "first");
    const CyUiElement second = fixture.make(fixture.screen(), CY_UI_BUTTON, "second");
    const CyUiElement panel = fixture.make(fixture.screen(), CY_UI_PANEL, "panel");
    UiAdapter& adapter = fixture.adapter;

    CY_CHECK_EQ(iface.ui_set_focus(&fixture.host, first), CY_RESULT_OK);
    CyUiElement focus = CY_UI_ELEMENT_NULL;
    CY_CHECK_EQ(iface.ui_focus(&fixture.host, &focus), CY_RESULT_OK);
    CY_CHECK_EQ(focus, first);
    CY_CHECK_EQ(iface.ui_set_focus(&fixture.host, second), CY_RESULT_OK);
    CY_REQUIRE_EQ(adapter.events().size(), 3U);
    CY_CHECK_EQ(adapter.events()[0].kind, static_cast<u32>(CY_UI_EVENT_FOCUS));
    CY_CHECK_EQ(adapter.events()[1].kind, static_cast<u32>(CY_UI_EVENT_BLUR));
    CY_CHECK_EQ(adapter.events()[1].element, first);
    CY_CHECK_EQ(adapter.events()[2].kind, static_cast<u32>(CY_UI_EVENT_FOCUS));
    CY_CHECK_EQ(adapter.events()[2].element, second);

    CY_CHECK_EQ(iface.ui_set_focus(&fixture.host, panel), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.ui_set_focus(&fixture.host, CY_UI_ELEMENT_NULL), CY_RESULT_OK);
    CY_CHECK_EQ(iface.ui_focus(&fixture.host, &focus), CY_RESULT_OK);
    CY_CHECK_EQ(focus, CY_UI_ELEMENT_NULL);
}

CY_TEST_CASE("a receiver that moves focus while handling a click loses none of the events") {
    // Regression: the embedder walked `events()` and cleared the queue after, so the BLUR and the
    // FOCUS a click handler's own `ui_set_focus` queued were appended to the array being walked
    // and then cleared, never delivered.
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const CyInterface& iface = table();
    const CyUiElement clicked = fixture.make(fixture.screen(), CY_UI_BUTTON, "clicked");
    const CyUiElement next = fixture.make(fixture.screen(), CY_UI_BUTTON, "next");
    CY_CHECK_EQ(fixture.place(clicked, 100, 100, 60, 20), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.place(next, 200, 100, 60, 20), CY_RESULT_OK);
    CY_REQUIRE(fixture.lay_out());
    UiAdapter& adapter = fixture.adapter;
    const Vec2 on{120.0F, 110.0F};
    CY_REQUIRE(adapter.route_pointer(on, CY_INPUT_BUTTON_LEFT, 0).has_value());
    CY_REQUIRE(adapter.route_pointer(on, 0, CY_INPUT_BUTTON_LEFT).has_value());

    CyUiEvent seen[8]{};
    u32 count = 0;
    const auto receive = [&](const CyUiEvent& event) noexcept {
        if (count < 8U) {
            seen[count] = event;
        }
        ++count;
        if (event.kind == CY_UI_EVENT_CLICK) {
            CY_CHECK_EQ(iface.ui_set_focus(&fixture.host, next), CY_RESULT_OK);
        }
    };
    adapter.drain_events(receive);
    // What the handler raised arrives with the next frame's delivery.
    adapter.drain_events(receive);
    CY_REQUIRE_EQ(count, 4U);
    CY_CHECK_EQ(seen[0].kind, static_cast<u32>(CY_UI_EVENT_FOCUS));
    CY_CHECK_EQ(seen[1].kind, static_cast<u32>(CY_UI_EVENT_CLICK));
    CY_CHECK_EQ(seen[2].kind, static_cast<u32>(CY_UI_EVENT_BLUR));
    CY_CHECK_EQ(seen[2].element, clicked);
    CY_CHECK_EQ(seen[3].kind, static_cast<u32>(CY_UI_EVENT_FOCUS));
    CY_CHECK_EQ(seen[3].element, next);
    CY_CHECK(adapter.events().empty());
}

CY_TEST_CASE("destroying an element takes its subtree, and every handle in it goes stale") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const CyInterface& iface = table();
    const CyUiElement panel = fixture.make(fixture.screen(), CY_UI_PANEL, "panel");
    const CyUiElement label = fixture.make(panel, CY_UI_LABEL, "label");
    const CyUiElement bar = fixture.make(panel, CY_UI_PROGRESS, "bar");
    CY_CHECK_EQ(iface.ui_set_text(&fixture.host, label, "gone", 0xFFFFFFFFU, 1), CY_RESULT_OK);
    const usize before = fixture.store.size();
    CY_CHECK_EQ(iface.ui_destroy(&fixture.host, panel), CY_RESULT_OK);
    // The panel, the label, the bar and the bar's fill.
    CY_CHECK_EQ(fixture.store.size(), before - 4U);
    CY_CHECK_EQ(fixture.adapter.elements(), 0U);
    CY_CHECK(fixture.text.text_of(element_of(label)).empty());
    for (const CyUiElement stale : {panel, label, bar}) {
        CY_CHECK_EQ(iface.ui_set_opacity(&fixture.host, stale, 1.0F), CY_RESULT_NOT_FOUND);
    }
    // A slot reused by a new element does not answer to the old handle.
    const CyUiElement fresh = fixture.make(fixture.screen(), CY_UI_PANEL, "fresh");
    CY_CHECK_NE(fresh, panel);
    CY_CHECK_EQ(iface.ui_destroy(&fixture.host, panel), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(iface.ui_destroy(&fixture.host, fresh), CY_RESULT_OK);
}
