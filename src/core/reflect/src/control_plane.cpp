// The control-plane check. Task 1.1.4.
//
// The counter is a relaxed atomic and the region depth is thread-local, and both are compiled in
// every configuration. That is the whole point of the design: CY_ASSERT is compiled out of Profile
// and Shipping, so an assertion here would mean the rule is only enforced in the two configurations
// where nobody profiles. The task asks for a check, and a check has to survive the build that
// matters.
//
// Relaxed ordering is correct: the counter is a diagnostic total, no other state is published
// through it, and a test reads it after the work it measured has been joined.

#include <cy/core/reflect/control_plane.h>

#include <atomic>

namespace cy::reflect {
namespace {

// The innermost region on this thread, or null. A pointer rather than a depth counter because the
// label is what makes a violation actionable; nesting is handled by each HotRegion saving and
// restoring whatever it displaced.
thread_local const char* g_hot_region = nullptr;

std::atomic<u64> g_violations{0};
std::atomic<const char*> g_last_label{nullptr};

}  // namespace

HotRegion::HotRegion(const char* label) noexcept : previous_(g_hot_region) {
    g_hot_region = (label != nullptr) ? label : "unnamed";
}

HotRegion::~HotRegion() noexcept {
    g_hot_region = previous_;
}

bool in_hot_region() noexcept {
    return g_hot_region != nullptr;
}

const char* hot_region_label() noexcept {
    return g_hot_region;
}

u64 control_plane_violations() noexcept {
    return g_violations.load(std::memory_order_relaxed);
}

const char* last_violation_label() noexcept {
    return g_last_label.load(std::memory_order_relaxed);
}

void reset_control_plane_violations() noexcept {
    g_violations.store(0, std::memory_order_relaxed);
    g_last_label.store(nullptr, std::memory_order_relaxed);
}

namespace detail {

void note_reflected_lookup() noexcept {
    if (g_hot_region == nullptr) {
        return;
    }
    g_last_label.store(g_hot_region, std::memory_order_relaxed);
    g_violations.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace detail
}  // namespace cy::reflect
