// SPDX-License-Identifier: MIT
#pragma once
// The decals adapter behind ABI 1.15's `decal_*` entries: the decal materials a host described,
// registered by name, and the `cy::rendering::DecalBudget` a game's marks join.
//
// The HOST owns the budget and the materials. It registers each material a game may spawn with
// under a name (`add_material`) — the index it names in the frame's decal table — binds the
// adapter, and once a frame tells it the time (`begin_frame`) and hands the budget's decals to the
// frame (`AssemblyView::decals`, `pack_decal_table`). A game resolves a material once and spawns
// marks; when the budget is full its own eviction decides, deterministically, which mark goes.
// Nothing here draws, and nothing feeds back into a fixed step.

#include <cy/abi/cy_abi.h>
#include <cy/abi/game/decals.h>
#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/rendering/lighting/decals.h>

namespace cy::game_backend {

/// The engine's decal for an ABI description: the box from the pose and the half extents, the
/// fades with their zero defaults taken, the material's index. Every channel, and the weights the
/// material decides — a mark writes albedo, roughness and relief, never metallic or emission
/// unless its material emits.
[[nodiscard]] rendering::DecalInstance decal_of(const CyDecalDesc& desc, u32 material_index,
                                                bool emissive) noexcept;

class DecalsAdapter final : public abi::game::DecalsBackend {
public:
    /// Spawns into `budget`, which outlives the adapter.
    DecalsAdapter(Allocator& allocator, rendering::DecalBudget& budget) noexcept;

    /// Register a material under `name`: `index` is what the frame's decal table resolves
    /// (`DecalTableInput::materials`), `emissive` whether its marks write emission. Its handle is
    /// its registration order, from one. Refuses a name already registered.
    [[nodiscard]] Status add_material(const char* name, u32 index, bool emissive = false) noexcept;

    /// The frame's time, which a mark is stamped with and ages by. Before the frame's scripts.
    void begin_frame(f64 now_seconds) noexcept { now_ = now_seconds; }

    [[nodiscard]] u64 spawned() const noexcept { return spawned_; }
    [[nodiscard]] u64 evicted() const noexcept { return evicted_; }

    // --- abi::game::DecalsBackend ----------------------------------------------------------------
    [[nodiscard]] CyResult find_material(const char* name,
                                         CyDecalMaterial& out) const noexcept override;
    [[nodiscard]] CyResult spawn(const CyDecalDesc& desc, CyDecal& out) noexcept override;
    [[nodiscard]] CyResult status(CyDecalStatus& out) const noexcept override;

private:
    struct Material {
        Name name;
        u32 index = 0;
        bool emissive = false;
    };

    rendering::DecalBudget* budget_;
    Array<Material> materials_;
    f64 now_ = 0.0;
    u64 spawned_ = 0;
    u64 evicted_ = 0;
};

/// Bind `adapter` as `host.game.decals`, or unbind with null.
void bind_decals(cy::abi::Host& host, DecalsAdapter* adapter) noexcept;

}  // namespace cy::game_backend
