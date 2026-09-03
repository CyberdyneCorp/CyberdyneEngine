#pragma once
// `Callable` — one representation for everything that can be called. Task 1.3.4.
//
// `core-type-system` — "Callable": a free function, a member function bound to a handle, a script
// function in the Swift overlay, and a bound-argument wrapper over any of those, all one type.
// Invocation takes `Var` arguments and returns `Expected<Var, CallError>`, and `CallError`
// distinguishes "no such method", "wrong argument count", "wrong argument type" and "target
// invalid" — four failures a caller at a scripting boundary has to tell apart, because three of
// them are the author's mistake and one of them is an object that went away.
//
// WHY THERE IS NO std::function HERE. A `Callable` is stored in a `Var`, connected to a signal and
// copied across a boundary; it has to be trivially destructible where it can be, comparable for
// identity so a connection can be disconnected, and free of the type erasure that -fno-rtti makes
// unpleasant. So it is a small tagged struct: three of the four kinds are plain data, and only the
// bound-argument wrapper allocates.
//
// TARGET VALIDITY IS THE POOL'S ANSWER, NOT A GUESS. A method callable carries the `AnyHandle` of
// its target and a probe supplied by whatever owns the pool that handle came from. `is_valid()`
// asks that probe. This is what lets a signal drop a connection to a destroyed node during
// destruction rather than calling into freed memory — see signal.h.
//
// A SCRIPT CALLABLE IS RESOLVED BY NAME, EVERY TIME. `core-type-system` requires that a callable
// referring to a script function survives a hot reload, re-resolved by name or reported invalid.
// Caching the resolved pointer and invalidating it on reload is the same behaviour with a coherence
// problem attached; a boundary call that costs one name lookup is not the cost worth taking it for.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/values/handle.h>
#include <cy/core/values/name.h>
#include <cy/core/values/var.h>

#include <memory>
#include <span>

namespace cy {

enum class CallableKind : u8 {
    Invalid = 0,
    Free,
    Method,
    Script,
    Bound,
};

/// Why a call did not happen, or did not produce a value.
enum class CallErrorKind : u8 {
    NoSuchMethod = 0,    ///< the name does not resolve, here or in the script host
    WrongArgumentCount,  ///< arity mismatch; `expected_arity` and `actual_arity` say what
    WrongArgumentType,   ///< an argument held a kind the callee cannot use
    TargetInvalid,       ///< the bound object has been destroyed, or the script module unloaded
    NotCallable,         ///< the callable is the default-constructed one
    Failed,              ///< the callee ran and reported its own failure
};

[[nodiscard]] const char* call_error_kind_name(CallErrorKind kind) noexcept;

struct CallError {
    CallErrorKind kind = CallErrorKind::NotCallable;
    /// The callable or argument the failure is about, when there is one. Names are metadata: this
    /// is for a diagnostic, never for control flow.
    Name name;
    /// A literal, or the empty string. Like `cy::Error::message`, it never owns its storage.
    const char* detail = "";
    u32 expected_arity = 0;
    u32 actual_arity = 0;
    /// The argument index a WrongArgumentType failure is about; otherwise zero.
    u32 argument_index = 0;
};

/// `return cy::call_failed(CallErrorKind::WrongArgumentCount, name, "…");` — the spelling a callee
/// uses, matching `cy::fail` for `Error`.
[[nodiscard]] inline Unexpected<CallError> call_failed(CallErrorKind kind, Name name = Name{},
                                                       const char* detail = "") noexcept {
    return Unexpected<CallError>(CallError{kind, name, detail, 0, 0, 0});
}

/// A callee. Free functions and the thunks a generator emits both have this shape.
using FreeCallFn = Expected<Var, CallError> (*)(std::span<const Var> arguments);

/// A method thunk. The target is passed erased; the thunk resolves it through whatever pool it came
/// from, which is knowledge the thunk has and this file does not.
using MethodCallFn = Expected<Var, CallError> (*)(AnyHandle target, std::span<const Var> arguments);

/// Whether a target is still alive. Supplied by the pool's owner at bind time.
using TargetProbeFn = bool (*)(AnyHandle target);

/// The script side of the boundary. `scripting-and-swift-overlay` owns the implementation at M5;
/// this is the seam it plugs into, and it is what makes the hot-reload scenario testable now.
class ScriptHost {
public:
    ScriptHost() = default;
    virtual ~ScriptHost() = default;

    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    /// Resolve a script function by name, or null when the module no longer exports it. Called on
    /// every invocation, so a hot reload needs no invalidation pass.
    [[nodiscard]] virtual FreeCallFn resolve(Name function) const noexcept = 0;

