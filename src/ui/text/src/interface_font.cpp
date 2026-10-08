// SPDX-License-Identifier: MIT
#include <cy/ui/text/interface_font.h>

#include <cy_features.h>

#include <cstring>

#if defined(CY_TEXT)
namespace cy::ui::embedded {
// cmake/embed.cmake writes the definitions from deps/fonts/NotoSans-Latin-VF.ttf.
extern const unsigned char interface_font[];
extern const unsigned long long interface_font_size;
}  // namespace cy::ui::embedded
#endif

namespace cy::ui {

bool has_interface_font() noexcept {
#if defined(CY_TEXT)
    return true;
#else
    return false;
#endif
}

Span<const u8> interface_font_bytes() noexcept {
#if defined(CY_TEXT)
    return {embedded::interface_font, static_cast<usize>(embedded::interface_font_size)};
#else
    return {};
#endif
}

cy::text::FontDesc interface_font_desc() noexcept {
    cy::text::FontDesc desc;
    desc.family = "Noto Sans";
    desc.size_pixels = 32.0F;
    desc.mode = cy::text::RenderMode::SignedDistanceField;
    desc.distance_range = 4.0F;
    std::memcpy(desc.axes[0].tag, "wght", 4);
    desc.axes[0].value = 400.0F;
    desc.axis_count = 1;
    return desc;
}

}  // namespace cy::ui
