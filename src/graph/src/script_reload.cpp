// SPDX-License-Identifier: MIT
// Hot reload: moving an instance's state between two compiled programs. See
// cy/graph/script_reload.h.

#include <cy/graph/script_reload.h>

#include <cy/graph/event_script.h>
#include <cy/graph/script_debug.h>

#include <cstdio>

namespace cy::graph::script {
namespace {

/// Whether `program` carries any register across a wait other than its variables'.
[[nodiscard]] bool keeps_transients(const ScriptProgram& program) noexcept {
    for (const StateSlot& slot : program.state_slots()) {
        bool variable = false;
        for (const Variable& declared : program.variables()) {
            variable = variable || declared.reg == slot.reg;
        }
        if (!variable) {
            return true;
        }
    }
    return false;
}

/// The wait point of `to` that `source`'s suspension can continue at, or null.
[[nodiscard]] const SuspendPoint* continuing_wait(const ScriptProgram& from,
                                                  const ScriptState& source,
                                                  const ScriptProgram& to) noexcept {
    const SuspendPoint* waiting = waiting_at(from, source);
    if (waiting == nullptr || keeps_transients(to)) {
        return nullptr;
    }
    for (const SuspendPoint& point : to.suspends()) {
        if (point.origin == waiting->origin && point.reason == waiting->reason) {
            return &point;
        }
    }
    return nullptr;
}

}  // namespace

u32 check_migration(const ScriptProgram& from, const ScriptProgram& to,
                    DiagnosticSink& sink) noexcept {
    u32 refused = 0;
    for (const Variable& next : to.variables()) {
        const Variable* previous = find_variable(from, next.id);
        if (previous == nullptr || previous->kind == next.kind) {
            continue;
        }
        char change[64] = {};
        (void)std::snprintf(change, sizeof(change), "%s -> %s", value_kind_name(previous->kind),
                            value_kind_name(next.kind));
        Diagnostic diagnostic;
        diagnostic.severity = Severity::Error;
        diagnostic.code = "script.reload.type";
        diagnostic.node = next.id;
        diagnostic.pin = Name::intern("type");
        diagnostic.message =
            "this variable changed type while the game runs, so its value cannot be carried over; "
            "the running program is kept. Stop Play to change it, or declare a new variable";
        diagnostic.detail = Name::intern(change);
        sink.report(diagnostic);
        ++refused;
    }
    return refused;
}

Expected<StateMigration, Error> migrate_state(const ScriptProgram& from, const ScriptState& source,
                                              const ScriptProgram& to,
                                              ScriptState& target) noexcept {
    if (source.paused()) {
        return fail(ErrorCode::Unavailable,
                    "an instance a debugger has paused mid-handler cannot be migrated; continue it "
                    "to the end of the tick first");
    }
    if (target.registers().size() != to.register_count()) {
        return fail(ErrorCode::InvalidArgument,
                    "the migrated state is not sized for the program it moves to");
    }
    StateMigration migration;
    for (const Variable& next : to.variables()) {
        const Variable* previous = find_variable(from, next.id);
        if (previous == nullptr) {
            ++migration.added;  // `target` was built holding the default.
            continue;
        }
        if (previous->kind != next.kind) {
            return fail(ErrorCode::InvalidArgument,
                        "a variable changed type; check_migration refuses this reload");
        }
        target.registers()[next.reg] = read_variable(*previous, source);
        ++migration.kept;
    }
    for (const Variable& previous : from.variables()) {
        migration.dropped += find_variable(to, previous.id) == nullptr ? 1U : 0U;
    }
    if (source.suspended()) {
        const SuspendPoint* point = continuing_wait(from, source, to);
        if (point != nullptr && point->resume != kNoBlock) {
            // The new program resumes at its own block for the same wait node. What it restores on
            // resuming is its own state slots, which are variables only, so persist them as
            // migrated.
            target.set_resume_block(point->resume);
            if (Status persisted = target.persist(to); !persisted) {
                return make_unexpected(persisted.error());
            }
            migration.wait_kept = true;
        } else {
            migration.wait_dropped = true;
        }
    }
    return migration;
}

}  // namespace cy::graph::script
