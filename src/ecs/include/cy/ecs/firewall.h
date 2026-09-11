#pragma once
// THE DETERMINISM FIREWALL, AND THE ANSWER TO "WHERE IS IT ENFORCED". M8.c tasks 1.1 and 1.2.
//
// ================================================================================================
// 1.1 — WHERE THE FIREWALL IS ENFORCED, AND WHY IT IS HERE RATHER THAN AT THE COMMAND ORIGIN
// ================================================================================================
//
// `vfx-system` and `ml-inference` each state the rule from the producer's side:
//
//   "VFX ... SHALL NOT be a source of truth for: damage, hit detection, entity creation or
//    destruction, physics state, network-replicated values, or any value consumed by deterministic
//    simulation."
//
//   "Model output SHALL NOT drive authoritative gameplay state ... unless the session is pinned."
//
// **Neither names the point at which it is enforced**, and a rule enforced nowhere is a convention.
// M8.b built two candidates and M8.c's delta requires one of them to be picked:
//
//   A. `gameplay-framework`'s command origin — `cy::gameplay::CommandStream`, which every command
//      passes through and which already carries a `Provenance`.
//   B. **The ECS write path** — this file.
//
// **B is the enforcement point, and A is rejected for three reasons that are each sufficient.**
//
//   1. **A command is the simulation's INPUT, not its write.** A firewall on the command origin
//      catches only a VFX system that was already well behaved enough to submit a command. The
//      failure the requirement is about is the other one: a collision readback that reaches into
//      the world and pokes a health value. That write never touches a `CommandStream` and the
//      command origin cannot see it. The delta spec's words are "a point every authoritative write
//      passes through" — commands are not that point; component storage is.
//
//   2. **`gameplay-framework` forbids the check A would need.** "Provenance SHALL NOT affect
//      validation, ordering, or execution", and `sequencing-and-cinematics` requires that "the
//      simulation SHALL NOT be able to distinguish" a sequence-issued command from any other.
//      Making a VFX-issued command fail validation is exactly a provenance that affects validation.
//      Enforcing here leaves that requirement untouched: a sequence-issued or VFX-issued *command*
//      is still indistinguishable, because the firewall never looks at commands.
//
//   3. **`vfx-system` states the diagnostic in terms of components, not commands**: "Development
//      builds SHALL detect and report attempts to write replicated or physics-owned COMPONENTS from
//      VFX-driven code paths." A component is an ECS concept. The check belongs where the component
//      is.
//
// **THE DOORS, ENUMERATED, BECAUSE "EVERY WRITE PASSES THROUGH" IS A CLAIM AND NOT A HOPE.** These
// are the ways a caller obtains the ability to change an entity's authoritative state, and each of
// them consults this firewall:
//
//   `World::get_mut`           the pointer-to-value door.        WritePath::GetMut
//   `QueryChunk::write`        the bulk column door.             WritePath::QueryWrite
//   `World::set_sparse` and `remove_sparse`                      WritePath::SparseWrite / Remove
//   `World::add`, `remove`, `set_shared`                         WritePath::Structural
//   `World::create*`, `destroy*`, `instantiate`                  WritePath::EntityLifetime
//   `World::set_parent`                                          WritePath::Relationship
//   `CommandBuffer::record`    the DEFERRED door — checked at RECORD time, because the flush runs
//                              under the simulation's own origin and would otherwise launder a
//                              VFX-driven structural change into an authoritative one.
//                                                                WritePath::DeferredRecord
//   `Snapshot::restore`        the WHOLESALE door. `Snapshot` is a friend of `World` and writes
//                              archetype rows directly, so the check is spelled at the call rather
//                              than inherited.                    WritePath::EntityLifetime
//
// `World::get` and `QueryChunk::read` are not doors: they hand back const. Reading gameplay state
// is what `vfx-system` explicitly permits — "VFX MAY read gameplay and world state".
//
// ================================================================================================
// WHAT THE ORIGIN IS, AND WHY IT IS THREAD-LOCAL RATHER THAN A FIELD ON THE WORLD
// ================================================================================================
//
// The question the firewall asks is "what code path is executing *right now*", and a code path is a
// property of a thread's call stack. Two systems of one ECS stage run concurrently over one world
// by construction, so a single field on the world would be answered wrongly for one of them the
// moment a VFX readback ran beside a gameplay system. A `WriteScope` is therefore a stack-scoped,
// thread-local frame: the VFX or inference code that opens one is the only code the firewall
// restricts, and every other thread continues at `WriteOrigin::Simulation`.
//
// THE DEFAULT IS PERMISSIVE, DELIBERATELY. Nothing in the engine changes behaviour until a caller
// opens a restricted scope; a default of "restricted" would mean every existing system had to
// declare itself before it could write, which is a migration and not a firewall. `vfx-system` and
// `ml-inference` are the two subsystems whose specifications say they must declare, and they are
// the two that open scopes.
//
// AND `Classified<>` IS THE OTHER HALF, NOT A COMPETITOR. `<cy/core/determinism/classification.h>`
// makes the illegal *read* unspellable at compile time for state that adopted its wrapper. This is
// the runtime half, and it exists because classification is opt-in per field while a component's
// storage is reachable through the doors above whether or not anybody adopted a wrapper. The
// two answer different questions: `Classified<>` asks "may this class of code name this value";
// this asks "may the code running on this thread change this component".

