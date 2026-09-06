// A deliberate violation: a file outside src/backends/physics-jolt/ that names a Jolt header.
//
// `physics`: "No Jolt type SHALL appear above src/backends/." The server that owns the interface is
// exactly the file that would break it first, because it is the one that knows what a body is.
//
// tools/layercheck/layercheck.py --check thirdparty must reject this file.

#include <Jolt/Physics/Body/Body.h>

void touch() {
    JPH::BodyID id;
    (void)id;
}
