#pragma once
// `EventChannel<T>` — typed, per-frame, double-buffered event queues. Task 1.3.3.
//
// `core-type-system` — "Events and signals": the typed event channel is the primary mechanism for
// system-to-system communication in the ECS. The collision system writes a `CollisionEvent`; any
// number of reader systems observe it in the next stage; neither side knows the other exists.
//
// LIFETIME IS THE REQUIREMENT, AND IT IS WHY THIS IS DOUBLE-BUFFERED. An event written in frame N
// is readable through the end of frame N+1 and is then discarded. That is exactly two buffers: the
// one being written now, and the one written last frame. `begin_frame()` swaps them and clears the
// new write buffer, which discards frame N-1 — so a reader that missed its window loses the event
// rather than the channel growing without bound. "A missed read cannot leak memory" is a property
// of the structure, not of anyone remembering to drain.
//
// THREADING. A channel is written by one producer at a time and read by many. That is the shape
// `core-jobs-and-concurrency` gives a system — one writer per channel per stage, readers in the
// stage that follows — so the channel itself takes no lock and pays for no atomic. A channel shared
// by concurrent writers is a scheduling error the access declarations (task 3.2.2) reject at
// registration, which is where it should be caught.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <span>
#include <vector>

namespace cy {

template <class T>
class EventChannel {
public:
    using value_type = T;

    EventChannel() noexcept = default;

    /// Append to the frame being written. Fails only when the growth allocation fails, which is
    /// reported rather than thrown because the engine is built with -fno-exceptions.
    Status write(const T& event) noexcept {
        std::vector<T>& target = buffers_[current_];
        if (target.size() == target.capacity()) {
            const usize next = target.capacity() == 0 ? 16 : target.capacity() * 2;
            target.reserve(next);
            if (target.capacity() < next) {
                return fail(ErrorCode::OutOfMemory, "event channel growth failed");
            }
        }
        target.push_back(event);
        return ok();
    }

    /// Start a new frame: the buffer holding the frame before last becomes the write buffer and is
    /// cleared. Capacity is kept, so a steady-state channel stops allocating after a few frames.
    void begin_frame() noexcept {
        current_ ^= 1u;
        buffers_[current_].clear();
    }

    /// Events written this frame.
    [[nodiscard]] std::span<const T> current() const noexcept {
        return std::span<const T>{buffers_[current_].data(), buffers_[current_].size()};
    }

    /// Events written last frame, still readable.
    [[nodiscard]] std::span<const T> previous() const noexcept {
        const std::vector<T>& other = buffers_[current_ ^ 1u];
        return std::span<const T>{other.data(), other.size()};
    }

    /// Everything a reader may see, oldest first. Two spans rather than one: the buffers are not
    /// contiguous with each other, and copying them together to pretend otherwise would allocate
    /// once per reader per frame.
    template <class Visitor>
    void for_each(Visitor&& visit) const {
        for (const T& event : previous()) {
            visit(event);
        }
        for (const T& event : current()) {
            visit(event);
        }
    }

    [[nodiscard]] usize readable() const noexcept {
        return buffers_[0].size() + buffers_[1].size();
    }
    [[nodiscard]] bool is_empty() const noexcept { return readable() == 0; }

    /// Drop everything, both frames. For a world reset, not for a frame boundary.
    void clear() noexcept {
        buffers_[0].clear();
        buffers_[1].clear();
    }

private:
    std::vector<T> buffers_[2];
    u32 current_ = 0;
};

}  // namespace cy
