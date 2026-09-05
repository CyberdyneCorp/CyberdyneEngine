// The audio format vocabulary. See cy/servers/audio/format.h.

#include <cy/servers/audio/format.h>

namespace cy::audio {

const char* channel_layout_name(ChannelLayout layout) noexcept {
    switch (layout) {
        case ChannelLayout::Mono:
            return "mono";
        case ChannelLayout::Stereo:
            return "stereo";
        case ChannelLayout::Count:
            break;
    }
    return "unknown";
}

u32 channel_count(ChannelLayout layout) noexcept {
    switch (layout) {
        case ChannelLayout::Mono:
            return 1;
        case ChannelLayout::Stereo:
            return 2;
        case ChannelLayout::Count:
            break;
    }
    return 0;
}

}  // namespace cy::audio
