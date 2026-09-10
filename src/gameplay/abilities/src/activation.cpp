// The activation pipeline, activation identity, prediction and cues. M8.b tasks 4.1 and 4.3.

#include <cy/gameplay/abilities/activation.h>

namespace cy::gameplay::abilities {
namespace {

constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;

[[nodiscard]] u64 mix(u64 hash, u64 value) noexcept {
    for (u32 byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8U)) & 0xFFULL;
        hash *= kFnvPrime;
    }
    return hash;
}

}  // namespace

const char* cancellation_cause_name(CancellationCause cause) noexcept {
    switch (cause) {
        case CancellationCause::None:
            return "None";
        case CancellationCause::ByOwner:
            return "ByOwner";
        case CancellationCause::ByTag:
            return "ByTag";
        case CancellationCause::ByDamage:
            return "ByDamage";
        case CancellationCause::ByMovement:
            return "ByMovement";
        case CancellationCause::AuthorityRejected:
            return "AuthorityRejected";
        case CancellationCause::Count:
            break;
    }
    return "None";
}

ActivationPipeline::ActivationPipeline(Allocator& allocator, AbilityRegistry& abilities,
                                       EffectSystem& effects, AttributeStore& attributes,
                                       CostLedger& costs, const EntityTagStore& tags,
                                       const TagRegistry& registry,
                                       const RelationshipService& relationships,
                                       const GameplayRandom& random) noexcept
    : allocator_(&allocator),
      abilities_(&abilities),
      effects_(&effects),
      attributes_(&attributes),
      costs_(&costs),
      tags_(&tags),
      registry_(&registry),
      relationships_(&relationships),
      random_(random),
      records_(allocator),
      cues_(allocator) {}

Status ActivationPipeline::reserve(u32 activations, u32 cues) noexcept {
    if (Status reserved = records_.reserve(activations); !reserved) {
        return reserved;
    }
    return cues_.reserve(cues);
}

ActivationId ActivationPipeline::derive_id(ecs::Entity owner, u32 ability_stable_id,
                                           determinism::SimulationPoint at, u32 ordinal) noexcept {
    // A pure function of things both peers have. Nothing here is a pointer, a counter of this
    // process's own, or a wall clock, so a client and the authority derive the same identity for
    // the same activation — which is what "activation identity SHALL determine which the
    // authority's response refers to" requires.
    u64 bits = mix(kFnvOffset, owner.bits());
    bits = mix(bits, ability_stable_id);
    bits = mix(bits, at.epoch.value);
    bits = mix(bits, at.tick);
    bits = mix(bits, ordinal);
    return ActivationId{bits == 0 ? 1 : bits};
}

