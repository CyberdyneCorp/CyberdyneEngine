// The accumulation window. Task 4.1.4.
//
// The whole file is small on purpose. Every decision that matters is stated in event.h, and the
// only one visible here is that `push()` contains no comparison against the previous event: the
// buffer cannot coalesce because there is no code path in which it could.

#include <cy/servers/input/event.h>

#include <algorithm>

namespace cy::input {

Status EventBuffer::reserve(usize capacity) noexcept {
    if (Status reserved = events_.reserve(capacity); !reserved) {
        return reserved;
    }
    capacity_ = capacity;
    return ok();
}

void EventBuffer::push(const DeviceEvent& event) noexcept {
    if (capacity_ != 0 && events_.size() >= capacity_) {
        ++dropped_;
        return;
    }
    DeviceEvent stamped = event;
    stamped.sequence = next_sequence_++;
    // The reserve above is what makes this succeed; a failure here means the window was never
    // sized, and the event is counted as dropped rather than lost silently.
    if (Status pushed = events_.push_back(stamped); !pushed) {
        ++dropped_;
    }
}

void EventBuffer::sort() noexcept {
    const auto ordered = [](const DeviceEvent& a, const DeviceEvent& b) { return a.before(b); };
    // TWO DELIBERATE CHOICES, BOTH ABOUT ALLOCATION.
    //
    // `std::sort`, not `std::stable_sort`: the latter may allocate a temporary buffer, and
    // `input-and-actions` requires evaluation to allocate nothing per frame. Stability is not
    // needed anyway — `sequence` is unique, so no two events compare equal and the order is total.
    //
    // And the `is_sorted` guard: pushes from one backend are already in order, which is the
    // ordinary case, so the ordinary cost is one linear scan rather than n log n.
    if (std::ranges::is_sorted(events_, ordered)) {
        return;
    }
    std::ranges::sort(events_, ordered);
}

}  // namespace cy::input