    /// Bumped by the host on every reload. Not consulted for correctness — it is reported in
    /// diagnostics so that "the callable stopped resolving" can be tied to the reload that did it.
    [[nodiscard]] virtual u64 revision() const noexcept = 0;
};

/// Install the process's script host, returning the previous one. Passing null removes it, which is
/// what an unload does; every script callable then reports `TargetInvalid`.
ScriptHost* set_script_host(ScriptHost* host) noexcept;
[[nodiscard]] ScriptHost* script_host() noexcept;

class Callable {
public:
    /// The default is not callable. Invoking it fails with `NotCallable` rather than doing nothing,
    /// because a signal connected to a default `Callable` is a bug at the connect site.
    Callable() noexcept = default;

    [[nodiscard]] static Callable from_free(Name name, FreeCallFn function) noexcept;

    /// `probe` may be null when the target's lifetime is guaranteed by other means; the callable is
    /// then always valid, and saying so explicitly is better than a probe that always returns true.
    [[nodiscard]] static Callable from_method(Name name, AnyHandle target, MethodCallFn function,
                                              TargetProbeFn probe) noexcept;

    [[nodiscard]] static Callable from_script(Name function) noexcept;

    /// Bind leading arguments. `bind(f, {a, b})` called with `(c)` invokes `f(a, b, c)`. Allocates
    /// once; the result is copyable and the bound arguments are shared, not copied again.
    [[nodiscard]] static Callable bind(const Callable& inner,
                                       std::span<const Var> bound_arguments) noexcept;

    [[nodiscard]] CallableKind kind() const noexcept { return kind_; }
    [[nodiscard]] Name name() const noexcept { return name_; }
    /// The bound target, or a null handle for the kinds that have none.
    [[nodiscard]] AnyHandle target() const noexcept;
    [[nodiscard]] usize bound_argument_count() const noexcept;

    /// Whether a call would reach a callee: the function is present, the target is alive, and a
    /// script function still resolves. Cheap enough to ask before every emission.
    [[nodiscard]] bool is_valid() const noexcept;

    [[nodiscard]] Expected<Var, CallError> invoke(std::span<const Var> arguments) const noexcept;
    [[nodiscard]] Expected<Var, CallError> invoke() const noexcept { return invoke({}); }

    /// Identity, not behaviour: two callables are equal when they would reach the same callee with
    /// the same binding. This is what `Signal::disconnect` compares, so it must not depend on
    /// anything that changes between connecting and disconnecting.
    friend bool operator==(const Callable& a, const Callable& b) noexcept;
    friend bool operator!=(const Callable& a, const Callable& b) noexcept { return !(a == b); }

private:
    /// The inner callable and its bound arguments. Declared here and defined in callable.cpp: it
    /// holds a `Callable` by value, which cannot be a member of the class it is nested in. The
    /// `shared_ptr` needs no more than the declaration — it captures its deleter when the state is
    /// created, which is the same reason a pimpl works.
    struct BoundState;

    CallableKind kind_ = CallableKind::Invalid;
    Name name_;
    AnyHandle target_;
    FreeCallFn free_ = nullptr;
    MethodCallFn method_ = nullptr;
    TargetProbeFn probe_ = nullptr;
    std::shared_ptr<const BoundState> bound_;
};

}  // namespace cy
