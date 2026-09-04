#pragma once
// State classification, and the determinism firewall as a compile-time rule. Task 4.2.5.
//
// `simulation-and-determinism` — "State classification": every field participating in simulation
// state carries a classification, derived from the field classification in
// `serialization-and-prefabs` and extended for simulation, and that one declaration determines
// participation in state hashing, rollback snapshots, replay checkpoints, saves, and network
// replication. And "The determinism firewall": authoritative simulation does not read
// presentation-produced state, presentation-only systems do not feed back into authoritative state,
// and where a presentation outcome must influence gameplay it is captured as an authoritative event
// or an external result rather than read directly.
//
// ================================================================================================
// WHAT IS UNSPELLABLE HERE, AND WHAT IS ONLY REFUSED. READ THIS BEFORE TRUSTING THE FILE.
// ================================================================================================
//
// design.md §5 sets the bar: "Classification has to make the illegal read **unspellable**, not
// merely refused — otherwise M9 inherits a validator that reports clean on code that is not." M1's
// precedent is the one to avoid: workers never block is enforced for *declared* blocking, and an
// undeclared `read()` is caught only by a watchdog.
//
// UNSPELLABLE. A firewall crossing between two values held in `Classified<>` **does not compile**.
// The read requires an `AccessContext<C>` witness and the overload is constrained on
// `may_read(C, source)`, so an authoritative system handed `AuthoritativeContext` cannot name the
// value inside a `Presentation<f32>` at all — not "gets an error at run time", not "is reported by
// a checkpoint": there is no expression that yields it. The same holds for writes: a presentation
// context cannot write an authoritative field, so the feedback direction is closed too. Assignment
// between differently classified wrappers is closed as well, because they are distinct types with
// no converting constructor, so a value cannot be laundered by copying it.
//
// ONLY REFUSED — and there is nothing here that changes it:
//
//   * a system that reads a plain global, a raw `float`, or a member of a struct that never adopted
//     `Classified<>`. Nothing in this header sees it. That is the determinism lint's, at M9;
//   * `bypass_classification()`, which every reflection-driven consumer needs and which is
//     deliberately spelled to be ugly, greppable and impossible to type by accident;
//   * `record_external()`, which is the sanctioned laundering point and is *supposed* to be
//     spellable — the requirement asks for the crossing to be captured, not prevented.
//
// So: the crossing is unspellable **for state that is classified**, and classification is opt-in
// per field. Adopting the wrapper is what buys the guarantee; a component that has not adopted it
// gets no more than a comment. That is the honest statement of what this file delivers.
//
// ================================================================================================

#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/reflect/attributes.h>

#include <type_traits>

namespace cy::determinism {

/// What a field is, for the five mechanisms that read the declaration.
///
/// The first three are `serialization-and-prefabs`' classes seen through the simulation's eyes;
/// `Predicted` and `Presentation` are the simulation extension, and they are the two the firewall
/// is actually about.
enum class SimulationClass : u8 {
    /// Part of the authoritative simulation. Hashed, snapshotted, replicated; not necessarily
    /// saved.
    Authoritative = 0,
    /// Client-side speculation, reconciled against authority. Never read by an authoritative
    /// system: prediction that feeds authority is prediction that has become authority.
    Predicted,
    /// Authoritative *and* written to the persistence overlay.
    Persistent,
    /// Appearance only: animation pose, camera, audio, VFX, GPU-produced data, illumination.
    Presentation,
    /// Computed from other state. Never hashed, snapshotted or saved; recomputed instead.
    Derived,
};

const char* simulation_class_name(SimulationClass value) noexcept;

/// True for the classes that carry authority, which is what the firewall's direction is about.
[[nodiscard]] constexpr bool is_authoritative(SimulationClass value) noexcept {
    return value == SimulationClass::Authoritative || value == SimulationClass::Persistent;
}

// --- One declaration, five behaviours ------------------------------------------------------------
//
// `simulation-and-determinism`'s "One declaration, five behaviours" scenario, as a table rather
// than as five subsystems each deciding for themselves. `Replicated` is the one entry that is
// *also* governed by a separate declaration — a field's `Replicated` attribute names the condition
// and the authority — so what this table says is "may be replicated", and the attribute says
// whether it is.

struct Participation {
    bool hashed = false;
    bool rollback = false;
    bool checkpoint = false;
    bool saved = false;
    bool replicable = false;

