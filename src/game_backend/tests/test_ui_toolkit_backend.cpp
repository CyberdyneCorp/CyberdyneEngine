// SPDX-License-Identifier: MIT
// `integration.game_backend_ui`, ABI 1.12: the widget toolkit's entries over a real
// `cy::ui::Toolkit`, through the table, the way CyberdyneKit reaches them. What is checked is what
// a module sees: widgets made and written, documents loaded and found in, events delivered to their
// owners, and the line between the module's widgets and the embedder's.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/host.h>
#include <cy/core/assets/file.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/ui_backend.h>
#include <cy/test/test.h>
#include <cy/ui/text/builtin_font.h>
#include <cy/ui/toolkit/document_host.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

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

constexpr CyEntity kOwner = 0x0000000100000007ULL;
constexpr CyEntity kDocumentOwner = 0x0000000100000009ULL;

constexpr const char* kMenu = R"(cyui 1
column #menu
  label #title text="Options"
  slider #volume min=0 max=10 step=1 value=5
  button #play text="Play"
)";

/// A scene and a screen root, the toolkit beside them on one store, and the adapter bound on a host
/// with the toolkit attached.
struct Fixture {
    explicit Fixture(bool attach = true) noexcept {
        started = server.start(cy::text::TextServerConfig{}).has_value() &&
                  text.start(server, ui::builtin_font(), 1).has_value() && root.is_valid() &&
                  adapter.start().has_value() && toolkit.start().has_value();
        settings.mode = ui::ScaleMode::FixedPixel;
        settings.reference = Vec2{800.0F, 600.0F};
        if (attach) {
            adapter.attach_toolkit(&toolkit, &resolver);
        }
        host.game.ui = &adapter;
    }

    [[nodiscard]] bool frame(i64 now_ns = 0) noexcept {
        return adapter.frame(1.0F / 60.0F, settings, Vec2{800.0F, 600.0F}, now_ns).has_value();
    }

    [[nodiscard]] CyUiElement widget(CyUiElement parent, u32 kind, const char* id,
                                     const char* caption = nullptr) noexcept {
        CyUiWidgetDesc desc{};
        desc.struct_size = sizeof(desc);
        desc.kind = kind;
        desc.id = id;
        desc.text = caption;
        desc.owner = kOwner;
        CyUiElement out = CY_UI_ELEMENT_NULL;
        CY_CHECK_EQ(table().ui_create_widget(&host, parent, &desc, &out), CY_RESULT_OK);
        return out;
    }

    [[nodiscard]] CyResult load(const char* document, CyUiElement& out) noexcept {
        CyUiDocumentDesc desc{};
        desc.struct_size = sizeof(desc);
        desc.text = document;
        desc.text_size = std::strlen(document);
        desc.owner = kDocumentOwner;
        return table().ui_load_document(&host, CY_UI_ELEMENT_NULL, &desc, &out);
    }

    [[nodiscard]] CyUiElement find(const char* id) noexcept {
        CyUiElement out = CY_UI_ELEMENT_NULL;
        CY_CHECK_EQ(table().ui_find(&host, CY_UI_ELEMENT_NULL, id, &out), CY_RESULT_OK);
        return out;
    }

    [[nodiscard]] u32 count(u32 kind, CyUiElement element) const noexcept {
        u32 seen = 0;
        for (const CyUiEvent& event : adapter.events()) {
            seen += (event.kind == kind && event.element == element) ? 1U : 0U;
        }
        return seen;
    }

    cy::text::TextServer server;
    ui::TextPainter text{allocator()};
    ui::ElementStore store{allocator()};
    ui::ElementId root = [this] {
        auto made = store.create(ui::kNoElement, Name::intern("screen"));
        if (!made.has_value()) {
            return ui::kNoElement;
        }
        store.layout_input(*made)->model = ui::LayoutModel::Absolute;
        return *made;
    }();
    UiAdapter adapter{allocator(), store, text, root};
    ui::Toolkit toolkit{allocator(), store, text};
    ui::FileDocumentResolver resolver{allocator(), "."};
    abi::Host host{allocator()};
    ui::ScaleSettings settings;
    bool started = false;
};

