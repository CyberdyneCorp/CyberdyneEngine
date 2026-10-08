// SPDX-License-Identifier: MIT
#pragma once
// A text painter's atlas pages, on the device. Issue #86.
//
// `TextPainter` draws from up to three pages — coverage, a distance field and colour — numbered
// from the page it was started on (cy/ui/text/text_painter.h). This uploads each that exists in the
// format the shader samples it as: coverage one byte a texel (`R8Unorm`, read as red), the other
// two four
// (`Rgba8Unorm`). Like `UiRenderer::upload_atlas`, it runs outside a frame.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/ui/render/ui_renderer.h>
#include <cy/ui/text/text_painter.h>

namespace cy::ui::render {

/// Upload every page `text` has, at the page numbers its primitives carry. A page with no glyph on
/// it yet is skipped; upload again when `text.atlas_revision()` changes.
[[nodiscard]] Status upload_text_atlases(UiRenderer& renderer, const TextPainter& text) noexcept;

}  // namespace cy::ui::render
