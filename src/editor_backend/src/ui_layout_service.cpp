// SPDX-License-Identifier: MIT
// `ui.catalogue.get` and `ui.preview.render`. See cy/editor/ui_layout_service.h.

#include <cy/editor/ui_layout_service.h>

#include <cy/servers/text/server.h>
#include <cy/ui/paint.h>
#include <cy/ui/render/encode.h>
#include <cy/ui/style.h>
#include <cy/ui/text/builtin_font.h>
#include <cy/ui/text/text_painter.h>
#include <cy/ui/toolkit/document.h>
#include <cy/ui/toolkit/document_host.h>
#include <cy/ui/toolkit/toolkit.h>

#if defined(CY_EDITOR_HAS_UI_FONT)
#    include <cy/backends/text/complete_backend.h>
#    include <cy/ui/text/interface_font.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <new>
#include <string_view>

#include "service_wire.h"

namespace cy::editor {
namespace {

constexpr std::string_view kCatalogue = "ui.catalogue.get";
constexpr std::string_view kPreview = "ui.preview.render";
constexpr std::string_view kCapabilities = "capabilities.get";
constexpr usize kOperationCapacity = 96;
/// The largest target a preview lays out, per axis: past 8K is a typing mistake, not a display.
constexpr u32 kMaxTarget = 8192;
/// The most files one preview may carry: the sheets and components a document reaches.
constexpr u32 kMaxFiles = 64;
/// What the canvas shows under the document: a neutral grey, so a transparent panel reads as one.
constexpr u32 kCanvasBackground = 0xFF262626U;
/// The atlas page the interface's text starts at; image pages below and above it read a checker.
constexpr u16 kTextPage = 1;
constexpr u32 kAtlasPages = 16;

using Clock = std::chrono::steady_clock;

[[nodiscard]] f64 milliseconds_since(Clock::time_point start) noexcept {
    return std::chrono::duration<f64, std::milli>(Clock::now() - start).count();
}

// --- The catalogue -------------------------------------------------------------------------------

struct PropertyEntry {
    std::string_view key;
    UiPropertyType type;
};

constexpr PropertyEntry kCommon[] = {{"enabled", UiPropertyType::Bool},
                                     {"visible", UiPropertyType::Bool},
                                     {"tooltip", UiPropertyType::Text},
                                     {"anchor", UiPropertyType::NumberList},
                                     {"offset", UiPropertyType::NumberList}};
constexpr PropertyEntry kCaption[] = {{"text", UiPropertyType::Text}};
constexpr PropertyEntry kRange[] = {{"value", UiPropertyType::Number},
                                    {"min", UiPropertyType::Number},
                                    {"max", UiPropertyType::Number},
                                    {"step", UiPropertyType::Number}};
constexpr PropertyEntry kChoice[] = {{"items", UiPropertyType::TextList},
                                     {"selected", UiPropertyType::Number}};
constexpr PropertyEntry kBinding[] = {{"bind", UiPropertyType::Word},
                                      {"bind-mode", UiPropertyType::Word}};

/// The properties a kind takes beyond the common ones, in the order an inspector lists them.
struct KindEntry {
    ui::WidgetKind kind;
    u8 flags;
    Span<const PropertyEntry> own;
    Span<const PropertyEntry> extra;
};

constexpr PropertyEntry kGrid[] = {{"columns", UiPropertyType::Number}};
constexpr PropertyEntry kImage[] = {{"page", UiPropertyType::Number},
                                    {"uv", UiPropertyType::NumberList},
                                    {"size", UiPropertyType::NumberList}};
constexpr PropertyEntry kToggle[] = {{"text", UiPropertyType::Text},
                                     {"checked", UiPropertyType::Bool}};
constexpr PropertyEntry kField[] = {{"text", UiPropertyType::Text},
                                    {"placeholder", UiPropertyType::Text},
                                    {"max-length", UiPropertyType::Number},
                                    {"mask", UiPropertyType::Word}};
constexpr PropertyEntry kList[] = {{"items", UiPropertyType::TextList},
                                   {"selected", UiPropertyType::Number},
                                   {"row-height", UiPropertyType::Number}};
constexpr PropertyEntry kPage[] = {{"title", UiPropertyType::Text}};
constexpr PropertyEntry kTabs[] = {{"selected", UiPropertyType::Number}};

constexpr u8 kContainer = kUiKindContainer;
constexpr u8 kFocusable = kUiKindFocusable;

/// Every kind an author places; the parts the toolkit makes for them (radio, tab, row, option)
/// are not placed by hand and are left out.
constexpr KindEntry kKinds[] = {
    {ui::WidgetKind::Panel, kContainer, {}, kPage},
    {ui::WidgetKind::Row, kContainer, {}, kPage},
    {ui::WidgetKind::Column, kContainer, {}, kPage},
    {ui::WidgetKind::Grid, kContainer, kGrid, kPage},
    {ui::WidgetKind::Stack, kContainer, {}, kPage},
    {ui::WidgetKind::ScrollView, kContainer, {}, kPage},
    {ui::WidgetKind::Label, 0, kCaption, kBinding},
    {ui::WidgetKind::RichText, 0, kCaption, kBinding},
    {ui::WidgetKind::Image, 0, kImage, {}},
    {ui::WidgetKind::Button, kFocusable, kCaption, kBinding},
    {ui::WidgetKind::Toggle, kFocusable, kToggle, kBinding},
    {ui::WidgetKind::Checkbox, kFocusable, kToggle, kBinding},
    {ui::WidgetKind::RadioGroup, kFocusable, kChoice, kBinding},
    {ui::WidgetKind::Slider, kFocusable, kRange, kBinding},
    {ui::WidgetKind::ProgressBar, 0, kRange, kBinding},
    {ui::WidgetKind::TextField, kFocusable, kField, kBinding},
    {ui::WidgetKind::Dropdown, kFocusable, kChoice, kBinding},
    {ui::WidgetKind::ListView, kFocusable, kList, kBinding},
    {ui::WidgetKind::Tabs, kContainer | kFocusable, kTabs, kBinding},
    {ui::WidgetKind::Dialog, kContainer, kPage, {}},
};

void write_properties(wire::Writer& out, Span<const PropertyEntry> entries) noexcept {
    for (const PropertyEntry& entry : entries) {
        out.text(entry.key).u8v(static_cast<u8>(entry.type));
    }
}

constexpr std::string_view kBindingModes[] = {"one-way", "two-way"};

// --- Reading a preview request -------------------------------------------------------------------

[[nodiscard]] bool valid_scale(const ui::ScaleSettings& scale) noexcept {
    return std::isfinite(scale.dpi_scale) && scale.dpi_scale > 0.0F && scale.dpi_scale <= 8.0F &&
           std::isfinite(scale.reference.x) && std::isfinite(scale.reference.y) &&
           scale.reference.x >= 1.0F && scale.reference.y >= 1.0F && std::isfinite(scale.match) &&
           scale.match >= 0.0F && scale.match <= 1.0F &&
           static_cast<u8>(scale.mode) <= static_cast<u8>(ui::ScaleMode::Match);
}

[[nodiscard]] bool valid_request(const UiPreviewRequest& request) noexcept {
    return request.width > 0 && request.height > 0 && request.width <= kMaxTarget &&
           request.height <= kMaxTarget && request.image_max_width > 0 &&
           request.image_max_height > 0 && request.image_max_width <= kMaxTarget &&
           request.image_max_height <= kMaxTarget && request.state < UiPreviewState::Count &&
           valid_scale(request.scale);
}

// --- Drawing
// ---------------------------------------------------------------------------------------

/// Reads `use` and `stylesheet` paths from the files the request carried.
class RequestFiles final : public ui::DocumentResolver {
public:
    explicit RequestFiles(Span<const UiPreviewFile> files) noexcept : files_(files) {}

