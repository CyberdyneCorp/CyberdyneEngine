// Gameplay inspection. M8.b task 3.2.

#include <cy/gameplay/diagnostics.h>

#include <algorithm>

namespace cy::gameplay {

EntityReport inspect_entity(ecs::Entity entity, const OwnershipRegistry& ownership,
                            const ControlRegistry& control,
                            const RelationshipService& relationships, const EntityTagStore& tags,
                            const CommandStream& commands) noexcept {
    EntityReport report;
    report.entity = entity;
    report.owner = ownership.resolve_owner(entity);
    report.stored_owner = ownership.owner(entity);
    report.authority = ownership.authority(entity);
    report.team = relationships.team_of(entity);
    report.capability_mask = commands.capabilities(entity);

    ControlSourceId sources[EntityReport::kMaxControllers] = {};
    const u32 found = control.sources_controlling(entity, sources, EntityReport::kMaxControllers);
    const u32 kept = std::min(found, EntityReport::kMaxControllers);
    for (u32 index = 0; index < kept; ++index) {
        ControllerReport& row = report.controllers[report.controller_count];
        row.source = sources[index];
        if (const ControlSourceRecord* record = control.source(sources[index]); record != nullptr) {
            row.kind = record->kind;
            row.participant = record->participant;
        }
        // The channel this source drives the entity on. A source may hold several bindings; the
        // first that names this entity is the one an inspector shows, and the count above says
        // whether there are more.
        for (u32 slot = 0; slot < control.binding_count(); ++slot) {
            const ControlBinding& binding = control.binding_at(slot);
            if (binding.source != sources[index]) {
                continue;
            }
            if (control.controls(sources[index], entity, binding.channel)) {
                row.channel = binding.channel;
                break;
            }
        }
        ++report.controller_count;
    }

    report.affiliation_count = std::min(
        relationships.affiliations_of(entity, report.affiliations, EntityReport::kMaxAffiliations),
        EntityReport::kMaxAffiliations);

    if (const TagSet* set = tags.tags_of(entity); set != nullptr) {
        for (const TagId tag : set->span()) {
            if (report.tag_count >= EntityReport::kMaxTags) {
                break;
            }
            report.tags[report.tag_count++] = tag;
        }
    }
    return report;
}

u32 command_timeline(const CommandStream& commands, u64 first_tick, u64 last_tick,
                     ControlSourceId source, TimelineEntry* out, u32 capacity) noexcept {
    const CommandLog& log = commands.log();
    u32 found = 0;
    for (u32 index = 0; index < log.size(); ++index) {
        const Command& command = log.at(index);
        if (command.tick < first_tick || command.tick > last_tick) {
            continue;
        }
        if (!source.is_null() && command.source != source) {
            continue;
        }
        if (out != nullptr && found < capacity) {
            TimelineEntry& row = out[found];
            row.tick = command.tick;
            row.type = command.type;
            row.type_name = command.type < commands.type_count()
                                ? commands.declaration(command.type).name
                                : Name{};
            row.participant = command.participant;
            row.source = command.source;
            row.provenance = command.provenance;
            row.target = command.target;
            row.sequence = command.sequence;
        }
        ++found;
    }
    return found;
}

u32 rejection_reports(const CommandStream& commands, RejectionReport* out, u32 capacity) noexcept {
    u32 found = 0;
    for (u32 index = 0; index < commands.rejection_count(); ++index) {
        const CommandStream::Rejection& rejection = commands.rejection(index);
        if (out != nullptr && found < capacity) {
            RejectionReport& row = out[found];
            row.tick = rejection.command.tick;
            row.type = rejection.command.type;
            row.type_name = rejection.command.type < commands.type_count()
                                ? commands.declaration(rejection.command.type).name
                                : Name{};
            row.participant = rejection.command.participant;
            row.target = rejection.command.target;
            row.reason = rejection.result.first();
            row.reason_count = rejection.result.reason_count();
        }
        ++found;
    }
    return found;
}

}  // namespace cy::gameplay
