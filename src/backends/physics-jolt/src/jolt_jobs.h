#pragma once
// Jolt's job system, bridged onto the engine's. Task 4.2.2.
//
// `physics` — "One job system": "WHEN physics steps THEN its internal parallelism SHALL run on
// engine job workers, so physics and other work share one thread pool and one scheduler".
//
// A `JobSystemThreadPool` would start its own threads, and a machine would then have two schedulers
// oversubscribing the same cores — which is the specific failure the requirement is about, and it
// shows up as jitter rather than as anything that names physics.
//
// WHAT THIS INHERITS AND WHY. `JPH::JobSystemWithBarrier` already implements barriers, and its
// implementation is deliberately generic: Jolt's own header says the class "can be used to make it
// easier to create a new JobSystem implementation that integrates with your own job system". What
// is left is job allocation (a fixed-size free list, so a step allocates nothing) and dispatch.
//
// THE FALLBACK IS CORRECT, NOT A DEGRADED MODE. `QueueJob` may decline — no engine job system was
// given, or its queue is full — and Jolt handles that by design: `JobSystemThreadPool` does exactly
// the same thing when it has no worker threads, and the barrier runs the job on the waiting thread
// when `Wait()` is called. So a headless test with no scheduler runs physics single-threaded and
// correctly, and `Capabilities::uses_engine_jobs` reports which of the two happened.

#include "jolt_common.h"

// clang-format off
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
// clang-format on

namespace cy::jobs {
class JobSystem;
struct TaskContext;
}  // namespace cy::jobs

namespace cy::physics::jolt {

class EngineJobSystem final : public JPH::JobSystemWithBarrier {
public:
    /// `jobs` may be null — see the header comment. `max_jobs` bounds the free list; Jolt's own
    /// default for a physics system is 2048 and a step never approaches it.
    EngineJobSystem(cy::jobs::JobSystem* jobs, JPH::uint max_jobs, JPH::uint max_barriers) noexcept;
    ~EngineJobSystem() override;

    /// True when jobs are actually reaching engine workers, which is what
    /// `Capabilities::uses_engine_jobs` reports.
    [[nodiscard]] bool bridged() const noexcept;

    [[nodiscard]] int GetMaxConcurrency() const override;

    JPH::JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                             JPH::uint32 dependencies) override;

protected:
    void QueueJob(Job* job) override;
    void QueueJobs(Job** jobs, JPH::uint count) override;
    void FreeJob(Job* job) override;

private:
    /// The engine-side body of one Jolt job. A member rather than a free function because
    /// `JPH::JobSystem::Job` is protected: only a derived class may name it.
    static void run(const cy::jobs::TaskContext& context, void* user) noexcept;

    cy::jobs::JobSystem* jobs_;
    JPH::FixedSizeFreeList<Job> pool_;
};

}  // namespace cy::physics::jolt
