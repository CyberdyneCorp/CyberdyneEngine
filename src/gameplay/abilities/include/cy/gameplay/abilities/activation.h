#pragma once
// The activation pipeline, activation identity, prediction and cues. M8.b tasks 4.1 and 4.3.
//
// `gameplay-abilities-and-effects` — "Activation pipeline and structured validation": activation
// proceeds through a defined pipeline, and the specification states it as an ORDER:
//
//     resolve owner and context, check state and tag requirements, check cost, check cooldown,
//     resolve and validate the target, apply the prediction and authority policy, commit the
//     activation, apply effects, and emit cues and events.
//
// "Validation SHALL return a **structured result** — permitted or not, with tagged reasons and
// their data — through the same path the interface, artificial intelligence, and the authority
// use", and "Validation SHALL be callable **without activating**."
//
// ================================================================================================
// THE ORDER IS THE STAGE ENUMERATION, AND IT IS THE COMPILER'S
// ================================================================================================
//
// The stages here are `cy::graph::script::AbilityStage` — the same enumeration the compiled ability
// program's stage table is indexed by. One list, so a graph that authors a cost check and a
// pipeline that runs one cannot disagree about where in the order it happens. `stage_is_check()`
// draws the line the whole design turns on: everything up to `Commit` may run without activating,
// and everything from `Commit` on may not.
//
// ================================================================================================
// A CUE IS PRESENTATION AND CANNOT REACH BACK
// ================================================================================================
//
// "Cues are presentation and sit on the presentation side of the determinism firewall. A cue SHALL
// NOT influence authoritative state." So `emit_cue` appends to a list that nothing in this module
// reads back, cues carry their `SimulationPoint`, and `CueLedger` suppresses a cue whose
// (activation, cue, point) triple has already been emitted — which is what makes a re-simulated
// rollback quiet rather than twice as loud. The requirement's own scenario, "a rolled-back cast
// does not play twice", is that suppression.
//
// ================================================================================================
// AND WHY RANDOMNESS COMES FROM THE ACTIVATION'S IDENTITY
// ================================================================================================
//
// "Randomness SHALL come from a stream derived from **activation identity, ability identity, and
// the session seed**, so that a critical hit or a random duration is reproducible in replay."
// `stream_for()` is that derivation and there is no other way to get a stream here: an ability that
// reached for a global generator would make a replay's critical hits a different set from the
// original's, and nothing would point at why.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/entity.h>
#include <cy/gameplay/abilities/abilities.h>
#include <cy/gameplay/abilities/effects.h>
#include <cy/gameplay/abilities/targeting.h>
#include <cy/gameplay/command.h>
#include <cy/gameplay/random.h>
#include <cy/graph/lower_script.h>

namespace cy::gameplay::abilities {

/// The pipeline's stages, in the specification's order. **The compiled program's own enumeration**
/// — see the header comment.
using AbilityStage = graph::script::AbilityStage;

/// A stable activation identity. Used for reconciliation, for cue suppression, for rollback and
/// for network debugging — the four the requirement names.
struct ActivationId {
    u64 bits = 0;

