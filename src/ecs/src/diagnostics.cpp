// The ECS's counters, on the one trace. Task 2.12.
//
// THE CLASSIFICATIONS, AND WHY. Every field here is Public: they are counts, ratios and the names
// of engine and game types — the identifiers a build is made of. Nothing here is a filesystem path,
// a user name or anything derived from one; the memory layer's `allocation_site` is the field in
// this engine that is Sensitive, and it is a source path. A component or system name is a compiled
// identifier and is what a shared capture has to contain to be readable at all.

#include <cy/ecs/diagnostics.h>

#include <cy/core/diagnostics/field.h>
#include <cy/core/diagnostics/log.h>
#include <cy/core/diagnostics/trace.h>

namespace cy::ecs {
namespace {

CY_TRACE_CATEGORY(category, "ecs")
CY_LOG_CATEGORY(log_category, "ecs")

CY_TRACE_NAME(world_event, "ecs.world")
CY_TRACE_NAME(system_event, "ecs.system")
CY_TRACE_NAME(query_event, "ecs.query")
CY_TRACE_NAME(thrash_event, "ecs.archetype-thrash")

CY_TRACE_FIELD(world_name, string, cy::Privacy::Public)
CY_TRACE_FIELD(archetypes, u64, cy::Privacy::Public)
CY_TRACE_FIELD(chunks, u64, cy::Privacy::Public)
CY_TRACE_FIELD(entities, u64, cy::Privacy::Public)
CY_TRACE_FIELD(fill_ratio, f64, cy::Privacy::Public)
CY_TRACE_FIELD(committed_bytes, bytes, cy::Privacy::Public)
CY_TRACE_FIELD(structural_changes, u64, cy::Privacy::Public)
CY_TRACE_FIELD(archetype_transitions, u64, cy::Privacy::Public)
CY_TRACE_FIELD(refused_during_iteration, u64, cy::Privacy::Public)

CY_TRACE_FIELD(system_name, string, cy::Privacy::Public)
CY_TRACE_FIELD(stage, string, cy::Privacy::Public)
CY_TRACE_FIELD(last_ns, duration_ns, cy::Privacy::Public)
CY_TRACE_FIELD(total_ns, duration_ns, cy::Privacy::Public)
CY_TRACE_FIELD(runs, u64, cy::Privacy::Public)
CY_TRACE_FIELD(commands_recorded, u64, cy::Privacy::Public)

CY_TRACE_FIELD(query_name, string, cy::Privacy::Public)
CY_TRACE_FIELD(matched_archetypes, u64, cy::Privacy::Public)
CY_TRACE_FIELD(chunks_visited, u64, cy::Privacy::Public)
CY_TRACE_FIELD(chunks_skipped, u64, cy::Privacy::Public)
CY_TRACE_FIELD(entities_visited, u64, cy::Privacy::Public)

CY_TRACE_FIELD(component_name, string, cy::Privacy::Public)
CY_TRACE_FIELD(entity_index, id, cy::Privacy::Public)
CY_TRACE_FIELD(transitions, u64, cy::Privacy::Public)

[[nodiscard]] diag::FieldValue text_field(diag::FieldId field, const char* text) noexcept {
    u32 length = 0;
    while (text != nullptr && text[length] != '\0' && length < diag::kMaxTextBytesPerRecord) {
        ++length;
    }
    return diag::field_text(field, (text == nullptr) ? "" : text, length);
}

}  // namespace

void EcsDiagnostics::report_world() noexcept {
    const WorldStats stats = world_->stats();
    const diag::FieldValue fields[] = {
        text_field(world_name(), world_->name()),
        diag::field_u64(archetypes(), stats.archetypes),
        diag::field_u64(chunks(), stats.chunks),
        diag::field_u64(entities(), stats.entities),
        diag::field_f64(fill_ratio(), stats.fill_ratio),
        diag::field_u64(committed_bytes(), stats.committed_bytes),
        diag::field_u64(structural_changes(), stats.structural_changes),
        diag::field_u64(archetype_transitions(), stats.archetype_transitions),
        diag::field_u64(refused_during_iteration(), stats.refused_during_iteration),
    };
    diag::trace_instant(world_event(), category(), diag::Channel::Verbose, fields,
                        static_cast<u32>(sizeof(fields) / sizeof(fields[0])));
}

void EcsDiagnostics::report_systems(const Schedule& schedule) noexcept {
    Array<SystemProfile> profiles(world_->allocator());
    if (!schedule.collect_profiles(profiles)) {
        return;
    }
    for (const SystemProfile& profile : profiles) {
        const diag::FieldValue fields[] = {
            text_field(world_name(), world_->name()),
            text_field(system_name(), profile.name),
            text_field(stage(), stage_name(profile.stage)),
            diag::field_u64(last_ns(), profile.last_ns),
            diag::field_u64(total_ns(), profile.total_ns),
            diag::field_u64(runs(), profile.runs),
            diag::field_u64(commands_recorded(), profile.commands_recorded),
        };
        diag::trace_instant(system_event(), category(), diag::Channel::Verbose, fields,
                            static_cast<u32>(sizeof(fields) / sizeof(fields[0])));
    }
}

void EcsDiagnostics::report_query(const char* name, const QueryStats& stats) noexcept {
    // The world is named on every record: with more than one world alive — the editor's and play
    // mode's — a query statistic that does not say which world it came from is unattributable.
    const diag::FieldValue fields[] = {
        text_field(world_name(), world_->name()),
        text_field(query_name(), name),
        diag::field_u64(matched_archetypes(), stats.matched_archetypes),
        diag::field_u64(chunks_visited(), stats.chunks_visited),
        diag::field_u64(chunks_skipped(), stats.chunks_skipped),
        diag::field_u64(entities_visited(), stats.entities_visited),
    };
    diag::trace_instant(query_event(), category(), diag::Channel::Verbose, fields,
                        static_cast<u32>(sizeof(fields) / sizeof(fields[0])));
}

ThrashReport EcsDiagnostics::check_thrash(u64 now_ns) noexcept {
    if (window_started_ns_ == 0) {
        // Opening the first window clears whatever the world accumulated during setup. A rate is
        // measured over a window, so the window has to start empty or the first report would
        // include every archetype change made before anyone was watching.
        window_started_ns_ = now_ns;
        world_->reset_transition_counters();
        return ThrashReport{};
    }
    if (now_ns - window_started_ns_ < policy_.window_ns) {
        return ThrashReport{};
    }
    window_started_ns_ = now_ns;

    const World::TransitionSample sample = world_->busiest_entity();
    world_->reset_transition_counters();

    last_ = ThrashReport{};
    if (sample.transitions < policy_.transitions_per_window) {
        return last_;
    }
    last_.entity = sample.entity;
    last_.component = sample.component;
    last_.transitions = sample.transitions;
    last_.component_name = world_->components().registered(sample.component)
                               ? world_->components().info(sample.component).name
                               : "";

    const diag::FieldValue fields[] = {
        text_field(component_name(), last_.component_name),
        diag::field_u64(entity_index(), last_.entity.index()),
        diag::field_u64(transitions(), last_.transitions),
    };
    // Important, not Verbose: a thrash report is a finding about the program, and dropping it under
    // buffer pressure would drop the one record that explains the frame time beside it.
    diag::trace_instant(thrash_event(), category(), diag::Channel::Important, fields,
                        static_cast<u32>(sizeof(fields) / sizeof(fields[0])));

    // The advice `ecs-core` asks for. The message is an identifier the viewer resolves, not a
    // format string, so the suggestion is in the identifier and the specifics are the fields.
    CY_LOG(log_category(), diag::LogLevel::Warning,
           "ecs.archetype-thrash: this component toggles often enough to be worth declaring sparse",
           text_field(component_name(), last_.component_name),
           diag::field_u64(entity_index(), last_.entity.index()),
           diag::field_u64(transitions(), last_.transitions));
    return last_;
}

}  // namespace cy::ecs
