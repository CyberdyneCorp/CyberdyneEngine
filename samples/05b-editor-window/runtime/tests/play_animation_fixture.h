// SPDX-License-Identifier: MIT
#pragma once
// A project with the hero imported and a graph baked for it, as the editor leaves one: shared by
// `integration.editor_window_play_animation` and `smoke.editor_animation_events`.

#include <cy/core/assets/cooked.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/editor/animation_rig.h>
#include <cy/test/test.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "animation_assets.h"
#include "animation_character_fixture.h"

namespace cy::sample::editor_window::testing {

inline constexpr AssetId kSkeletonId{0x0e0e, 1};
inline constexpr AssetId kClipId{0x0e0e, 2};
inline constexpr AssetId kMeshId{0x0e0e, 3};

/// Two events between the ticks of a 60 Hz clock: 0.26 s is crossed by tick 16, 0.76 s by tick 46.
inline constexpr std::string_view kGraph =
    "cygraph 1\n"
    "graph \"hero\" version 1\n"
    "capability\n"
    "deterministic true\n"
    "node 1 \"pose.clip\" v1 {\n"
    "    prop \"clip\" : \"name\" = \"hero\"\n"
    "    prop \"duration\" : \"float\" = (1, 0, 0, 0, 0)\n"
    "    prop \"events\" : \"name\" = \"footstep@0.26; land@0.76\"\n"
    "    prop \"loop\" : \"bool\" = (0, 0, 0, 0, 1)\n"
    "    prop \"time_parameter\" : \"name\" = (0, 0, 0, 0, 0)\n"
    "}\n"
    "node 2 \"pose.state\" v1 {\n"
    "    prop \"name\" : \"name\" = \"idle\"\n"
    "}\n"
    "link 1 \"pose\" -> 2 \"pose\"\n";

inline Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

inline void write_file(const std::filesystem::path& path, const void* data, usize size) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    CY_REQUIRE(out.good());
}

/// What the importer leaves in a project for one cooked sub-asset.
inline void write_cooked(const std::filesystem::path& project, AssetId id,
                         const editor::testing::CookedPart& part) {
    Array<u8> file(allocator());
    CY_REQUIRE(assets::write_cooked_asset(part.kind, assets::VariantKey{},
                                          Span<const u8>(part.payload.data(), part.payload.size()),
                                          file)
                   .has_value());
    char text[AssetId::kTextLength + 1] = {};
    (void)id.format(text);
    write_file(project / ".cy" / "cooked" / (std::string(text) + ".cyasset"), file.data(),
               file.size());
}

/// A project with the hero imported and `kGraph` baked for it, as the editor leaves one.
inline std::filesystem::path baked_project(std::string_view name) {
    const std::filesystem::path project = std::filesystem::path(CY_TEST_BINARY_DIR) / name;
    std::filesystem::remove_all(project);
    const editor::testing::ImportedCharacter hero = editor::testing::import_character();
    write_cooked(project, kSkeletonId, hero.skeleton);
    write_cooked(project, kClipId, hero.clip);
    write_cooked(project, kMeshId, hero.mesh);

    ProjectAnimationAssets assets(allocator(), project.string().c_str());
    editor::AnimationRigBaker baker(allocator(), assets);
    const editor::AnimationCharacterClip clips[] = {{Name::intern("hero"), kClipId}};
    editor::AnimationBakeRequest request;
    request.rig = "hero";
    request.source = kGraph;
    request.character.model = "characters/hero.fbx";
    request.character.skeleton = kSkeletonId;
    request.character.mesh = kMeshId;
    request.character.clips = Span<const editor::AnimationCharacterClip>(clips, 1);
    editor::AnimationBakeResult result(allocator());
    CY_REQUIRE(baker.bake(request, result).has_value());
    CY_REQUIRE(result.baked);
    const std::filesystem::path rig = project / ".cy" / "cooked" / "animation" / "hero";
    for (const editor::AnimationBakedFile& file : result.files) {
        write_file(rig / std::string(file.path.data(), file.path.size()), file.bytes.data(),
                   file.bytes.size());
    }
    return project;
}

}  // namespace cy::sample::editor_window::testing
