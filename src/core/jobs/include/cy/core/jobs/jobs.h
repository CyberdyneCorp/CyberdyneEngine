#pragma once
// The job system, in one include. Section 3.2, `core-jobs-and-concurrency`.
//
// Umbrella headers are a convenience for a consumer and a cost for a compiler, so the module's own
// sources never include this one — they include the two or three headers they actually use, and a
// change to the coroutine machinery does not rebuild everything that submits a job. It exists for a
// subsystem that genuinely wants the whole vocabulary, and for a reader who wants the map:
//
//   types.h          priorities, job handles, deadlines, the monotonic clock
//   thread_role.h    the thread-role table and its counted enforcement
//   sync.h           Thread, Mutex, RwLock, Semaphore, ConditionVariable, SpinLock, Event, atomics
//   double_buffer.h  DoubleBuffered<T> and the single-producer/single-consumer queue
//   scratch.h        the per-task bump arena, poisoned on release
//   cancellation.h   cooperative cancellation: a source, a token, and a tree
//   context.h        what every task body is handed
//   job_system.h     the one system: submission, dependencies, waiting, gates, the blocking rule
//   parallel.h       parallel_for, parallel_reduce, parallel_exclusive_scan, parallel_sort
//   access.h         Read/Write/Exclude declarations and the conflict test
//   schedule.h       the stage built from those declarations, and deferred structural changes
//   coroutine.h      Task<T>, awaiting a job, awaiting a cancellation
//   async.h          the dedicated thread where blocking is legal, and the GPU fence
//   diagnostics.h    the statistics and the critical path
//
// The two invariants a reader should take away, because everything else follows from them:
// parallel execution is safe because access is *declared* and the schedule is *derived* (access.h),
// and a worker never blocks — a blocking call is refused on a worker and belongs on the async
// service instead (async.h).

#include <cy/core/jobs/access.h>
#include <cy/core/jobs/async.h>
#include <cy/core/jobs/cancellation.h>
#include <cy/core/jobs/command_queue.h>
#include <cy/core/jobs/context.h>
#include <cy/core/jobs/coroutine.h>
#include <cy/core/jobs/diagnostics.h>
#include <cy/core/jobs/double_buffer.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/jobs/parallel.h>
#include <cy/core/jobs/schedule.h>
#include <cy/core/jobs/scratch.h>
#include <cy/core/jobs/sync.h>
#include <cy/core/jobs/thread_role.h>
#include <cy/core/jobs/types.h>
