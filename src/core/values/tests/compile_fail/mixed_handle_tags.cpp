// A mesh handle passed where a texture handle is expected. The tag is a phantom type precisely so
// that this is a type error rather than a convention nobody checks.

#include <cy/core/values/handle.h>

namespace {
CY_HANDLE_TAG(Mesh);
CY_HANDLE_TAG(Texture);

void takes_texture(cy::Handle<TextureTag>) {}
}  // namespace

void use() {
    const cy::Handle<MeshTag> mesh = cy::Handle<MeshTag>::from_slot(3, 1);
    takes_texture(mesh);
}
