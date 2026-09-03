#pragma once
// Cooperative cancellation. Task 3.2.6.
//
// `core-jobs-and-concurrency`: cancellation never forcibly terminates running code. A cancelled
// task observes its token at a natural boundary, releases what it holds, and returns. Cancellation
// propagates to child tasks and to awaited operations that support it, so cancelling a composite
// operation cancels its parts, and a task that does not observe cancellation within a configured
// interval is reported.
//
// The shape is a *source* and a *token*. A source is the authority — whoever owns the operation
// holds it and is the only thing that can cancel. A token is the observer, copied freely into every
// task and coroutine that participates; it can be asked and it can be registered against, and it
// cannot cancel. That split is what stops a task deep in a composite operation from cancelling its
// own parent by holding the wrong type.
//
// A default-constructed token is the never-cancelled token. It is not an error state: work that
// nobody may cancel carries it, `is_cancelled()` is a constant false, and no allocation is made.
//
// ALLOCATION. The shared state is one `new (std::nothrow)` per source, at the point an operation is
// created — a control-plane act, not a per-task one, and the failure is reported as
// ErrorCode::OutOfMemory rather than terminating. This is the seam where task 2.1's allocator
// interface plugs in; there is exactly one place to change (`cancellation.cpp`).

#include <cy/core/jobs/sync.h>
#include <cy/core/jobs/types.h>

namespace cy::jobs {

namespace detail {
class CancellationState;
}

/// A function invoked when the token it was registered against is cancelled. It runs on whichever
/// thread called `cancel()`, so it must be short and must not block: the usual body schedules a
/// continuation rather than doing work.
using CancellationCallback = void (*)(void* user) noexcept;

/// A registration, so that a waiter which finished for another reason can withdraw its callback.
/// Zero is "not registered".
using CancellationRegistration = u32;

inline constexpr CancellationRegistration kNoCancellationRegistration = 0;

class CancellationSource;

/// The observer half. Copyable, comparable, and cheap: a token is one pointer plus a reference
/// count operation on copy.
class CancellationToken {
public:
    constexpr CancellationToken() noexcept = default;

    CancellationToken(const CancellationToken& other) noexcept;
    CancellationToken(CancellationToken&& other) noexcept;
    CancellationToken& operator=(const CancellationToken& other) noexcept;
    CancellationToken& operator=(CancellationToken&& other) noexcept;
    ~CancellationToken();

    /// False for the never-cancelled token, which is what a default-constructed one is.
    [[nodiscard]] bool can_be_cancelled() const noexcept { return state_ != nullptr; }

    /// The one question a task asks at a natural boundary. Relaxed: observing a cancellation one
    /// iteration late is the cooperative model working, and an acquire on every loop iteration
    /// would be a cost paid by the work that is not being cancelled.
    [[nodiscard]] bool is_cancelled() const noexcept;

    /// Register a callback, invoked immediately on the calling thread when the token is already
    /// cancelled. Returns kNoCancellationRegistration when the token can never be cancelled, when
    /// it fired inline, or when the registration table for this token is full — in each of those
    /// cases there is nothing to withdraw.
    CancellationRegistration on_cancel(CancellationCallback callback, void* user) noexcept;

    /// Withdraw a callback that has not fired. Safe to call with kNoCancellationRegistration.
    void withdraw(CancellationRegistration registration) noexcept;

    /// When cancellation was requested, on the monotonic clock, or 0 while it has not been. The job
    /// system's watchdog reads it to decide that a task has run past its grace period without
    /// observing its token — the "unresponsive task is reported" scenario.
    [[nodiscard]] i64 cancelled_at_ns() const noexcept;

    friend bool operator==(const CancellationToken& lhs, const CancellationToken& rhs) noexcept {
        return lhs.state_ == rhs.state_;
    }
    friend bool operator!=(const CancellationToken& lhs, const CancellationToken& rhs) noexcept {
        return lhs.state_ != rhs.state_;
    }

private:
    friend class CancellationSource;
    explicit CancellationToken(detail::CancellationState* state) noexcept;

    detail::CancellationState* state_ = nullptr;
};

/// The authority half. Move-only: an operation has one owner, and two sources for one operation
/// would mean two things believing they decide when it stops.
class CancellationSource {
public:
    constexpr CancellationSource() noexcept = default;

    CancellationSource(const CancellationSource&) = delete;
    CancellationSource& operator=(const CancellationSource&) = delete;
    CancellationSource(CancellationSource&& other) noexcept;
    CancellationSource& operator=(CancellationSource&& other) noexcept;
    ~CancellationSource();

    /// A new, independent source.
    static Expected<CancellationSource, cy::Error> create() noexcept;

    /// A source whose token is cancelled when `parent` is — the propagation the specification
    /// requires of a composite operation. Cancelling the child does not cancel the parent.
    ///
    /// A parent that is already cancelled yields an already-cancelled child rather than an error:
    /// the caller asked for a child of a dead operation, and the honest answer is a dead child.
    static Expected<CancellationSource, cy::Error> create_child(
        const CancellationToken& parent) noexcept;

    /// Cancel. Idempotent, and safe from any thread. Propagates to every child, then invokes every
    /// registered callback on the calling thread.
    void cancel() noexcept;

    [[nodiscard]] bool is_cancelled() const noexcept;
    [[nodiscard]] bool is_valid() const noexcept { return state_ != nullptr; }

    /// A token for this source. Copy it into every task that participates.
    [[nodiscard]] CancellationToken token() const noexcept;

    /// When cancellation was requested, on the monotonic clock, or 0 while it has not been. The
    /// watchdog reads it to decide that a task has run past its grace period without observing.
    [[nodiscard]] i64 cancelled_at_ns() const noexcept;

private:
    explicit CancellationSource(detail::CancellationState* state) noexcept : state_(state) {}

    detail::CancellationState* state_ = nullptr;
};

/// How many cancellation states are live. A test's leak check, and a diagnostic: a source that is
/// never destroyed is an operation nobody finished.
u64 cancellation_states_live() noexcept;

}  // namespace cy::jobs
