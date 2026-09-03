// `Signal` and `SignalQueue`. Task 1.3.3.

#include <cy/core/values/signal.h>

#include "counters.h"

namespace cy {

// --- SignalQueue ---------------------------------------------------------------------------------

Status SignalQueue::enqueue(const Callable& callable, std::span<const Var> arguments,
                            Name signal) noexcept {
    if (entries_.size() == entries_.capacity()) {
        const usize next = entries_.capacity() == 0 ? 8 : entries_.capacity() * 2;
        entries_.reserve(next);
        if (entries_.capacity() < next) {
            return fail(ErrorCode::OutOfMemory, "deferred signal queue growth failed");
        }
    }
    Entry entry;
    entry.callable = callable;
    entry.signal = signal;
    for (const Var& argument : arguments) {
        if (!entry.arguments.push(argument)) {
            return fail(ErrorCode::OutOfMemory, "deferred signal argument copy failed");
        }
    }
    entries_.push_back(std::move(entry));
    return ok();
}

usize SignalQueue::flush() noexcept {
    // Taken by swap, so an invocation that queues onto this queue lands in the next flush rather
    // than in the vector being iterated — which would also invalidate the iteration.
    std::vector<Entry> running;
    running.swap(entries_);

    usize invoked = 0;
    for (const Entry& entry : running) {
        if (!entry.callable.is_valid()) {
            ++dropped_;
            values::detail::bump(values::detail::counters().signal_connections_pruned);
            continue;
        }
        const std::span<const Var> arguments{entry.arguments.data(), entry.arguments.size()};
        const Expected<Var, CallError> result = entry.callable.invoke(arguments);
        (void)result;  // a listener's own failure is the listener's; it is counted in call_failures
        ++invoked;
        values::detail::bump(values::detail::counters().signal_invocations);
    }
    return invoked;
}

// --- Signal --------------------------------------------------------------------------------------

Expected<ConnectionId, Error> Signal::connect(const Callable& callable,
                                              ConnectionFlags flags) noexcept {
    if (callable.kind() == CallableKind::Invalid) {
        return fail(ErrorCode::InvalidArgument,
                    "a default-constructed Callable cannot be connected to a signal");
    }
    if (connections_.size() == connections_.capacity()) {
        const usize next = connections_.capacity() == 0 ? 4 : connections_.capacity() * 2;
        connections_.reserve(next);
        if (connections_.capacity() < next) {
            return fail(ErrorCode::OutOfMemory, "signal connection list growth failed");
        }
    }
    const ConnectionId id = next_id_++;
    connections_.push_back(Connection{id, callable, flags});
    return id;
}

bool Signal::disconnect(ConnectionId connection) noexcept {
    for (usize i = 0; i < connections_.size(); ++i) {
        if (connections_[i].id == connection) {
            connections_.erase(connections_.begin() + static_cast<isize>(i));
            return true;
        }
    }
    return false;
}

usize Signal::disconnect_target(AnyHandle target) noexcept {
    usize removed = 0;
    usize keep = 0;
    for (usize i = 0; i < connections_.size(); ++i) {
        if (connections_[i].callable.target() == target && !target.is_null()) {
            ++removed;
            continue;
        }
        if (keep != i) {
            connections_[keep] = std::move(connections_[i]);
        }
        ++keep;
    }
    connections_.resize(keep);
    values::detail::bump(values::detail::counters().signal_connections_pruned, removed);
    return removed;
}

usize Signal::disconnect_transient() noexcept {
    usize removed = 0;
    usize keep = 0;
    for (usize i = 0; i < connections_.size(); ++i) {
        if (!has_flag(connections_[i].flags, ConnectionFlags::Persist)) {
            ++removed;
            continue;
        }
        if (keep != i) {
            connections_[keep] = std::move(connections_[i]);
        }
        ++keep;
    }
    connections_.resize(keep);
    return removed;
}

usize Signal::prune_invalid() noexcept {
    usize removed = 0;
    usize keep = 0;
    for (usize i = 0; i < connections_.size(); ++i) {
        if (!connections_[i].callable.is_valid()) {
            ++removed;
            continue;
        }
        if (keep != i) {
            connections_[keep] = std::move(connections_[i]);
        }
        ++keep;
    }
    connections_.resize(keep);
    values::detail::bump(values::detail::counters().signal_connections_pruned, removed);
    return removed;
}

ConnectionFlags Signal::flags_of(ConnectionId connection) const noexcept {
    for (const Connection& entry : connections_) {
        if (entry.id == connection) {
            return entry.flags;
        }
    }
    return ConnectionFlags::None;
}

Expected<Signal::EmitResult, Error> Signal::emit(std::span<const Var> arguments,
                                                 SignalQueue* queue) noexcept {
    if (arguments.size() != arity_) {
        return fail(ErrorCode::InvalidArgument, "signal emitted with the wrong argument count");
    }

    values::detail::bump(values::detail::counters().signal_emissions);

    EmitResult result;

    // The connection list is copied before iterating. A listener may connect to or disconnect from
    // this signal while it runs — an authoring layer does that constantly, and OneShot does it by
    // design — and iterating the live vector while it reallocates is the classic way a signal
    // system corrupts itself.
    std::vector<Connection> snapshot = connections_;
    std::vector<ConnectionId> consumed;

    for (const Connection& connection : snapshot) {
        if (!connection.callable.is_valid()) {
            consumed.push_back(connection.id);
            ++result.disconnected;
            continue;
        }

        if (has_flag(connection.flags, ConnectionFlags::Deferred)) {
            if (queue == nullptr) {
                return fail(ErrorCode::InvalidArgument,
                            "a Deferred connection was emitted with no SignalQueue to defer to");
            }
            if (Status queued = queue->enqueue(connection.callable, arguments, name_); !queued) {
                return make_unexpected(queued.error());
            }
            ++result.deferred;
            values::detail::bump(values::detail::counters().signal_deferred);
        } else {
            const Expected<Var, CallError> outcome = connection.callable.invoke(arguments);
            if (!outcome) {
                ++result.failed;
            }
            ++result.invoked;
            values::detail::bump(values::detail::counters().signal_invocations);
        }

        if (has_flag(connection.flags, ConnectionFlags::OneShot)) {
            consumed.push_back(connection.id);
            ++result.disconnected;
        }
    }

    for (const ConnectionId id : consumed) {
        disconnect(id);
    }
    values::detail::bump(values::detail::counters().signal_connections_pruned, consumed.size());
    return result;
}

}  // namespace cy
