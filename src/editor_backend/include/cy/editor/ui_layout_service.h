// SPDX-License-Identifier: MIT
#pragma once
// The engine side of the editor's UI layout editor: `ui.catalogue.get` and `ui.preview.render`.
// Issue #29 ("UI layout"), over issue #91's toolkit.
//
// THE CANVAS IS THE ENGINE'S. The editor holds a `.cyui` document as text and sends it here; the
// engine makes it into widgets with the toolkit a game runs (`DocumentInstance`), lays it out at
// the resolution and DPI the author previews, puts one widget into the state the author previews
// (hovered, pressed, focused, disabled, open), flattens it and draws the stream with the interface
// renderer's host path — `ui::render::draw_reference`, the shader the device runs, which
// `render.ui` holds the device to within one step of. The editor draws its handles over that image
// from the rectangles the engine's layout gave each line, so what the author drags is what a game
// lays out, never a second layout written in Rust.
//
// REQUESTS (schema 1), little-endian; a text is a u32 byte count and UTF-8:
//
//   ui.catalogue.get     ()
//   ui.preview.render    u32 format = 1,
//                        text document (`cyui 1`), u32 file count, per file (text path, text
//                        bytes) — the sheets and documents it reaches, read by the editor from
//                        the project — text theme (`.cyss`; empty is the built-in theme),
//                        u32 width, u32 height (the target's pixels), f32 dpi scale,
//                        u8 scale mode (`ui::ScaleMode`), f32 reference width, f32 reference
//                        height, f32 match, u32 image max width, u32 image max height,
//                        u32 state line (0: none), u8 state (`UiPreviewState`)
//
// REPLIES:
//
//   catalogue  u32 format = 1,
//              u32 kinds, per kind (text name, u8 flags `kUiKind*`, u32 properties, per property
//              (text key, u8 `UiPropertyType`)),
//              u32 style properties, per property text name,
//              u32 states, per state text name (the `UiPreviewState` names, in order, from 1),
//              u32 binding modes, per mode text name
//   preview    u32 format = 1,
//              u32 image width, u32 image height, f32 scale (pixels per reference unit at the
//              target), f32 image factor (image pixels per target pixel, at most 1),
//              u32 byte count, then that many bytes: the image, RGBA8, top row first,
//              u32 nodes, per node (u32 line, u8 made, f32 x, f32 y, f32 width, f32 height — the
//              rectangle in reference units),
//              u32 diagnostics, per diagnostic (u32 line, u32 column, text message, text detail),
//              u32 classes, per class text name — every class the theme and the sheets select,
//              u32 widgets, u32 primitives, u32 draws, f64 layout ms, f64 draw ms
//
// A preview that does not parse still answers: the diagnostics name the line, and the image is the
// last document that did — so the canvas does not blank while an author types.

#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/ui/layout.h>

#include <string_view>

namespace cy::editor {

/// The request and reply format the `ui.*` operations speak.
inline constexpr u32 kUiLayoutFormat = 1;
/// The `capabilities.get` feature bit the UI layout service sets.
inline constexpr u64 kUiLayoutFeature = 0x400U;

/// A widget kind's catalogue flag: it holds children, so the palette can drop into it.
inline constexpr u8 kUiKindContainer = 1U;
/// A widget kind's catalogue flag: it takes focus, so it can be previewed focused.
inline constexpr u8 kUiKindFocusable = 2U;

/// What a property's value is, so an inspector offers the right control.
enum class UiPropertyType : u8 {
    Number = 0,
    Text,
    Word,
    Bool,
    /// A list of texts: `items`.
    TextList,
    /// A list of numbers: `anchor`, `offset`, `uv`, `size`.
    NumberList,
    Count,
};

/// The state a preview puts one widget into.
enum class UiPreviewState : u8 {
    None = 0,
    Hover,
    Pressed,
    Focused,
    Disabled,
    /// A drop-down's list or a dialog, opened.
    Open,
    Count,
};

[[nodiscard]] const char* ui_preview_state_name(UiPreviewState state) noexcept;

/// A file a previewed document reaches, as the editor read it from the project.
struct UiPreviewFile {
    std::string_view path;
    std::string_view bytes;
};

/// One `ui.preview.render` request, decoded. The views borrow the payload.
struct UiPreviewRequest {
    std::string_view document;
    Span<const UiPreviewFile> files;
    std::string_view theme;
    u32 width = 1920;
    u32 height = 1080;
    ui::ScaleSettings scale;
    /// The image is drawn no larger than this; a larger target is drawn at a fraction.
    u32 image_max_width = 1920;
    u32 image_max_height = 1080;
    /// The line of the widget put into `state`; zero for none.
    u32 state_line = 0;
    UiPreviewState state = UiPreviewState::None;
};

/// One line of the document, where the engine laid out the widget it made.
struct UiPreviewNode {
    u32 line = 0;
    bool made = false;
    ui::Rect rect;
};

/// A problem the document's reader or style sheets reported, at its line and column.
struct UiPreviewDiagnostic {
    u32 line = 0;
    u32 column = 0;
    const char* message = "";
    Name detail;
};

/// What `UiLayoutPreview::render` produced.
struct UiPreviewResult {
    explicit UiPreviewResult(Allocator& allocator) noexcept
        : pixels(allocator), nodes(allocator), diagnostics(allocator), classes(allocator) {}

