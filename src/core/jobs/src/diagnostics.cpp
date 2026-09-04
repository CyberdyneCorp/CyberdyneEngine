// The job system's diagnostics, emitted onto the M0 shared trace. Task 3.2.11.
//
// One category, `jobs`, and a classified field per number. Every field is declared with
// CY_TRACE_FIELD, which takes its privacy classification as a required third argument — there is no
// overload without one — so the classification is not a convention a later edit can drop.
//
// Everything here is Public: a count, a duration and a worker index are numbers about the engine,
// and none of them identifies a person or a machine. A task's *name* is Developer, for the same
// reason core-values classifies a callable's name that way: it is a project's own identifier, which
// belongs in a developer's own capture rather than in an artefact prepared to leave the machine.
//
// This is the only translation unit in the module that includes a diagnostics header. That is what
// makes cy::core-diagnostics a private dependency in the honest sense: nothing that includes
// <cy/core/jobs/job_system.h> acquires the trace's headers.

#include <cy/core/jobs/diagnostics.h>

#include <cy/core/diagnostics/field.h>
#include <cy/core/diagnostics/log.h>
#include <cy/core/diagnostics/trace.h>

#include "internal.h"

#include <cstring>

namespace cy::jobs {
namespace {

// One category for the trace and the log alike. CY_LOG_CATEGORY is CY_TRACE_CATEGORY:
// a log record is a record on the one timeline, not a second mechanism beside it.
CY_TRACE_CATEGORY(category, "jobs")

// A task's own identifier, not a number about the engine. See the file comment.
CY_TRACE_FIELD(task_name, string, cy::Privacy::Developer)

CY_TRACE_FIELD(worker_index, u64, cy::Privacy::Public)
CY_TRACE_FIELD(priority_class, u64, cy::Privacy::Public)
CY_TRACE_FIELD(task_sequence, u64, cy::Privacy::Public)
CY_TRACE_FIELD(task_duration, duration_ns, cy::Privacy::Public)

CY_TRACE_FIELD(tasks_submitted, u64, cy::Privacy::Public)
CY_TRACE_FIELD(tasks_executed, u64, cy::Privacy::Public)
CY_TRACE_FIELD(tasks_cancelled, u64, cy::Privacy::Public)
CY_TRACE_FIELD(steal_attempts, u64, cy::Privacy::Public)
CY_TRACE_FIELD(steal_successes, u64, cy::Privacy::Public)
CY_TRACE_FIELD(queue_depth, u64, cy::Privacy::Public)
CY_TRACE_FIELD(queue_latency, duration_ns, cy::Privacy::Public)
CY_TRACE_FIELD(worker_busy, duration_ns, cy::Privacy::Public)
CY_TRACE_FIELD(worker_idle, duration_ns, cy::Privacy::Public)
CY_TRACE_FIELD(blocked_workers, u64, cy::Privacy::Public)
CY_TRACE_FIELD(long_tasks, u64, cy::Privacy::Public)
CY_TRACE_FIELD(unresponsive_cancellations, u64, cy::Privacy::Public)
CY_TRACE_FIELD(blocking_violations_field, u64, cy::Privacy::Public)
CY_TRACE_FIELD(scheduling_allocations, u64, cy::Privacy::Public)
CY_TRACE_FIELD(peak_tasks_in_flight, u64, cy::Privacy::Public)

CY_TRACE_FIELD(critical_path_total, duration_ns, cy::Privacy::Public)
CY_TRACE_FIELD(critical_path_length, u64, cy::Privacy::Public)
CY_TRACE_FIELD(critical_path_index, u64, cy::Privacy::Public)
CY_TRACE_FIELD(frame_task_total, duration_ns, cy::Privacy::Public)
CY_TRACE_FIELD(frame_tasks_recorded, u64, cy::Privacy::Public)
CY_TRACE_FIELD(frame_entries_dropped, u64, cy::Privacy::Public)

CY_TRACE_FIELD(watchdog_finding, string, cy::Privacy::Public)

diag::u32 text_length(const char* text) noexcept {
    return text != nullptr ? static_cast<diag::u32>(std::strlen(text)) : 0;
}

void emit_counter(const char* name, u64 value) noexcept {
    const diag::NameId id = diag::register_name(name);
    diag::trace_counter(id, category(), diag::Channel::Verbose, value);
}

}  // namespace

namespace detail {

// TaskBegin and TaskEnd are Critical, unlike the counters below. That is the loss policy working as
// designed: `diagnostics-profiling-and-crash` refuses sampled and verbose records first under
// buffer pressure, and a timeline missing half a task's boundaries is worse than one missing a
// counter snapshot — a begin without its end is read as a task that never finished.

void trace_task_begin(const char* name, WorkerIndex worker, Priority priority,
                      u64 sequence) noexcept {
    if (!diag::trace_is_open()) {
        return;
    }
    static const diag::NameId event = diag::register_name("jobs.task");
    const diag::FieldValue fields[] = {
        diag::field_text(task_name(), name, text_length(name)),
        diag::field_u64(worker_index(), worker),
        diag::field_u64(priority_class(), static_cast<u64>(priority)),
        diag::field_u64(task_sequence(), sequence),
    };
    diag::trace_emit(diag::EventKind::TaskBegin, diag::Channel::Critical, event, category(),
                     sequence, static_cast<u64>(priority), fields,
                     static_cast<diag::u32>(sizeof(fields) / sizeof(fields[0])));
}

void trace_task_end(const char* name, WorkerIndex worker, Priority priority,
                    u64 duration_ns) noexcept {
    if (!diag::trace_is_open()) {
        return;
    }
    static const diag::NameId event = diag::register_name("jobs.task");
    const diag::FieldValue fields[] = {
        diag::field_text(task_name(), name, text_length(name)),
        diag::field_u64(worker_index(), worker),
        diag::field_u64(priority_class(), static_cast<u64>(priority)),
        diag::field_u64(task_duration(), duration_ns),
    };
    diag::trace_emit(diag::EventKind::TaskEnd, diag::Channel::Critical, event, category(),
                     duration_ns, static_cast<u64>(priority), fields,
                     static_cast<diag::u32>(sizeof(fields) / sizeof(fields[0])));
}

}  // namespace detail

void jobs_trace_report(const JobSystemStats& stats) noexcept {
    if (!diag::trace_is_open()) {
        return;
    }

    emit_counter("jobs.tasks_executed", stats.tasks_executed);
    emit_counter("jobs.queue_depth", stats.queue_depth);
    emit_counter("jobs.worker_busy_ns", stats.worker_busy_ns);
    emit_counter("jobs.worker_idle_ns", stats.worker_idle_ns);
    emit_counter("jobs.steal_successes", stats.steal_successes);
    emit_counter("jobs.blocked_worker_detections", stats.blocked_worker_detections);

    // One instant carrying the snapshot, so a capture has the figures together as well as as
    // separate series. Sixteen fields is the record limit, and the static_assert is what stops a
    // seventeenth from being silently dropped by the writer.
    const diag::FieldValue fields[] = {
        diag::field_u64(tasks_submitted(), stats.tasks_submitted),
        diag::field_u64(tasks_executed(), stats.tasks_executed),
        diag::field_u64(tasks_cancelled(), stats.tasks_cancelled),
        diag::field_u64(steal_attempts(), stats.steal_attempts),
        diag::field_u64(steal_successes(), stats.steal_successes),
        diag::field_u64(queue_depth(), stats.queue_depth),
        diag::field_u64(queue_latency(), stats.queue_latency_ns),
        diag::field_u64(worker_busy(), stats.worker_busy_ns),
        diag::field_u64(worker_idle(), stats.worker_idle_ns),
        diag::field_u64(blocked_workers(), stats.blocked_worker_detections),
        diag::field_u64(long_tasks(), stats.long_task_detections),
        diag::field_u64(unresponsive_cancellations(), stats.unresponsive_cancellations),
        diag::field_u64(blocking_violations_field(), stats.blocking_violations),
        diag::field_u64(scheduling_allocations(), stats.scheduling_allocations),
        diag::field_u64(peak_tasks_in_flight(), stats.peak_tasks_in_flight),
    };
    static_assert(sizeof(fields) / sizeof(fields[0]) <= diag::kMaxFieldsPerRecord,
                  "the snapshot must fit one record; split it rather than dropping fields");

    static const diag::NameId report = diag::register_name("jobs.report");
    diag::trace_instant(report, category(), diag::Channel::Verbose, fields,
                        static_cast<diag::u32>(sizeof(fields) / sizeof(fields[0])));
}

void jobs_trace_critical_path(const CriticalPath& path) noexcept {
    if (!diag::trace_is_open()) {
        return;
    }

    static const diag::NameId summary = diag::register_name("jobs.critical_path");
    const diag::FieldValue summary_fields[] = {
        diag::field_u64(critical_path_total(), path.total_ns),
        diag::field_u64(critical_path_length(), path.length),
        diag::field_u64(frame_task_total(), path.total_task_ns),
        diag::field_u64(frame_tasks_recorded(), path.tasks_recorded),
        diag::field_u64(frame_entries_dropped(), path.entries_dropped),
    };
    diag::trace_instant(summary, category(), diag::Channel::Important, summary_fields,
                        static_cast<diag::u32>(sizeof(summary_fields) / sizeof(summary_fields[0])));

    static const diag::NameId step = diag::register_name("jobs.critical_path.step");
    for (u32 i = 0; i < path.length; ++i) {
        const CriticalPathEntry& entry = path.entries[i];
        const diag::FieldValue fields[] = {
            diag::field_u64(critical_path_index(), i),
            diag::field_text(task_name(), entry.name, text_length(entry.name)),
            diag::field_u64(worker_index(), entry.worker),
            diag::field_u64(priority_class(), static_cast<u64>(entry.priority)),
            diag::field_u64(task_duration(), entry.duration_ns),
            diag::field_u64(critical_path_total(), entry.path_ns),
        };
        diag::trace_instant(step, category(), diag::Channel::Important, fields,
                            static_cast<diag::u32>(sizeof(fields) / sizeof(fields[0])));
    }
}

void jobs_log_report(const JobSystemStats& stats) noexcept {
    CY_LOG(category(), diag::LogLevel::Info, "jobs.report",
           diag::field_u64(tasks_submitted(), stats.tasks_submitted),
           diag::field_u64(tasks_executed(), stats.tasks_executed),
           diag::field_u64(tasks_cancelled(), stats.tasks_cancelled),
           diag::field_u64(steal_successes(), stats.steal_successes),
           diag::field_u64(worker_busy(), stats.worker_busy_ns),
           diag::field_u64(worker_idle(), stats.worker_idle_ns),
           diag::field_u64(blocked_workers(), stats.blocked_worker_detections),
           diag::field_u64(blocking_violations_field(), stats.blocking_violations));
}

void jobs_log_watchdog(const char* what, const char* task_name_text, u64 duration_ns,
                       WorkerIndex worker) noexcept {
    CY_LOG(category(), diag::LogLevel::Warning, "jobs.watchdog",
           diag::field_text(watchdog_finding(), what, text_length(what)),
           diag::field_text(task_name(), task_name_text, text_length(task_name_text)),
           diag::field_u64(task_duration(), duration_ns), diag::field_u64(worker_index(), worker));
}

}  // namespace cy::jobs
