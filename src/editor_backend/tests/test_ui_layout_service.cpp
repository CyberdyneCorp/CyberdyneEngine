// SPDX-License-Identifier: MIT
// The UI layout editor, engine side (#29, "UI layout"): the widget vocabulary, and a `.cyui`
// document made into the toolkit's widgets and drawn on the host, as the editor's canvas shows it.
//
// THE CANVAS IS HELD TO THE TOOLKIT, NOT TO ITSELF. Every rectangle the preview reports is compared
// with the same document made into widgets by a toolkit this suite starts on its own, and every
// colour it reads is one the built-in theme or the request's own sheet says that widget has in that
// state — so a preview that drew a second layout, or put the widget into the wrong state, is red.
//
// THE WIRE, FROM BOTH SIDES. `data/ui_preview_request_v1.wire` is the request the Rust editor's
// encoder writes for `kGallery` at 640x360; this suite decodes those bytes and draws them.
// `data/ui_catalogue_v1.wire` is the engine's vocabulary, which the Rust suites decode and replay
// as the runtime's. Regenerate both with `CY_UPDATE_UI_LAYOUT_WIRE=1` after a deliberate change.

#include <cy/abi/cy_abi.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/editor/ui_layout_service.h>
#include <cy/servers/text/server.h>
#include <cy/test/test.h>
#include <cy/ui/text/builtin_font.h>
#include <cy/ui/text/text_painter.h>
#include <cy/ui/toolkit/document_host.h>
#include <cy/ui/toolkit/toolkit.h>
#if defined(CY_EDITOR_HAS_UI_FONT)
#    include <cy/backends/text/complete_backend.h>
#    include <cy/ui/text/interface_font.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace cy;
using editor::UiPreviewRequest;
using editor::UiPreviewResult;
using editor::UiPreviewState;

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

/// Eight lines a designer would place: a panel, a heading, three controls and a row of buttons.
constexpr std::string_view kGallery = R"(cyui 1
panel #menu .panel anchor=[0.5, 0.5, 0.5, 0.5] offset=[-150, -120, 150, 120]
  column #body gap=8 anchor=[0, 0, 1, 1] offset=[10, 10, -10, -10]
    label #title text="Options"
    slider #volume min=0 max=10 step=1 value=4
    dropdown #quality items=["Low", "Medium", "High"] selected=1
    row #buttons gap=6
      button #back text="Back"
      button #play .primary text="Play"
)";

constexpr u32 kWidth = 640;
constexpr u32 kHeight = 360;
constexpr u32 kBackLine = 8;
constexpr u32 kQualityLine = 6;

UiPreviewRequest request_for(std::string_view document) noexcept {
    UiPreviewRequest request;
    request.document = document;
    request.width = kWidth;
    request.height = kHeight;
    request.scale.mode = ui::ScaleMode::FixedPixel;
    request.scale.dpi_scale = 1.0F;
    request.image_max_width = kWidth;
    request.image_max_height = kHeight;
    return request;
}

/// The preview's own toolkit, started once for the suite: the font is the slow part.
editor::UiLayoutPreview& preview() {
    static editor::UiLayoutPreview instance(allocator());
    return instance;
}

UiPreviewResult render(const UiPreviewRequest& request) {
    UiPreviewResult result(allocator());
    const Status drawn = preview().render(request, result);
    CY_REQUIRE(drawn.has_value());
    if (const char* dump = std::getenv("CY_UI_LAYOUT_DUMP"); dump != nullptr) {
        static int counter = 0;
        const std::string path =
            std::string(dump) + "/preview" + std::to_string(counter++) + ".ppm";
        std::ofstream out(path, std::ios::binary);
        out << "P6\n" << result.image_width << " " << result.image_height << "\n255\n";
        for (const u32 texel : result.pixels) {
            const char rgb[3] = {static_cast<char>(texel & 0xFFU),
                                 static_cast<char>((texel >> 8U) & 0xFFU),
                                 static_cast<char>((texel >> 16U) & 0xFFU)};
            out.write(rgb, 3);
        }
        for (const editor::UiPreviewNode& node : result.nodes) {
            std::fprintf(stderr, "%s line %u made %d rect %g %g %g %g\n", path.c_str(), node.line,
                         node.made ? 1 : 0, static_cast<double>(node.rect.x),
                         static_cast<double>(node.rect.y), static_cast<double>(node.rect.width),
                         static_cast<double>(node.rect.height));
        }
    }
    return result;
}