#include <cy/core/base/assert.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/ecs/component.h>
#include <cy/ecs/entity.h>

#include <atomic>

namespace cy::ecs {

/// What kind of code is executing. The two that may write authoritative state are named first so
/// that the permissive test is a comparison against a small number.
enum class WriteOrigin : u8 {
    /// The authoritative simulation, and everything that has not said otherwise. Unrestricted.
    Simulation = 0,
    /// A model result from a session that is **pinned** — fixed backend, fixed precision, and a
    /// configuration the model asset declares verified reproducible. `ml-inference` permits exactly
    /// this to drive authoritative state, and the cook-time gate
    /// (`<cy/gameplay/cook_firewall.h>`) is what makes the claim checkable rather than asserted.
    PinnedInference = 1,
    /// A VFX-driven code path: a GPU readback, a collision event, a spawn callback.
    Vfx = 2,
    /// A model result from a session that is not pinned.
    Inference = 3,
    /// Animation, camera, audio, interface. Restricted for the same reason VFX is —
    /// `simulation-and-determinism`'s "presentation-only systems SHALL NOT feed back into
    /// authoritative state".
    Presentation = 4,
    Count = 5,
};

const char* write_origin_name(WriteOrigin origin) noexcept;

/// May code of this origin change authoritative state at all?
[[nodiscard]] constexpr bool origin_may_write_authoritative(WriteOrigin origin) noexcept {
    return origin == WriteOrigin::Simulation || origin == WriteOrigin::PinnedInference;
}

/// What a component is, as far as the firewall is concerned.
///
/// `Presentation` is the default because a component nobody classified is a component the firewall
/// has no claim about, and refusing those would be a guess wearing a rule's clothes. The other
/// three are the categories `vfx-system` names by hand: "network-replicated values", "physics
/// state", and "any value consumed by deterministic simulation".
enum class ComponentAuthority : u8 {
    Presentation = 0,
    /// Consumed by the deterministic simulation and covered by the state hash.
    Authoritative = 1,
    /// Authoritative and on the wire. `vfx-system`'s "network-replicated values".
    Replicated = 2,
    /// Authoritative and owned by the physics step. `vfx-system`'s "physics state".
    PhysicsOwned = 3,
    Count = 4,
};

const char* component_authority_name(ComponentAuthority authority) noexcept;

[[nodiscard]] constexpr bool authority_is_guarded(ComponentAuthority authority) noexcept {
    return authority != ComponentAuthority::Presentation;
}

/// Which door the refusal happened at. Reported, because "the path" is one of the three
/// things `vfx-system` asks a development build to name.
enum class WritePath : u8 {
    GetMut = 0,
    QueryWrite = 1,
    Structural = 2,
    SparseWrite = 3,
    SparseRemove = 4,
    EntityLifetime = 5,
    Relationship = 6,
    DeferredRecord = 7,
    Count = 8,
};

const char* write_path_name(WritePath path) noexcept;

/// One refused write, as a development build reports it: **the writer, the component and the
/// path**, which is `vfx-system`'s list in `vfx-system`'s order.
struct FirewallViolation {
    WriteOrigin origin = WriteOrigin::Simulation;
    /// The name the `WriteScope` was opened with — "vfx.collision-readback", "ml.threat-estimate".
    const char* writer = "";
    WritePath path = WritePath::GetMut;
    ComponentTypeId component = kInvalidComponent;
    const char* component_name = "";
    ComponentAuthority authority = ComponentAuthority::Presentation;
    Entity entity;
    /// Which refusal this was, counting from one. Survives the ring wrapping.
    u64 ordinal = 0;
};

/// The origin the calling thread is running under, and the name it declared.
[[nodiscard]] WriteOrigin current_write_origin() noexcept;
[[nodiscard]] const char* current_writer() noexcept;

/// Declare, for the duration of a scope, that the code running on this thread is VFX-driven,
/// inference-driven or presentation-only.
///
/// Nests: a VFX callback that calls into presentation code restores the VFX frame on the way out.
class WriteScope {
public:
    WriteScope(WriteOrigin origin, const char* writer) noexcept;
    ~WriteScope();

