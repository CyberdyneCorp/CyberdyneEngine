// Targeting: acquisition, target data, and validation. M8.b task 4.3.

#include <cy/gameplay/abilities/targeting.h>

namespace cy::gameplay::abilities {
namespace {

/// The point a target denotes, for the range and sight tests. False when there is not one — a
/// direction has no position of its own and is tested from the instigator.
[[nodiscard]] bool point_of(const TargetData& target, const TargetContext& context,
                            Vec3& out) noexcept {
    switch (target.kind) {
        case TargetKind::Entity:
            return context.position != nullptr &&
                   context.position(target.entity, out, context.user);
        case TargetKind::Point:
        case TargetKind::Area:
        case TargetKind::Cone:
        case TargetKind::Line:
        case TargetKind::Volume:
            out = target.point;
            return true;
        case TargetKind::None:
        case TargetKind::SelfTarget:
        case TargetKind::EntitySet:
        case TargetKind::Direction:
        case TargetKind::Region:
        case TargetKind::Count:
            break;
    }
    return false;
}

/// One entity's part of the validation, so a set and a single target face the same rules.
void validate_one(ecs::Entity subject, const TargetRules& rules, ecs::Entity instigator,
                  const TargetContext& context, const RelationshipService& relationships,
                  const EntityTagStore& tags, const TagRegistry& registry,
                  ValidationResult& result) noexcept {
    if (rules.allowed_relationships != 0) {
        const Relationship relationship = relationships.between(relationships.team_of(instigator),
                                                                relationships.team_of(subject));
        if ((rules.allowed_relationships & relationship_bit(relationship)) == 0) {
            ValidationReason reason;
            reason.tag = ReasonTag::ProjectDefined;
            reason.detail = Name::intern("relationship");
            reason.subject = subject;
            result.reject(reason);
        }
    }
    if (rules.required_tag != kInvalidTag && !tags.has(registry, subject, rules.required_tag)) {
        ValidationReason reason;
        reason.tag = ReasonTag::ProjectDefined;
        reason.detail = Name::intern("required_tag");
        reason.subject = subject;
        result.reject(reason);
    }
    if (rules.forbidden_tag != kInvalidTag && tags.has(registry, subject, rules.forbidden_tag)) {
        ValidationReason reason;
        reason.tag = ReasonTag::ProjectDefined;
        reason.detail = Name::intern("forbidden_tag");
        reason.subject = subject;
        result.reject(reason);
    }
    if (rules.max_range > 0.0F && context.position != nullptr) {
        Vec3 from;
        Vec3 to;
        if (context.position(instigator, from, context.user) &&
            context.position(subject, to, context.user)) {
            const f32 separation = distance(from, to);
            if (separation > rules.max_range) {
                ValidationReason reason;
                reason.tag = ReasonTag::OutOfRange;
                reason.required = rules.max_range;
                reason.available = separation;
                reason.subject = subject;
                result.reject(reason);
            } else if (rules.require_line_of_sight && context.line_of_sight != nullptr &&
                       !context.line_of_sight(from, to, context.user)) {
                ValidationReason reason;
                reason.tag = ReasonTag::ProjectDefined;
                reason.detail = Name::intern("line_of_sight");
                reason.subject = subject;
                result.reject(reason);
            }
        }
    }
}

}  // namespace

const char* target_kind_name(TargetKind kind) noexcept {
    switch (kind) {
        case TargetKind::None:
            return "None";
        case TargetKind::SelfTarget:
            return "SelfTarget";
        case TargetKind::Entity:
            return "Entity";
        case TargetKind::EntitySet:
            return "EntitySet";
        case TargetKind::Point:
            return "Point";
        case TargetKind::Direction:
            return "Direction";
        case TargetKind::Area:
            return "Area";
        case TargetKind::Cone:
            return "Cone";
        case TargetKind::Line:
            return "Line";
        case TargetKind::Volume:
            return "Volume";
        case TargetKind::Region:
            return "Region";
        case TargetKind::Count:
            break;
    }
    return "None";
}

Expected<TargetData, Error> TargetBuffer::make_set(Span<const ecs::Entity> members,
                                                   Acquisition acquisition) noexcept {
    TargetData target;
    target.kind = TargetKind::EntitySet;
    target.acquisition = acquisition;
    target.first_member = static_cast<u32>(members_.size());
    target.member_count = static_cast<u32>(members.size());
    if (Status appended = members_.append(members); !appended) {
        return make_unexpected(appended.error());
    }
    return target;
}

Span<const ecs::Entity> TargetBuffer::members_of(const TargetData& target) const noexcept {
    if (target.kind != TargetKind::EntitySet ||
        target.first_member + target.member_count > members_.size()) {
        return {};
    }
    return {members_.data() + target.first_member, target.member_count};
}

Expected<TargetData, Error> acquire_target(const AcquisitionRequest& request,
                                           const TargetContext& context,
                                           TargetBuffer& buffer) noexcept {
    TargetData target;
    target.acquisition = request.how;
    switch (request.how) {
        case Acquisition::Explicit:
        case Acquisition::AgentChoice:
            if (!request.chosen.valid()) {
                return make_unexpected(
                    Error{ErrorCode::InvalidArgument, "no entity was chosen", 0});
            }
            target.kind = TargetKind::Entity;
            target.entity = request.chosen;
            return target;
        case Acquisition::Cursor:
            target.kind = TargetKind::Point;
            target.point = request.origin;
            return target;
        case Acquisition::AimRay: {
            // An aim ray is a DIRECTION, not a camera. "the camera SHALL not be authoritative": the
            // ray travels in the command and the authority resolves it, so what the client's view
            // happened to be does not decide the outcome.
            target.kind = TargetKind::Direction;
            target.point = request.origin;
            target.direction = request.direction;
            target.length = request.radius;
            return target;
        }
        case Acquisition::Proximity:
        case Acquisition::AreaQuery:
        case Acquisition::Chain: {
            ecs::Entity selected[64] = {};
            u32 count = 0;
            Vec3 origin = request.origin;
            if (request.how != Acquisition::AreaQuery && context.position != nullptr &&
                request.instigator.valid()) {
                (void)context.position(request.instigator, origin, context.user);
            }
            for (const ecs::Entity candidate : request.candidates) {
                if (count >= 64) {
                    break;
                }
                Vec3 position;
                if (context.position == nullptr ||
                    !context.position(candidate, position, context.user)) {
                    continue;
                }
                if (distance(origin, position) > request.radius) {
                    continue;
                }
                selected[count++] = candidate;
            }
            return buffer.make_set(Span<const ecs::Entity>(selected, count), request.how);
        }
        case Acquisition::Count:
            break;
    }
    return make_unexpected(Error{ErrorCode::InvalidArgument, "no such acquisition", 0});
}

ValidationResult validate_target(const TargetData& target, const TargetRules& rules,
                                 ecs::Entity instigator, const TargetContext& context,
                                 const RelationshipService& relationships,
                                 const EntityTagStore& tags, const TagRegistry& registry,
                                 const TargetBuffer& buffer) noexcept {
    ValidationResult result;
    if (target.kind == TargetKind::None) {
        result.reject(ReasonTag::TargetInvalid);
        return result;
    }
    if (target.kind == TargetKind::SelfTarget) {
        // Self is always in range of self and always in sight of self. Testing it would ask the
        // world a question whose answer is a tautology.
        return result;
    }
    if (target.kind == TargetKind::EntitySet) {
        const Span<const ecs::Entity> members = buffer.members_of(target);
        if (rules.max_targets != 0 && members.size() > rules.max_targets) {
            ValidationReason reason;
            reason.tag = ReasonTag::ProjectDefined;
            reason.detail = Name::intern("too_many_targets");
            reason.required = static_cast<f32>(rules.max_targets);
            reason.available = static_cast<f32>(members.size());
            result.reject(reason);
            return result;
        }
        for (const ecs::Entity member : members) {
            validate_one(member, rules, instigator, context, relationships, tags, registry, result);
            if (!result.permitted()) {
                return result;
            }
        }
        return result;
    }
    if (target.kind == TargetKind::Entity) {
        if (!target.entity.valid()) {
            result.reject(ReasonTag::TargetInvalid);
            return result;
        }
        validate_one(target.entity, rules, instigator, context, relationships, tags, registry,
                     result);
        return result;
    }

    // A placed target: range and sight against its point.
    Vec3 point;
    if (!point_of(target, context, point)) {
        return result;
    }
    if (rules.max_range > 0.0F && context.position != nullptr) {
        Vec3 from;
        if (context.position(instigator, from, context.user)) {
            const f32 separation = distance(from, point);
            if (separation > rules.max_range) {
                ValidationReason reason;
                reason.tag = ReasonTag::OutOfRange;
                reason.required = rules.max_range;
                reason.available = separation;
                result.reject(reason);
                return result;
            }
            if (rules.require_line_of_sight && context.line_of_sight != nullptr &&
                !context.line_of_sight(from, point, context.user)) {
                ValidationReason reason;
                reason.tag = ReasonTag::ProjectDefined;
                reason.detail = Name::intern("line_of_sight");
                result.reject(reason);
                return result;
            }
        }
    }
    if (rules.require_reachable && context.reachable != nullptr &&
        !context.reachable(instigator, point, context.user)) {
        ValidationReason reason;
        reason.tag = ReasonTag::ProjectDefined;
        reason.detail = Name::intern("unreachable");
        result.reject(reason);
    }
    return result;
}

}  // namespace cy::gameplay::abilities
