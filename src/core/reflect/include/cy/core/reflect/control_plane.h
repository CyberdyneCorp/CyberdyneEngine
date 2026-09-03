// The control-plane rule, made checkable. Task 1.1.4.
//
// `core-type-system` states it plainly: reflection is for editor inspection, serialization,
// migration, schema generation, dynamic registration, bindings and debugging. "Work executed per
// entity per frame SHALL use typed generated code, not reflection: field iteration, offset
// arithmetic, and dynamic dispatch SHALL NOT appear in per-entity hot paths."
//
// Task 1.1.4 asks for that to be *checked rather than asserted*, and this is the check. Two
// mechanisms, because neither alone covers the rule:
//
//   Structural — a per-frame path holds a TypedAccessor (type_info.h), which is a byte offset. It
//   carries no registry pointer and exposes no lookup, so a path written against one cannot reach
//   reflection through it. That is the "resolved once" shape the specification requires, and it is
//   the part that cannot be violated rather than the part that is noticed.
//
//   Detected — a hot region is declared with CY_REFLECT_HOT_REGION, and every reflected lookup made
//   inside one increments a counter. The counter is a plain atomic, live in **all four
//   configurations**, so the check does not evaporate in Profile and Shipping the way an assertion
//   would. A test asserts the count is zero across a synthetic per-entity loop, and non-zero for a
//   deliberate violation, which is what keeps the check itself honest.
//
// The counter is process-wide and the region is per thread: a job that iterates entities on a
// worker declares its own region, and a control-plane lookup on another thread at the same time is
// not a violation of anything.

#ifndef CY_CORE_REFLECT_CONTROL_PLANE_H
#define CY_CORE_REFLECT_CONTROL_PLANE_H

#include <cy/core/base/types.h>

namespace cy::reflect {

/// Marks the calling thread as executing per-entity or per-frame work for the object's lifetime.
///
/// Nesting is counted, so a region inside a region behaves as one. `label` is recorded on the
/// innermost violation, which is what turns "reflection was used in a hot path" into "reflection
/// was used in the particle update".
class HotRegion {
public:
    explicit HotRegion(const char* label) noexcept;
    ~HotRegion() noexcept;

    HotRegion(const HotRegion&) = delete;
    HotRegion& operator=(const HotRegion&) = delete;

private:
    const char* previous_;
};

/// True while the calling thread is inside a hot region.
[[nodiscard]] bool in_hot_region() noexcept;

/// The label of the innermost hot region on this thread, or null outside one.
[[nodiscard]] const char* hot_region_label() noexcept;

/// How many reflected lookups have been made inside a hot region since the process started, or
/// since the last reset. Zero is the invariant; anything else names a defect.
[[nodiscard]] u64 control_plane_violations() noexcept;

/// The label of the region in which the most recent violation happened, or null when there has been
/// none. Reported alongside the count so a failure says where rather than only how many.
[[nodiscard]] const char* last_violation_label() noexcept;

/// Reset the counter. For tests; a subsystem that calls this in earnest is hiding a violation.
void reset_control_plane_violations() noexcept;

namespace detail {

/// Called by every reflected lookup. Cheap outside a hot region — one thread-local read — and this
/// is control-plane code, where that is not a cost anyone is counting.
void note_reflected_lookup() noexcept;

}  // namespace detail

}  // namespace cy::reflect

/// Declare the enclosing scope a per-entity or per-frame region.
#define CY_REFLECT_HOT_REGION(label)                  \
    ::cy::reflect::HotRegion cy_reflect_hot_region_ { \
        (label)                                       \
    }

#endif  // CY_CORE_REFLECT_CONTROL_PLANE_H
