#include <cy/core/platform/display_support.h>

#include <cy/core/base/diagnostic_sink.h>

namespace cy {
namespace {

// The flags a request can carry, in declaration order, so that filter_unsupported_flags() walks
// them without knowing how many there are.
constexpr WindowFlags kAllFlags[] = {
    WindowFlags::Resizable,        WindowFlags::Borderless, WindowFlags::AlwaysOnTop,
    WindowFlags::Transparent,      WindowFlags::NoFocus,    WindowFlags::Popup,
    WindowFlags::MousePassthrough, WindowFlags::HighDpi,
};

}  // namespace

Feature feature_for_flag(WindowFlags flag) {
    switch (flag) {
        case WindowFlags::Resizable:
            return Feature::WindowResizable;
        case WindowFlags::Borderless:
            return Feature::WindowBorderless;
        case WindowFlags::AlwaysOnTop:
            return Feature::WindowAlwaysOnTop;
        case WindowFlags::Transparent:
            return Feature::WindowTransparency;
        case WindowFlags::NoFocus:
            return Feature::WindowNoFocus;
        case WindowFlags::Popup:
            return Feature::WindowPopup;
        case WindowFlags::MousePassthrough:
            return Feature::MousePassthrough;
        case WindowFlags::HighDpi:
            return Feature::HighDpi;
        case WindowFlags::None:
            break;
    }
    return Feature::HighDpi;
}

WindowFlags filter_unsupported_flags(const DisplayServer& server, WindowFlags requested) {
    WindowFlags supported = requested;
    for (WindowFlags flag : kAllFlags) {
        if (!has_flag(requested, flag)) {
            continue;
        }
        const Feature feature = feature_for_flag(flag);
        if (server.has_feature(feature)) {
            continue;
        }
        supported &= ~flag;
        emit_diagnosticf(DiagnosticSeverity::Warning, "display",
                         "%.*s does not support %s; the flag is ignored and the window is created "
                         "without it",
                         static_cast<int>(server.name().size()), server.name().data(),
                         feature_name(feature));
    }
    return supported;
}

void WindowEventQueue::push(const WindowEvent& event) {
    if (size_ == kCapacity) {
        ++dropped_;
        return;
    }
    events_[(head_ + size_) % kCapacity] = event;
    ++size_;
}

bool WindowEventQueue::pop(WindowEvent& event) {
    if (size_ == 0) {
        return false;
    }
    event = events_[head_];
    head_ = (head_ + 1) % kCapacity;
    --size_;
    return true;
}

u32 WindowEventQueue::take_dropped_count() {
    const u32 dropped = dropped_;
    dropped_ = 0;
    return dropped;
}

void WindowEventQueue::clear() {
    head_ = 0;
    size_ = 0;
}

}  // namespace cy
