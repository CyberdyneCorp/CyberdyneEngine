// Comparing an asset id against a handle. Neither an implicit conversion nor a mixed comparison
// operator exists, so this is an error rather than a comparison that is always false.

#include <cy/core/values/asset_id.h>
#include <cy/core/values/handle.h>

namespace {
CY_HANDLE_TAG(Mesh);
}

bool use() {
    const cy::AssetId asset(1, 2);
    const cy::Handle<MeshTag> handle = cy::Handle<MeshTag>::from_slot(3, 1);
    return asset == handle;
}