    u32 image_width = 0;
    u32 image_height = 0;
    /// Pixels per reference unit at the target resolution.
    f32 scale = 1.0F;
    /// Image pixels per target pixel: one unless the target was larger than the image limit.
    f32 image_factor = 1.0F;
    /// RGBA8, red in the low byte, top row first.
    Array<u32> pixels;
    Array<UiPreviewNode> nodes;
    Array<UiPreviewDiagnostic> diagnostics;
    Array<Name> classes;
    u32 widgets = 0;
    u32 primitives = 0;
    u32 draws = 0;
    f64 layout_ms = 0.0;
    f64 draw_ms = 0.0;
};

/// Decode a `ui.preview.render` payload. `files` receives the file table; the request's `files`
/// points into it.
[[nodiscard]] Status decode_ui_preview_request(Span<const u8> payload, UiPreviewRequest& out,
                                               Array<UiPreviewFile>& files) noexcept;
/// Encode a `ui.preview.render` request — for the suites, and for a host that previews itself.
[[nodiscard]] Status encode_ui_preview_request(const UiPreviewRequest& request,
                                               Array<u8>& out) noexcept;
/// Encode a `ui.preview.render` reply: the picture, the nodes, the diagnostics and the counts.
[[nodiscard]] Status encode_ui_preview(const UiPreviewResult& result, Array<u8>& out) noexcept;
/// Encode the `ui.catalogue.get` reply: the toolkit's kinds, style properties, states and modes.
[[nodiscard]] Status encode_ui_catalogue(Array<u8>& out) noexcept;

/// A document made into widgets and drawn, on the host, as the device draws it.
class UiLayoutPreview {
public:
    explicit UiLayoutPreview(Allocator& allocator) noexcept;
    ~UiLayoutPreview();

    UiLayoutPreview(const UiLayoutPreview&) = delete;
    UiLayoutPreview& operator=(const UiLayoutPreview&) = delete;
    UiLayoutPreview(UiLayoutPreview&&) = delete;
    UiLayoutPreview& operator=(UiLayoutPreview&&) = delete;

    /// The text server, the interface font (the built-in font without `CY_TEXT`) and the toolkit.
    /// Called by the first `render`; idempotent.
    [[nodiscard]] Status start() noexcept;

    /// Make `request.document` into widgets — from scratch, so a preview never carries the state a
    /// previous one left — and draw it.
    [[nodiscard]] Status render(const UiPreviewRequest& request, UiPreviewResult& out) noexcept;

private:
    struct State;
    /// Flatten what `render` laid out and draw it at the image's size.
    [[nodiscard]] Status draw(const UiPreviewRequest& request, UiPreviewResult& out) noexcept;

    Allocator* allocator_;
    State* state_ = nullptr;
};

/// Engine-owned UI layout backend: one pending request per session, answered on the next poll
/// with exactly one terminal event. Needs no host seam: the preview is drawn here.
class UiLayoutService final : public abi::EditorServiceBackend {
public:
    explicit UiLayoutService(Allocator& allocator) noexcept : allocator_(&allocator) {}

    [[nodiscard]] CyResult open(CyServiceSession* out_session) noexcept override;
    void close(CyServiceSession session) noexcept override;
    [[nodiscard]] CyResult submit(CyServiceSession session,
                                  const CyServiceRequest& request) noexcept override;
    [[nodiscard]] CyResult cancel(CyServiceSession session, u64 request_id) noexcept override;
    [[nodiscard]] CyResult poll(CyServiceSession session, CyServiceEvent& out_event,
                                bool& out_has_event) noexcept override;

private:
    Allocator* allocator_;
};

}  // namespace cy::editor
