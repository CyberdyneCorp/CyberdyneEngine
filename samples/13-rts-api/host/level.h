// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_API_LEVEL_H
#define CY_SAMPLE_RTS_API_LEVEL_H
// level.h — the content this host loads, and nothing that decides anything.
//
// A shipped game would read all of this out of cooked assets. The sample has no asset pipeline for
// sound or navigation, so it builds the content in code, the way samples/04-character generates its
// clips. Every item is registered under a NAME that game/Contract.swift also knows:
//
//   the ground          a 32 m square, a static box on collision layer 0
//   /Level              Barracks, Crate (a 20 kg dynamic box on layer 2), Commander and Scout
//   the navigation mesh one flat tile over the same square, navigation world 0
//   "units/worker"      a one-node prefab, registered resident with the spawn adapter
//   "unit.arrived"      a short generated click, registered as a cue with the audio adapter
//   "camera.pan"        a two-axis action on WASD and the arrow keys, in the "rts" context
//   "unit.spawn"        a digital action on B, in the same context
//
// None of these says how many units there are, where they go, how fast they move or what a click
// means. Those are Swift's (game/Commander.swift).

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/navigation/navmesh.h>
#include <cy/scene/scene.h>
#include <cy/servers/input/server.h>

namespace sample::rts {

/// The level's size and the one number the navigation mesh and the ground share.
inline constexpr cy::f32 kLevelSize = 32.0F;
/// Navigation cells per side of the tile: 2 m quads.
inline constexpr cy::u32 kNavCells = 16;
/// Collision layers. game/Contract.swift's `Layers` masks select them.
inline constexpr cy::u8 kGroundLayer = 0;
inline constexpr cy::u8 kUnitLayer = 1;
/// Level props and the scout's hero: neither a unit a click selects nor ground an order lands on.
inline constexpr cy::u8 kPropLayer = 2;
/// Where the crate starts: its centre, resting on the ground, away from the units' paths.
inline constexpr cy::Vec3 kCrateStart{28.0F, 0.5F, 6.0F};

/// The names game/Contract.swift's `Content` uses.
inline constexpr const char* kWorkerPrefab = "units/worker";
inline constexpr const char* kArrivedCue = "unit.arrived";
inline constexpr const char* kPanAction = "camera.pan";
inline constexpr const char* kSpawnAction = "unit.spawn";

/// One flat navigation tile covering the level, wound counter-clockwise from above.
[[nodiscard]] cy::navigation::NavTileData make_ground_tile(cy::Allocator& allocator) noexcept;

/// The worker prefab: one node named "Worker". Static storage; valid for the process.
[[nodiscard]] cy::scene::SceneDescription worker_prefab() noexcept;

/// A short decaying click, generated rather than loaded. The same samples on every machine.
[[nodiscard]] cy::Status make_click(cy::Array<cy::f32>& out, cy::u32 sample_rate) noexcept;

/// Declare the two actions and register and push the "rts" mapping context for user 0. Call
/// before `finalize_declarations` is due; this function calls it.
[[nodiscard]] cy::Status declare_input(cy::input::InputServer& input,
                                       cy::Allocator& allocator) noexcept;

}  // namespace sample::rts

#endif  // CY_SAMPLE_RTS_API_LEVEL_H