    friend constexpr bool operator==(const Participation&, const Participation&) noexcept = default;
};

[[nodiscard]] constexpr Participation participation_of(SimulationClass value) noexcept {
    switch (value) {
        case SimulationClass::Authoritative:
            return {true, true, true, false, true};
        case SimulationClass::Predicted:
            // Rolled back and checkpointed because reconciliation needs the predicted value to
            // rewind with; never hashed, because two peers legitimately disagree about it, and
            // hashing it would make correct prediction look like divergence.
            return {false, true, true, false, false};
        case SimulationClass::Persistent:
            return {true, true, true, true, true};
        case SimulationClass::Presentation:
        case SimulationClass::Derived:
            // Every column false for both, which is what "never hashed, snapshotted or saved;
            // recomputed" means when it is a rule and not a sentence. They share a branch because
            // they share an answer, not by accident: neither is part of authoritative state.
            return {};
    }
    return {};
}

// --- The firewall -----------------------------------------------------------------------------

/// May a system whose own class is `reader` read a field classified `source`?
///
/// The table, and the reasoning for each row that is not obvious:
///
///   Authoritative / Persistent reader — may read authoritative, persistent and derived. NOT
///     presentation (the firewall) and NOT predicted (prediction is client-local speculation;
///     authority that reads it has adopted a guess).
///   Predicted reader — the client's speculation, which legitimately reads authority and its own
///     predictions. Still not presentation.
///   Presentation reader — reads anything. Appearance is allowed to look at everything; what it may
///     not do is write back, and that is `may_write`'s business.
///   Derived reader — restricted to the authoritative set. A cache computed from presentation data
///     is a presentation cache and must be classified `Presentation`; allowing `Derived` to read
///     presentation would make the class a hole through which presentation reaches authority in two
///     hops, which is precisely the taint M9's tracking is meant to find rather than to inherit.
[[nodiscard]] constexpr bool may_read(SimulationClass reader, SimulationClass source) noexcept {
    if (reader == SimulationClass::Presentation) {
        return true;
    }
    if (source == SimulationClass::Presentation) {
        return false;
    }
    if (source == SimulationClass::Predicted) {
        return reader == SimulationClass::Predicted;
    }
    return true;
}

/// May a system whose own class is `writer` write a field classified `target`?
///
/// **Deliberately not "may_read and then some".** `simulation-and-determinism` states the two
/// directions separately — "Presentation-only systems SHALL NOT feed back into authoritative state"
/// is its own sentence — and they are genuinely different questions. A presentation system may read
/// authority perfectly legally and may never write it; an authoritative system may write a
/// presentation field (setting a flash intensity is an ordinary thing for gameplay to do) while
/// still being unable to *read* one. Deriving one predicate from the other would have made the
/// second of those unspellable for no reason.
///
/// The rule is about authority flowing downhill: a writer must carry at least the authority of what
/// it writes.
[[nodiscard]] constexpr bool may_write(SimulationClass writer, SimulationClass target) noexcept {
    if (is_authoritative(target)) {
        return is_authoritative(writer);
    }
    if (target == SimulationClass::Predicted) {
        return writer == SimulationClass::Predicted || is_authoritative(writer);
    }
    // Presentation and Derived: anything may write them. Neither can corrupt authority, because
    // authority cannot read either of them.
    return true;
}

// --- The witness ------------------------------------------------------------------------------

/// The proof a caller carries that it is a system of class `C`. Empty, so it costs nothing.
///
/// It is passed as a value rather than read from a thread-local or a global, because a global
/// "current context" would be readable by anything and settable by anything, which is a runtime
/// convention wearing a type's clothes.
template <SimulationClass C>
struct AccessContext {
    static constexpr SimulationClass value = C;
};

using AuthoritativeContext = AccessContext<SimulationClass::Authoritative>;
using PredictedContext = AccessContext<SimulationClass::Predicted>;
using PersistentContext = AccessContext<SimulationClass::Persistent>;
using PresentationContext = AccessContext<SimulationClass::Presentation>;
using DerivedContext = AccessContext<SimulationClass::Derived>;

/// A field, and the class that decides what may touch it and which of the five mechanisms carry it.
///
/// Layout-transparent: the same size and alignment as `T`, trivially copyable when `T` is, and
/// therefore usable directly as a member of an ECS component without changing the chunk layout.
/// Both properties are asserted below rather than assumed, because the ECS refuses a component that
/// is not trivially relocatable and a wrapper that broke that would be found by a registration
/// failure rather than by a reading of this file.
template <SimulationClass C, class T>
class Classified {
public:
    using value_type = T;
    static constexpr SimulationClass kClass = C;

    constexpr Classified() = default;
    constexpr explicit Classified(const T& value) noexcept : value_(value) {}

    /// Read, given a witness that the reader's class may. **This overload does not exist for a
    /// crossing the firewall forbids**, so the crossing is a compile error at the point of use.
    template <SimulationClass R>
        requires(may_read(R, C))
    [[nodiscard]] constexpr const T& read(AccessContext<R> /*witness*/) const noexcept {
        return value_;
    }

    /// Write, given a witness that the writer's class may.
    template <SimulationClass W>
        requires(may_write(W, C))
    constexpr void write(AccessContext<W> /*witness*/, const T& value) noexcept {
        value_ = value;
    }

