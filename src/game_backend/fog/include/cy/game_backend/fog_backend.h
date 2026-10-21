// SPDX-License-Identifier: MIT
#pragma once
// The fog of war adapter behind ABI 1.10's `fog_*` entries: `cy::gameplay::fog::FogOfWar` with a
// table of the entities that see.
//
// A game gives an entity a vision through `fog_set_vision` — its team, its reveal radius, whether
// it leaves a ghost — and the HOST runs the grid: after each fixed step it calls `update(tick)`,
// which asks the host's `FogPlacer` where every entity with a vision stands, in the same Q32.32
// metres the simulation keeps, and updates the grid. The units go to the grid ORDERED BY ENTITY,
// whatever order the game gave them their visions in, so the digest is a function of the state and
// not of the call order. A renderer reads `fog()`; nothing here draws.

#include <cy/abi/cy_abi.h>
#include <cy/abi/game/fog.h>
#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/gameplay/fog/fog_of_war.h>

namespace cy::game_backend {

/// Where an entity stands on the ground plane, Q32.32 metres; false when the host does not place it
/// — it was despawned, or it was never the host's.
using FogPlacer = bool (*)(CyEntity entity, i64& x_raw, i64& z_raw, void* user) noexcept;

class FogAdapter final : public abi::game::FogBackend {
public:
    explicit FogAdapter(Allocator& allocator) noexcept;

    /// Size the grid. Every vision is kept; every cell starts unexplored.
    [[nodiscard]] Status create(const gameplay::fog::FogGridDescription& description) noexcept;
    /// How the adapter learns where an entity stands. Without one, `set_vision` accepts any entity
    /// and `update` places none.
    void set_placer(FogPlacer placer, void* user) noexcept;
    /// Allies sharing vision. See `FogOfWar::share_vision`.
    [[nodiscard]] Status share_vision(gameplay::TeamId a, gameplay::TeamId b) noexcept;
    /// One fixed step: every entity with a vision placed and the grid updated. An entity the
    /// placer no longer places is left out, so a dead unit stops seeing and its ghost is
    /// forgotten the next time its place is seen.
    [[nodiscard]] Status update(u64 tick) noexcept;
    /// Forget an entity's vision: a despawn.
    void forget(CyEntity entity) noexcept;

    [[nodiscard]] const gameplay::fog::FogOfWar& fog() const noexcept { return fog_; }
    [[nodiscard]] u32 vision_count() const noexcept { return static_cast<u32>(visions_.size()); }

    // --- abi::game::FogBackend -------------------------------------------------------------------
    [[nodiscard]] CyResult set_vision(CyEntity entity, const CyFogVision& vision) noexcept override;
    [[nodiscard]] CyResult vision(CyEntity entity, CyFogVision& out) const noexcept override;
    [[nodiscard]] CyResult cell(u32 team, const CyFixedVec2& position,
                                u32& out) const noexcept override;
    [[nodiscard]] CyResult sight(u32 team, CyEntity entity, u32& out) const noexcept override;
    [[nodiscard]] CyResult status(CyFogStatus& out) const noexcept override;

private:
    struct Vision {
        CyEntity entity = 0;
        CyFogVision vision{};
    };

    [[nodiscard]] usize find(CyEntity entity) const noexcept;

    gameplay::fog::FogOfWar fog_;
    /// Ordered by entity, so `update` hands the grid one order whatever the calls' order was.
    Array<Vision> visions_;
    Array<gameplay::fog::FogUnit> units_;
    FogPlacer placer_ = nullptr;
    void* placer_user_ = nullptr;
};

/// Bind `adapter` as `host.game.fog`, or unbind with null.
void bind_fog(cy::abi::Host& host, FogAdapter* adapter) noexcept;

}  // namespace cy::game_backend
