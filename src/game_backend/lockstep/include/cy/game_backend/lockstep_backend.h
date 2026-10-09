// SPDX-License-Identifier: MIT
#pragma once
// The lockstep adapter behind ABI 1.8's `lockstep_*` entries. openspec/changes/
// add-deterministic-math stage 8.
//
// It answers the entries from an ISSUING session (lockstep_session.h) the embedder owns, and — when
// the embedder gives it one — drives a FOLLOWER: a second session, built separately, that receives
// only the issuer's command log and must agree with it on every tick. That is the claim lockstep
// makes, checked in the process that runs the game; `CyLockstepStatus::disagreements` is the count
// of ticks it failed.
//
// The embedder calls `tick()` once per fixed step, after the scripts' fixed updates have recorded
// their orders for it; the first `tick()` starts both sessions, which is when enlisting stops.

#include <cy/abi/game/lockstep.h>
#include <cy/abi/host.h>
#include <cy/game_backend/lockstep_session.h>

namespace cy::game_backend {

class LockstepAdapter final : public abi::game::LockstepBackend {
public:
    /// Over `issuer`, and `follower` when not null. Both are borrowed and outlive the adapter.
    explicit LockstepAdapter(LockstepSession& issuer, LockstepSession* follower = nullptr) noexcept
        : issuer_(issuer), follower_(follower) {}

    [[nodiscard]] CyResult enlist(const CyLockstepUnitDesc& desc, u32& out_unit) noexcept override;
    [[nodiscard]] CyResult order(const CyLockstepOrder& order) noexcept override;
    [[nodiscard]] CyResult unit(u32 index, CyLockstepUnit& out) const noexcept override;
    [[nodiscard]] CyResult status(CyLockstepStatus& out) const noexcept override;

    /// One fixed step: the follower receives the issuer's commands for this tick, both advance,
    /// and their state hashes are compared.
    [[nodiscard]] Status tick(jobs::JobSystem* jobs = nullptr) noexcept;

    [[nodiscard]] u32 disagreements() const noexcept { return disagreements_; }

private:
    LockstepSession& issuer_;
    LockstepSession* follower_ = nullptr;
    u32 follower_cursor_ = 0;
    u32 disagreements_ = 0;
};

/// Bind `adapter` as `host.game.lockstep`, or unbind with null.
void bind_lockstep(abi::Host& host, LockstepAdapter* adapter) noexcept;

}  // namespace cy::game_backend