    [[nodiscard]] Status read(std::string_view path, Array<char>& out) noexcept override {
        for (const UiPreviewFile& file : files_) {
            if (file.path == path) {
                return out.append(Span<const char>(file.bytes.data(), file.bytes.size()));
            }
        }
        return fail(ErrorCode::NotFound, "ui preview: the editor did not send this file");
    }

private:
    Span<const UiPreviewFile> files_;
};

/// A 4x4 checker for an image whose page the preview does not have: visibly a placeholder.
constexpr u8 kChecker[64] = {200, 200, 200, 255, 90,  90,  90,  255, 200, 200, 200, 255, 90,
                             90,  90,  255, 90,  90,  90,  255, 200, 200, 200, 255, 90,  90,
                             90,  255, 200, 200, 200, 255, 200, 200, 200, 255, 90,  90,  90,
                             255, 200, 200, 200, 255, 90,  90,  90,  255, 90,  90,  90,  255,
                             200, 200, 200, 255, 90,  90,  90,  255, 200, 200, 200, 255};

}  // namespace

const char* ui_preview_state_name(UiPreviewState state) noexcept {
    switch (state) {
        case UiPreviewState::None:
            return "none";
        case UiPreviewState::Hover:
            return "hover";
        case UiPreviewState::Pressed:
            return "pressed";
        case UiPreviewState::Focused:
            return "focused";
        case UiPreviewState::Disabled:
            return "disabled";
        case UiPreviewState::Open:
            return "open";
        case UiPreviewState::Count:
            break;
    }
    return "unknown";
}

Status encode_ui_catalogue(Array<u8>& out) noexcept {
    wire::Writer writer(out);
    writer.u32v(kUiLayoutFormat).u32v(static_cast<u32>(std::size(kKinds)));
    for (const KindEntry& entry : kKinds) {
        const auto count =
            static_cast<u32>(entry.own.size() + entry.extra.size() + std::size(kCommon));
        writer.text(ui::widget_kind_name(entry.kind)).u8v(entry.flags).u32v(count);
        write_properties(writer, entry.own);
        write_properties(writer, entry.extra);
        write_properties(writer, Span<const PropertyEntry>(kCommon, std::size(kCommon)));
    }
    const auto styles = static_cast<u32>(ui::StyleProperty::Count);
    writer.u32v(styles);
    for (u32 property = 0; property < styles; ++property) {
        writer.text(ui::style_property_name(static_cast<ui::StyleProperty>(property)));
    }
    const auto states = static_cast<u32>(UiPreviewState::Count) - 1U;
    writer.u32v(states);
    for (u32 state = 1; state <= states; ++state) {
        writer.text(ui_preview_state_name(static_cast<UiPreviewState>(state)));
    }
    writer.u32v(static_cast<u32>(std::size(kBindingModes)));
    for (const std::string_view mode : kBindingModes) {
        writer.text(mode);
    }
    return writer.status();
}

Status decode_ui_preview_request(Span<const u8> payload, UiPreviewRequest& out,
                                 Array<UiPreviewFile>& files) noexcept {
    wire::Reader in(payload);
    files.clear();
    const u32 format = in.read_u32();
    out.document = in.read_text();
    const u32 count = in.read_u32();
    if (in.failed() || format != kUiLayoutFormat || count > kMaxFiles) {
        return fail(ErrorCode::InvalidArgument,
                    "a preview is u32 1, the document and at most 64 files");
    }
    for (u32 index = 0; index < count; ++index) {
        UiPreviewFile file;
        file.path = in.read_text();
        file.bytes = in.read_text();
        if (Status pushed = files.push_back(file); !pushed) {
            return pushed;
        }
    }
    out.files = files.span();
    out.theme = in.read_text();
    out.width = in.read_u32();
    out.height = in.read_u32();
    out.scale.dpi_scale = in.read_f32();
    out.scale.mode = static_cast<ui::ScaleMode>(in.read_u8());
    out.scale.reference.x = in.read_f32();
    out.scale.reference.y = in.read_f32();
    out.scale.match = in.read_f32();
    out.image_max_width = in.read_u32();
    out.image_max_height = in.read_u32();
    out.state_line = in.read_u32();
    out.state = static_cast<UiPreviewState>(in.read_u8());
    if (!in.complete() || !valid_request(out)) {
        return fail(ErrorCode::InvalidArgument,
                    "a preview's target is 1 to 8192 pixels a side, its DPI scale in (0, 8], its "
                    "scale mode and state known, and nothing follows the state");
    }
    return ok();
}

Status encode_ui_preview_request(const UiPreviewRequest& request, Array<u8>& out) noexcept {
    wire::Writer writer(out);
    writer.u32v(kUiLayoutFormat)
        .text(request.document)
        .u32v(static_cast<u32>(request.files.size()));
    for (const UiPreviewFile& file : request.files) {
        writer.text(file.path).text(file.bytes);
    }
    writer.text(request.theme)
        .u32v(request.width)
        .u32v(request.height)
        .f32v(request.scale.dpi_scale)
        .u8v(static_cast<u8>(request.scale.mode))
        .f32v(request.scale.reference.x)
        .f32v(request.scale.reference.y)
        .f32v(request.scale.match)
        .u32v(request.image_max_width)
        .u32v(request.image_max_height)
        .u32v(request.state_line)
        .u8v(static_cast<u8>(request.state));
    return writer.status();
}

Status encode_ui_preview(const UiPreviewResult& result, Array<u8>& out) noexcept {
    wire::Writer writer(out);
    writer.u32v(kUiLayoutFormat)
        .u32v(result.image_width)
        .u32v(result.image_height)
        .f32v(result.scale)
        .f32v(result.image_factor)
        .u32v(static_cast<u32>(result.pixels.size() * sizeof(u32)));
    writer.bytes(Span<const u8>(reinterpret_cast<const u8*>(result.pixels.data()),
                                result.pixels.size() * sizeof(u32)));
    writer.u32v(static_cast<u32>(result.nodes.size()));
    for (const UiPreviewNode& node : result.nodes) {
        writer.u32v(node.line)
            .u8v(node.made ? 1U : 0U)
            .f32v(node.rect.x)
            .f32v(node.rect.y)
            .f32v(node.rect.width)
            .f32v(node.rect.height);
    }
    writer.u32v(static_cast<u32>(result.diagnostics.size()));
    for (const UiPreviewDiagnostic& diagnostic : result.diagnostics) {
        writer.u32v(diagnostic.line)
            .u32v(diagnostic.column)
            .text(diagnostic.message)
            .text(diagnostic.detail.text());
    }
    writer.u32v(static_cast<u32>(result.classes.size()));
    for (const Name name : result.classes) {
        writer.text(name.text());
    }
    writer.u32v(result.widgets)
        .u32v(result.primitives)
        .u32v(result.draws)
        .f64v(result.layout_ms)
        .f64v(result.draw_ms);
    return writer.status();
}

// --- The preview ---------------------------------------------------------------------------------

struct UiLayoutPreview::State {
    explicit State(Allocator& allocator) noexcept
        : store(allocator),
          text(allocator),
          toolkit(allocator, store, text),
          instance(allocator, toolkit),
          buffer(allocator),
          draws(allocator),
          good_document(allocator),
          good_files(allocator),
          good_paths(allocator) {}

#if defined(CY_EDITOR_HAS_UI_FONT)
    cy::text::CompleteTextBackend backend;
#endif
    cy::text::TextServer server;
    ui::ElementStore store;
    ui::TextPainter text;
    ui::Toolkit toolkit;
    ui::DocumentInstance instance;
    ui::PrimitiveBuffer buffer;
    ui::render::UiDrawList draws;
    /// The last document that loaded, and its files, drawn again while the author's text does not
    /// parse.
    Array<char> good_document;
    Array<char> good_files;
    Array<UiPreviewFile> good_paths;
    bool started = false;
};

UiLayoutPreview::UiLayoutPreview(Allocator& allocator) noexcept : allocator_(&allocator) {}

UiLayoutPreview::~UiLayoutPreview() {
    if (state_ != nullptr) {
        state_->~State();
        allocator_->deallocate(state_, sizeof(State), alignof(State));
    }
}

Status UiLayoutPreview::start() noexcept {
    if (state_ != nullptr && state_->started) {
        return ok();
    }
    if (state_ == nullptr) {
        void* memory = allocator_->allocate(sizeof(State), alignof(State));
        if (memory == nullptr) {
            return fail(ErrorCode::OutOfMemory, "ui preview: no memory for its toolkit");
        }
        state_ = new (memory) State(*allocator_);
    }
    State& state = *state_;
#if defined(CY_EDITOR_HAS_UI_FONT)
    cy::text::TextServerConfig config;
    config.atlas.initial_extent = 512;
    if (Status backend = state.backend.start(); !backend) {
        return backend;
    }
    if (Status server = state.server.start_with(config, state.backend); !server) {
        return server;
    }
    if (Status text =
            state.text.start(state.server, cy::text::FontSource{ui::interface_font_bytes(), 0},
                             ui::interface_font_desc(), kTextPage);
        !text) {
        return text;
    }
#else
    if (Status server = state.server.start(cy::text::TextServerConfig{}); !server) {
        return server;
    }
    if (Status text = state.text.start(state.server, ui::builtin_font(), kTextPage); !text) {
        return text;
    }
#endif
    if (Status toolkit = state.toolkit.start(); !toolkit) {
        return toolkit;
    }
    state.started = true;
    return ok();
}

namespace {

/// Copy the request's document and files, the ones the next unparsable edit falls back to.
[[nodiscard]] Status keep_good(const UiPreviewRequest& request, Array<char>& document,
                               Array<char>& pool, Array<UiPreviewFile>& files) noexcept {
    document.clear();
    pool.clear();
    files.clear();
    if (Status copied =
            document.append(Span<const char>(request.document.data(), request.document.size()));
        !copied) {
        return copied;
    }
    usize total = 0;
    for (const UiPreviewFile& file : request.files) {
        total += file.path.size() + file.bytes.size();
    }
    // Reserved first, so the views below stay where they were written.
    if (Status reserved = pool.reserve(total); !reserved) {
        return reserved;
    }
    for (const UiPreviewFile& file : request.files) {
        const usize path_at = pool.size();
        (void)pool.append(Span<const char>(file.path.data(), file.path.size()));
        const usize bytes_at = pool.size();
        (void)pool.append(Span<const char>(file.bytes.data(), file.bytes.size()));
        UiPreviewFile kept;
        kept.path = std::string_view(pool.data() + path_at, file.path.size());
        kept.bytes = std::string_view(pool.data() + bytes_at, file.bytes.size());
        if (Status pushed = files.push_back(kept); !pushed) {
            return pushed;
        }
    }
    return ok();
}

[[nodiscard]] ui::Rect rect_of(const ui::ElementStore& store, ui::ElementId element) noexcept {
    const ui::LayoutOutput* output = store.layout_output(element);
    return output != nullptr ? output->rect : ui::Rect{};
}

[[nodiscard]] Vec2 centre_of(const ui::Rect& rect) noexcept {
    return Vec2{rect.x + (rect.width * 0.5F), rect.y + (rect.height * 0.5F)};
}

/// Put one widget into the previewed state, as a player would: the pointer over it, pressed on
/// it, focus moved to it by the keyboard, or the game disabling or opening it.
[[nodiscard]] Status apply_state(ui::Toolkit& toolkit, const ui::ElementStore& store,
                                 ui::WidgetId widget, UiPreviewState state) noexcept {
    const Vec2 centre = centre_of(rect_of(store, widget));
    switch (state) {
        case UiPreviewState::Hover:
            (void)toolkit.pointer(ui::PointerInput{centre, false, {}, 0});
            return ok();
        case UiPreviewState::Pressed:
            (void)toolkit.pointer(ui::PointerInput{centre, false, {}, 0});
            (void)toolkit.pointer(ui::PointerInput{centre, true, {}, 0});
            return ok();
        case UiPreviewState::Focused:
            return toolkit.focus(widget);
        case UiPreviewState::Disabled:
            return toolkit.set_enabled(widget, false);
        case UiPreviewState::Open:
            return toolkit.open(widget);
        case UiPreviewState::None:
        case UiPreviewState::Count:
            break;
    }
    return ok();
}

/// Every class a rule of `sheet` selects, appended once each.
[[nodiscard]] Status collect_classes(const ui::StyleSheet& sheet, Array<Name>& out) noexcept {
    for (const ui::StyleRule& rule : sheet.rules()) {
        for (u32 part = 0; part < rule.part_count; ++part) {
            for (u32 index = 0; index < rule.parts[part].class_count; ++index) {
                const Name name = rule.parts[part].classes[index];
                if (std::find(out.begin(), out.end(), name) != out.end()) {
                    continue;
                }
                if (Status pushed = out.push_back(name); !pushed) {
                    return pushed;
                }
            }
        }
    }
    return ok();
}

[[nodiscard]] Status note(Array<UiPreviewDiagnostic>& out, u32 line, u32 column,
                          const char* message, Name detail) noexcept {
    UiPreviewDiagnostic diagnostic;
    diagnostic.line = line;
    diagnostic.column = column;
    diagnostic.message = message;
    diagnostic.detail = detail;
    return out.push_back(diagnostic);
}

/// The document's own diagnostics, then the sheets' and the theme's, each named as such.
[[nodiscard]] Status collect_diagnostics(const ui::DocumentInstance& instance,
                                         const ui::Toolkit& toolkit,
                                         Array<UiPreviewDiagnostic>& out) noexcept {
    for (const ui::DocumentDiagnostic& diagnostic : instance.diagnostics()) {
        if (Status kept = note(out, diagnostic.line, diagnostic.column, diagnostic.message,
                               diagnostic.detail);
            !kept) {
            return kept;
        }
    }
    for (const ui::StyleDiagnostic& diagnostic : toolkit.stylesheet().diagnostics()) {
        if (Status kept =
                note(out, 0, diagnostic.column, diagnostic.message, Name::intern("sheet"));
            !kept) {
            return kept;
        }
    }
    for (const ui::StyleDiagnostic& diagnostic : toolkit.theme_sheet().diagnostics()) {
        if (Status kept =
                note(out, 0, diagnostic.column, diagnostic.message, Name::intern("theme"));
            !kept) {
            return kept;
        }
    }
    return ok();
}

/// The rectangle each element line of the document was laid out at.
[[nodiscard]] Status collect_nodes(Allocator& allocator, const ui::ElementStore& store,
                                   const ui::DocumentInstance& instance, std::string_view document,
                                   Array<UiPreviewNode>& out) noexcept {
    ui::Document parsed(allocator);
    (void)parse_document(document, parsed);
    for (const ui::DocumentNode& node : parsed.nodes()) {
        if (node.is_comment()) {
            continue;
        }
        UiPreviewNode entry;
        entry.line = node.line;
        const ui::WidgetId widget = instance.widget_at_line(node.line);
        entry.made = widget.is_valid();
        if (entry.made) {
            entry.rect = rect_of(store, widget);
        }
        if (Status pushed = out.push_back(entry); !pushed) {
            return pushed;
        }
    }
    return ok();
}

/// The text pages the painter has, and a checker everywhere an image could ask for one.
void reference_atlases(const ui::TextPainter& text,
                       ui::render::ReferenceAtlas (&atlases)[kAtlasPages]) noexcept {
    for (ui::render::ReferenceAtlas& atlas : atlases) {
        atlas.texels = Span<const u8>(kChecker, sizeof(kChecker));
        atlas.width = 4;
        atlas.height = 4;
        atlas.channels = 4;
    }
    for (u32 format = 0; format < 3; ++format) {
        const auto pixel_format = static_cast<cy::text::PixelFormat>(format);
        const u32 extent = text.atlas_extent(pixel_format);
        const u16 page = text.atlas_page(pixel_format);
        if (page >= kAtlasPages) {
            continue;
        }
        ui::render::ReferenceAtlas& atlas = atlases[page];
        atlas.texels = text.atlas_pixels(pixel_format);
        atlas.width = extent;
        atlas.height = extent;
        atlas.channels = format == 0 ? 1U : 4U;
    }
}

}  // namespace

Status UiLayoutPreview::render(const UiPreviewRequest& request, UiPreviewResult& out) noexcept {
    if (Status started = start(); !started) {
        return started;
    }
    State& state = *state_;
    const Clock::time_point began = Clock::now();
    out.diagnostics.clear();
    out.nodes.clear();
    out.classes.clear();
    if (Status themed = state.toolkit.set_theme(request.theme); !themed) {
        return themed;
    }
    // From scratch: no focus, no hover and no player's value carried over from the last preview.
    if (Status unloaded = state.instance.unload(); !unloaded) {
        return unloaded;
    }
    (void)state.toolkit.pointer(ui::PointerInput{Vec2{-1.0e6F, -1.0e6F}, false, {}, 0});
    RequestFiles files(request.files);
    std::string_view drawn = request.document;
    if (state.instance.load(request.document, ui::kNoElement, &files, nullptr)) {
        if (Status kept =
                keep_good(request, state.good_document, state.good_files, state.good_paths);
            !kept) {
            return kept;
        }
    } else {
        // Named, then the last document that loaded drawn in its place.
        if (Status noted = collect_diagnostics(state.instance, state.toolkit, out.diagnostics);
            !noted) {
            return noted;
        }
        drawn = std::string_view(state.good_document.data(), state.good_document.size());
        RequestFiles good(state.good_paths.span());
        if (!drawn.empty()) {
            (void)state.instance.load(drawn, ui::kNoElement, &good, nullptr);
        }
    }
    const Vec2 target{static_cast<f32>(request.width), static_cast<f32>(request.height)};
    if (Status laid = state.toolkit.update(1.0F / 60.0F, request.scale, target); !laid) {
        return laid;
    }
    const ui::WidgetId stated = state.instance.widget_at_line(request.state_line);
    if (stated.is_valid() && drawn == request.document) {
        if (Status applied = apply_state(state.toolkit, state.store, stated, request.state);
            !applied) {
            return applied;
        }
    }
    // Long enough for every transition and the tooltip's delay to settle: the state, not a frame
    // of the way to it. A state change restyles in the first update and starts its transition
    // there, so the second is the one that runs it to its end.
    for (const f32 seconds : {5.0F, 5.0F, 1.0F / 60.0F}) {
        if (Status settled = state.toolkit.update(seconds, request.scale, target); !settled) {
            return settled;
        }
    }
    if (out.diagnostics.empty()) {
        if (Status noted = collect_diagnostics(state.instance, state.toolkit, out.diagnostics);
            !noted) {
            return noted;
        }
    }
    if (Status nodes = collect_nodes(*allocator_, state.store, state.instance, drawn, out.nodes);
        !nodes) {
        return nodes;
    }
    if (Status classes = collect_classes(state.toolkit.theme_sheet(), out.classes); !classes) {
        return classes;
    }
    if (Status classes = collect_classes(state.toolkit.stylesheet(), out.classes); !classes) {
        return classes;
    }
    out.scale = ui::resolve_scale(request.scale, target);
    out.layout_ms = milliseconds_since(began);
    return draw(request, out);
}

Status UiLayoutPreview::draw(const UiPreviewRequest& request, UiPreviewResult& out) noexcept {
    State& state = *state_;
    const Clock::time_point began = Clock::now();
    const f32 factor =
        std::min({1.0F, static_cast<f32>(request.image_max_width) / static_cast<f32>(request.width),
                  static_cast<f32>(request.image_max_height) / static_cast<f32>(request.height)});
    out.image_factor = factor;
    out.image_width = std::max(1U, static_cast<u32>(std::lround(request.width * factor)));
    out.image_height = std::max(1U, static_cast<u32>(std::lround(request.height * factor)));
    const ui::Rect viewport{0.0F, 0.0F, static_cast<f32>(request.width) / out.scale,
                            static_cast<f32>(request.height) / out.scale};
    ui::FlattenReport flattened;
    if (Status flat = ui::flatten(state.store, viewport, state.buffer, flattened, &state.text);
        !flat) {
        return flat;
    }
    state.draws.clear();
    if (Status built = ui::render::build_draws(state.buffer, out.scale * factor, out.image_width,
                                               out.image_height, state.draws);
        !built) {
        return built;
    }
    ui::render::ReferenceAtlas atlases[kAtlasPages];
    reference_atlases(state.text, atlases);
    out.pixels.clear();
    if (Status sized = out.pixels.resize(static_cast<usize>(out.image_width) * out.image_height);
        !sized) {
        return sized;
    }
    std::fill(out.pixels.begin(), out.pixels.end(), kCanvasBackground);
    if (Status drawn = ui::render::draw_reference(
            state.draws, Span<const ui::render::ReferenceAtlas>(atlases, kAtlasPages),
            out.image_width, out.image_height, out.pixels.span());
        !drawn) {
        return drawn;
    }
    out.widgets = state.toolkit.widget_count();
    out.primitives = static_cast<u32>(state.buffer.primitives().size());
    out.draws = static_cast<u32>(state.draws.draws.size() + state.draws.group_draws.size());
    out.draw_ms = milliseconds_since(began);
    return ok();
}

// --- The service ---------------------------------------------------------------------------------

namespace {

struct UiLayoutSession {
    explicit UiLayoutSession(Allocator& allocator) noexcept
        : request_payload(allocator),
          event_payload(allocator),
          files(allocator),
          preview(allocator),
          result(allocator) {}

