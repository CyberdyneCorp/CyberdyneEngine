// An asset id used as a runtime handle: the reverse confusion, which would dereference content
// identity as a slot index.

#include <cy/core/values/asset_id.h>
#include <cy/core/values/handle.h>

namespace {
CY_HANDLE_TAG(Mesh);
}

void use() {
    const cy::AssetId asset(1, 2);
    const cy::Handle<MeshTag> handle = asset;
    (void)handle;
}
