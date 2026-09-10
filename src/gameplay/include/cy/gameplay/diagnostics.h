#pragma once
// Gameplay inspection: the entity report, the command timeline, and the rule debugger. M8.b
// task 3.2.
//
// `gameplay-framework` — "Gameplay diagnostics": inspection covers "the session and its phase,
// participants, players, teams and their relationships, ownership, control bindings and channels,
// capabilities, gameplay tags, active features, time domains, and pending timers". Selecting an
// entity reports "its owner, its controllers and their channels, its team and affiliations, its
// capabilities, and its tags". A **command timeline** shows commands by tick and source, and a
// **rule debugger** shows rejected commands with their structured reasons and the data behind them.
//
// ================================================================================================
// WHY THIS IS A READER AND NOT A PANEL
// ================================================================================================
//
// The engine's diagnostics live where the data is; the editor draws them. So this file produces
// values — `EntityReport`, `TimelineEntry`, `Rejection` — and knows nothing about a window. That
// keeps `gameplay-framework`'s "Headless operation" true of the diagnostics too: a dedicated server
// can print the rule debugger's answer into a log without linking an interface.
//
// AND WHY THE REJECTION READER IS NOT A SECOND STORE. `CommandStream` already keeps its rejections
// with their `ValidationResult`s, because it is the thing that produced them. A debugger that kept
// its own copy would be a second answer to "why was this rejected", and the first day the two
// disagree is the day somebody trusts the wrong one.

#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/entity.h>
#include <cy/gameplay/command.h>
#include <cy/gameplay/control.h>
#include <cy/gameplay/indexes.h>
#include <cy/gameplay/ownership.h>
#include <cy/gameplay/tags.h>
#include <cy/gameplay/teams.h>

namespace cy::gameplay {

/// One controller of one entity, and the channel it drives.
struct ControllerReport {
    ControlSourceId source;
    ControlSourceKind kind = ControlSourceKind::Human;
    ParticipantId participant;
    Name channel;
};

/// What selecting an entity says. Owner, controllers, team, affiliations, capabilities and tags —
/// distinctly, because conflating any two of them is on the forbidden-patterns list.
struct EntityReport {
    ecs::Entity entity;
    Owner owner;
    /// The owner as stored, which is `Inherited` for a part. Reported beside the resolved one so an
    /// inspector can see that the part is inheriting rather than holding a stale copy.
    Owner stored_owner;
    NetworkAuthority authority = NetworkAuthority::Server;
    TeamId team = kNoTeam;
    u32 capability_mask = 0;
    u32 controller_count = 0;
    static constexpr u32 kMaxControllers = 8;
    ControllerReport controllers[kMaxControllers] = {};
    u32 affiliation_count = 0;
    static constexpr u32 kMaxAffiliations = 8;
    Affiliation affiliations[kMaxAffiliations] = {};
    u32 tag_count = 0;
    static constexpr u32 kMaxTags = 16;
    TagId tags[kMaxTags] = {};
};

/// Everything the entity inspector needs, in one call.
[[nodiscard]] EntityReport inspect_entity(ecs::Entity entity, const OwnershipRegistry& ownership,
                                          const ControlRegistry& control,
                                          const RelationshipService& relationships,
                                          const EntityTagStore& tags,
                                          const CommandStream& commands) noexcept;

/// One row of the command timeline: a command, when it was committed, and who produced it.
struct TimelineEntry {
    u64 tick = 0;
    CommandTypeId type = kInvalidCommandType;
    Name type_name;
    ParticipantId participant;
    ControlSourceId source;
    Provenance provenance;
    ecs::Entity target;
    u32 sequence = 0;
};

/// The committed command log as a timeline, oldest first, optionally filtered by tick and source.
///
/// `kInvalidCommandType` as `type` and a null `source` mean "everything", so the unfiltered call is
/// the same call.
[[nodiscard]] u32 command_timeline(const CommandStream& commands, u64 first_tick, u64 last_tick,
                                   ControlSourceId source, TimelineEntry* out,
                                   u32 capacity) noexcept;

/// One rejected command with the structured reason and the values behind it. The rule debugger's
/// row — read straight out of `CommandStream`, never copied. See the header comment.
struct RejectionReport {
    u64 tick = 0;
    CommandTypeId type = kInvalidCommandType;
    Name type_name;
    ParticipantId participant;
    ecs::Entity target;
    ValidationReason reason;
    u32 reason_count = 0;
};

[[nodiscard]] u32 rejection_reports(const CommandStream& commands, RejectionReport* out,
                                    u32 capacity) noexcept;

}  // namespace cy::gameplay
