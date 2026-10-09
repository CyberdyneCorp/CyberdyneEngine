// SPDX-License-Identifier: MIT
#include "animation_assets.h"

#include <cy/core/assets/cooked.h>
#include <cy/core/assets/file.h>
#include <cy/import/gltf.h>
#include <cy/import/mesh.h>

namespace cy::sample::editor_window {

Status read_cooked_file(const std::string& path, Array<u8>& payload) noexcept {
    Array<u8> cooked(payload.allocator());
    if (Status read = assets::fs::read_whole(path.c_str(), cooked); !read) {
        return read;
    }
    Expected<Span<const u8>, Error> record =
        assets::read_cooked_payload(cooked.data(), cooked.size(), true);
    if (!record) {
        return make_unexpected(record.error());
    }
    payload.clear();
    return payload.append(*record);
}

Status read_cooked_record(const std::string& project, AssetId id, Array<u8>& payload) noexcept {
    if (project.empty()) {
        return fail(ErrorCode::Unavailable,
                    "this runtime has no project to read cooked assets from");
    }
    char text[AssetId::kTextLength + 1] = {};
    (void)id.format(text);
    return read_cooked_file(project + "/.cy/cooked/" + text + ".cyasset", payload);
}

Status ProjectAnimationAssets::read(AssetId id, Array<u8>& payload) noexcept {
    return read_cooked_record(project_, id, payload);
}

Status ProjectAnimationAssets::read_mesh(AssetId id, editor::AnimationPreviewMesh& out) noexcept {
    Array<u8> record(*allocator_);
    if (Status read = read_cooked_record(project_, id, record); !read) {
        return read;
    }
    import::MeshData mesh;
    if (Status decoded = import::read_cooked_mesh(record.span(), mesh); !decoded) {
        return decoded;
    }
    if (mesh.skin.size() != mesh.positions.size() || mesh.positions.empty()) {
        return fail(ErrorCode::InvalidArgument, "the mesh carries no skin");
    }
    if (mesh.normals.size() != mesh.positions.size()) {
        // A cooked mesh without normals is shaded smooth, as the importer's default angle would.
        constexpr f32 kSmooth = 180.0F;
        if (Status generated = import::generate_normals(mesh, kSmooth); !generated) {
            return generated;
        }
    }
    out.clear();
    if (Status copied = out.positions.append(mesh.positions.span()); !copied) {
        return copied;
    }
    if (Status copied = out.normals.append(mesh.normals.span()); !copied) {
        return copied;
    }
    for (const import::SkinInfluence& influence : mesh.skin) {
        if (Status joints = out.joints.append({influence.joints, import::kSkinInfluences});
            !joints) {
            return joints;
        }
        if (Status weights = out.weights.append({influence.weights, import::kSkinInfluences});
            !weights) {
            return weights;
        }
    }
    return out.indices.append(mesh.indices.span());
}

}  // namespace cy::sample::editor_window
