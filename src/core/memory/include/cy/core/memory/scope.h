#pragma once
// The allocator scope: a thread-local stack that says where an unannotated allocation goes.
// Task 2.1, and the mechanism behind task 2.2's "scope attributes automatically".
//
// `core-memory-and-containers` — "Allocator propagation": containers accept an allocator at
// construction and default to the current allocator scope, a thread-local stack pushed and popped
// by RAII guards. And "Scope attributes automatically": the renderer pushes its scope, and the
// containers constructed inside it allocate from the renderer's arena and are attributed to the
// renderer's domain with no per-allocation annotation.
//
// The scope carries an allocator, and an allocator carries a domain — so there is one mechanism
// here rather than two. A subsystem that wants attribution but not a private arena pushes
// `system_allocator(MemoryDomain::Renderer)` and gets exactly that.
//
// PER THREAD, ALWAYS. A scope is a property of the code that is running, not of the process, and a
// worker that inherited another thread's scope would attribute its allocations to whatever that
// thread happened to be doing. A task that wants its parent's scope is given it explicitly.

#include <cy/core/memory/allocator.h>

namespace cy {

/// The allocator an unannotated allocation goes to on this thread. `default_allocator()` when no
/// scope is open, so this never returns null and a caller never has to check.
[[nodiscard]] Allocator& current_allocator() noexcept;

/// How deep the scope stack is on this thread. Zero means the default allocator is current.
[[nodiscard]] u32 allocator_scope_depth() noexcept;

/// How many pushes this thread has refused because the stack was full. Non-zero means a scope is
/// being pushed in a loop without being popped — reported rather than asserted, because the
/// assertion would be compiled out of exactly the configuration where it would be discovered.
[[nodiscard]] u64 allocator_scope_overflows() noexcept;

/// The most scopes one thread may have open at once. Deep enough that a legitimate nesting —
/// subsystem inside frame inside engine — never approaches it, shallow enough that the storage is
/// a cache line or two of thread-local space.
inline constexpr u32 kMaxAllocatorScopeDepth = 32;

/// Push an allocator for the lifetime of this object.
///
///   cy::AllocatorScope scope(renderer_arena);
///   cy::Array<Batch> batches;            // allocates from renderer_arena, attributed to Renderer
class AllocatorScope {
public:
    explicit AllocatorScope(Allocator& allocator) noexcept;
    ~AllocatorScope();

    AllocatorScope(const AllocatorScope&) = delete;
    AllocatorScope& operator=(const AllocatorScope&) = delete;
    AllocatorScope(AllocatorScope&&) = delete;
    AllocatorScope& operator=(AllocatorScope&&) = delete;

private:
    // False when the push was refused. The destructor then pops nothing, so an overflow costs the
    // attribution of one scope rather than corrupting the stack for every scope after it.
    bool pushed_;
};

}  // namespace cy