    /// The value, with no classification check at all.
    ///
    /// For reflection-driven machinery only — serialization, the state hasher, the editor's
    /// inspector — which must see every field regardless of class and cannot carry a witness
    /// because it does not know the class until it reads it. Named to be long, unlovely and
    /// greppable: `grep -rn bypass_classification src/` is the audit, and a system body that
    /// contains it is a review finding rather than a subtlety.
    [[nodiscard]] constexpr const T& bypass_classification() const noexcept { return value_; }
    [[nodiscard]] constexpr T& bypass_classification() noexcept { return value_; }

private:
    T value_{};
};

template <class T>
using Authoritative = Classified<SimulationClass::Authoritative, T>;
template <class T>
using Predicted = Classified<SimulationClass::Predicted, T>;
template <class T>
using Persistent = Classified<SimulationClass::Persistent, T>;
template <class T>
using Presentation = Classified<SimulationClass::Presentation, T>;
template <class T>
using Derived = Classified<SimulationClass::Derived, T>;

static_assert(sizeof(Authoritative<f32>) == sizeof(f32),
              "Classified<> must not change a field's size: it is a member of ECS components whose "
              "chunk layout is computed from the struct");
static_assert(alignof(Authoritative<f32>) == alignof(f32));
static_assert(std::is_trivially_copyable_v<Authoritative<f32>>,
              "Classified<> must stay trivially copyable, or the ECS will refuse every component "
              "holding one as not trivially relocatable");

// --- The sanctioned crossing ------------------------------------------------------------------

/// A presentation or non-deterministic outcome, captured so that authority may act on it.
///
/// `simulation-and-determinism`: "Where a presentation system's outcome must influence gameplay,
/// the outcome SHALL be captured as an authoritative event or an external result rather than read
/// directly." This is that capture. It is the *only* route from the presentation half to the
/// authoritative half, it records the moment and a reason, and it is a distinct type so that a
/// replay recording external results can find them without being told where they are.
///
/// It is spellable on purpose. Preventing the crossing would prevent the feature; what the
/// requirement asks is that the crossing be a named artefact rather than a read.
template <class T>
class ExternalResult {
public:
    constexpr ExternalResult() = default;

    /// `reason` is a literal naming why this value had to cross — "hit-test from the render
    /// thread", "service latency". It is recorded, so a divergence between two runs can be traced
    /// to a crossing rather than to arithmetic. There is no constructor that omits it.
    constexpr ExternalResult(SimulationPoint at, const T& observed, const char* reason) noexcept
        : value_(observed), reason_(reason), at_(at) {}

    [[nodiscard]] constexpr const T& value() const noexcept { return value_; }
    [[nodiscard]] constexpr const char* reason() const noexcept { return reason_; }
    [[nodiscard]] constexpr SimulationPoint captured_at() const noexcept { return at_; }

    /// The captured value as authoritative state. Legal because the capture *is* the record the
    /// requirement asks for: a recorded external result replays identically, which is what makes it
    /// authoritative rather than merely available.
    [[nodiscard]] constexpr Authoritative<T> as_authoritative() const noexcept {
        return Authoritative<T>(value_);
    }

private:
    T value_{};
    const char* reason_ = "";
    SimulationPoint at_;
};

/// Capture a presentation or non-deterministic observation as an authoritative input.
///
/// The one function in the engine that turns a presentation-classified value into an authoritative
/// one. It takes a witness for the *presentation* side, so it can only be called from code that
/// admits it is on that side, and it names the moment and the reason.
template <SimulationClass C, class T>
[[nodiscard]] constexpr ExternalResult<T> record_external(
    AccessContext<C> witness, SimulationPoint at,
    const Classified<SimulationClass::Presentation, T>& observed, const char* reason) noexcept {
    return ExternalResult<T>(at, observed.read(witness), reason);
}

// --- Deriving a class from the serialization declaration ----------------------------------------

/// The simulation class a reflected field carries when nothing overrides it.
///
/// `simulation-and-determinism` says the classification is "derived from the field classification
/// already defined in `serialization-and-prefabs` and extended for simulation". This is the
/// derivation. The extension — `Predicted` and `Presentation` — has no `PersistenceKind` to come
/// from, because M1's attribute set has no enumerator for it; a field that is one of those two is
/// declared in a `StateSchema` (state_schema.h) instead, which is also where a built-in component
/// with no reflection at all declares its fields. Adding a `SimulationClass` attribute to the
/// generator would remove the second declaration, and that is a change to `src/core/reflect/`.
[[nodiscard]] constexpr SimulationClass class_of(reflect::PersistenceKind kind) noexcept {
    switch (kind) {
        // Authoring and RuntimeState both map to Authoritative, and the shared branch is the
        // point: the two differ in whether an asset update overwrites the value, which is
        // serialization's question, not the simulation's. Both are part of the simulated state.
        case reflect::PersistenceKind::Authoring:
        case reflect::PersistenceKind::RuntimeState:
            return SimulationClass::Authoritative;
        case reflect::PersistenceKind::PersistentState:
            return SimulationClass::Persistent;
        case reflect::PersistenceKind::Derived:
            return SimulationClass::Derived;
    }
    return SimulationClass::Derived;
}

}  // namespace cy::determinism
