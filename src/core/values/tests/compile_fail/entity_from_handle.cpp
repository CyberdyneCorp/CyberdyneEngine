// An entity id built from a handle. Entities are owned by the ECS world and handles by a server's
// pool; the two are both 64-bit index-and-generation pairs and are not interchangeable.

#include <cy/core/values/handle.h>

namespace {
CY_HANDLE_TAG(Mesh);
}

void use() {
    const cy::Handle<MeshTag> handle = cy::Handle<MeshTag>::from_slot(3, 1);
    const cy::EntityId entity = handle;
    (void)entity;
}
