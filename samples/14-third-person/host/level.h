// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_THIRD_PERSON_LEVEL_H
#define CY_SAMPLE_THIRD_PERSON_LEVEL_H
// level.h — the courtyard's pieces, and the input the game reads.
//
// THE PIECES are content built in code: boxes the RTS map's kit has no mesh for. They are what the
// demo is about — the hero climbs them, runs down them, and the camera is pushed in by them:
//
//   stairs     eight 18 cm risers on 32 cm treads up to the platform, three metres wide
//   platform   10 x 7.4 m at 1.44 m, with a parapet on its west edge and two pillars at its front
//   ramp       eight metres down from the platform's east edge to the courtyard: a 10-degree slope
//   north wall 4.4 m high along the platform's back: what pulls the camera in when the player
//              turns it round the hero standing at the platform's back
//   walls      the courtyard's, 1.6 m, open at the south arch and at the ramp's foot to the east
//   arch       two pillars and a lintel over the courtyard's south gate
//   floor      the courtyard's flagstones as a collider, not drawn (the terrain draws them): so the
//              game can tell stone from grass by what the character stands on
//
// Each is a static box collider on the physics world, drawn with its material, and held by the
// GI. The ramp is the one turned box: about +Z by its slope.
//
// THE INPUT: four actions — `hero.move` (WASD), `camera.look` (the arrow keys), `hero.jump`
// (space) and `hero.sprint` (left shift) — in one mapping context on user 0.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/servers/input/server.h>

#include <vector>

namespace sample::tps {

/// One box of the courtyard.
struct LevelPiece {
    const char* name = "";
    cy::Vec3 centre{0.0F, 0.0F, 0.0F};
    cy::Vec3 half{0.5F, 0.5F, 0.5F};
    /// Radians about +Z: the ramp's slope. Zero for every other piece.
    cy::f32 roll = 0.0F;
    /// The level material it is drawn with; empty for a collider that is not drawn.
    const char* material = "";
    /// Linear albedo, for the GI's cards.
    cy::Vec3 albedo{0.6F, 0.58F, 0.54F};
};

/// Every piece, in a fixed order.
[[nodiscard]] const std::vector<LevelPiece>& level_pieces();

/// The courtyard floor's name: the collider the game hears stone on.
inline constexpr const char* kFloorPiece = "floor";

/// The input's actions, by the names game/Contract.swift reads.
inline constexpr const char* kMoveAction = "hero.move";
inline constexpr const char* kLookAction = "camera.look";
inline constexpr const char* kJumpAction = "hero.jump";
inline constexpr const char* kSprintAction = "hero.sprint";

/// Declare the four actions, bind them, and push the context on user 0.
[[nodiscard]] cy::Status declare_input(cy::input::InputServer& input,
                                       cy::Allocator& allocator) noexcept;

}  // namespace sample::tps

#endif  // CY_SAMPLE_THIRD_PERSON_LEVEL_H
