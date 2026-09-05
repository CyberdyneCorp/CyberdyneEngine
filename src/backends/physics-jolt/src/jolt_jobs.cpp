// Jolt's job system, bridged onto the engine's. Task 4.2.2.

#include "jolt_jobs.h"

#include <cy/core/jobs/context.h>
#include <cy/core/jobs/job_system.h>

namespace cy::physics::jolt {

/// `Execute()` then `Release()`, in that order and never the other way: the release may destroy the
/// job, and Jolt's own worker loop does exactly this pair.
void EngineJobSystem::run(const cy::jobs::TaskContext& context, void* user) noexcept {
    (void)context;
    auto* job = static_cast<Job*>(user);
    job->Execute();
    job->Release();
}

EngineJobSystem::EngineJobSystem(cy::jobs::JobSystem* jobs, JPH::uint max_jobs,
                                 JPH::uint max_barriers) noexcept
    : jobs_(jobs) {
    JobSystemWithBarrier::Init(max_barriers);
    // One page holds every job: the free list is a fixed allocation and a step must not grow it.
    pool_.Init(max_jobs, max_jobs);
}

EngineJobSystem::~EngineJobSystem() = default;

bool EngineJobSystem::bridged() const noexcept {
    return jobs_ != nullptr && jobs_->is_running();
}

int EngineJobSystem::GetMaxConcurrency() const {
    // One more than the workers, because the thread that waits on a barrier runs jobs too. With no
    // engine scheduler that thread is the only one, and Jolt then partitions its work into one
    // piece rather than into pieces nothing will pick up.
    if (!bridged()) {
        return 1;
    }
    return static_cast<int>(jobs_->worker_count()) + 1;
}

JPH::JobHandle EngineJobSystem::CreateJob(const char* name, JPH::ColorArg color,
                                          const JobFunction& function, JPH::uint32 dependencies) {
    const JPH::uint32 index = pool_.ConstructObject(name, color, this, function, dependencies);
    if (index == JPH::FixedSizeFreeList<Job>::cInvalidObjectIndex) {
        // Jolt's own pool spins here. Returning an empty handle is the honest alternative for a
        // fixed-size pool that is genuinely exhausted: Jolt treats an unqueued job as one the
        // barrier will run, and a spin inside a physics step is a frame nobody can explain.
        return {};
    }
    Job* job = &pool_.Get(index);
    // The handle takes a reference before the job is queued: queuing may complete it immediately on
    // another thread, and the handle must not be the second owner to arrive.
    const JPH::JobHandle handle(job);
    if (dependencies == 0) {
        QueueJob(job);
    }
    return handle;
}

void EngineJobSystem::QueueJob(Job* job) {
    if (!bridged()) {
        return;  // the barrier will run it on the waiting thread — see the header
    }
    // The reference the engine job holds. Released by `run_jolt_job`, or here when the submission
    // was declined, so the count is balanced on both paths.
    job->AddRef();
    const Expected<cy::jobs::JobHandle, Error> submitted =
        jobs_->submit(&EngineJobSystem::run, job, "physics.jolt");
    if (!submitted) {
        job->Release();
    }
}

void EngineJobSystem::QueueJobs(Job** jobs, JPH::uint count) {
    for (JPH::uint index = 0; index < count; ++index) {
        QueueJob(jobs[index]);
    }
}

void EngineJobSystem::FreeJob(Job* job) {
    pool_.DestructObject(job);
}

}  // namespace cy::physics::jolt
