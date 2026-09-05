// The query filter's one non-inline member. Task 4.2.4.

#include <cy/servers/physics/queries.h>

namespace cy::physics {

Status reject_query_during_step(bool stepping) noexcept {
#if defined(CY_DEVELOPMENT)
    if (stepping) {
        return fail(ErrorCode::Unavailable,
                    "physics: a query was attempted while the step was in progress; the world is "
                    "mid-solve and the answer would be neither the previous tick's nor this one's");
    }
#else
    (void)stepping;
#endif
    return ok();
}

bool QueryFilter::ignores(BodyHandle body) const noexcept {
    // A linear scan, deliberately. An ignore list is a character's own colliders and the thing it
    // is standing on — two or three entries — and a set would cost an allocation per query on a
    // path `physics` requires to be callable from every entity in a parallel system.
    for (u32 index = 0; index < ignore_count; ++index) {
        if (ignore[index] == body) {
            return true;
        }
    }
    return false;
}

}  // namespace cy::physics