    [[nodiscard]] bool valid() const noexcept { return bits != 0; }
    friend bool operator==(ActivationId a, ActivationId b) noexcept { return a.bits == b.bits; }
    friend bool operator!=(ActivationId a, ActivationId b) noexcept { return a.bits != b.bits; }
};

/// Why an activation was cancelled. Declared causes, because "cancellation SHALL emit a structured
/// reason".
enum class CancellationCause : u8 {
    None = 0,
    ByOwner,
    ByTag,
    ByDamage,
    ByMovement,
    /// The authority rejected a predicted activation.
    AuthorityRejected,
    Count,
};

const char* cancellation_cause_name(CancellationCause cause) noexcept;

/// One presentation signal. Carries its simulation point so the ledger can suppress it.
struct CueEmission {
    TagId cue = kInvalidTag;
    ActivationId activation;
    determinism::SimulationPoint at;
    ecs::Entity subject;
    /// May it be realised before the authority confirms? Declared, per the requirement.
    bool speculative = false;
};

/// What one activation asked for.
struct ActivationRequest {
    ecs::Entity owner;
    AbilityId ability = kInvalidAbility;
    TargetData target;
    ParticipantId participant;
    ControlSourceId source;
    /// The cue this activation emits on commit. `kInvalidTag` emits none.
    TagId cue = kInvalidTag;
    /// This is a client's speculative run of a predictable ability.
    bool predicted = false;
    /// The activation's identity, when it is being RE-simulated rather than started. A rollback
    /// replays a recorded command with the identity it had, which is what lets the cue ledger
    /// recognise the repeat; leave it null and the pipeline derives a fresh one.
    ActivationId identity;
};

/// One activation, as it happened. The activation timeline's row.
struct ActivationRecord {
    ActivationId id;
    ecs::Entity owner;
    AbilityId ability = kInvalidAbility;
    u64 tick = 0;
    /// The last stage that ran. On a refusal, the stage that refused.
    AbilityStage reached = AbilityStage::ResolveOwner;
    bool permitted = false;
    bool committed = false;
    /// A predicted activation the authority has confirmed.
    bool confirmed = false;
    bool reverted = false;
    CancellationCause cancellation = CancellationCause::None;
    u32 effects_applied = 0;
    u32 cues_emitted = 0;
    /// The first reason it was refused for, with the numbers behind it.
    ValidationReason reason;
};

/// What one `activate` did.
struct ActivationReport {
    ActivationId id;
    ValidationResult validation;
    AbilityStage reached = AbilityStage::ResolveOwner;
    bool committed = false;
    u32 effects_applied = 0;
    u32 cues_emitted = 0;
    u32 cues_suppressed = 0;
};

/// What one bulk activation did. `gameplay-abilities-and-effects`: "ten thousand units activate the
/// same ability in one tick — they SHALL be processed as a batch over their shared program."
struct BatchReport {
    u32 requested = 0;
    u32 committed = 0;
    u32 refused = 0;
    u32 effects_applied = 0;
    /// Heap allocations the batch caused. **Zero on the normal path**, which is the requirement,
    /// and a number rather than a claim.
    u32 allocations = 0;
};

/// The pipeline. One per session; it holds no per-activation object and allocates nothing per
/// activation once its arrays are reserved.
class ActivationPipeline {
public:
    ActivationPipeline(Allocator& allocator, AbilityRegistry& abilities, EffectSystem& effects,
                       AttributeStore& attributes, CostLedger& costs, const EntityTagStore& tags,
                       const TagRegistry& registry, const RelationshipService& relationships,
                       const GameplayRandom& random) noexcept;

    ActivationPipeline(const ActivationPipeline&) = delete;
    ActivationPipeline& operator=(const ActivationPipeline&) = delete;

    void set_target_context(const TargetContext& context) noexcept { target_ = context; }
    void set_target_buffer(const TargetBuffer* buffer) noexcept { buffer_ = buffer; }

    /// Reserve for `activations` records and `cues` cue emissions, so the normal path allocates
    /// nothing.
    [[nodiscard]] Status reserve(u32 activations, u32 cues) noexcept;

    /// The CHECK stages only, in order, stopping at the first refusal. **Nothing is reserved,
    /// nothing is committed and nothing is emitted** — which is what makes this the call an
    /// interface makes for every button and an agent makes for every option.
    ///
    /// ONE COST TO KNOW ABOUT: an ability that carries a compiled graph program allocates a
    /// `ScriptState` here, because the program's register file is per instance and this call has no
    /// instance of its own. Every other path through `validate()` allocates nothing. An interface
    /// polling a hundred graph-backed abilities every frame would want a state pooled per caller;
    /// nothing in the tree does that yet, and pretending otherwise would be worse than saying so.
    [[nodiscard]] ValidationResult validate(const ActivationRequest& request,
                                            i64 tick) const noexcept;

    /// The whole pipeline. Runs the checks through the same `validate()` the interface called, so
    /// the two cannot disagree.
    [[nodiscard]] Expected<ActivationId, Error> activate(const ActivationRequest& request,
                                                         determinism::SimulationPoint at,
                                                         ActivationReport& report) noexcept;

    /// Many owners activating the same ability, over one shared definition. One loop.
    [[nodiscard]] Status activate_batch(AbilityId ability, Span<const ecs::Entity> owners,
                                        const TargetData& target, determinism::SimulationPoint at,
                                        BatchReport& report) noexcept;

    /// The authority's answer to a predicted activation. A rejection reverts the predicted effects
    /// and records the cause; **prediction never bypasses this**.
    [[nodiscard]] Status reconcile(ActivationId activation, bool confirmed) noexcept;