ValidationResult ActivationPipeline::run_checks(const ActivationRequest& request, i64 tick,
                                                AbilityStage& reached) const noexcept {
    ValidationResult result;

    // 1 — RESOLVE OWNER AND CONTEXT.
    reached = AbilityStage::ResolveOwner;
    if (!request.owner.valid() || request.ability >= abilities_->definition_count()) {
        result.reject(ReasonTag::TargetInvalid);
        return result;
    }
    if (!abilities_->has_ability(request.owner, request.ability)) {
        ValidationReason reason;
        reason.tag = ReasonTag::ProjectDefined;
        reason.detail = Name::intern("not_granted");
        reason.subject = request.owner;
        result.reject(reason);
        return result;
    }
    const AbilityDefinition& definition = abilities_->definition(request.ability);

    // 2 — CHECK STATE AND TAG REQUIREMENTS.
    reached = AbilityStage::CheckState;
    if (definition.required_tag != kInvalidTag &&
        !tags_->has(*registry_, request.owner, definition.required_tag)) {
        ValidationReason reason;
        reason.tag = ReasonTag::ProjectDefined;
        reason.detail = Name::intern("required_tag");
        reason.subject = request.owner;
        result.reject(reason);
        return result;
    }
    if (definition.forbidden_tag != kInvalidTag &&
        tags_->has(*registry_, request.owner, definition.forbidden_tag)) {
        ValidationReason reason;
        reason.tag = ReasonTag::ProjectDefined;
        reason.detail = Name::intern("forbidden_tag");
        reason.subject = request.owner;
        result.reject(reason);
        return result;
    }

    // 3 — CHECK COST. Against what is AVAILABLE, which subtracts what this tick already reserved.
    reached = AbilityStage::CheckCost;
    for (const Cost& cost : abilities_->costs_of(request.ability)) {
        if (cost.kind == CostKind::Attribute) {
            const f32 available = costs_->available(request.owner, cost.attribute);
            if (available < cost.amount) {
                ValidationReason reason;
                reason.tag = ReasonTag::InsufficientResource;
                reason.detail = attributes_ != nullptr ? Name::intern("attribute") : Name{};
                reason.required = cost.amount;
                reason.available = available;
                reason.subject = request.owner;
                result.reject(reason);
                return result;
            }
        } else if (cost.kind == CostKind::Charge) {
            const AbilityState* state = abilities_->state_of(request.owner, request.ability);
            const f32 charges = state != nullptr ? static_cast<f32>(state->charges) : 0.0F;
            if (charges < cost.amount) {
                ValidationReason reason;
                reason.tag = ReasonTag::InsufficientResource;
                reason.detail = Name::intern("charges");
                reason.required = cost.amount;
                reason.available = charges;
                reason.subject = request.owner;
                result.reject(reason);
                return result;
            }
        }
    }

    // 4 — CHECK COOLDOWN. A tick comparison; nothing counts down.
    reached = AbilityStage::CheckCooldown;
    if (!abilities_->ready(request.owner, request.ability, tick)) {
        ValidationReason reason;
        reason.tag = ReasonTag::Cooldown;
        reason.required = static_cast<f32>(abilities_->ready_tick(request.owner, request.ability));
        reason.available = static_cast<f32>(tick);
        reason.subject = request.owner;
        result.reject(reason);
        return result;
    }

    // 5 — RESOLVE AND VALIDATE THE TARGET. The same call an agent's target faces.
    reached = AbilityStage::ResolveTarget;
    if (request.target.kind != TargetKind::None && buffer_ != nullptr) {
        const ValidationResult target_result =
            validate_target(request.target, definition.targeting, request.owner, target_,
                            *relationships_, *tags_, *registry_, *buffer_);
        if (!target_result.permitted()) {
            return target_result;
        }
    }

    // 6 — THE PREDICTION AND AUTHORITY POLICY. A predicted run of an ability that declares no
    // prediction is refused HERE, before anything is committed: "Prediction SHALL NOT bypass
    // authoritative validation."
    reached = AbilityStage::PredictionPolicy;
    if (request.predicted && definition.prediction != PredictionPolicy::ClientPredicted &&
        definition.prediction != PredictionPolicy::DeterministicLockstep) {
        ValidationReason reason;
        reason.tag = ReasonTag::ProjectDefined;
        reason.detail = Name::intern("not_predictable");
        reason.subject = request.owner;
        result.reject(reason);
        return result;
    }

    // 7 — THE COMPILED PROGRAM'S OWN CHECK STAGES, if the ability has one. `validate_activation`
    // stops before `Commit` whatever the graph says, which is why running it here is safe.
    if (definition.program != nullptr) {
        AbilityScriptHost host(*attributes_, *tags_, *registry_, *abilities_, request.owner,
                               request.ability, tick);
        graph::script::ScriptState state(*allocator_, definition.program->program());
        auto verdict = graph::script::validate_activation(*definition.program, state, host);
        if (verdict.has_value() && !verdict.value().allowed) {
            ValidationReason reason;
            reason.tag = ReasonTag::ProjectDefined;
            reason.detail = verdict.value().reason;
            reason.subject = request.owner;
            result.reject(reason);
            reached = verdict.value().failed;
            return result;
        }
    }
    return result;
}