    WriteScope(const WriteScope&) = delete;
    WriteScope& operator=(const WriteScope&) = delete;
    WriteScope(WriteScope&&) = delete;
    WriteScope& operator=(WriteScope&&) = delete;

private:
    WriteOrigin previous_origin_;
    const char* previous_writer_;
};

/// What `declare_from_reflection` derived, and what it could not.
///
/// `guarded_by_replication` and `guarded_by_persistence` are the two derivations; `underived` is
/// the number of registered components the reflection said nothing about, and it is reported rather
/// than hidden for the reason `WorldHashReport::subjects_undeclared` is: a firewall that silently
/// guards a tenth of the world is worse than one that says how much it guards.
struct AuthorityDerivationReport {
    u32 components_examined = 0;
    u32 guarded_by_replication = 0;
    u32 guarded_by_persistence = 0;
    u32 already_declared = 0;
    u32 underived = 0;
};

/// The enforcement point. One per world; reached as `World::firewall()`.
class WriteFirewall {
public:
    /// How many refusals are retained for inspection. Fixed and small: a refusal is a defect, and
    /// the first few are the ones a developer reads. The count is exact regardless.
    static constexpr u32 kMaxRecordedViolations = 32;

    WriteFirewall() = default;

    WriteFirewall(const WriteFirewall&) = delete;
    WriteFirewall& operator=(const WriteFirewall&) = delete;

    // --- Declaration --------------------------------------------------------------------------

    /// Declare what a component is. Idempotent for the same value; raising a component's authority
    /// is allowed, lowering it is refused — a component that was once replicated does not become
    /// presentation because a second caller said so.
    [[nodiscard]] Status declare(ComponentTypeId component, ComponentAuthority authority) noexcept;

