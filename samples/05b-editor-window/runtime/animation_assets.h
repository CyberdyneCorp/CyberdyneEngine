// SPDX-License-Identifier: MIT
// animation_assets.h — a project's cooked animation records, read for the animation panel (#29).
//
// The importer writes every cooked sub-asset as `<project>/.cy/cooked/<id>.cyasset`: a cooked
// header (`cy/core/assets/cooked.h`) and the record. This reads a record by id for the panel's
// preview character and its bake (`cy::editor::AnimationAssetSource`), and turns a cooked mesh
// (`cy::import::read_cooked_mesh`) into the bind-pose mesh the preview draws skinned.

#pragma once

#include <cy/editor/animation_character.h>

#include <string>

namespace cy::sample::editor_window {

/// Read the record of cooked asset `id` under `project`, checking its header and hash.
[[nodiscard]] Status read_cooked_record(const std::string& project, AssetId id,
                                        Array<u8>& payload) noexcept;
/// Read a cooked file at `path`, checking its header and hash, into `payload`.
[[nodiscard]] Status read_cooked_file(const std::string& path, Array<u8>& payload) noexcept;

/// A project's cooked assets, as the animation panel's character reads them.
class ProjectAnimationAssets final : public editor::AnimationAssetSource {
public:
    ProjectAnimationAssets(Allocator& allocator, const char* project) noexcept
        : allocator_(&allocator), project_(project != nullptr ? project : "") {}

    [[nodiscard]] Status read(AssetId id, Array<u8>& payload) noexcept override;
    [[nodiscard]] Status read_mesh(AssetId id, editor::AnimationPreviewMesh& out) noexcept override;

private:
    Allocator* allocator_;
    std::string project_;
};

}  // namespace cy::sample::editor_window
