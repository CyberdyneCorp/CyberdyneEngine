// A runtime handle assigned to an asset id. This is the confusion that puts a slot index into a
// saved scene, and it must not compile.

#include <cy/core/values/asset_id.h>
#include <cy/core/values/handle.h>

namespace {
CY_HANDLE_TAG(Mesh);
}

void use() {
    const cy::Handle<MeshTag> handle = cy::Handle<MeshTag>::from_slot(3, 1);
    const cy::AssetId asset = handle;
    (void)asset;
}
