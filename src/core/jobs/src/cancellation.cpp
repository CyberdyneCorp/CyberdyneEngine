// Cooperative cancellation: the shared state, the parent/child tree, and the callback table.
//
// THE OWNERSHIP DIRECTION IS THE WHOLE DESIGN. A child holds a counted reference to its parent, so
// a parent always outlives its children; a parent holds only a raw link to each child, and a child
// unlinks itself from its parent under the parent's lock before it dies. The obvious alternative —
// counted references both ways — is a cycle, and a cycle here means a cancellation tree that is
// never freed for the lifetime of the process.
//
// Lock order is parent before child, which is the direction cancellation propagates and therefore
// the only direction a lock is ever taken in two states at once. A child's destructor takes its
// parent's lock and never its own while it holds it, so the destructor cannot invert the order.

#include <cy/core/jobs/cancellation.h>

#include <cy/core/base/assert.h>

#include <atomic>
#include <new>
#include <utility>

namespace cy::jobs {
namespace {

std::atomic<u64> g_states_live{0};

}  // namespace

namespace detail {

/// One cancellation state, shared by a source and every token copied from it.
class CancellationState {
public:
    /// Callbacks one token may register. Small on purpose: a registration is an awaiting operation,
    /// and an operation with more than a handful of awaiters is a task group, not a token.
    static constexpr u32 kMaxCallbacks = 16;

    explicit CancellationState(CancellationState* parent) noexcept : parent_(parent), refs_(1) {
        g_states_live.fetch_add(1, std::memory_order_relaxed);
    }

    ~CancellationState() {
        if (parent_ != nullptr) {
            parent_->unlink_child(this);
            parent_->release();
        }
        g_states_live.fetch_sub(1, std::memory_order_relaxed);
    }

    CancellationState(const CancellationState&) = delete;
    CancellationState& operator=(const CancellationState&) = delete;

    void retain() noexcept { refs_.add_ref(); }

    void release() noexcept {
        if (refs_.release()) {
            delete this;
        }
    }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return cancelled_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] i64 cancelled_at_ns() const noexcept {
        return cancelled_at_ns_.load(std::memory_order_relaxed);
    }

    /// Link `child` under this state. Called while the child is being constructed, so the child is
    /// not yet visible to anything else.
    void link_child(CancellationState* child) noexcept {
        ScopedLock<Mutex> held(lock_);
        child->next_sibling_ = first_child_;
        first_child_ = child;
    }

    void unlink_child(CancellationState* child) noexcept {
        ScopedLock<Mutex> held(lock_);
        CancellationState** link = &first_child_;
        while (*link != nullptr) {
            if (*link == child) {
                *link = child->next_sibling_;
                child->next_sibling_ = nullptr;
                return;
            }
            link = &(*link)->next_sibling_;
        }
    }

    void cancel() noexcept {
        // exchange, not store: cancellation is idempotent and must propagate exactly once, or a
        // second cancel() would run every callback a second time.
        if (cancelled_.exchange(true, std::memory_order_release)) {
            return;
        }
        cancelled_at_ns_.store(monotonic_now_ns(), std::memory_order_relaxed);

        CancellationCallback callbacks[kMaxCallbacks];
        void* users[kMaxCallbacks];
        u32 count = 0;
        {
            ScopedLock<Mutex> held(lock_);
            // Children first, so that a callback on this state observes a tree that is already
            // fully cancelled rather than one still collapsing underneath it.
            for (CancellationState* child = first_child_; child != nullptr;
                 child = child->next_sibling_) {
                child->cancel();
            }
            for (u32 i = 0; i < callback_count_; ++i) {
                if (callbacks_[i] != nullptr) {
                    callbacks[count] = callbacks_[i];
                    users[count] = users_[i];
                    ++count;
                    callbacks_[i] = nullptr;
                }
            }
        }
        // Outside the lock: a callback schedules a continuation, and a continuation that submitted
        // a job while this lock was held would hold it across the scheduler.
        for (u32 i = 0; i < count; ++i) {
            callbacks[i](users[i]);
        }
    }

    /// Returns the registration, or 0 when it fired inline or the table is full.
    CancellationRegistration add_callback(CancellationCallback callback, void* user) noexcept {
        if (callback == nullptr) {
            return kNoCancellationRegistration;
        }
        {
            ScopedLock<Mutex> held(lock_);
            if (!cancelled_.load(std::memory_order_relaxed)) {
                for (u32 i = 0; i < callback_count_; ++i) {
                    if (callbacks_[i] == nullptr) {
                        callbacks_[i] = callback;
                        users_[i] = user;
                        return i + 1;
                    }
                }
                if (callback_count_ < kMaxCallbacks) {
                    const u32 index = callback_count_++;
                    callbacks_[index] = callback;
                    users_[index] = user;
                    return index + 1;
                }
                return kNoCancellationRegistration;
            }
        }
        // Already cancelled: the registration's whole purpose has happened, so it happens now, on
        // the registering thread, and there is nothing to withdraw.
        callback(user);
        return kNoCancellationRegistration;
    }