/// The line's node; a line the preview did not report fails the case and reads as nothing made.
const editor::UiPreviewNode& node_at(const UiPreviewResult& result, u32 line) {
    static const editor::UiPreviewNode kMissing{};
    for (const editor::UiPreviewNode& node : result.nodes) {
        if (node.line == line) {
            return node;
        }
    }
    CY_CHECK_MESSAGE(false, "the preview reported no node for the line");
    return kMissing;
}

/// Every diagnostic, one a line: what a failing case prints.
std::string diagnostics_of(const UiPreviewResult& result) {
    std::string out;
    for (const editor::UiPreviewDiagnostic& diagnostic : result.diagnostics) {
        out += std::to_string(diagnostic.line) + ":" + std::to_string(diagnostic.column) + " " +
               diagnostic.message + " (" + std::string(diagnostic.detail.text()) + ")\n";
    }
    return out;
}

/// The image's texel under a point given in reference units.
u32 texel(const UiPreviewResult& result, f32 x, f32 y) {
    const f32 per_unit = result.scale * result.image_factor;
    const auto px = static_cast<u32>(x * per_unit);
    const auto py = static_cast<u32>(y * per_unit);
    CY_REQUIRE(px < result.image_width);
    CY_REQUIRE(py < result.image_height);
    return result.pixels[(static_cast<usize>(py) * result.image_width) + px];
}

/// A point inside a widget's rectangle, clear of its caption, its border and its rounded corners:
/// half way down, in the theme's ten units of padding before the caption.
u32 body_texel(const UiPreviewResult& result, u32 line) {
    const editor::UiPreviewNode& node = node_at(result, line);
    CY_REQUIRE(node.made);
    return texel(result, node.rect.x + 5.0F, node.rect.y + (node.rect.height * 0.5F));
}

/// An RGBA8 texel from a `#rrggbb` the theme writes.
constexpr u32 rgba(u32 rrggbb) noexcept {
    return 0xFF000000U | ((rrggbb & 0xFFU) << 16U) | (rrggbb & 0xFF00U) | ((rrggbb >> 16U) & 0xFFU);
}

u32 channel_delta(u32 a, u32 b) noexcept {
    u32 worst = 0;
    for (u32 shift = 0; shift < 32; shift += 8) {
        const auto ca = static_cast<i32>((a >> shift) & 0xFFU);
        const auto cb = static_cast<i32>((b >> shift) & 0xFFU);
        worst = std::max(worst, static_cast<u32>(std::abs(ca - cb)));
    }
    return worst;
}

std::filesystem::path wire(std::string_view name) {
    return std::filesystem::path(CY_UI_LAYOUT_WIRE_DIR) / name;
}

bool updating_wire() noexcept {
    const char* value = std::getenv("CY_UPDATE_UI_LAYOUT_WIRE");
    return value != nullptr && value[0] == '1';
}

