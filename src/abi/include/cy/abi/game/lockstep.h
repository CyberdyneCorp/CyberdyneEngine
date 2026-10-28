// SPDX-License-Identifier: MIT
// cy/abi/game/lockstep.h — the backend behind ABI 1.8's `lockstep_*` entries.
// openspec/changes/add-deterministic-math stage 8.
//
// Over a fixed-point session: `cy::game_backend::LockstepAdapter` (src/game_backend/lockstep/), the
// kinematic mover, the converted navigation mesh and a `cy::gameplay::CommandStream` under
// `Lockstep`. The thunk has checked the phase, the pointers, the enumerators and `struct_size`;
// the backend answers the domain facts — a unit index never enlisted, a group no unit is in, an
// enlistment after the first tick.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>

namespace cy::abi::game {

/// What `lockstep_*` reaches on the engine side. Without one bound, the entries answer
/// `CY_RESULT_UNAVAILABLE`.
class LockstepBackend {
public:
    virtual ~LockstepBackend() = default;

    /// `lockstep_enlist`: `desc` is whole. PERMISSION_DENIED once the session has ticked.
    [[nodiscard]] virtual CyResult enlist(const CyLockstepUnitDesc& desc,
                                          u32& out_unit) noexcept = 0;
    /// `lockstep_order`: `order` is whole and its kind is a CyLockstepOrderKind. NOT_FOUND for a
    /// group no unit is in.
    [[nodiscard]] virtual CyResult order(const CyLockstepOrder& order) noexcept = 0;
    /// `lockstep_unit`: NOT_FOUND for an index never enlisted. `out` is whole.
    [[nodiscard]] virtual CyResult unit(u32 index, CyLockstepUnit& out) const noexcept = 0;
    /// `lockstep_status`: `out` is whole.
    [[nodiscard]] virtual CyResult status(CyLockstepStatus& out) const noexcept = 0;

    // --- ABI 1.16: combat. A backend without combat answers UNSUPPORTED. ---------------------
    /// `lockstep_arm`: `desc` is whole, its weapon a CyLockstepWeapon and its reserved word zero.
    [[nodiscard]] virtual CyResult arm(u32 unit, const CyLockstepCombatDesc& desc) noexcept;
    /// `lockstep_fortify`: `desc` is whole.
    [[nodiscard]] virtual CyResult fortify(const CyLockstepStructureDesc& desc,
                                           u32& out_structure) noexcept;
    /// `lockstep_combatant`: `out` is whole.
    [[nodiscard]] virtual CyResult combatant(u32 unit, CyLockstepCombatant& out) const noexcept;
    /// `lockstep_structure`: `out` is whole.
    [[nodiscard]] virtual CyResult structure(u32 index, CyLockstepStructure& out) const noexcept;
    /// `lockstep_events`: write up to `capacity` events numbered `first` and after into `out`
    /// (null when `capacity` is zero) and set `out_count` to the number written — or, asked for
    /// the count, to the number there are.
    [[nodiscard]] virtual CyResult events(u64 first, CyLockstepEvent* out, u32 capacity,
                                          u32& out_count) const noexcept;
    /// `lockstep_projectiles`: as `events`, of what is in flight.
    [[nodiscard]] virtual CyResult projectiles(CyLockstepProjectile* out, u32 capacity,
                                               u32& out_count) const noexcept;
};

}  // namespace cy::abi::game