ValidationResult ActivationPipeline::validate(const ActivationRequest& request,
                                              i64 tick) const noexcept {
    AbilityStage reached = AbilityStage::ResolveOwner;
    return run_checks(request, tick, reached);
}

Status ActivationPipeline::emit_cue(const CueEmission& cue, u32& emitted,
                                    u32& suppressed) noexcept {
    // THE LEDGER. A cue already emitted for this (activation, cue, simulation point) is suppressed
    // rather than repeated, which is what makes a re-simulated rollback quiet.
    for (const CueEmission& existing : cues_) {
        if (existing.activation == cue.activation && existing.cue == cue.cue &&
            existing.at.tick == cue.at.tick && existing.at.epoch.value == cue.at.epoch.value) {
            ++suppressed;
            ++suppressed_;
            return ok();
        }
    }
    if (Status pushed = cues_.push_back(cue); !pushed) {
        return pushed;
    }
    ++emitted;
    return ok();
}

Expected<ActivationId, Error> ActivationPipeline::activate(const ActivationRequest& request,
                                                           determinism::SimulationPoint at,
                                                           ActivationReport& report) noexcept {
    report = ActivationReport{};
    AbilityStage reached = AbilityStage::ResolveOwner;
    report.validation = run_checks(request, static_cast<i64>(at.tick), reached);
    report.reached = reached;

    const bool known = request.ability < abilities_->definition_count();
    const u32 stable = known ? abilities_->definition(request.ability).stable_id : 0;
    // A re-simulation supplies the identity it recorded; a fresh activation derives one. Both
    // paths produce a value both peers can compute, which is what reconciliation needs.
    const ActivationId id = request.identity.valid()
                                ? request.identity
                                : derive_id(request.owner, stable, at, ordinal_++);
    report.id = id;

    ActivationRecord record;
    record.id = id;
    record.owner = request.owner;
    record.ability = request.ability;
    record.tick = at.tick;
    record.reached = reached;
    record.permitted = report.validation.permitted();
    record.reason = report.validation.first();

    if (!report.validation.permitted()) {
        costs_->release(id.bits);
        if (Status pushed = records_.push_back(record); !pushed) {
            return make_unexpected(pushed.error());
        }
        return id;
    }

    const AbilityDefinition& definition = abilities_->definition(request.ability);

    // RESERVE, THEN COMMIT. Two activations in one tick cannot both spend the last of a resource,
    // because the second one's `available()` has already had the first's reservation subtracted.
    for (const Cost& cost : abilities_->costs_of(request.ability)) {
        if (cost.kind != CostKind::Attribute) {
            continue;
        }
        if (Status reserved = costs_->reserve(request.owner, cost.attribute, cost.amount, id.bits);
            !reserved) {
            costs_->release(id.bits);
            ValidationReason reason;
            reason.tag = ReasonTag::InsufficientResource;
            reason.required = cost.amount;
            reason.available = costs_->available(request.owner, cost.attribute);
            reason.subject = request.owner;
            report.validation.reject(reason);
            record.permitted = false;
            record.reason = reason;
            record.reached = AbilityStage::CheckCost;
            report.reached = AbilityStage::CheckCost;
            if (Status pushed = records_.push_back(record); !pushed) {
                return make_unexpected(pushed.error());
            }
            return id;
        }
    }

    // 7 — COMMIT THE ACTIVATION.
    report.reached = AbilityStage::Commit;
    record.reached = AbilityStage::Commit;
    if (Status committed = costs_->commit(id.bits); !committed) {
        costs_->release(id.bits);
        return make_unexpected(committed.error());
    }
    if (Status cooled =
            abilities_->start_cooldown(request.owner, request.ability, static_cast<i64>(at.tick));
        !cooled) {
        return make_unexpected(cooled.error());
    }
    record.committed = true;
    report.committed = true;

    // 8 — APPLY EFFECTS. To the target set when there is one, which is what makes an area effect
    // one resolution and many applications rather than many activations.
    report.reached = AbilityStage::ApplyEffects;
    record.reached = AbilityStage::ApplyEffects;
    ecs::Entity direct[1] = {request.owner};
    Span<const ecs::Entity> targets(direct, 1);
    if (request.target.kind == TargetKind::Entity && request.target.entity.valid()) {
        direct[0] = request.target.entity;
    } else if (request.target.kind == TargetKind::EntitySet && buffer_ != nullptr) {
        targets = buffer_->members_of(request.target);
    }
    for (const EffectDefId effect : abilities_->effects_of(request.ability)) {
        for (const ecs::Entity subject : targets) {
            ApplyReport applied;
            if (Status result = effects_->apply(effect, subject, request.owner,
                                                static_cast<i64>(at.tick), tags_, applied);
                !result) {
                return make_unexpected(result.error());
            }
            if (applied.outcome != ApplyOutcome::RefusedImmune &&
                applied.outcome != ApplyOutcome::RefusedRequirement) {
                ++report.effects_applied;
            }
        }
    }
    record.effects_applied = report.effects_applied;

    // 9 — EMIT CUES AND EVENTS.
    report.reached = AbilityStage::EmitCues;
    record.reached = AbilityStage::EmitCues;
    if (request.cue != kInvalidTag) {
        CueEmission cue;
        cue.cue = request.cue;
        cue.activation = id;
        cue.at = at;
        cue.subject = request.owner;
        // A predicted activation's cue is speculative; an authority's is not.
        cue.speculative =
            request.predicted && definition.prediction == PredictionPolicy::ClientPredicted;
        if (Status emitted = emit_cue(cue, report.cues_emitted, report.cues_suppressed); !emitted) {
            return make_unexpected(emitted.error());
        }
    }
    record.cues_emitted = report.cues_emitted;
    if (Status pushed = records_.push_back(record); !pushed) {
        return make_unexpected(pushed.error());
    }
    return id;
}