    void remove_callback(CancellationRegistration registration) noexcept {
        if (registration == kNoCancellationRegistration) {
            return;
        }
        ScopedLock<Mutex> held(lock_);
        const u32 index = registration - 1;
        if (index < callback_count_) {
            callbacks_[index] = nullptr;
        }
    }

private:
    CancellationState* parent_ = nullptr;
    CancellationState* first_child_ = nullptr;
    CancellationState* next_sibling_ = nullptr;

    Mutex lock_;
    AtomicRefCount refs_;
    std::atomic<bool> cancelled_{false};
    std::atomic<i64> cancelled_at_ns_{0};

    CancellationCallback callbacks_[kMaxCallbacks] = {};
    void* users_[kMaxCallbacks] = {};
    u32 callback_count_ = 0;
};

}  // namespace detail

// --- CancellationToken ---------------------------------------------------------------------------

CancellationToken::CancellationToken(detail::CancellationState* state) noexcept : state_(state) {
    if (state_ != nullptr) {
        state_->retain();
    }
}

CancellationToken::CancellationToken(const CancellationToken& other) noexcept
    : state_(other.state_) {
    if (state_ != nullptr) {
        state_->retain();
    }
}

CancellationToken::CancellationToken(CancellationToken&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {}

CancellationToken& CancellationToken::operator=(const CancellationToken& other) noexcept {
    if (this != &other) {
        detail::CancellationState* previous = state_;
        state_ = other.state_;
        if (state_ != nullptr) {
            state_->retain();
        }
        if (previous != nullptr) {
            previous->release();
        }
    }
    return *this;
}

CancellationToken& CancellationToken::operator=(CancellationToken&& other) noexcept {
    if (this != &other) {
        if (state_ != nullptr) {
            state_->release();
        }
        state_ = std::exchange(other.state_, nullptr);
    }
    return *this;
}

CancellationToken::~CancellationToken() {
    if (state_ != nullptr) {
        state_->release();
        state_ = nullptr;
    }
}

bool CancellationToken::is_cancelled() const noexcept {
    return state_ != nullptr && state_->is_cancelled();
}

i64 CancellationToken::cancelled_at_ns() const noexcept {
    return state_ != nullptr ? state_->cancelled_at_ns() : 0;
}

CancellationRegistration CancellationToken::on_cancel(CancellationCallback callback,
                                                      void* user) noexcept {
    if (state_ == nullptr) {
        return kNoCancellationRegistration;
    }
    return state_->add_callback(callback, user);
}

void CancellationToken::withdraw(CancellationRegistration registration) noexcept {
    if (state_ != nullptr) {
        state_->remove_callback(registration);
    }
}

// --- CancellationSource --------------------------------------------------------------------------

CancellationSource::CancellationSource(CancellationSource&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {}

CancellationSource& CancellationSource::operator=(CancellationSource&& other) noexcept {
    if (this != &other) {
        if (state_ != nullptr) {
            state_->release();
        }
        state_ = std::exchange(other.state_, nullptr);
    }
    return *this;
}

CancellationSource::~CancellationSource() {
    if (state_ != nullptr) {
        state_->release();
        state_ = nullptr;
    }
}

Expected<CancellationSource, cy::Error> CancellationSource::create() noexcept {
    auto* state = new (std::nothrow) detail::CancellationState(nullptr);
    if (state == nullptr) {
        return fail(ErrorCode::OutOfMemory, "a cancellation state could not be allocated");
    }
    return CancellationSource(state);
}

Expected<CancellationSource, cy::Error> CancellationSource::create_child(
    const CancellationToken& parent) noexcept {
    if (!parent.can_be_cancelled()) {
        // A child of the never-cancelled token is an ordinary independent source: nothing upstream
        // will ever cancel it, which is exactly what an independent source means.
        return create();
    }

    detail::CancellationState* parent_state = parent.state_;
    parent_state->retain();
    auto* state = new (std::nothrow) detail::CancellationState(parent_state);
    if (state == nullptr) {
        parent_state->release();
        return fail(ErrorCode::OutOfMemory, "a child cancellation state could not be allocated");
    }
    parent_state->link_child(state);

    // The parent may have been cancelled between the retain above and the link. Re-checking here is
    // what turns that race into the documented behaviour — a child of a dead operation is born dead
    // — rather than into a child that never observes its parent's cancellation.
    if (parent_state->is_cancelled()) {
        state->cancel();
    }
    return CancellationSource(state);
}

void CancellationSource::cancel() noexcept {
    if (state_ != nullptr) {
        state_->cancel();
    }
}

bool CancellationSource::is_cancelled() const noexcept {
    return state_ != nullptr && state_->is_cancelled();
}

CancellationToken CancellationSource::token() const noexcept {
    return CancellationToken(state_);
}

i64 CancellationSource::cancelled_at_ns() const noexcept {
    return state_ != nullptr ? state_->cancelled_at_ns() : 0;
}

u64 cancellation_states_live() noexcept {
    return g_states_live.load(std::memory_order_relaxed);
}

}  // namespace cy::jobs
