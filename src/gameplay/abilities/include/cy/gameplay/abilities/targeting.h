#pragma once
// Targeting: acquisition, target data, and validation. M8.b task 4.3.
//
// `gameplay-abilities-and-effects` — "Targeting": targeting is first-class, with "**target data**
// that is typed, serialisable, and networkable, covering: self, an entity, an entity set, a point,
// a direction, an area, a cone, a line, a volume, and a world region."
//
// "**Target acquisition and target validation SHALL be distinct.** Acquisition selects candidates —
// explicitly, from a cursor, from an aim ray, by proximity, by area query, by chaining, or by an
// agent's choice; validation checks range, line of sight, relationship, required and forbidden
// tags, reachability, and resource availability. Validation SHALL be compiled where the rules are
// static, and SHALL run identically for the interface, artificial intelligence, and the authority."
//
// ================================================================================================
// WHY THE TWO ARE SEPARATE FUNCTIONS AND NOT TWO HALVES OF ONE
// ================================================================================================
//
// Because the same target is acquired four ways and validated once. A player's cursor, an agent's
// proximity search, a chained bounce and a replayed command all produce a `TargetData`; every one
// of them then faces `validate_target`. Merging them would give the agent its own validation, which
// is the "separate agent-only ability path" the specification forbids by name — and the first time
// the two disagree, an agent attempts something a player cannot.
//
// ================================================================================================
// AND WHY THIS MODULE HAS NO WORLD
// ================================================================================================
//
// `gameplay-framework` requires the framework to be "fully functional with no renderer, no audio,
// no interface, and no GPU". Positions, line of sight and reachability are the world's answers, so
// they arrive through `TargetContext`'s function pointers. A dedicated server supplies real ones; a
// test supplies three lines. Neither links a renderer to ask where something is.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/entity.h>
#include <cy/gameplay/tags.h>
#include <cy/gameplay/teams.h>
#include <cy/gameplay/validation.h>

namespace cy::gameplay::abilities {

/// What a target IS. Typed, so a command carries a target rather than a variant.
enum class TargetKind : u8 {
    None = 0,
    SelfTarget,
    Entity,
    EntitySet,
    Point,
    Direction,
    Area,
    Cone,
    Line,
    Volume,
    Region,
    Count,
};

const char* target_kind_name(TargetKind kind) noexcept;

/// How candidates were selected. Diagnostics and planning; validation never reads it, because a
/// target acquired by an agent must face exactly what a player's faces.
enum class Acquisition : u8 {
    Explicit = 0,
    Cursor,
    AimRay,
    Proximity,
    AreaQuery,
    Chain,
    AgentChoice,
    Count,
};

/// Target data. **Trivially copyable and self-contained**, so it travels in a command payload, in a
/// replay and on the wire without marshalling. An entity SET is the one shape that cannot fit, and
/// it is held as a range into a `TargetBuffer` the caller owns.
struct TargetData {
    TargetKind kind = TargetKind::None;
    Acquisition acquisition = Acquisition::Explicit;
    ecs::Entity entity;
    Vec3 point;
    Vec3 direction;
    /// The radius of an area, the half-angle of a cone in radians, the width of a line.
    f32 radius = 0.0F;
    f32 angle = 0.0F;
    f32 length = 0.0F;
    /// Where an `EntitySet`'s members are in the caller's `TargetBuffer`.
    u32 first_member = 0;
    u32 member_count = 0;
    /// A world region's name, for `TargetKind::Region`.
    Name region;
};

/// The members of every entity-set target in flight. One array, so a set is a range and not an
/// allocation per activation.
class TargetBuffer {
public:
    explicit TargetBuffer(Allocator& allocator) noexcept : members_(allocator) {}

    TargetBuffer(const TargetBuffer&) = delete;
    TargetBuffer& operator=(const TargetBuffer&) = delete;

    [[nodiscard]] Expected<TargetData, Error> make_set(Span<const ecs::Entity> members,
                                                       Acquisition acquisition) noexcept;
    [[nodiscard]] Span<const ecs::Entity> members_of(const TargetData& target) const noexcept;
    void clear() noexcept { members_.clear(); }
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(members_.size()); }

private:
    Array<ecs::Entity> members_;
};

/// Where things are and what can see what. Supplied by the host, so this module has no world.
struct TargetContext {
    /// The position of an entity. Returns false when the entity is not resident.
    bool (*position)(ecs::Entity entity, Vec3& out, void* user) noexcept = nullptr;
    /// Is there line of sight from `from` to `to`?
    bool (*line_of_sight)(const Vec3& from, const Vec3& to, void* user) noexcept = nullptr;
    /// Can the instigator reach the point — navigation's answer.
    bool (*reachable)(ecs::Entity instigator, const Vec3& to, void* user) noexcept = nullptr;
    void* user = nullptr;
};

/// The static half of targeting, compiled from the definition. Every field is a compact query.
struct TargetRules {
    f32 max_range = 0.0F;
    bool require_line_of_sight = false;
    bool require_reachable = false;
    /// Which relationships a target may stand in. A bit per `Relationship` enumerator; zero means
    /// every relationship is allowed, which is what an ability that heals or harms anything wants.
    u8 allowed_relationships = 0;
    TagId required_tag = kInvalidTag;
    TagId forbidden_tag = kInvalidTag;
    /// A target set larger than this is refused rather than truncated.
    u32 max_targets = 0;
};

[[nodiscard]] constexpr u8 relationship_bit(Relationship relationship) noexcept {
    return static_cast<u8>(1U << static_cast<u32>(relationship));
}

/// What acquisition was asked for. Acquisition reads the world; validation does not.
struct AcquisitionRequest {
    Acquisition how = Acquisition::Explicit;
    ecs::Entity instigator;
    /// For `Explicit`: the entity chosen. For `AimRay`: the origin and direction are `origin` and
    /// `direction`.
    ecs::Entity chosen;
    Vec3 origin;
    Vec3 direction;
    f32 radius = 0.0F;
    /// The candidates a proximity or area acquisition selects from. The host supplies them; this
    /// module has no spatial index of its own — `gameplay::InteractionRegistry` has one, and a
    /// project with a world uses the world's.
    Span<const ecs::Entity> candidates;
};

/// Select candidates. **Does not validate**: the result goes to `validate_target` next, whoever
/// asked.
[[nodiscard]] Expected<TargetData, Error> acquire_target(const AcquisitionRequest& request,
                                                         const TargetContext& context,
                                                         TargetBuffer& buffer) noexcept;

/// Check a target against the rules. The SAME call for the interface, an agent and the authority —
/// which is the requirement, and the reason the answer is a `ValidationResult` with reasons rather
/// than a bool.
[[nodiscard]] ValidationResult validate_target(const TargetData& target, const TargetRules& rules,
                                               ecs::Entity instigator, const TargetContext& context,
                                               const RelationshipService& relationships,
                                               const EntityTagStore& tags,
                                               const TagRegistry& registry,
                                               const TargetBuffer& buffer) noexcept;

}  // namespace cy::gameplay::abilities
