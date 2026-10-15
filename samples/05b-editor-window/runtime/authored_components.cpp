// SPDX-License-Identifier: MIT
// See authored_components.h.

#include "authored_components.h"

#include <cy/core/assets/cooked.h>
#include <cy/core/assets/file.h>
#include <cy/import/gltf.h>
#include <cy/import/primitive.h>

namespace cy::sample::editor_window {

Status read_primitive_mesh(const std::string& path, Allocator& allocator,
                           import::MeshData& out) noexcept {
    Array<u8> source(allocator);
    if (Status status = assets::fs::read_whole(path.c_str(), source); !status) {
        return status;
    }
    const std::string_view text(reinterpret_cast<const char*>(source.data()), source.size());
    Expected<import::PrimitiveSpec, Error> spec = import::parse_primitive_source(text);
    if (!spec) {
        return make_unexpected(spec.error());
    }
    return import::build_primitive_mesh(*spec, out);
}

Status read_cooked_mesh_file(const std::string& path, Allocator& allocator,
                             import::MeshData& out) noexcept {
    Array<u8> cooked(allocator);
    if (Status status = assets::fs::read_whole(path.c_str(), cooked); !status) {
        return status;
    }
    Expected<Span<const u8>, Error> payload =
        assets::read_cooked_payload(cooked.data(), cooked.size(), true);
    if (!payload) {
        return make_unexpected(payload.error());
    }
    return import::read_cooked_mesh(*payload, out);
}

Status read_mesh_reference(const std::string& project, const std::string& reference,
                           Allocator& allocator, import::MeshData& out) noexcept {
    const bool primitive = reference.ends_with(".cyprim");
    const std::string path =
        primitive ? project + "/" + reference : project + "/.cy/cooked/" + reference + ".cyasset";
    Status read = primitive ? read_primitive_mesh(path, allocator, out)
                            : read_cooked_mesh_file(path, allocator, out);
    if (read) {
        read = out.validate();
    }
    return read;
}

}  // namespace cy::sample::editor_window
