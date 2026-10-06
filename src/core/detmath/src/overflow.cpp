// OverflowGuard: the thread's current guard, and the counting. Design §4.3.

#include <cy/core/detmath/overflow.h>

namespace cy::detmath::inline CY_DETMATH_VARIANT {
namespace {

/// The innermost live guard on this thread, or null. Per thread because job workers run
/// authoritative arithmetic concurrently, and one worker's overflow is not another's.
thread_local OverflowGuard* t_current = nullptr;

}  // namespace

OverflowGuard::OverflowGuard() noexcept : previous_(t_current) {
    t_current = this;
}

OverflowGuard::~OverflowGuard() noexcept {
    t_current = previous_;
}

void OverflowGuard::clear() noexcept {
    overflows_ = 0;
    first_site_ = "";
}

void OverflowGuard::note(const char* site) noexcept {
    OverflowGuard* guard = t_current;
    if (guard == nullptr) {
        return;
    }
    if (guard->overflows_ == 0) {
        guard->first_site_ = site;
    }
    ++guard->overflows_;
}

}  // namespace cy::detmath::inline CY_DETMATH_VARIANT