std::string read_file(std::string_view name) {
    std::ifstream input(wire(name), std::ios::binary);
    CY_REQUIRE(input.good());
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

void write_file(std::string_view name, const Array<u8>& bytes) {
    std::ofstream output(wire(name), std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    CY_REQUIRE(output.good());
}

/// A front-to-back reader over a reply, failing the case on a short read.
class Reply {
public:
    explicit Reply(Span<const u8> bytes) : bytes_(bytes) {}

    u8 u8v() { return take(1)[0]; }
    u32 u32v() {
        u32 value = 0;
        std::memcpy(&value, take(4), 4);
        return value;
    }
    std::string text() {
        const u32 size = u32v();
        return {reinterpret_cast<const char*>(take(size)), size};
    }
    [[nodiscard]] bool done() const { return at_ == bytes_.size(); }

private:
    const u8* take(usize count) {
        CY_REQUIRE(bytes_.size() - at_ >= count);
        const u8* out = bytes_.data() + at_;
        at_ += count;
        return out;
    }

    Span<const u8> bytes_;
    usize at_ = 0;
};

/// One request through the service's own session, as the runtime's bridge submits it.
struct Session {
    explicit Session(editor::UiLayoutService& backend) : service(&backend) {
        CY_REQUIRE(backend.open(&handle) == CY_RESULT_OK);
    }
    ~Session() { service->close(handle); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    /// Submit, poll once, and keep the terminal event's kind and payload.
    u32 ask(const char* operation, Span<const u8> payload) {
        CyServiceRequest request{};
        request.struct_size = sizeof(CyServiceRequest);
        request.request_id = ++next;
        request.schema_version = 1;
        request.operation = operation;
        request.payload = payload.data();
        request.payload_size = payload.size();
        CY_REQUIRE(service->submit(handle, request) == CY_RESULT_OK);
        CyServiceEvent event{};
        bool has = false;
        CY_REQUIRE(service->poll(handle, event, has) == CY_RESULT_OK);
        CY_REQUIRE(has);
        CY_REQUIRE_EQ(event.request_id, request.request_id);
        reply.assign(event.payload, event.payload + event.payload_size);
        return event.kind;
    }

    editor::UiLayoutService* service;
    CyServiceSession handle = nullptr;
    std::vector<u8> reply;
    u64 next = 0;
};

}  // namespace

CY_TEST_CASE("ui layout: the catalogue is the toolkit's vocabulary, with each kind's properties") {
    Array<u8> bytes(allocator());
    CY_REQUIRE(editor::encode_ui_catalogue(bytes).has_value());
    if (updating_wire()) {
        write_file("ui_catalogue_v1.wire", bytes);
    }
    Reply reply(bytes.span());
    CY_REQUIRE_EQ(reply.u32v(), editor::kUiLayoutFormat);
    const u32 kinds = reply.u32v();
    CY_CHECK_EQ(kinds, 20U);
    bool slider_has_range = false;
    bool panel_is_container = false;
    for (u32 kind = 0; kind < kinds; ++kind) {
        const std::string name = reply.text();
        const u8 flags = reply.u8v();
        // Every name the catalogue offers is one the toolkit makes.
        CY_CHECK(ui::widget_kind_from_name(name) != ui::WidgetKind::Count);
        const u32 properties = reply.u32v();
        std::vector<std::string> keys;
        for (u32 property = 0; property < properties; ++property) {
            keys.push_back(reply.text());
            CY_CHECK(reply.u8v() < static_cast<u8>(editor::UiPropertyType::Count));
        }
        const auto has = [&keys](std::string_view key) {
            return std::find(keys.begin(), keys.end(), key) != keys.end();
        };
        // Every kind can be anchored, offset, hidden and given a tooltip.
        const bool placed = has("anchor") && has("offset") && has("visible") && has("tooltip");
        CY_CHECK(placed);
        if (name == "slider") {
            slider_has_range = has("value") && has("min") && has("max") && has("step") &&
                               has("bind") && (flags & editor::kUiKindFocusable) != 0;
        }
        if (name == "panel") {
            panel_is_container = (flags & editor::kUiKindContainer) != 0 && !has("bind");
        }
    }
    CY_CHECK(slider_has_range);
    CY_CHECK(panel_is_container);
    const u32 styles = reply.u32v();
    CY_CHECK_EQ(styles, static_cast<u32>(ui::StyleProperty::Count));
    for (u32 style = 0; style < styles; ++style) {
        CY_CHECK(ui::style_property_from_name(reply.text()) != ui::StyleProperty::Count);
    }
    const u32 states = reply.u32v();
    CY_REQUIRE_EQ(states, 5U);
    CY_CHECK_EQ(reply.text(), std::string("hover"));
    CY_CHECK_EQ(reply.text(), std::string("pressed"));
    CY_CHECK_EQ(reply.text(), std::string("focused"));
    CY_CHECK_EQ(reply.text(), std::string("disabled"));
    CY_CHECK_EQ(reply.text(), std::string("open"));
    CY_CHECK_EQ(reply.u32v(), 2U);
    CY_CHECK_EQ(reply.text(), std::string("one-way"));
    CY_CHECK_EQ(reply.text(), std::string("two-way"));
    CY_CHECK(reply.done());
}

CY_TEST_CASE("ui layout: every line's rectangle is the toolkit's own layout of the document") {
    const UiPreviewResult result = render(request_for(kGallery));
    CY_CHECK_MESSAGE(result.diagnostics.empty(), diagnostics_of(result));
    CY_REQUIRE_EQ(result.nodes.size(), 8U);
    CY_CHECK_EQ(result.image_width, kWidth);
    CY_CHECK_EQ(result.image_height, kHeight);
    CY_CHECK_EQ(result.scale, 1.0F);

    // The same document, made into widgets by a toolkit this case starts itself.
    // In the font the preview draws in: the interface font with the complete text backend.
#if defined(CY_EDITOR_HAS_UI_FONT)
    cy::text::CompleteTextBackend backend;
#endif
    ui::ElementStore store(allocator());
    cy::text::TextServer server;
    ui::TextPainter text(allocator());
    ui::Toolkit toolkit(allocator(), store, text);
#if defined(CY_EDITOR_HAS_UI_FONT)
    CY_REQUIRE(backend.start().has_value());
    CY_REQUIRE(server.start_with(cy::text::TextServerConfig{}, backend).has_value());
    CY_REQUIRE(text.start(server, cy::text::FontSource{ui::interface_font_bytes(), 0},
                          ui::interface_font_desc(), 1)
                   .has_value());
#else
    CY_REQUIRE(server.start(cy::text::TextServerConfig{}).has_value());
    CY_REQUIRE(text.start(server, ui::builtin_font(), 1).has_value());
#endif
    CY_REQUIRE(toolkit.start().has_value());
    ui::DocumentInstance instance(allocator(), toolkit);
    CY_REQUIRE(instance.load(kGallery, ui::kNoElement, nullptr, nullptr).has_value());
    ui::ScaleSettings scale;
    scale.mode = ui::ScaleMode::FixedPixel;
    CY_REQUIRE(toolkit.update(1.0F / 60.0F, scale, Vec2{640.0F, 360.0F}).has_value());
    const auto rect = [&](std::string_view id) {
        return store.layout_output(instance.find(id))->rect;
    };
    // The panel is centred, 300 by 240, wherever the font's widths fall.
    const editor::UiPreviewNode& menu = node_at(result, 2);
    CY_CHECK_EQ(menu.rect, rect("menu"));
    CY_CHECK_EQ(menu.rect, (ui::Rect{170.0F, 60.0F, 300.0F, 240.0F}));
    // The column fills it inside the offsets; its children's positions depend on the font, so they
    // are held to the toolkit's — exactly, since the preview IS the toolkit — and to the order.
    // Inside the panel's padding (the theme's `.panel`: 10) and its own offsets.
    CY_CHECK_EQ(node_at(result, 3).rect, (ui::Rect{190.0F, 80.0F, 260.0F, 200.0F}));
    const char* ids[] = {"title", "volume", "quality", "buttons", "back", "play"};
    for (u32 line = 4; line <= 9; ++line) {
        CY_CHECK(node_at(result, line).made);
        CY_CHECK_EQ(node_at(result, line).rect.width, rect(ids[line - 4]).width);
    }
    CY_CHECK(node_at(result, 5).rect.y > node_at(result, 4).rect.y);
    CY_CHECK(node_at(result, 9).rect.x > node_at(result, 8).rect.x);
    CY_CHECK_EQ(result.widgets, 8U);
    CY_CHECK(result.primitives > 0U);
    CY_CHECK(result.draws > 0U);
}

CY_TEST_CASE("ui layout: the canvas is drawn in the theme's colours, and a theme replaces them") {
    UiPreviewRequest request = request_for(kGallery);
    const UiPreviewResult themed = render(request);
    // Outside the panel, the canvas; inside it, the theme's surface; on the button, its raised.
    CY_CHECK_EQ(texel(themed, 5.0F, 5.0F), 0xFF262626U);
    CY_CHECK_LE(channel_delta(texel(themed, 175.0F, 150.0F), rgba(0x1b1f27)), 1U);
    CY_CHECK_LE(channel_delta(body_texel(themed, kBackLine), rgba(0x2b3240)), 1U);
    // The primary button is the accent.
    CY_CHECK_LE(channel_delta(body_texel(themed, kBackLine + 1), rgba(0x3d8bfd)), 1U);

    // A theme of the author's: switching it recolours the button and nothing else is needed.
    request.theme = "button { background-color: #00ff00; } .panel { background-color: #000080; }";
    const UiPreviewResult green = render(request);
    // No padding and no corner radius under this theme: the button's own top-left texel.
    const ui::Rect bare = node_at(green, kBackLine).rect;
    CY_CHECK_LE(channel_delta(texel(green, bare.x + 0.5F, bare.y + 0.5F), rgba(0x00ff00)), 1U);
    CY_CHECK_LE(channel_delta(texel(green, 175.0F, 150.0F), rgba(0x000080)), 1U);
    // A theme is the whole base: without the built-in theme's padding the button is its caption.
    CY_CHECK(node_at(green, kBackLine).rect.height < node_at(themed, kBackLine).rect.height);
}

CY_TEST_CASE("ui layout: a state previewed is the state the toolkit draws for it") {
    UiPreviewRequest request = request_for(kGallery);
    const u32 rest = body_texel(render(request), kBackLine);
    request.state_line = kBackLine;
    request.state = UiPreviewState::Hover;
    CY_CHECK_LE(channel_delta(body_texel(render(request), kBackLine), rgba(0x36405a)), 1U);
    request.state = UiPreviewState::Pressed;
    CY_CHECK_LE(channel_delta(body_texel(render(request), kBackLine), rgba(0x22334f)), 1U);
    request.state = UiPreviewState::Disabled;
    const u32 disabled = body_texel(render(request), kBackLine);
    CY_CHECK(channel_delta(disabled, rest) > 4U);
    // Focused by the keyboard: the ring is drawn round the button, where nothing was before.
    request.state = UiPreviewState::Focused;
    const UiPreviewResult focused = render(request);
    const ui::Rect back = node_at(focused, kBackLine).rect;
    const u32 ring = texel(focused, back.x + (back.width * 0.5F), back.y - 1.5F);
    CY_CHECK_LE(channel_delta(ring, rgba(0xffd34d)), 2U);
    request.state = UiPreviewState::None;
    CY_CHECK(channel_delta(texel(render(request), back.x + (back.width * 0.5F), back.y - 1.5F),
                           rgba(0xffd34d)) > 40U);
    // A drop-down opened: its list is drawn below it, over what was there.
    request.state_line = kQualityLine;
    request.state = UiPreviewState::Open;
    const UiPreviewResult open = render(request);
    request.state = UiPreviewState::None;
    const UiPreviewResult closed = render(request);
    const ui::Rect quality = node_at(open, kQualityLine).rect;
    const f32 below = quality.y + quality.height + 30.0F;
    CY_CHECK(channel_delta(texel(open, quality.x + 20.0F, below),
                           texel(closed, quality.x + 20.0F, below)) > 4U);
    // And the state is the preview's alone: the next preview of the same text starts at rest.
    CY_CHECK_EQ(body_texel(closed, kBackLine), rest);
}

CY_TEST_CASE("ui layout: resolution, aspect and DPI scale the document as a game's scale does") {
    UiPreviewRequest request = request_for(kGallery);
    request.scale.mode = ui::ScaleMode::ScaleWithSmaller;
    request.scale.reference = Vec2{640.0F, 360.0F};
    const UiPreviewResult reference = render(request);
    request.width = 1280;
    request.height = 720;
    request.image_max_width = 1280;
    request.image_max_height = 720;
    const UiPreviewResult doubled = render(request);
    CY_CHECK_EQ(doubled.scale, 2.0F);
    CY_CHECK_EQ(doubled.image_width, 1280U);
    // Reference units: the same rectangles at twice the pixels.
    CY_CHECK_EQ(node_at(doubled, 2).rect, node_at(reference, 2).rect);
    // A wider aspect letterboxes on the smaller axis: the panel stays centred, its size the same.
    request.width = 1280;
    request.height = 360;
    const UiPreviewResult wide = render(request);
    CY_CHECK_EQ(wide.scale, 1.0F);
    CY_CHECK_EQ(node_at(wide, 2).rect.width, 300.0F);
    CY_CHECK_EQ(node_at(wide, 2).rect.x, (1280.0F - 300.0F) * 0.5F);
    // The platform's DPI multiplies the scale.
    request = request_for(kGallery);
    request.scale.dpi_scale = 2.0F;
    request.width = 1280;
    request.height = 720;
    request.image_max_width = 1280;
    request.image_max_height = 720;
    const UiPreviewResult dpi = render(request);
    CY_CHECK_EQ(dpi.scale, 2.0F);
    CY_CHECK_EQ(node_at(dpi, 2).rect, node_at(reference, 2).rect);
}

CY_TEST_CASE("ui layout: a target larger than the image limit is drawn at a fraction") {
    UiPreviewRequest request = request_for(kGallery);
    request.scale.mode = ui::ScaleMode::ScaleWithSmaller;
    request.scale.reference = Vec2{640.0F, 360.0F};
    request.width = 3840;
    request.height = 2160;
    request.image_max_width = 960;
    request.image_max_height = 960;
    const UiPreviewResult result = render(request);
    CY_CHECK_EQ(result.image_width, 960U);
    CY_CHECK_EQ(result.image_height, 540U);
    CY_CHECK_EQ(result.image_factor, 0.25F);
    CY_CHECK_EQ(result.scale, 6.0F);
    CY_CHECK_EQ(result.pixels.size(), static_cast<usize>(960U) * 540U);
    // The button still reads its colour where the layout put it.
    CY_CHECK_LE(channel_delta(body_texel(result, kBackLine), rgba(0x2b3240)), 1U);
}

CY_TEST_CASE("ui layout: a document that does not parse is named, and the last good one is drawn") {
    const UiPreviewResult good = render(request_for(kGallery));
    constexpr std::string_view kBroken = R"(cyui 1
panel #menu .panel
  sliderr #volume
  label text="unterminated
)";
    const UiPreviewResult broken = render(request_for(kBroken));
    CY_REQUIRE(!broken.diagnostics.empty());
    bool named_the_kind = false;
    for (const editor::UiPreviewDiagnostic& diagnostic : broken.diagnostics) {
        named_the_kind = named_the_kind || diagnostic.detail.text() == "sliderr";
    }
    CY_CHECK(named_the_kind);
    // Byte for byte the last document that loaded, so the canvas does not blank while typing.
    CY_REQUIRE_EQ(broken.pixels.size(), good.pixels.size());
    CY_CHECK(std::memcmp(broken.pixels.data(), good.pixels.data(), good.pixels.size() * 4) == 0);
    CY_CHECK_EQ(broken.nodes.size(), good.nodes.size());
}

CY_TEST_CASE("ui layout: a component and a sheet the editor sends are used and styled") {
    constexpr std::string_view kMenu = R"(cyui 1
stylesheet "ui/menu.cyss"
column #menu gap=4 anchor=[0, 0, 0, 0] offset=[20, 20, 220, 200]
  use "ui/stat.cyui" #health label="Health"
  button #go .big text="Go"
)";
    const editor::UiPreviewFile files[] = {
        {"ui/stat.cyui", "cyui 1\nparam label \"Stat\"\nrow .stat gap=4\n  label text=$label\n"},
        {"ui/menu.cyss",
         ".big { background-color: #ff0000; min-height: 40; }\n.stat { height: 30; }"}};
    UiPreviewRequest request = request_for(kMenu);
    request.files = Span<const editor::UiPreviewFile>(files, 2);
    const UiPreviewResult result = render(request);
    CY_CHECK_MESSAGE(result.diagnostics.empty(), diagnostics_of(result));
    CY_REQUIRE(node_at(result, 4).made);  // the `use` line: the component's row
    CY_CHECK_EQ(node_at(result, 4).rect.height, 30.0F);
    CY_CHECK_EQ(node_at(result, 5).rect.height, 40.0F);
    CY_CHECK_LE(channel_delta(body_texel(result, 5), rgba(0xff0000)), 1U);
    // The classes the inspector offers: the theme's and the sheet's.
    bool big = false;
    bool primary = false;
    for (const Name name : result.classes) {
        big = big || name.text() == "big";
        primary = primary || name.text() == "primary";
    }
    CY_CHECK(big);
    CY_CHECK(primary);
    // Without the files, the `use` cannot be read, and says so.
    request.files = {};
    const UiPreviewResult missing = render(request);
    CY_CHECK(!missing.diagnostics.empty());
}

