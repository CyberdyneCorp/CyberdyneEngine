// The thread-local allocator scope stack. Task 2.1.

#include <cy/core/memory/scope.h>

#include <cy/core/memory/system_allocator.h>

namespace cy {
namespace {

struct ScopeStack {
    Allocator* entries[kMaxAllocatorScopeDepth] = {};
    u32 depth = 0;
    u64 overflows = 0;
};

// A thread_local aggregate with no destructor: it is trivially destructible, so a thread's exit
// costs nothing and there is no static-destruction order to reason about.
thread_local ScopeStack t_scopes;

}  // namespace

Allocator& current_allocator() noexcept {
    if (t_scopes.depth == 0) {
        return default_allocator();
    }
    return *t_scopes.entries[t_scopes.depth - 1];
}

u32 allocator_scope_depth() noexcept {
    return t_scopes.depth;
}

u64 allocator_scope_overflows() noexcept {
    return t_scopes.overflows;
}

AllocatorScope::AllocatorScope(Allocator& allocator) noexcept {
    if (t_scopes.depth >= kMaxAllocatorScopeDepth) {
        ++t_scopes.overflows;
        pushed_ = false;
        return;
    }
    t_scopes.entries[t_scopes.depth++] = &allocator;
    pushed_ = true;
}

AllocatorScope::~AllocatorScope() {
    if (pushed_ && t_scopes.depth > 0) {
        --t_scopes.depth;
    }
}

}  // namespace cy
