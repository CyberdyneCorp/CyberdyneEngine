// The control: an asset id and a handle used as themselves both compile. Without this the suite
// would pass if the header simply failed to compile at all.

#include <cy/core/values/asset_id.h>
#include <cy/core/values/handle.h>

namespace {
CY_HANDLE_TAG(Mesh);
}

void use() {
    const cy::AssetId asset(1, 2);
    const cy::Handle<MeshTag> handle = cy::Handle<MeshTag>::from_slot(3, 1);

    const cy::AssetId same_asset = asset;
    const cy::Handle<MeshTag> same_handle = handle;

    (void)same_asset;
    (void)same_handle;
}
