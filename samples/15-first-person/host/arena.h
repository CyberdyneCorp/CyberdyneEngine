// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_FIRST_PERSON_ARENA_H
#define CY_SAMPLE_FIRST_PERSON_ARENA_H
// arena.h — the level samples/15-first-person plays in, as data.
//
// A walled courtyard twenty-eight metres square, open to a low sun: a raised platform the player
// has to jump onto, a beam across a passage the player has to crouch under, cover, two pillars, a
// glowing lamp strip on the north wall, a stack of crates and a barrel that a shot pushes, and four
// targets the engine's AI drives. Everything is a box, in metres, with its colour — the physics
// world, the GI and the renderer each read THIS table, so the three cannot disagree about where a
// wall is. The names a game reaches the content by (actions, cues, effects, decal materials, node
// paths, collision layers) are here too, and game/Contract.swift states the same names.
//
// THE ORIGIN IS THE COURTYARD'S CENTRE, +Y up, and the floor's top is y = 0. The player starts at
// the south end looking north (−Z), the targets stand in the north half.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>

namespace sample::fps {

/// What a box is made of, for the renderer's material table and the GI's albedo.
enum class Surface : cy::u8 {
    Concrete = 0,
    PaintRed,
    PaintTeal,
    Plaster,
    Steel,
    Lamp,
    Wood,
    Barrel,
    Target,
    TargetHead,
    WeaponMetal,
    WeaponPolymer,
    Sleeve,
    /// The sky the renderer hangs round the courtyard: emission only.
    Sky,
    Count,
};

/// One static box of the level.
struct ArenaBox {
    cy::Vec3 centre;
    cy::Vec3 half;
    Surface surface = Surface::Concrete;
};

/// One dynamic prop: a box a shot can push.
struct PropBox {
    const char* node = "";
    cy::Vec3 centre;
    cy::Vec3 half;
    cy::f32 mass = 20.0F;
    Surface surface = Surface::Wood;
};

/// One target the AI drives: where it stands, the other end of its patrol, and its node.
struct TargetSpot {
    const char* node = "";
    cy::Vec3 home;
    cy::Vec3 patrol;
};

/// The level's static boxes: the floor first, then the walls, the platform, the beam and its
/// posts, the cover and the pillars, and the lamp strip.
[[nodiscard]] cy::Span<const ArenaBox> arena_boxes() noexcept;
/// The props, in their nodes' order.
[[nodiscard]] cy::Span<const PropBox> prop_boxes() noexcept;
/// The targets, in their nodes' order.
[[nodiscard]] cy::Span<const TargetSpot> target_spots() noexcept;
/// The linear albedo of a surface, and its radiance when it glows.
[[nodiscard]] cy::Vec3 surface_albedo(Surface surface) noexcept;
[[nodiscard]] cy::Vec3 surface_emission(Surface surface) noexcept;
[[nodiscard]] cy::f32 surface_roughness(Surface surface) noexcept;
[[nodiscard]] cy::f32 surface_metallic(Surface surface) noexcept;

inline constexpr cy::u32 kFloorBox = 0;
/// The beam across the passage: its underside is at `kBeamUnderside`, which a standing character
/// (1.8 m) does not fit under and a crouching one (1.1 m) does.
inline constexpr cy::f32 kBeamUnderside = 1.4F;

/// A target's body: a box this tall and this wide, its feet on the floor, and its head above it.
inline constexpr cy::Vec3 kTargetHalf{0.32F, 0.8F, 0.22F};
inline constexpr cy::Vec3 kTargetHeadHalf{0.14F, 0.14F, 0.14F};
inline constexpr cy::u32 kTargetCount = 4;
inline constexpr cy::u32 kPropCount = 4;

/// Where the player starts: the feet, and which way it looks (yaw, radians, right of −Z).
inline constexpr cy::Vec3 kPlayerStart{5.0F, 0.0F, 10.0F};

// --- The collision layers: one bit each in a query's mask --------------------------------------
inline constexpr cy::u32 kWorldLayer = 0;
inline constexpr cy::u32 kPropLayer = 1;
inline constexpr cy::u32 kTargetLayer = 2;
inline constexpr cy::u32 kPlayerLayer = 3;
/// A fallen target's body: nothing is shot through it any more, and nothing collides with it.
inline constexpr cy::u32 kFallenLayer = 4;

// --- The names the game reaches the content by --------------------------------------------------
inline constexpr const char* kMoveAction = "player.move";
inline constexpr const char* kSprintAction = "player.sprint";
inline constexpr const char* kJumpAction = "player.jump";
inline constexpr const char* kCrouchAction = "player.crouch";
inline constexpr const char* kFireAction = "weapon.fire";
inline constexpr const char* kAimAction = "weapon.aim";
inline constexpr const char* kShotCue = "weapon.shot";
inline constexpr const char* kImpactCue = "weapon.impact";
inline constexpr const char* kHitCue = "target.hit";
inline constexpr const char* kDownCue = "target.down";
inline constexpr const char* kMuzzleEffect = "weapon.muzzle";
inline constexpr const char* kSparksEffect = "impact.sparks";
inline constexpr const char* kScorchMaterial = "impact.scorch";
inline constexpr const char* kPlayerNode = "Player";

/// The node paths a target and a prop are reached by: `/Arena/Targets/T0`, `/Arena/Props/Crate0`.
inline constexpr const char* kArenaNode = "Arena";
inline constexpr const char* kTargetsNode = "Targets";
inline constexpr const char* kPropsNode = "Props";

/// The sun: the direction its light travels, its illuminance in lux, and its colour.
inline constexpr cy::Vec3 kSunTravel{-0.42F, -0.62F, -0.66F};
inline constexpr cy::f32 kSunLux = 22000.0F;

/// The layer the arms and the weapon are on, which the weapon view draws and the world does not.
inline constexpr cy::u32 kWeaponLayerBit = 5;

/// The weapon: a receiver, a barrel, a grip, a magazine and a sleeve, in the WEAPON's space (−Z
/// along the barrel), metres — `render.weapon_view`'s weapon.
struct WeaponPart {
    cy::Vec3 centre;
    cy::Vec3 half;
    Surface surface = Surface::WeaponMetal;
};
[[nodiscard]] cy::Span<const WeaponPart> weapon_parts() noexcept;

/// The sky: five glowing panels far round the courtyard, which only the renderer draws — no body,
/// no GI card, no shadow. The engine's forward frame clears to black where nothing is drawn, and
/// an open courtyard under a 22 000 lux sun has a sky over it.
[[nodiscard]] cy::Span<const ArenaBox> sky_panels() noexcept;
/// Where the muzzle is, in the weapon's space: the barrel's tip.
inline constexpr cy::Vec3 kMuzzle{0.0F, 0.012F, -0.56F};

}  // namespace sample::fps

#endif  // CY_SAMPLE_FIRST_PERSON_ARENA_H