    /// Derive authority from what the components already say about themselves.
    ///
    /// Two derivations, both conservative on purpose:
    ///   * a field carrying the `Replicated` attribute makes its component `Replicated`;
    ///   * a field that **explicitly declares** a `Persistence` whose simulation class is
    ///     authoritative (`determinism::class_of`) makes its component `Authoritative`.
    ///
    /// A field that declares neither derives nothing. `FieldAttributes::persistence` defaults to
    /// `Authoring`, which maps to `Authoritative`, so treating an undeclared field as authoritative
    /// would guard every reflected component in the engine on the strength of a default value —
    /// which is a guess, and would refuse writes the specifications permit. `PhysicsOwned` is not
    /// derivable at all: nothing in reflection says "the physics step owns this", so the physics
    /// module declares it by hand.
    [[nodiscard]] Status declare_from_reflection(const ComponentRegistry& registry,
                                                 AuthorityDerivationReport& report) noexcept;

    [[nodiscard]] ComponentAuthority authority_of(ComponentTypeId component) const noexcept {
        return (component < kMaxComponentTypes) ? authority_[component]
                                                : ComponentAuthority::Presentation;
    }
    [[nodiscard]] bool guards(ComponentTypeId component) const noexcept {
        return authority_is_guarded(authority_of(component));
    }
    [[nodiscard]] u32 guarded_count() const noexcept { return guarded_count_; }

    // --- The check ----------------------------------------------------------------------------

    /// Decide whether a write may proceed, and record and report it when it may not.
    ///
    /// `component` may be `kInvalidComponent` for the lifetime and relationship paths, which are
    /// refused for a restricted origin whatever components are involved: `vfx-system` names "entity
    /// creation or destruction" itself, and a `Parent` edge is hashed
    /// (`<cy/ecs/state_schema.h>`), so re-parenting is an authoritative write.
    [[nodiscard]] bool admit(WritePath path, ComponentTypeId component, Entity entity,
                             const ComponentRegistry& registry) noexcept;

    // --- Arming -------------------------------------------------------------------------------

    [[nodiscard]] bool armed() const noexcept { return armed_; }

    /// **Disable the enforcement point.** `vfx-system`'s delta requires the firewall's own test to
    /// be a negative control — "WHEN the enforcement point is disabled THEN the firewall's own test
    /// SHALL fail" — and this is how a test disables it without a rebuild.
    ///
    /// Named to be long, unlovely and greppable, exactly as `bypass_classification()` is: a
    /// production call site containing it is a review finding rather than a subtlety. `reason` is
    /// retained and reported so a capture taken with the firewall down says so.
    void disarm_for_negative_control(const char* reason) noexcept;
    void rearm() noexcept;
    [[nodiscard]] const char* disarm_reason() const noexcept { return disarm_reason_; }

    // --- What it refused ----------------------------------------------------------------------

    [[nodiscard]] u64 refusals() const noexcept {
        return refusals_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] u64 refusals_by(WriteOrigin origin) const noexcept {
        const u32 index = static_cast<u32>(origin);
        return (index < static_cast<u32>(WriteOrigin::Count))
                   ? by_origin_[index].load(std::memory_order_relaxed)
                   : 0;
    }
    [[nodiscard]] u32 violation_count() const noexcept;
    [[nodiscard]] const FirewallViolation& violation(u32 index) const noexcept {
        CY_ASSERT_MSG(index < kMaxRecordedViolations, "violation index out of range");
        return violations_[index];
    }
    /// The most recent refusal, or a default-constructed one when there has been none.
    [[nodiscard]] FirewallViolation last_violation() const noexcept;
    void clear_violations() noexcept;

private:
    void record(WriteOrigin origin, WritePath path, ComponentTypeId component,
                ComponentAuthority authority, Entity entity,
                const ComponentRegistry& registry) noexcept;

    ComponentAuthority authority_[kMaxComponentTypes] = {};
    u32 guarded_count_ = 0;
    bool armed_ = true;
    const char* disarm_reason_ = "";

    std::atomic<u64> refusals_{0};
    std::atomic<u64> by_origin_[static_cast<u32>(WriteOrigin::Count)] = {};
    FirewallViolation violations_[kMaxRecordedViolations] = {};
};

}  // namespace cy::ecs
