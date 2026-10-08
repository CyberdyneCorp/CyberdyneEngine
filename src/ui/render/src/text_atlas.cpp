// SPDX-License-Identifier: MIT
#include <cy/ui/render/text_atlas.h>

namespace cy::ui::render {

Status upload_text_atlases(UiRenderer& renderer, const TextPainter& text) noexcept {
    for (u32 index = 0; index < cy::text::kPixelFormatCount; ++index) {
        const auto format = static_cast<cy::text::PixelFormat>(index);
        const u32 extent = text.atlas_extent(format);
        if (extent == 0) {
            continue;
        }
        const rhi::Format device_format = format == cy::text::PixelFormat::Coverage
                                              ? rhi::Format::R8Unorm
                                              : rhi::Format::Rgba8Unorm;
        if (Status uploaded = renderer.upload_atlas(text.atlas_page(format), device_format, extent,
                                                    extent, text.atlas_pixels(format));
            !uploaded) {
            return uploaded;
        }
    }
    return ok();
}

}  // namespace cy::ui::render