[[nodiscard]] std::string text_of(const Fixture& fixture, CyUiElement widget) {
    u32 size = 0;
    CY_CHECK_EQ(
        table().ui_widget_text(const_cast<abi::Host*>(&fixture.host), widget, nullptr, 0, &size),
        CY_RESULT_OK);
    std::string text(size + 1U, '\0');
    CY_CHECK_EQ(table().ui_widget_text(const_cast<abi::Host*>(&fixture.host), widget, text.data(),
                                       size + 1U, &size),
                CY_RESULT_OK);
    text.resize(size);
    return text;
}

void write_file(const std::string& path, std::string_view text) {
    FILE* file = std::fopen(path.c_str(), "wb");
    CY_REQUIRE(file != nullptr);
    CY_REQUIRE_EQ(std::fwrite(text.data(), 1, text.size(), file), text.size());
    CY_REQUIRE_EQ(std::fclose(file), 0);
}

}  // namespace

CY_TEST_CASE("ui toolkit: with no toolkit attached every widget entry is UNAVAILABLE") {
    Fixture fixture(false);
    CY_REQUIRE(fixture.started);
    const CyInterface& iface = table();
    CyUiWidgetDesc desc{};
    desc.struct_size = sizeof(desc);
    CyUiElement out = CY_UI_ELEMENT_NULL;
    CY_CHECK_EQ(iface.ui_create_widget(&fixture.host, CY_UI_ELEMENT_NULL, &desc, &out),
                CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(fixture.load(kMenu, out), CY_RESULT_UNAVAILABLE);
    u32 consumed = 1;
    CY_CHECK_EQ(iface.ui_navigate(&fixture.host, CY_UI_ACTION_ACCEPT, &consumed),
                CY_RESULT_UNAVAILABLE);
    // 1.6 is untouched: the frame is its layout.
    CY_CHECK(fixture.frame());
}

CY_TEST_CASE("ui toolkit: a module's widgets are written and read through the table") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const CyInterface& iface = table();
    CyEngine host = &fixture.host;
    const CyUiElement panel = fixture.widget(CY_UI_ELEMENT_NULL, CY_UI_WIDGET_COLUMN, "panel");
    const CyUiElement name = fixture.widget(panel, CY_UI_WIDGET_TEXT_FIELD, "name", "Ada");
    const CyUiElement volume = fixture.widget(panel, CY_UI_WIDGET_SLIDER, "volume");
    const CyUiElement sound = fixture.widget(panel, CY_UI_WIDGET_CHECKBOX, "sound");
    const CyUiElement quality = fixture.widget(panel, CY_UI_WIDGET_DROPDOWN, "quality");
    CY_CHECK_EQ(fixture.toolkit.parent(element_of(name)), element_of(panel));

    CY_CHECK_EQ(text_of(fixture, name), "Ada");
    CY_CHECK_EQ(iface.ui_widget_set_text(host, name, "Grace"), CY_RESULT_OK);
    CY_CHECK_EQ(text_of(fixture, name), "Grace");

    // A slider's value is clamped to its range — zero to one by default.
    f32 value = 0.0F;
    CY_CHECK_EQ(iface.ui_widget_set_value(host, volume, 4.0F), CY_RESULT_OK);
    CY_CHECK_EQ(iface.ui_widget_value(host, volume, &value), CY_RESULT_OK);
    CY_CHECK_EQ(value, 1.0F);
    // A checkbox's value is its state.
    CY_CHECK_EQ(iface.ui_widget_set_value(host, sound, 0.75F), CY_RESULT_OK);
    CY_CHECK(fixture.toolkit.checked(element_of(sound)));
    CY_CHECK_EQ(iface.ui_widget_value(host, sound, &value), CY_RESULT_OK);
    CY_CHECK_EQ(value, 1.0F);

    const char* const levels[] = {"Low", "Medium", "High"};
    u32 selected = 9;
    CY_CHECK_EQ(iface.ui_widget_set_items(host, quality, levels, 3), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.toolkit.item_count(element_of(quality)), 3U);
    CY_CHECK_EQ(iface.ui_widget_set_selected(host, quality, 2), CY_RESULT_OK);
    CY_CHECK_EQ(iface.ui_widget_selected(host, quality, &selected), CY_RESULT_OK);
    CY_CHECK_EQ(selected, 2U);
    // A slider has no selection.
    CY_CHECK_EQ(iface.ui_widget_selected(host, volume, &selected), CY_RESULT_INVALID_ARGUMENT);

    CY_CHECK_EQ(iface.ui_widget_set_enabled(host, volume, 0), CY_RESULT_OK);
    CY_CHECK_FALSE(fixture.toolkit.enabled(element_of(volume)));
    CY_CHECK_EQ(iface.ui_widget_set_classes(host, panel, "wide primary"), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.find("quality"), quality);
    CY_CHECK_EQ(fixture.find("nothing"), CY_UI_ELEMENT_NULL);
    CyUiElement under = CY_UI_ELEMENT_NULL;
    CY_CHECK_EQ(iface.ui_find(host, panel, "sound", &under), CY_RESULT_OK);
    CY_CHECK_EQ(under, sound);

    // An image is not made through this entry, and a kind past the last is refused.
    CyUiWidgetDesc image{};
    image.struct_size = sizeof(image);
    image.kind = CY_UI_WIDGET_IMAGE;
    CyUiElement out = CY_UI_ELEMENT_NULL;
    CY_CHECK_EQ(iface.ui_create_widget(host, CY_UI_ELEMENT_NULL, &image, &out),
                CY_RESULT_INVALID_ARGUMENT);
    image.kind = CY_UI_WIDGET_DIALOG + 1U;
    CY_CHECK_EQ(iface.ui_create_widget(host, CY_UI_ELEMENT_NULL, &image, &out),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK(fixture.frame());
}

CY_TEST_CASE("ui toolkit: a widget the embedder made is not the module's") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const CyInterface& iface = table();
    ui::WidgetDesc desc;
    desc.kind = ui::WidgetKind::Button;
    desc.id = "console-close";
    auto made = fixture.toolkit.create(ui::kNoElement, desc);
    CY_REQUIRE(made.has_value());
    const CyUiElement console = to_abi(*made);
    CY_CHECK_EQ(iface.ui_widget_set_text(&fixture.host, console, "mine"), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(iface.ui_destroy(&fixture.host, console), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(fixture.find("console-close"), CY_UI_ELEMENT_NULL);
    CyUiWidgetDesc child{};
    child.struct_size = sizeof(child);
    CyUiElement out = CY_UI_ELEMENT_NULL;
    CY_CHECK_EQ(iface.ui_create_widget(&fixture.host, console, &child, &out), CY_RESULT_NOT_FOUND);
    CY_CHECK(fixture.toolkit.alive(*made));
}

CY_TEST_CASE("ui toolkit: a document's widgets are found by id and report to its owner") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const CyInterface& iface = table();
    CyUiElement menu = CY_UI_ELEMENT_NULL;
    CY_REQUIRE_EQ(fixture.load(kMenu, menu), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.adapter.documents(), 1U);
    CY_CHECK_EQ(menu, fixture.find("menu"));
    CY_CHECK_EQ(text_of(fixture, fixture.find("title")), "Options");
    CY_REQUIRE(fixture.frame());

    // The player moves the slider: a CHANGED to the document's owner, carrying the value.
    const CyUiElement volume = fixture.find("volume");
    CY_REQUIRE_EQ(iface.ui_set_focus(&fixture.host, volume), CY_RESULT_OK);
    CyUiElement focused = CY_UI_ELEMENT_NULL;
    CY_CHECK_EQ(iface.ui_focus(&fixture.host, &focused), CY_RESULT_OK);
    CY_CHECK_EQ(focused, volume);
    fixture.adapter.clear_events();
    u32 consumed = 0;
    CY_CHECK_EQ(iface.ui_navigate(&fixture.host, CY_UI_ACTION_NAVIGATE_RIGHT, &consumed),
                CY_RESULT_OK);
    CY_CHECK_EQ(consumed, 1U);
    CY_REQUIRE_EQ(fixture.count(CY_UI_EVENT_CHANGED, volume), 1U);
    const CyUiEvent& changed = fixture.adapter.events()[0];
    CY_CHECK_EQ(changed.owner, kDocumentOwner);
    CY_CHECK_EQ(changed.position[0], 6.0F);
    CY_CHECK_EQ(changed.struct_size, sizeof(CyUiEvent));

    // Down to the button, and Accept: its focus, the slider's blur, and a click.
    fixture.adapter.clear_events();
    CY_CHECK_EQ(iface.ui_navigate(&fixture.host, CY_UI_ACTION_NAVIGATE_DOWN, &consumed),
                CY_RESULT_OK);
    const CyUiElement play = fixture.find("play");
    CY_CHECK_EQ(fixture.count(CY_UI_EVENT_BLUR, volume), 1U);
    CY_CHECK_EQ(fixture.count(CY_UI_EVENT_FOCUS, play), 1U);
    CY_CHECK_EQ(iface.ui_navigate(&fixture.host, CY_UI_ACTION_ACCEPT, &consumed), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.count(CY_UI_EVENT_CLICK, play), 1U);

    // Delivered once: the frame clears the toolkit's queue after translating it.
    const usize queued = fixture.adapter.events().size();
    CY_REQUIRE(fixture.frame());
    CY_CHECK_EQ(fixture.adapter.events().size(), queued);
    CY_CHECK(fixture.toolkit.events().empty());
}

CY_TEST_CASE("ui toolkit: a document that does not parse names its line and makes nothing") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const u32 before = fixture.toolkit.widget_count();
    CyUiElement out = CY_UI_ELEMENT_NULL;
    CY_CHECK_EQ(fixture.load("cyui 1\ncolumn #menu\n   label\n", out), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK(std::strstr(abi::last_error_message(), "line 3") != nullptr);
    CY_CHECK_EQ(fixture.toolkit.widget_count(), before);
    CY_CHECK_EQ(fixture.adapter.documents(), 0U);
}

CY_TEST_CASE("ui toolkit: a click on a widget through the pointer reaches its owner") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const CyUiElement ok = fixture.widget(CY_UI_ELEMENT_NULL, CY_UI_WIDGET_BUTTON, "ok", "OK");
    CY_REQUIRE(
        fixture.toolkit
            .set_anchors(element_of(ok), Vec2{0, 0}, Vec2{0, 0}, Vec2{100, 100}, Vec2{200, 140})
            .has_value());
    CY_REQUIRE(fixture.frame());
    const Vec2 on{150.0F, 120.0F};
    CY_REQUIRE(fixture.adapter.route_pointer(on, CY_INPUT_BUTTON_LEFT, 0).has_value());
    CY_CHECK(fixture.adapter.pointer_over());
    CY_REQUIRE(fixture.adapter.route_pointer(on, 0, CY_INPUT_BUTTON_LEFT).has_value());
    CY_REQUIRE_EQ(fixture.count(CY_UI_EVENT_CLICK, ok), 1U);
    for (const CyUiEvent& event : fixture.adapter.events()) {
        CY_CHECK_EQ(event.owner, kOwner);
    }
    // Over nothing: the world.
    CY_REQUIRE(fixture.adapter.route_pointer(Vec2{700.0F, 500.0F}, 0, 0).has_value());
    CY_CHECK_FALSE(fixture.adapter.pointer_over());
}

