#pragma once
// Thread roles, and what each one may touch. Task 3.2.1.
//
// `core-jobs-and-concurrency` fixes this table:
//
//   Main         platform event pump, window and input, the frame schedule, editor UI
//   Simulation   ECS world mutation outside parallel system execution, deferred command flush
//   Worker       parallel system execution, culling, animation sampling, asset decode, physics
//   Render       render graph recording and GPU submission; owns every RHI object
//   Audio        realtime mixing and effects; owns playback state; never blocks, never waits
//   AssetIo      file reads, decompression, streaming; never touches ECS or GPU objects
//
// Where the platform requires it — the macOS and Windows message pumps — Main and Simulation MAY
// share one OS thread. The ownership rules still apply, so `require_thread_role` accepts a thread
// that has declared both.
//
// ENFORCEMENT IS COUNTED, NOT ONLY ASSERTED. The specification asks for a development-build
// assertion naming the violated role, and CY_ASSERT gives exactly that — but CY_ASSERT is compiled
// out of Profile and Shipping, so a test written against it alone reports nothing in the two
// configurations where nobody would notice. Every violation therefore also increments a relaxed
// counter that is compiled into all four, which is what the suite asserts on. This mirrors
// src/core/reflect/'s control-plane check, and for the same reason.

#include <cy/core/base/assert.h>
#include <cy/core/jobs/types.h>

namespace cy::jobs {

enum class ThreadRole : u8 {
    Unknown = 0,
    Main = 1,
    Simulation = 2,
    Worker = 3,
    Render = 4,
    Audio = 5,
    AssetIo = 6,
};

inline constexpr u32 kThreadRoleCount = 7;

/// The enumerator's own spelling, for a diagnostic. Never null.
const char* thread_role_name(ThreadRole role) noexcept;

/// Declare the calling thread's role. The job system calls it for every worker it starts; the host
/// calls it for the main thread and for each dedicated thread it creates.
///
/// A thread may hold more than one role — the platform-mandated Main+Simulation pairing — so this
/// adds a role rather than replacing the set. `clear_thread_roles()` is how a test returns a thread
/// to Unknown.
void set_thread_role(ThreadRole role, WorkerIndex worker = kNotAWorker) noexcept;
void clear_thread_roles() noexcept;

/// The calling thread's primary role: the first one it declared. Unknown on a thread that declared
/// none, which is every thread the engine did not create.
ThreadRole current_thread_role() noexcept;

/// True when the calling thread holds `role`, whether as its primary role or as a second one.
bool thread_holds_role(ThreadRole role) noexcept;

/// The calling thread's worker index, or kNotAWorker off a job worker.
WorkerIndex current_worker_index() noexcept;

// --- The check ------------------------------------------------------------------------------------

/// True when the calling thread holds `required`. Otherwise records the violation — the role that
/// was required, the role the thread actually holds, and `what` was attempted — and returns false.
///
/// `what` is a string literal naming the operation, because "a worker touched the render thread's
/// data" is not actionable and "record_rhi_command on a Worker thread, which requires Render" is.
bool require_thread_role(ThreadRole required, const char* what) noexcept;

/// How many role violations this process has recorded. Compiled into every configuration.
u64 thread_role_violations() noexcept;

/// What the most recent violation was attempting, or "" when there has been none.
const char* last_thread_role_violation() noexcept;

/// The role that was required by the most recent violation.
ThreadRole last_required_thread_role() noexcept;

void reset_thread_role_violations() noexcept;

}  // namespace cy::jobs

/// Assert a thread role at a boundary an owner defends.
///
///     CY_ASSERT_THREAD_ROLE(cy::jobs::ThreadRole::Render, "record_rhi_command");
///
/// CY_VERIFY, not CY_ASSERT: the expression must be evaluated in every configuration, because the
/// call is what records the violation. CY_ASSERT does not evaluate its argument in Profile or
/// Shipping — it holds it inside sizeof() — so an assertion here would leave those two
/// configurations counting nothing. CY_VERIFY evaluates always and checks the result where
/// assertions are live, which is exactly the split this file's header comment describes.
#define CY_ASSERT_THREAD_ROLE(role, what) \
    CY_VERIFY(::cy::jobs::require_thread_role((role), (what)))
