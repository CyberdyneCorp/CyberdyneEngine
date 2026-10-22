// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_API_FOG_H
#define CY_SAMPLE_RTS_API_FOG_H
// fog.h — the level's fog of war, behind ABI 1.10.
//
// The host keeps the grid and nothing else: `cy::game_backend::FogAdapter` over a grid of 1 m
// cells covering everything the run draws — the level's 32 m square, or the RTS test map's whole
// terrain when the run is on it (fog_grid.h) — updated after every fixed step from where each
// entity with a vision stands. Who sees, how far, and what blue is shown is the Swift game's
// (game/Fog.swift).
//
// WHERE AN ENTITY STANDS is the one thing only the host knows: a worker's scene node, which the
// navigation adapter moves, and for the level's fixed nodes — the barracks and the enemy camp —
// the places below, which game/Contract.swift's `Camp` states too. A position goes to the grid as
// Q32.32 metres, converted once from the node's float, so every peer that placed a unit at the same
// float puts it in the same cells.

#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/game_backend/fog_backend.h>

#include <vector>

#include "fog_grid.h"

namespace sample::rts {

class NodeMotion;

/// The enemy camp: the level's nodes and where each stands. game/Contract.swift's `Camp`.
struct CampMember {
    const char* node = "";
    cy::Vec3 at{};
};
inline constexpr CampMember kCamp[] = {
    {"RaiderNear", cy::Vec3{8.0F, 0.0F, 13.0F}},
    {"RaiderFar", cy::Vec3{27.0F, 0.0F, 29.0F}},
    {"Watchtower", cy::Vec3{25.0F, 0.0F, 13.0F}},
};
inline constexpr cy::u32 kCampSize = 3;

class RtsFog {
public:
    explicit RtsFog(cy::Allocator& allocator) noexcept;

    /// Size the grid over `bounds`: the level's square, or the map the run draws (fog_grid.h).
    [[nodiscard]] cy::Status build(const cy::Aabb& bounds) noexcept;
    /// A level node that never moves, placed for the fog: the barracks, a member of the camp.
    void place(CyEntity entity, cy::Vec3 at);
    /// Where the workers stand: their scene nodes.
    void set_motion(const NodeMotion* motion) noexcept { motion_ = motion; }

    void bind(cy::abi::Host& host) noexcept;
    static void unbind(cy::abi::Host& host) noexcept;
    /// After the fixed step: every entity with a vision placed and the grid updated.
    [[nodiscard]] cy::Status update(cy::u64 tick) noexcept;

    [[nodiscard]] const cy::game_backend::FogAdapter& adapter() const noexcept { return adapter_; }
    [[nodiscard]] const cy::gameplay::fog::FogOfWar& grid() const noexcept {
        return adapter_.fog();
    }

private:
    static bool locate(CyEntity entity, cy::i64& x_raw, cy::i64& z_raw, void* user) noexcept;

    cy::game_backend::FogAdapter adapter_;
    std::vector<std::pair<CyEntity, cy::Vec3>> fixed_;
    const NodeMotion* motion_ = nullptr;
};

}  // namespace sample::rts

#endif  // CY_SAMPLE_RTS_API_FOG_H