CY_TEST_CASE("ui toolkit: a dialog opens and closes with events, and an animation runs by time") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const CyInterface& iface = table();
    const CyUiElement dialog = fixture.widget(CY_UI_ELEMENT_NULL, CY_UI_WIDGET_DIALOG, "quit");
    (void)fixture.widget(dialog, CY_UI_WIDGET_BUTTON, "yes", "Yes");
    CY_REQUIRE(fixture.frame());
    CY_CHECK_EQ(iface.ui_widget_open(&fixture.host, dialog, 1), CY_RESULT_OK);
    CY_CHECK(fixture.toolkit.is_open(element_of(dialog)));
    CY_CHECK_EQ(fixture.count(CY_UI_EVENT_OPENED, dialog), 1U);
    CY_CHECK_EQ(iface.ui_widget_open(&fixture.host, dialog, 0), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.count(CY_UI_EVENT_CLOSED, dialog), 1U);

    const CyUiElement badge = fixture.widget(CY_UI_ELEMENT_NULL, CY_UI_WIDGET_LABEL, "badge", "!");
    CyUiAnimation fade{};
    fade.struct_size = sizeof(fade);
    fade.property = CY_UI_ANIMATE_OPACITY;
    fade.from = 0.0F;
    fade.to = 1.0F;
    fade.duration = 0.5F;
    fade.easing = CY_UI_EASING_LINEAR;
    CY_CHECK_EQ(iface.ui_widget_animate(&fixture.host, badge, &fade), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.toolkit.running_animations(), 1U);
    for (u32 frame = 0; frame < 15; ++frame) {
        CY_REQUIRE(fixture.frame());
    }
    // A quarter second of a half-second linear fade.
    const f32 halfway = fixture.store.paint(element_of(badge))->opacity;
    CY_CHECK_GT(halfway, 0.45F);
    CY_CHECK_LT(halfway, 0.55F);
    for (u32 frame = 0; frame < 30; ++frame) {
        CY_REQUIRE(fixture.frame());
    }
    CY_CHECK_EQ(fixture.store.paint(element_of(badge))->opacity, 1.0F);
    CY_CHECK_EQ(fixture.toolkit.running_animations(), 0U);
    fade.easing = CY_UI_EASING_BACK + 1U;
    CY_CHECK_EQ(iface.ui_widget_animate(&fixture.host, badge, &fade), CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("ui toolkit: 1.6's destroy, visibility, rect and focus take a module's widget") {
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    const CyInterface& iface = table();
    const CyUiElement panel = fixture.widget(CY_UI_ELEMENT_NULL, CY_UI_WIDGET_COLUMN, "panel");
    const CyUiElement button = fixture.widget(panel, CY_UI_WIDGET_BUTTON, "go", "Go");
    CY_REQUIRE(fixture.frame());
    f32 rect[4] = {};
    CY_CHECK_EQ(iface.ui_element_rect(&fixture.host, button, rect), CY_RESULT_OK);
    CY_CHECK(rect[2] > 0.0F);
    CY_CHECK_EQ(iface.ui_set_visibility(&fixture.host, panel, CY_UI_COLLAPSED), CY_RESULT_OK);
    CY_CHECK_FALSE(fixture.toolkit.visible(element_of(panel)));
    CY_CHECK_EQ(iface.ui_set_visibility(&fixture.host, panel, CY_UI_VISIBLE), CY_RESULT_OK);
    CY_REQUIRE(fixture.frame());
    CY_CHECK_EQ(iface.ui_set_focus(&fixture.host, button), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.toolkit.focused(), element_of(button));
    CY_CHECK_EQ(iface.ui_set_focus(&fixture.host, panel), CY_RESULT_INVALID_ARGUMENT);

    CY_CHECK_EQ(iface.ui_destroy(&fixture.host, panel), CY_RESULT_OK);
    CY_CHECK_FALSE(fixture.toolkit.alive(element_of(button)));
    CY_CHECK_EQ(iface.ui_widget_set_text(&fixture.host, button, "stale"), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(iface.ui_destroy(&fixture.host, panel), CY_RESULT_NOT_FOUND);
    CY_REQUIRE(fixture.frame());
}

CY_TEST_CASE("ui toolkit: a document loaded by path reloads on a save, keeping its handles") {
    const std::string directory = "cy_game_backend_ui_documents";
    (void)assets::fs::remove_directory_recursive(directory.c_str());
    CY_REQUIRE(assets::fs::create_directories(directory.c_str()).has_value());
    write_file(directory + "/menu.cyui", kMenu);
    assets::VirtualFileSystem files;
    auto mount =
        assets::DirectoryMount::create(directory.c_str(), assets::MountKind::Project, false);
    CY_REQUIRE(mount.has_value());
    CY_REQUIRE(files.mount_owned(std::move(*mount), 0).has_value());

    Fixture fixture;
    CY_REQUIRE(fixture.started);
    ui::VfsDocumentResolver resolver(files);
    fixture.adapter.attach_toolkit(&fixture.toolkit, &resolver, &files);
    CyUiDocumentDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.path = "menu.cyui";
    desc.owner = kDocumentOwner;
    CyUiElement menu = CY_UI_ELEMENT_NULL;
    CY_REQUIRE_EQ(table().ui_load_document(&fixture.host, CY_UI_ELEMENT_NULL, &desc, &menu),
                  CY_RESULT_OK);
    const CyUiElement title = fixture.find("title");
    CY_CHECK_EQ(text_of(fixture, title), "Options");
    CY_REQUIRE(fixture.frame(0));

    constexpr i64 kSettle = 100'000'000;  // the host's default
    std::string saved = kMenu;
    saved.replace(saved.find("Options"), 7, "Settings");
    write_file(directory + "/menu.cyui", saved);
    CY_REQUIRE(fixture.frame(2 * kSettle));
    CY_REQUIRE(fixture.frame(4 * kSettle));
    CY_CHECK_EQ(fixture.find("title"), title);
    CY_CHECK_EQ(text_of(fixture, title), "Settings");

    desc.path = "missing.cyui";
    CyUiElement none = CY_UI_ELEMENT_NULL;
    CY_CHECK_NE(table().ui_load_document(&fixture.host, CY_UI_ELEMENT_NULL, &desc, &none),
                CY_RESULT_OK);
    (void)assets::fs::remove_directory_recursive(directory.c_str());
}

CY_TEST_CASE(
    "ui toolkit: a module's document binds to the embedder's values, and takes a tooltip") {
    // ABI 1.17's `set_document_bindings`: the host owns the game value a slider drives — a bus's
    // gain, say — and a module's document reaches it by name with no module code between.
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    class Gain final : public ui::BindingSource {
    public:
        [[nodiscard]] ui::BoundValue read() noexcept override {
            ui::BoundValue value;
            value.kind = ui::BoundValue::Kind::Number;
            value.number = gain;
            dirty = false;
            return value;
        }
        [[nodiscard]] bool changed() const noexcept override { return dirty; }
        [[nodiscard]] Status write(const ui::BoundValue& value) noexcept override {
            gain = value.number;
            ++writes;
            return ok();
        }
        f32 gain = 0.25F;
        u32 writes = 0;
        bool dirty = true;
    };
    class Values final : public ui::DocumentBindings {
    public:
        [[nodiscard]] ui::BindingSource* find(std::string_view name) noexcept override {
            return name == "volume.music" ? &music : nullptr;
        }
        Gain music;
    };
    Values values;
    fixture.adapter.set_document_bindings(&values);
    constexpr const char* kSettings = R"(cyui 1
column #settings
  slider #music min=0 max=1 step=0.25 value=1 bind=volume.music bind-mode=two-way
)";
    CyUiElement root = CY_UI_ELEMENT_NULL;
    CY_REQUIRE_EQ(fixture.load(kSettings, root), CY_RESULT_OK);
    CY_REQUIRE(fixture.frame());
    const CyUiElement music = fixture.find("music");
    f32 value = 0.0F;
    CY_REQUIRE_EQ(table().ui_widget_value(&fixture.host, music, &value), CY_RESULT_OK);
    CY_CHECK_EQ(value, 0.25F);  // the embedder's value, not the document's 1
    CY_REQUIRE_EQ(table().ui_set_focus(&fixture.host, music), CY_RESULT_OK);
    u32 consumed = 0;
    CY_REQUIRE_EQ(table().ui_navigate(&fixture.host, CY_UI_ACTION_NAVIGATE_RIGHT, &consumed),
                  CY_RESULT_OK);
    CY_REQUIRE(fixture.frame());
    CY_CHECK_EQ(values.music.gain, 0.5F);  // the player's step written back to the embedder
    CY_CHECK(values.music.writes >= 1U);

    // And a tooltip: set through the table, shown when the pointer rests on the widget.
    CY_REQUIRE_EQ(table().ui_widget_set_tooltip(&fixture.host, music, "The music bus"),
                  CY_RESULT_OK);
    // A stale handle — the slider's index in a later generation — is no widget of the module's.
    const CyUiElement stale = music + (1ULL << 40U);
    CY_CHECK_EQ(table().ui_widget_set_tooltip(&fixture.host, stale, "x"), CY_RESULT_NOT_FOUND);
}

CY_TEST_CASE("ui toolkit: clearing the focus takes it from a widget too") {
    // Regression: `ui_set_focus(NULL)` cleared only the 1.6 elements' focus. A widget keeping the
    // toolkit's focus kept the input bridge in menu mode, so a game going back to gameplay from a
    // menu never saw its own actions again.
    Fixture fixture;
    CY_REQUIRE(fixture.started);
    constexpr const char* kPlayMenu = R"(cyui 1
column #menu
  button #play text="Play"
)";
    CyUiElement root = CY_UI_ELEMENT_NULL;
    CY_REQUIRE_EQ(fixture.load(kPlayMenu, root), CY_RESULT_OK);
    CY_REQUIRE(fixture.frame());
    const CyUiElement play = fixture.find("play");
    CY_REQUIRE_EQ(table().ui_set_focus(&fixture.host, play), CY_RESULT_OK);
    CY_REQUIRE(fixture.toolkit.focused().is_valid());
    CY_REQUIRE_EQ(table().ui_set_focus(&fixture.host, CY_UI_ELEMENT_NULL), CY_RESULT_OK);
    CY_CHECK_FALSE(fixture.toolkit.focused().is_valid());
    CY_CHECK_FALSE(fixture.toolkit.focus_visible());
}