    char operation[kOperationCapacity] = {};
    Array<u8> request_payload;
    Array<u8> event_payload;
    Array<UiPreviewFile> files;
    UiLayoutPreview preview;
    UiPreviewResult result;
    u64 request = 0;
    u32 schema = 0;
    /// A second request while one is pending, answered `ui.busy` on the next poll.
    u64 busy_request = 0;
    bool pending = false;
    bool cancelled = false;
};

[[nodiscard]] UiLayoutSession* session_of(CyServiceSession session) noexcept {
    return reinterpret_cast<UiLayoutSession*>(session);
}

void begin_event(CyServiceEvent& event, u64 request, u32 kind) noexcept {
    event = {};
    event.struct_size = sizeof(CyServiceEvent);
    event.request_id = request;
    event.schema_version = 1;
    event.kind = kind;
}

[[nodiscard]] CyResult written(const Status& status) noexcept {
    return status ? CY_RESULT_OK : CY_RESULT_OUT_OF_MEMORY;
}

[[nodiscard]] CyResult failed(UiLayoutSession& session, bool& failed_event, std::string_view code,
                              std::string_view detail) noexcept {
    failed_event = true;
    return written(wire::encode_failure(session.event_payload, code, detail));
}

[[nodiscard]] CyResult capabilities(UiLayoutSession& session) noexcept {
    wire::Writer out(session.event_payload);
    out.u32v(1).u32v(3).text(kCapabilities).text(kCatalogue).text(kPreview).u64v(kUiLayoutFeature);
    return written(out.status());
}

[[nodiscard]] CyResult preview(UiLayoutSession& session, bool& failed_event) noexcept {
    UiPreviewRequest request;
    if (Status decoded =
            decode_ui_preview_request(session.request_payload.span(), request, session.files);
        !decoded) {
        return failed(session, failed_event, "ui.request.malformed", decoded.error().message);
    }
    if (Status rendered = session.preview.render(request, session.result); !rendered) {
        return failed(session, failed_event, "ui.preview.failed", rendered.error().message);
    }
    return written(encode_ui_preview(session.result, session.event_payload));
}

[[nodiscard]] CyResult answer(UiLayoutSession& session, bool& failed_event) noexcept {
    const std::string_view operation(session.operation);
    if (session.schema != 1) {
        return failed(session, failed_event, "ui.schema.unsupported",
                      "this operation supports schema 1");
    }
    if (operation == kCapabilities) {
        return capabilities(session);
    }
    if (operation == kCatalogue) {
        return written(encode_ui_catalogue(session.event_payload));
    }
    if (operation == kPreview) {
        return preview(session, failed_event);
    }
    return failed(session, failed_event, "operation-unsupported",
                  "this backend does not support the operation");
}

}  // namespace

CyResult UiLayoutService::open(CyServiceSession* out_session) noexcept {
    void* memory = allocator_->allocate(sizeof(UiLayoutSession), alignof(UiLayoutSession));
    if (memory == nullptr) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    *out_session = reinterpret_cast<CyServiceSession>(new (memory) UiLayoutSession(*allocator_));
    return CY_RESULT_OK;
}

void UiLayoutService::close(CyServiceSession session) noexcept {
    if (session == nullptr) {
        return;
    }
    UiLayoutSession* state = session_of(session);
    state->~UiLayoutSession();
    allocator_->deallocate(state, sizeof(UiLayoutSession), alignof(UiLayoutSession));
}

CyResult UiLayoutService::submit(CyServiceSession session,
                                 const CyServiceRequest& request) noexcept {
    UiLayoutSession* state = session_of(session);
    if (request.request_id == 0 || request.operation == nullptr) {
        return CY_RESULT_INVALID_ARGUMENT;
    }
    if (state->pending) {
        if (state->busy_request != 0 || request.request_id == state->request) {
            return CY_RESULT_ALREADY_EXISTS;
        }
        state->busy_request = request.request_id;
        return CY_RESULT_OK;
    }
    const usize size = std::strlen(request.operation);
    if (size >= sizeof(state->operation)) {
        return CY_RESULT_INVALID_ARGUMENT;
    }
    std::memcpy(state->operation, request.operation, size + 1);
    state->request_payload.clear();
    if (Status copied = state->request_payload.append({request.payload, request.payload_size});
        !copied) {
        return CY_RESULT_OUT_OF_MEMORY;
    }
    state->request = request.request_id;
    state->schema = request.schema_version;
    state->pending = true;
    state->cancelled = false;
    return CY_RESULT_OK;
}

CyResult UiLayoutService::cancel(CyServiceSession session, u64 request_id) noexcept {
    UiLayoutSession* state = session_of(session);
    if (!state->pending || state->request != request_id) {
        return CY_RESULT_NOT_FOUND;
    }
    state->cancelled = true;
    return CY_RESULT_OK;
}

CyResult UiLayoutService::poll(CyServiceSession session, CyServiceEvent& out_event,
                               bool& out_has_event) noexcept {
    UiLayoutSession* state = session_of(session);
    out_has_event = false;
    if (state->busy_request != 0) {
        begin_event(out_event, state->busy_request, CY_SERVICE_EVENT_FAILED);
        state->busy_request = 0;
        if (!wire::encode_failure(state->event_payload, "ui.busy",
                                  "another UI layout request is pending in this session")) {
            return CY_RESULT_OUT_OF_MEMORY;
        }
        out_event.payload = state->event_payload.data();
        out_event.payload_size = state->event_payload.size();
        out_has_event = true;
        return CY_RESULT_OK;
    }
    if (!state->pending) {
        return CY_RESULT_OK;
    }
    begin_event(out_event, state->request, CY_SERVICE_EVENT_COMPLETED);
    state->event_payload.clear();
    bool failed_event = false;
    if (state->cancelled) {
        out_event.kind = CY_SERVICE_EVENT_CANCELLED;
    } else if (const CyResult result = answer(*state, failed_event); result != CY_RESULT_OK) {
        return result;
    }
    if (failed_event) {
        out_event.kind = CY_SERVICE_EVENT_FAILED;
    }
    out_event.payload = state->event_payload.data();
    out_event.payload_size = state->event_payload.size();
    out_has_event = true;
    state->pending = false;
    return CY_RESULT_OK;
}

}  // namespace cy::editor
