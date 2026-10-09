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
};

}  // namespace cy::abi::game