Status ActivationPipeline::activate_batch(AbilityId ability, Span<const ecs::Entity> owners,
                                          const TargetData& target, determinism::SimulationPoint at,
                                          BatchReport& report) noexcept {
    report = BatchReport{};
    report.requested = static_cast<u32>(owners.size());
    // ONE SHARED DEFINITION, ONE LOOP. The capacities below are the "no heap allocation per
    // activation" claim made checkable: the count is what actually grew, and a reserved pipeline
    // reports zero.
    const usize records_before = records_.capacity();
    const usize cues_before = cues_.capacity();
    for (const ecs::Entity owner : owners) {
        ActivationRequest request;
        request.owner = owner;
        request.ability = ability;
        request.target = target;
        ActivationReport one;
        auto activated = activate(request, at, one);
        if (!activated) {
            return make_unexpected(activated.error());
        }
        if (one.committed) {
            ++report.committed;
            report.effects_applied += one.effects_applied;
        } else {
            ++report.refused;
        }
    }
    report.allocations = (records_.capacity() != records_before ? 1U : 0U) +
                         (cues_.capacity() != cues_before ? 1U : 0U);
    return ok();
}

Status ActivationPipeline::reconcile(ActivationId activation, bool confirmed) noexcept {
    for (ActivationRecord& record : records_) {
        if (record.id != activation) {
            continue;
        }
        record.confirmed = confirmed;
        if (confirmed) {
            return ok();
        }
        // A REJECTED PREDICTION IS UNDONE. The effects it applied are removed and the cause is
        // recorded, so the rejection is reportable rather than a state nobody can explain.
        record.reverted = true;
        record.cancellation = CancellationCause::AuthorityRejected;
        u32 instances[64] = {};
        // BY SOURCE, not by target: an activation's effects land on whoever it targeted, and it is
        // the activation that was rejected.
        const u32 count =
            effects_->applied_by(record.owner, static_cast<i64>(record.tick), instances, 64);
        const u32 kept = count < 64 ? count : 64;
        for (u32 index = 0; index < kept; ++index) {
            (void)effects_->remove(instances[index]);
        }
        return ok();
    }
    return make_unexpected(Error{ErrorCode::NotFound, "no such activation", 0});
}

