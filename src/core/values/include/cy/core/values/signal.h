#pragma once
// `Signal` — named, argument-typed emitters for the authoring and scripting layer. Task 1.3.3.
//
// `core-type-system` — "Events and signals": the second of the two decoupled notification
// mechanisms. An `EventChannel<T>` is what systems use; a `Signal` is what a node exposes to an
// author and to a script. It is connected to `Callable`s with `Deferred`, `OneShot` and `Persist`
// flags, and a connection to a destroyed target is removed during destruction so that emission
// never touches freed memory.
//
// EMISSION NEVER CALLS A DEAD TARGET, AND THERE ARE TWO REASONS IT DOES NOT. The owner of a
// destroyed object calls `disconnect_target()` during destruction, which is the deterministic path
// and the one the specification names. Emission also asks each connection's `Callable::is_valid()`
// before invoking it and drops the ones that answer no, which covers the object destroyed by
// something that did not know it was connected. The second is a safety net, not the mechanism: a
// system that relies on it leaves connections alive until the next emission.
//
// A SIGNAL IS NOT AN EVENT CHANNEL. It invokes; it does not queue, unless the connection asked for
// `Deferred`, in which case the invocation goes on a `SignalQueue` that a flush point drains. That
// difference is the whole reason both exist: a signal is for the authoring layer, where the
// connection is named and the call is direct, and a channel is for the simulation, where the reader
// is anonymous and the delivery is a frame boundary.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/values/callable.h>
#include <cy/core/values/handle.h>
#include <cy/core/values/name.h>
#include <cy/core/values/var.h>

#include <span>
#include <vector>

namespace cy {

enum class ConnectionFlags : u8 {
    None = 0,
    /// Queue the invocation instead of running it inline; a flush point runs it later.
    Deferred = 1u << 0,
    /// Disconnect after the first invocation.
    OneShot = 1u << 1,
    /// The connection is part of the authored document: it is written by serialization and it
    /// survives `disconnect_transient()`. A connection made by code at run time is not persistent,
    /// and clearing those without disturbing the authored ones is what the flag is for.
    Persist = 1u << 2,
};

[[nodiscard]] constexpr ConnectionFlags operator|(ConnectionFlags a, ConnectionFlags b) noexcept {
    return static_cast<ConnectionFlags>(static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr bool has_flag(ConnectionFlags value, ConnectionFlags flag) noexcept {
    return (static_cast<u8>(value) & static_cast<u8>(flag)) != 0;
}

/// Identifies one connection for the lifetime of its signal. Never reused, so a stale id
/// disconnects nothing rather than disconnecting whatever took its place.
using ConnectionId = u64;

inline constexpr ConnectionId kInvalidConnection = 0;

/// Where a `Deferred` invocation waits. A flush point — the end of a frame, the end of a stage —
/// drains it. Kept separate from `Signal` because the flush point belongs to whoever owns the loop,
/// and many signals share one queue.
class SignalQueue {
public:
    SignalQueue() noexcept = default;

    /// Invoke everything queued, oldest first, and empty the queue. Returns how many ran.
    ///
    /// An invocation queued *during* a flush goes to the next flush rather than extending this one:
    /// a signal that re-emits itself would otherwise never let the flush return.
    usize flush() noexcept;

    [[nodiscard]] usize pending() const noexcept { return entries_.size(); }
    void clear() noexcept { entries_.clear(); }

    /// How many queued invocations were dropped because their target had gone away between the
    /// emission and the flush.
    [[nodiscard]] u64 dropped() const noexcept { return dropped_; }

private:
    friend class Signal;

    struct Entry {
        Callable callable;
        VarArray arguments;
        Name signal;
    };

    Status enqueue(const Callable& callable, std::span<const Var> arguments, Name signal) noexcept;

    std::vector<Entry> entries_;
    u64 dropped_ = 0;
};

class Signal {
public:
    /// `arity` is the number of arguments the signal declares. Emission with a different count
    /// fails; that is the "argument-typed" half of the requirement that this layer can check
    /// without a type registry, and the generator checks the rest at build time.
    Signal(Name name, u32 arity) noexcept : name_(name), arity_(arity) {}

    [[nodiscard]] Name name() const noexcept { return name_; }
    [[nodiscard]] u32 arity() const noexcept { return arity_; }

    /// Connect a callable. Fails on a callable that is not callable at all — a default-constructed
    /// one — because that is a mistake at the connect site rather than a target that went away.
    [[nodiscard]] Expected<ConnectionId, Error> connect(
        const Callable& callable, ConnectionFlags flags = ConnectionFlags::None) noexcept;

    /// Remove one connection. False when the id names nothing, which includes an id that was
    /// already disconnected.
    bool disconnect(ConnectionId connection) noexcept;

    /// Remove every connection whose callable is bound to `target`. This is what an object's
    /// destructor calls, and it is the deterministic half of "emission never touches freed memory".
    usize disconnect_target(AnyHandle target) noexcept;

    /// Remove every connection that is not `Persist`. What a scene reload does before rebinding.
    usize disconnect_transient() noexcept;

    /// Remove every connection whose callable no longer resolves. Called by `emit`; exposed so an
    /// owner can sweep without emitting.
    usize prune_invalid() noexcept;

    /// Invoke, or queue, every live connection.
    ///
    /// A connection's own failure does not stop the others: a signal is a broadcast, and one bad
    /// listener must not silence the rest. The count of failures is returned so a caller that cares
    /// can report it, and each failure is counted in `values_diagnostics()`.
    struct EmitResult {
        usize invoked = 0;
        usize deferred = 0;
        usize failed = 0;
        usize disconnected = 0;  ///< OneShot connections consumed, plus invalid ones dropped
    };

    [[nodiscard]] Expected<EmitResult, Error> emit(std::span<const Var> arguments,
                                                   SignalQueue* queue = nullptr) noexcept;

    [[nodiscard]] usize connection_count() const noexcept { return connections_.size(); }

    /// The flags a connection was made with, for a test or an inspector. `ConnectionFlags::None`
    /// when the id names nothing.
    [[nodiscard]] ConnectionFlags flags_of(ConnectionId connection) const noexcept;

private:
    struct Connection {
        ConnectionId id = kInvalidConnection;
        Callable callable;
        ConnectionFlags flags = ConnectionFlags::None;
    };

    Name name_;
    u32 arity_ = 0;
    ConnectionId next_id_ = 1;
    std::vector<Connection> connections_;
};

}  // namespace cy
