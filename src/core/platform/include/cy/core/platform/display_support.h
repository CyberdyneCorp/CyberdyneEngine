// Machinery every DisplayServer implementation needs and none of them should write twice.
//
// Both halves exist because of a requirement rather than for convenience:
//
//   * filter_unsupported_flags() is the "unsupported feature degrades" scenario, in one place. A
//     backend that implemented the drop-and-warn rule itself would be a backend that could get it
//     subtly wrong, and the failure mode — silently getting the flag you asked for — is invisible.
//
//   * WindowEventQueue is bounded and counts what it dropped, so a burst of events cannot grow
//     unboundedly and cannot vanish unrecorded.

#pragma once

#include <cy/core/base/types.h>
#include <cy/core/platform/display_server.h>

namespace cy {

// The capability a single window flag depends on. Feature::HighDpi for WindowFlags::None, which no
// caller asks about.
Feature feature_for_flag(WindowFlags flag);

// The subset of `requested` that `server` can honour, warning once per flag it drops. This is the
// specification's "requesting transparency SHALL be ignored with a warning, not fail window
// creation" — the caller passes the result to the platform and creates the window regardless.
WindowFlags filter_unsupported_flags(const DisplayServer& server, WindowFlags requested);

// A fixed-capacity ring of window events.
//
// The capacity is a frame's worth of window events several times over — these are window
// management events, not input, which arrives on its own path at M2 with its own budget. Overflow
// drops the newest event and counts it, rather than overwriting an older one: the events that
// matter most on a saturated queue are the first ones, and the count is what tells the reader the
// tail is incomplete.
class WindowEventQueue {
public:
    static constexpr usize kCapacity = 128;

    void push(const WindowEvent& event);
    bool pop(WindowEvent& event);

    [[nodiscard]] usize size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }

    // The number dropped since the last call, and resets it.
    u32 take_dropped_count();

    void clear();

private:
    WindowEvent events_[kCapacity];
    usize head_ = 0;
    usize size_ = 0;
    u32 dropped_ = 0;
};

}  // namespace cy
