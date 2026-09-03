// `Callable` — the four kinds, and what invoking each one does. Task 1.3.4.

#include <cy/core/values/callable.h>

#include "counters.h"

#include <atomic>
#include <new>

namespace cy {

/// See the declaration in callable.h.
struct Callable::BoundState {
    Callable inner;
    VarArray arguments;
};

namespace {

/// The process's script host. An atomic pointer rather than a mutex: it is read on every script
/// invocation and written twice per hot reload.
std::atomic<ScriptHost*>& host_slot() noexcept {
    static std::atomic<ScriptHost*> host{nullptr};
    return host;
}

/// The arguments a bound callable passes on: its own, then the caller's. A small stack buffer
/// covers every call shape the engine actually makes; beyond it the vector allocates, and a
/// boundary call with more than sixteen arguments has already paid for worse.
class ArgumentBuffer {
public:
    Status build(const VarArray& bound, std::span<const Var> tail) noexcept {
        combined_.reserve(bound.size() + tail.size());
        if (combined_.capacity() < bound.size() + tail.size()) {
            return fail(ErrorCode::OutOfMemory, "bound argument buffer allocation failed");
        }
        for (const Var& value : bound) {
            combined_.push_back(value);
        }
        for (const Var& value : tail) {
            combined_.push_back(value);
        }
        return ok();
    }