Status ActivationPipeline::cancel(ActivationId activation, CancellationCause cause) noexcept {
    for (ActivationRecord& record : records_) {
        if (record.id != activation) {
            continue;
        }
        record.cancellation = cause;
        return ok();
    }
    return make_unexpected(Error{ErrorCode::NotFound, "no such activation", 0});
}

RandomStream ActivationPipeline::stream_for(ActivationId activation,
                                            AbilityId ability) const noexcept {
    const u32 stable =
        ability < abilities_->definition_count() ? abilities_->definition(ability).stable_id : 0;
    const StreamId root = determinism::stream_id("ability");
    return random_.keyed(determinism::substream(root, stable), activation.bits);
}

void ActivationPipeline::clear_cues() noexcept {
    cues_.clear();
    suppressed_ = 0;
}

const ActivationRecord* ActivationPipeline::record(ActivationId activation) const noexcept {
    for (const ActivationRecord& row : records_) {
        if (row.id == activation) {
            return &row;
        }
    }
    return nullptr;
}

AbilityScriptHost::AbilityScriptHost(const AttributeStore& attributes, const EntityTagStore& tags,
                                     const TagRegistry& registry, const AbilityRegistry& abilities,
                                     ecs::Entity owner, AbilityId ability, i64 tick) noexcept
    : attributes_(&attributes),
      tags_(&tags),
      registry_(&registry),
      abilities_(&abilities),
      owner_(owner),
      ability_(ability),
      tick_(tick) {}

graph::script::Value AbilityScriptHost::call(const graph::script::ExternalRef& callee,
                                             Span<const graph::script::Value> arguments) {
    (void)callee;
    (void)arguments;
    return graph::script::Value{};
}

graph::script::Value AbilityScriptHost::query(const graph::script::ExternalRef& query,
                                              Span<const graph::script::Value> arguments) {
    // The names a compiled ability program may ask for. A program that asks for anything else gets
    // zero rather than a store it was not granted — which is what makes the graph's capability
    // audit an enforceable statement rather than documentation.
    const std::string_view name = query.name.text();
    if (name == "ability.attribute" && !arguments.empty()) {
        const auto attribute = static_cast<AttributeId>(arguments[0].integer);
        return graph::script::Value::from_float(attributes_->current(owner_, attribute));
    }
    if (name == "ability.has_tag" && !arguments.empty()) {
        const auto tag = static_cast<TagId>(arguments[0].integer);
        return graph::script::Value::from_bool(tags_->has(*registry_, owner_, tag));
    }
    if (name == "ability.ready") {
        return graph::script::Value::from_bool(abilities_->ready(owner_, ability_, tick_));
    }
    if (name == "ability.charges") {
        const AbilityState* state = abilities_->state_of(owner_, ability_);
        return graph::script::Value::from_int(state != nullptr ? state->charges : 0);
    }
    if (name == "ability.tick") {
        return graph::script::Value::from_int(tick_);
    }
    return graph::script::Value{};
}

void AbilityScriptHost::emit_event(const graph::script::ExternalRef& event,
                                   Span<const graph::script::Value> arguments) {
    (void)event;
    (void)arguments;
    ++events_;
}

void AbilityScriptHost::emit_command(const graph::script::ExternalRef& command,
                                     Span<const graph::script::Value> arguments) {
    (void)command;
    (void)arguments;
    ++commands_;
}

graph::script::Value AbilityScriptHost::get_field(const graph::script::ExternalRef& field,
                                                  const graph::script::Value& subject) {
    (void)field;
    (void)subject;
    return graph::script::Value{};
}

void AbilityScriptHost::set_field(const graph::script::ExternalRef& field,
                                  const graph::script::Value& subject,
                                  const graph::script::Value& value) {
    (void)field;
    (void)subject;
    (void)value;
}

bool AbilityScriptHost::wait_satisfied(const graph::script::SuspendPoint& point) {
    (void)point;
    return false;
}

}  // namespace cy::gameplay::abilities
