// Coroutine frames, continuations, and starting a coroutine from ordinary code. Task 3.2.4.
//
// THE FRAME ALLOCATOR is a thread-local free list per size class. A coroutine frame's size is a
// compile-time constant the compiler passes to `operator new`, and engine coroutines cluster into a
// handful of sizes, so a free list per class turns "allocate a frame" into a pointer load and a
// store on the common path. Blocks migrate: a frame allocated on one worker and destroyed on
// another joins the destroying thread's list, which is correct and is why the lists are per thread
// rather than per worker index.
//
// The lists are emptied when the thread exits, so a run under LeakSanitizer is clean. A frame still
// alive at thread exit is not freed — it belongs to a coroutine somebody is still holding, and
// freeing it would be the bug rather than the fix.

#include <cy/core/jobs/coroutine.h>

#include <atomic>
#include <cstring>
#include <new>

namespace cy::jobs {
namespace {

/// The size classes. A frame larger than the last falls back to the general heap and is counted.
/// Four classes rather than a general allocator: the point is to make the common path a pointer
/// swap, not to write a second malloc.
constexpr usize kSizeClasses[] = {128, 256, 512, 1024};
constexpr u32 kSizeClassCount = sizeof(kSizeClasses) / sizeof(kSizeClasses[0]);

std::atomic<u64> g_from_slab{0};
std::atomic<u64> g_from_heap{0};
std::atomic<u64> g_live{0};
std::atomic<u64> g_slab_bytes{0};

u32 size_class_of(usize bytes) noexcept {
    for (u32 i = 0; i < kSizeClassCount; ++i) {
        if (bytes <= kSizeClasses[i]) {
            return i;
        }
    }
    return kSizeClassCount;
}

/// One thread's free lists. The `next` pointer lives in the first word of a free block, which is
/// why every size class is at least a pointer wide.
struct FrameSlab {
    void* free_list[kSizeClassCount] = {};

    ~FrameSlab() {
        for (u32 i = 0; i < kSizeClassCount; ++i) {
            void* block = free_list[i];
            while (block != nullptr) {
                void* next = nullptr;
                std::memcpy(&next, block, sizeof(next));
                ::operator delete(block, std::nothrow);
                g_slab_bytes.fetch_sub(kSizeClasses[i], std::memory_order_relaxed);
                block = next;
            }
            free_list[i] = nullptr;
        }
    }
};

thread_local FrameSlab t_slab;

void noop_task(const TaskContext&, void*) noexcept {}

}  // namespace

CoroutineFrameStats coroutine_frame_stats() noexcept {
    CoroutineFrameStats stats;
    stats.from_slab = g_from_slab.load(std::memory_order_relaxed);
    stats.from_heap = g_from_heap.load(std::memory_order_relaxed);
    stats.live = g_live.load(std::memory_order_relaxed);
    stats.slab_bytes = g_slab_bytes.load(std::memory_order_relaxed);
    return stats;
}

namespace detail {

void* coroutine_frame_allocate(usize bytes) noexcept {
    const u32 klass = size_class_of(bytes);
    g_live.fetch_add(1, std::memory_order_relaxed);

    if (klass == kSizeClassCount) {
        g_from_heap.fetch_add(1, std::memory_order_relaxed);
        return ::operator new(bytes, std::nothrow);
    }

    g_from_slab.fetch_add(1, std::memory_order_relaxed);
    void* block = t_slab.free_list[klass];
    if (block != nullptr) {
        void* next = nullptr;
        std::memcpy(&next, block, sizeof(next));
        t_slab.free_list[klass] = next;
        return block;
    }

    void* fresh = ::operator new(kSizeClasses[klass], std::nothrow);
    if (fresh != nullptr) {
        g_slab_bytes.fetch_add(kSizeClasses[klass], std::memory_order_relaxed);
    } else {
        g_live.fetch_sub(1, std::memory_order_relaxed);
    }
    return fresh;
}

void coroutine_frame_free(void* frame, usize bytes) noexcept {
    if (frame == nullptr) {
        return;
    }
    g_live.fetch_sub(1, std::memory_order_relaxed);

    const u32 klass = size_class_of(bytes);
    if (klass == kSizeClassCount) {
        ::operator delete(frame, std::nothrow);
        return;
    }
    void* next = t_slab.free_list[klass];
    std::memcpy(frame, &next, sizeof(next));
    t_slab.free_list[klass] = frame;
}

void resume_coroutine(const TaskContext& context, void*) noexcept {
    if (context.data == nullptr) {
        return;
    }
    void* address = nullptr;
    std::memcpy(&address, context.data, sizeof(address));
    if (address == nullptr) {
        return;
    }
    // The resume runs the coroutine on this worker until its next suspension or its end. The
    // worker's stack is unwound back to the scheduler loop afterwards; nothing is switched.
    std::coroutine_handle<>::from_address(address).resume();
}

Expected<JobHandle, cy::Error> spawn_coroutine(JobSystem& jobs, std::coroutine_handle<> coroutine,
                                               TaskPromiseBase& promise, const char* name,
                                               Priority priority) noexcept {
    // The completion is a gated job: it exists from the moment the coroutine starts, so a caller
    // has something to wait on immediately, and it becomes runnable only when the coroutine's
    // final suspension signals it.
    JobDesc completion_desc;
    completion_desc.body = &noop_task;
    completion_desc.name = name;
    completion_desc.priority = priority;
    completion_desc.gated = true;
    auto completion = jobs.submit(completion_desc);
    if (!completion) {
        return completion;
    }

    promise.set_priority(priority);
    promise.set_completion(Completion{&jobs, completion.value()});

    JobDesc start;
    start.body = &resume_coroutine;
    start.name = name;
    start.priority = priority;
    void* address = coroutine.address();
    start.inline_data = &address;
    start.inline_size = static_cast<u32>(sizeof(address));

    auto started = jobs.submit(start);
    if (!started) {
        // Release the gate, or the completion job would never run and anything waiting on the
        // handle this function was about to return would wait forever.
        (void)jobs.signal(completion.value());
        return started;
    }
    return completion.value();
}

}  // namespace detail

bool CancellationAwaiter::await_suspend(std::coroutine_handle<> awaiting) noexcept {
    // A gated job stands in for the cancellation: the continuation depends on it, and the token's
    // callback releases it. One mechanism — see JobDesc::gated — rather than a second way to make a
    // coroutine runnable.
    JobDesc gate_desc;
    gate_desc.body = &noop_task;
    gate_desc.name = "cancellation.gate";
    gate_desc.priority = priority_;
    gate_desc.gated = true;
    auto gate = jobs_->submit(gate_desc);
    if (!gate) {
        return false;
    }
    gate_ = gate.value();

    JobDesc continuation;
    continuation.body = &detail::resume_coroutine;
    continuation.name = "cancellation.continuation";
    continuation.priority = priority_;
    continuation.dependencies = &gate_;
    continuation.dependency_count = 1;
    void* address = awaiting.address();
    continuation.inline_data = &address;
    continuation.inline_size = static_cast<u32>(sizeof(address));

    auto scheduled = jobs_->submit(continuation);
    if (!scheduled) {
        (void)jobs_->signal(gate_);
        return false;
    }

    // `this` is stable while the coroutine is suspended: the awaiter lives in the coroutine frame.
    // on_cancel invokes the callback inline when the token is already cancelled, which closes the
    // race between await_ready and this registration.
    token_.on_cancel(
        [](void* user) noexcept {
            auto* self = static_cast<CancellationAwaiter*>(user);
            (void)self->jobs_->signal(self->gate_);
        },
        this);
    return true;
}

}  // namespace cy::jobs