    [[nodiscard]] std::span<const Var> view() const noexcept {
        return std::span<const Var>{combined_.data(), combined_.size()};
    }

private:
    std::vector<Var> combined_;
};

}  // namespace

const char* call_error_kind_name(CallErrorKind kind) noexcept {
    switch (kind) {
        case CallErrorKind::NoSuchMethod:
            return "NoSuchMethod";
        case CallErrorKind::WrongArgumentCount:
            return "WrongArgumentCount";
        case CallErrorKind::WrongArgumentType:
            return "WrongArgumentType";
        case CallErrorKind::TargetInvalid:
            return "TargetInvalid";
        case CallErrorKind::NotCallable:
            return "NotCallable";
        case CallErrorKind::Failed:
            return "Failed";
    }
    return "Unknown";
}

ScriptHost* set_script_host(ScriptHost* host) noexcept {
    return host_slot().exchange(host, std::memory_order_acq_rel);
}

ScriptHost* script_host() noexcept {
    return host_slot().load(std::memory_order_acquire);
}

Callable Callable::from_free(Name name, FreeCallFn function) noexcept {
    Callable result;
    if (function == nullptr) {
        return result;  // Invalid: a null function is not a callable that fails, it is not one
    }
    result.kind_ = CallableKind::Free;
    result.name_ = name;
    result.free_ = function;
    return result;
}

Callable Callable::from_method(Name name, AnyHandle target, MethodCallFn function,
                               TargetProbeFn probe) noexcept {
    Callable result;
    if (function == nullptr) {
        return result;
    }
    result.kind_ = CallableKind::Method;
    result.name_ = name;
    result.target_ = target;
    result.method_ = function;
    result.probe_ = probe;
    return result;
}

Callable Callable::from_script(Name function) noexcept {
    Callable result;
    if (function.is_empty()) {
        return result;
    }
    result.kind_ = CallableKind::Script;
    result.name_ = function;
    return result;
}

Callable Callable::bind(const Callable& inner, std::span<const Var> bound_arguments) noexcept {
    Callable result;
    if (inner.kind_ == CallableKind::Invalid) {
        return result;
    }
    auto* state = new (std::nothrow) BoundState();
    if (state == nullptr) {
        return result;
    }
    state->inner = inner;
    for (const Var& value : bound_arguments) {
        if (!state->arguments.push(value)) {
            delete state;
            return result;
        }
    }
    result.kind_ = CallableKind::Bound;
    result.name_ = inner.name_;
    result.bound_ = std::shared_ptr<const BoundState>(state);
    return result;
}

AnyHandle Callable::target() const noexcept {
    if (kind_ == CallableKind::Method) {
        return target_;
    }
    if (kind_ == CallableKind::Bound && bound_ != nullptr) {
        return bound_->inner.target();
    }
    return AnyHandle{};
}

usize Callable::bound_argument_count() const noexcept {
    return bound_ != nullptr ? bound_->arguments.size() : 0;
}

bool Callable::is_valid() const noexcept {
    switch (kind_) {
        case CallableKind::Invalid:
            return false;
        case CallableKind::Free:
            return free_ != nullptr;
        case CallableKind::Method:
            return method_ != nullptr && (probe_ == nullptr || probe_(target_));
        case CallableKind::Script: {
            const ScriptHost* host = script_host();
            return host != nullptr && host->resolve(name_) != nullptr;
        }
        case CallableKind::Bound:
            return bound_ != nullptr && bound_->inner.is_valid();
    }
    return false;
}

Expected<Var, CallError> Callable::invoke(std::span<const Var> arguments) const noexcept {
    values::detail::bump(values::detail::counters().call_invocations);

    // The dispatch is a lambda so that every way of failing — this function's own refusals and the
    // callee's — is counted in one place afterwards. Counting inside each branch left a callee that
    // reported its own failure uncounted, which made `call_failures` a count of the failures the
    // engine noticed rather than of the failures that happened.
    const auto refuse = [this](CallErrorKind kind, const char* detail) noexcept {
        return call_failed(kind, name_, detail);
    };

    Expected<Var, CallError> result = [&]() -> Expected<Var, CallError> {
        switch (kind_) {
            case CallableKind::Invalid:
                return refuse(CallErrorKind::NotCallable, "the callable was never bound");

            case CallableKind::Free:
                return free_(arguments);

            case CallableKind::Method:
                if (probe_ != nullptr && !probe_(target_)) {
                    return refuse(CallErrorKind::TargetInvalid,
                                  "the bound object has been destroyed");
                }
                return method_(target_, arguments);

            case CallableKind::Script: {
                // Resolved here, on every call: a hot reload replaces the host's table and the next
                // call picks up the new function, or reports that it is gone.
                const ScriptHost* host = script_host();
                if (host == nullptr) {
                    return refuse(CallErrorKind::TargetInvalid, "no script host is installed");
                }
                const FreeCallFn resolved = host->resolve(name_);
                if (resolved == nullptr) {
                    return refuse(CallErrorKind::NoSuchMethod,
                                  "the script module no longer exports this function");
                }
                return resolved(arguments);
            }

            case CallableKind::Bound: {
                if (bound_ == nullptr) {
                    return refuse(CallErrorKind::NotCallable, "bound callable has no state");
                }
                ArgumentBuffer buffer;
                if (!buffer.build(bound_->arguments, arguments)) {
                    return refuse(CallErrorKind::Failed, "bound argument buffer allocation failed");
                }
                // The inner invocation counts itself, so a bound call over a free function counts
                // as two invocations. That is the honest figure: two callables ran.
                return bound_->inner.invoke(buffer.view());
            }
        }
        return refuse(CallErrorKind::NotCallable, "unknown callable kind");
    }();

    if (!result) {
        values::detail::bump(values::detail::counters().call_failures);
    }
    return result;
}

bool operator==(const Callable& a, const Callable& b) noexcept {
    if (a.kind_ != b.kind_ || a.name_ != b.name_) {
        return false;
    }
    switch (a.kind_) {
        case CallableKind::Invalid:
            return true;
        case CallableKind::Free:
            return a.free_ == b.free_;
        case CallableKind::Method:
            return a.method_ == b.method_ && a.target_ == b.target_;
        case CallableKind::Script:
            return true;  // the name is the identity
        case CallableKind::Bound:
            // Pointer identity of the shared state. Two separately bound callables over the same
            // inner callable and equal arguments are deliberately not equal: `disconnect` must
            // remove the connection that was made, not one that looks like it.
            return a.bound_ == b.bound_;
    }
    return false;
}

}  // namespace cy
