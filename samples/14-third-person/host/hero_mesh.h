// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_THIRD_PERSON_HERO_MESH_H
#define CY_SAMPLE_THIRD_PERSON_HERO_MESH_H
// hero_mesh.h — the hero's skinned mesh, built in code from primitives.
//
// NO MESH IS A FILE, as in samples/13-rts-api/map/map_meshes.h: a head with its hair, a neck, a
// tapered torso with a satchel on its back, a belt over the hips, sleeves, forearms and hands, and
// trousers into boots — boxes, cylinders and spheres in the bind pose hero_rig.h's skeleton stands
// in, each vertex bound rigidly to the one joint its limb turns with. Faceted where a part is a box
// and smooth round a limb, the way a low-polygon game's characters are.
//
// Each part names its material; the renderer resolves the name against the level's materials
// (content/third-person/hillside.cymap declares them).

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>

#include <string>
#include <vector>

namespace sample::tps {

/// A run of the mesh's indices drawn with one material.
struct HeroPart {
    cy::u32 first_index = 0;
    cy::u32 index_count = 0;
    std::string material;
};

struct HeroMesh {
    std::vector<cy::Vec3> positions;
    std::vector<cy::Vec3> normals;
    std::vector<cy::Vec2> uvs;
    /// The one joint each vertex follows.
    std::vector<cy::u8> joints;
    std::vector<cy::u32> indices;
    std::vector<HeroPart> parts;
};

/// The hero, in the bind pose, facing −Z.
[[nodiscard]] HeroMesh build_hero_mesh();

}  // namespace sample::tps

#endif  // CY_SAMPLE_THIRD_PERSON_HERO_MESH_H