CY_TEST_CASE("ui layout: the service answers over a session, and the editor's bytes decode") {
    editor::UiLayoutService service(allocator());
    Session session(service);
    CY_CHECK_EQ(session.ask("capabilities.get", {}), static_cast<u32>(CY_SERVICE_EVENT_COMPLETED));
    {
        Reply reply(Span<const u8>(session.reply.data(), session.reply.size()));
        CY_CHECK_EQ(reply.u32v(), 1U);
        CY_REQUIRE_EQ(reply.u32v(), 3U);
        CY_CHECK_EQ(reply.text(), std::string("capabilities.get"));
        CY_CHECK_EQ(reply.text(), std::string("ui.catalogue.get"));
        CY_CHECK_EQ(reply.text(), std::string("ui.preview.render"));
    }
    CY_CHECK_EQ(session.ask("ui.catalogue.get", {}), static_cast<u32>(CY_SERVICE_EVENT_COMPLETED));

    // The request the Rust editor writes for the gallery at 640x360.
    Array<u8> encoded(allocator());
    CY_REQUIRE(editor::encode_ui_preview_request(request_for(kGallery), encoded).has_value());
    if (updating_wire()) {
        write_file("ui_preview_request_v1.wire", encoded);
    }
    const std::string committed = read_file("ui_preview_request_v1.wire");
    CY_CHECK_EQ(committed.size(), encoded.size());
    CY_CHECK(std::memcmp(committed.data(), encoded.data(), encoded.size()) == 0);
    CY_CHECK_EQ(session.ask("ui.preview.render",
                            Span<const u8>(reinterpret_cast<const u8*>(committed.data()),
                                           committed.size())),
                static_cast<u32>(CY_SERVICE_EVENT_COMPLETED));
    Reply reply(Span<const u8>(session.reply.data(), session.reply.size()));
    CY_CHECK_EQ(reply.u32v(), 1U);
    CY_CHECK_EQ(reply.u32v(), kWidth);
    CY_CHECK_EQ(reply.u32v(), kHeight);

    // A request cut short is refused by name, and changes nothing.
    const u8 short_payload[] = {1, 0, 0};
    CY_CHECK_EQ(session.ask("ui.preview.render", Span<const u8>(short_payload, 3)),
                static_cast<u32>(CY_SERVICE_EVENT_FAILED));
    Reply refusal(Span<const u8>(session.reply.data(), session.reply.size()));
    CY_CHECK_EQ(refusal.u32v(), 1U);
    CY_CHECK_EQ(refusal.text(), std::string("ui.request.malformed"));
    CY_CHECK_EQ(session.ask("ui.unknown", {}), static_cast<u32>(CY_SERVICE_EVENT_FAILED));
}

CY_TEST_CASE("ui layout: a target, scale or state out of range is refused before it is drawn") {
    Array<editor::UiPreviewFile> files(allocator());
    const auto refused = [&files](UiPreviewRequest request) {
        Array<u8> bytes(allocator());
        CY_REQUIRE(editor::encode_ui_preview_request(request, bytes).has_value());
        UiPreviewRequest decoded;
        return !editor::decode_ui_preview_request(bytes.span(), decoded, files).has_value();
    };
    CY_CHECK(!refused(request_for(kGallery)));
    UiPreviewRequest request = request_for(kGallery);
    request.width = 0;
    CY_CHECK(refused(request));
    request = request_for(kGallery);
    request.height = 9000;
    CY_CHECK(refused(request));
    request = request_for(kGallery);
    request.scale.dpi_scale = 0.0F;
    CY_CHECK(refused(request));
    request = request_for(kGallery);
    request.scale.dpi_scale = std::nanf("");
    CY_CHECK(refused(request));
    request = request_for(kGallery);
    request.state = UiPreviewState::Count;
    CY_CHECK(refused(request));
    request = request_for(kGallery);
    request.image_max_width = 0;
    CY_CHECK(refused(request));
}