    /// Cancel a committed activation with a declared cause.
    [[nodiscard]] Status cancel(ActivationId activation, CancellationCause cause) noexcept;

    /// The randomness an activation draws from: the session seed, the ability, and the activation.
    [[nodiscard]] RandomStream stream_for(ActivationId activation,
                                          AbilityId ability) const noexcept;

    [[nodiscard]] Span<const CueEmission> cues() const noexcept { return cues_.span(); }
    [[nodiscard]] u32 cues_suppressed() const noexcept { return suppressed_; }
    void clear_cues() noexcept;

    /// The activation timeline: activations by tick, with their validation results, what they
    /// committed and what they emitted.
    [[nodiscard]] Span<const ActivationRecord> timeline() const noexcept { return records_.span(); }
    [[nodiscard]] const ActivationRecord* record(ActivationId activation) const noexcept;

    /// Derive an identity. Exposed because a client and the authority must derive the SAME one for
    /// the same activation, which is what makes reconciliation exact rather than heuristic.
    [[nodiscard]] static ActivationId derive_id(ecs::Entity owner, u32 ability_stable_id,
                                                determinism::SimulationPoint at,
                                                u32 ordinal) noexcept;

private:
    [[nodiscard]] ValidationResult run_checks(const ActivationRequest& request, i64 tick,
                                              AbilityStage& reached) const noexcept;
    [[nodiscard]] Status emit_cue(const CueEmission& cue, u32& emitted, u32& suppressed) noexcept;

    Allocator* allocator_;
    AbilityRegistry* abilities_;
    EffectSystem* effects_;
    AttributeStore* attributes_;
    CostLedger* costs_;
    const EntityTagStore* tags_;
    const TagRegistry* registry_;
    const RelationshipService* relationships_;
    GameplayRandom random_;
    TargetContext target_;
    const TargetBuffer* buffer_ = nullptr;
    Array<ActivationRecord> records_;
    Array<CueEmission> cues_;
    u32 ordinal_ = 0;
    u32 suppressed_ = 0;
};

/// The bridge from a compiled ability program to this module's data.
///
/// `cy::graph::script::compile_ability` produces a program whose external calls are names; this is
/// what resolves them, and it is the ONLY place they are resolved. A program asks
/// `ability.attribute` for a value and `ability.has_tag` for a tag test; it never reaches a store
/// directly, which is what makes the capability audit over a graph mean something.
class AbilityScriptHost final : public graph::script::ScriptHost {
public:
    AbilityScriptHost(const AttributeStore& attributes, const EntityTagStore& tags,
                      const TagRegistry& registry, const AbilityRegistry& abilities,
                      ecs::Entity owner, AbilityId ability, i64 tick) noexcept;

    graph::script::Value call(const graph::script::ExternalRef& callee,
                              Span<const graph::script::Value> arguments) override;
    graph::script::Value query(const graph::script::ExternalRef& query,
                               Span<const graph::script::Value> arguments) override;
    void emit_event(const graph::script::ExternalRef& event,
                    Span<const graph::script::Value> arguments) override;
    void emit_command(const graph::script::ExternalRef& command,
                      Span<const graph::script::Value> arguments) override;
    graph::script::Value get_field(const graph::script::ExternalRef& field,
                                   const graph::script::Value& subject) override;
    void set_field(const graph::script::ExternalRef& field, const graph::script::Value& subject,
                   const graph::script::Value& value) override;
    /// ALWAYS FALSE. A check stage may not wait — `validate_activation` stops before `Commit`, and
    /// an ability that suspended during a requirement check would be an interface that blocked on a
    /// button. The async ability behaviour the specification asks for runs on the COMMITTED side,
    /// where a host that can answer the wait supplies its own `ScriptHost`.
    [[nodiscard]] bool wait_satisfied(const graph::script::SuspendPoint& point) override;

    /// What the program emitted. A CHECK-stage run must leave both at zero, and
    /// `tests/test_activation.cpp` asserts exactly that.
    [[nodiscard]] u32 events() const noexcept { return events_; }
    [[nodiscard]] u32 commands() const noexcept { return commands_; }

private:
    const AttributeStore* attributes_;
    const EntityTagStore* tags_;
    const TagRegistry* registry_;
    const AbilityRegistry* abilities_;
    ecs::Entity owner_;
    AbilityId ability_;
    i64 tick_;
    u32 events_ = 0;
    u32 commands_ = 0;
};

}  // namespace cy::gameplay::abilities
