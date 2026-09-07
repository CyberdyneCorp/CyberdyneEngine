#include <cy/rendering/temporal/history.h>

namespace cy::rendering {

const char* history_format_name(HistoryFormat format) noexcept {
    switch (format) {
        case HistoryFormat::Rgba16F:
            return "Rgba16F";
        case HistoryFormat::Rg16F:
            return "Rg16F";
        case HistoryFormat::R16F:
            return "R16F";
        case HistoryFormat::Rgba8:
            return "Rgba8";
        case HistoryFormat::R32F:
            return "R32F";
        case HistoryFormat::Count:
            break;
    }
    return "Unknown";
}

u32 history_bytes_per_texel(HistoryFormat format) noexcept {
    switch (format) {
        case HistoryFormat::Rgba16F:
            return 8;
        // Four bytes for three different reasons — two 16-bit channels, four 8-bit ones, one 32-bit
        // one — which is why they share a label rather than each repeating the number.
        case HistoryFormat::Rg16F:
        case HistoryFormat::Rgba8:
        case HistoryFormat::R32F:
            return 4;
        case HistoryFormat::R16F:
            return 2;
        case HistoryFormat::Count:
            break;
    }
    return 0;
}

u64 HistoryResource::bytes() const noexcept {
    const u64 texels = static_cast<u64>(width) * static_cast<u64>(height);
    return texels * history_bytes_per_texel(declaration.format) *
           static_cast<u64>(declaration.frames);
}

}  // namespace cy::rendering
