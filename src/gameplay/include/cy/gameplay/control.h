#pragma once
// Control sources, channels, bindings and entity groups. Task 4.4.2.
//
// `gameplay-framework` — "Control sources and bindings": control is a **control source** — human,
// artificial intelligence, remote peer, replay, script or automation — bound to entities through
// control bindings; a binding names a **channel**; control is **many-to-many**; entity sets are
// addressable as **groups**; and "A binding SHALL NOT be limited to one controller possessing one
// entity."
//
// ================================================================================================
// THE THREE SCENARIOS ARE THREE DIFFERENT SHAPES, AND ONE MODEL HAS TO CARRY ALL OF THEM
// ================================================================================================
//
//   * a player commands two hundred units — ONE source, MANY entities, and the requirement is
//     explicit that this must be one binding to a group rather than two hundred relationships.
//     `gameplay-framework`'s forbidden-patterns list names "Representing a large controlled group
//     as one control relationship per entity" outright.
//   * two players share a tank — MANY sources, ONE entity, on different channels.
//   * an AI stabilises a human's aim — MANY sources, ONE entity, on different channels, and the
//     two sources are of different *kinds*.
//
// A possession model — one controller, one pawn — carries none of the three. That is why there is
// no `possess()` here and no "the controller of this entity": there is `sources_controlling()`,
// which returns several, and `controls()`, which takes a channel.
//
// ================================================================================================
// WHY THE SOURCE IS NOT THE PARTICIPANT
// ================================================================================================
//
// A participant is *who is playing*; a source is *a thing issuing intent*. One participant may hold
// several sources — their own input and an assist AI acting on their behalf — and a replay's source
// belongs to the participant whose commands it is replaying. Collapsing the two would make
// "who owns this command" and "what produced it" the same field, and `gameplay-framework` requires
// provenance to be diagnostic-only while the participant is what validation checks.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/handle.h>
#include <cy/core/values/name.h>
#include <cy/ecs/entity.h>
#include <cy/gameplay/context.h>

namespace cy::gameplay {

/// What is issuing intent. **The simulation never sees this**: it reaches validation as nothing and
/// execution as nothing, and travels on a command only as provenance. See command.h.
enum class ControlSourceKind : u8 {
    Human = 0,
    ArtificialIntelligence,
    RemotePeer,
    Replay,
    Script,
    Automation,
    Count,
};

const char* control_source_kind_name(ControlSourceKind kind) noexcept;

CY_HANDLE_TAG(ControlSource);
using ControlSourceId = Handle<ControlSourceTag>;

CY_HANDLE_TAG(EntityGroup);
using GroupId = Handle<EntityGroupTag>;

/// The channels the engine names. A project adds its own by interning a `Name`; there is no
/// enumeration to extend, which is what "or a project-defined channel" requires.
namespace channels {
[[nodiscard]] Name primary() noexcept;
[[nodiscard]] Name movement() noexcept;
[[nodiscard]] Name weapons() noexcept;
[[nodiscard]] Name camera() noexcept;
[[nodiscard]] Name turret() noexcept;
[[nodiscard]] Name command() noexcept;
}  // namespace channels

struct ControlSourceRecord {
    ControlSourceId id;
    ControlSourceKind kind = ControlSourceKind::Human;
    /// Whose intent this source carries. Several sources may name one participant — a human's own
    /// input and the assist AI acting for them.
    ParticipantId participant;
    Name debug_name;
};

/// One binding. Either an entity or a group, never both: a binding to a group is the *whole point*
/// of groups, and a binding that carried both would let a caller express "these two hundred and
/// also that one" as one relationship whose membership two different mechanisms decide.
struct ControlBinding {
    ControlSourceId source;
    Name channel;
    ecs::Entity entity;
    GroupId group;

    [[nodiscard]] bool is_group() const noexcept { return !group.is_null(); }
};

/// Sources, groups and bindings.
///
/// Not thread-safe: binding changes happen at the commit boundary, like every other structural
/// change. Queries are const and are what validation calls.
class ControlRegistry {
public:
    static constexpr u32 kMaxGroups = 64;

    explicit ControlRegistry(Allocator& allocator) noexcept;

    ControlRegistry(const ControlRegistry&) = delete;
    ControlRegistry& operator=(const ControlRegistry&) = delete;

    [[nodiscard]] Expected<ControlSourceId, Error> create_source(ControlSourceKind kind,
                                                                 ParticipantId participant,
                                                                 Name debug_name) noexcept;
    void destroy_source(ControlSourceId source_id) noexcept;
    [[nodiscard]] const ControlSourceRecord* source(ControlSourceId id) const noexcept;
    [[nodiscard]] u32 source_count() const noexcept { return static_cast<u32>(sources_.size()); }

    [[nodiscard]] Expected<GroupId, Error> create_group(Name debug_name) noexcept;
    [[nodiscard]] Status add_to_group(GroupId group, ecs::Entity entity) noexcept;
    void remove_from_group(GroupId group, ecs::Entity entity) noexcept;
    [[nodiscard]] u32 group_size(GroupId group) const noexcept;
    [[nodiscard]] ecs::Entity group_member(GroupId group, u32 index) const noexcept;

    /// Bind a source to one entity on a channel.
    [[nodiscard]] Status bind_entity(ControlSourceId source_id, Name channel,
                                     ecs::Entity entity) noexcept;
    /// Bind a source to a **group** on a channel. One binding, however many members — see the
    /// header comment and `gameplay-framework`'s forbidden-patterns list.
    [[nodiscard]] Status bind_group(ControlSourceId source_id, Name channel,
                                    GroupId group) noexcept;
    void unbind(ControlSourceId source_id, Name channel) noexcept;

    /// Does `source` control `entity` on `channel`? The question validation asks, and the reason
    /// group membership is resolved here rather than by the caller.
    [[nodiscard]] bool controls(ControlSourceId source_id, ecs::Entity entity,
                                Name channel) const noexcept;

    /// Every source controlling `entity`, on any channel. Several is the ordinary answer.
    [[nodiscard]] u32 sources_controlling(ecs::Entity entity, ControlSourceId* out,
                                          u32 capacity) const noexcept;

    /// The entities a source drives on a channel, expanding groups. `capacity` may be zero, in
    /// which case the return value is the count and nothing is written — which is how a caller
    /// sizes a buffer without a second data structure.
    [[nodiscard]] u32 controlled_entities(ControlSourceId source_id, Name channel, ecs::Entity* out,
                                          u32 capacity) const noexcept;

    [[nodiscard]] u32 binding_count() const noexcept { return static_cast<u32>(bindings_.size()); }
    [[nodiscard]] const ControlBinding& binding_at(u32 index) const noexcept {
        return bindings_[index];
    }

private:
    struct Group {
        GroupId id;
        Name debug_name;
        Array<ecs::Entity> members;
    };

    [[nodiscard]] const Group* find_group(GroupId group) const noexcept;
    [[nodiscard]] Group* find_group(GroupId group) noexcept;

    Allocator* allocator_;
    Array<ControlSourceRecord> sources_;
    Array<ControlBinding> bindings_;
    Array<Group> groups_;
    u32 next_source_ = 1;
    u32 next_group_ = 1;
};

}  // namespace cy::gameplay
