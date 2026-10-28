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
//
// ABI 1.16: arming and fortifying go to both peers, as enlisting does. The issuer's combat events
// are kept, after every tick, in a bounded history (`kEventHistory`) numbered as the session
// numbered them, which `lockstep_events` answers from: a game reads them from a frame update, and
// however many ticks ran since the last frame, each event once.

#include <cy/abi/game/lockstep.h>
#include <cy/abi/host.h>
#include <cy/core/memory/array.h>
#include <cy/game_backend/lockstep_session.h>

namespace cy::game_backend {

class LockstepAdapter final : public abi::game::LockstepBackend {
public:
    /// Over `issuer`, and `follower` when not null. Both are borrowed and outlive the adapter.
    /// `history` is how many of the most recent events `lockstep_events` can still hand out.
    LockstepAdapter(LockstepSession& issuer, LockstepSession* follower = nullptr,
                    u32 history = kEventHistory) noexcept;

    /// The events a host keeps by default: minutes of a large battle at sixty ticks a second.
    static constexpr u32 kEventHistory = 4096;

    [[nodiscard]] CyResult enlist(const CyLockstepUnitDesc& desc, u32& out_unit) noexcept override;
    [[nodiscard]] CyResult order(const CyLockstepOrder& order) noexcept override;
    [[nodiscard]] CyResult unit(u32 index, CyLockstepUnit& out) const noexcept override;
    [[nodiscard]] CyResult status(CyLockstepStatus& out) const noexcept override;
    [[nodiscard]] CyResult arm(u32 unit, const CyLockstepCombatDesc& desc) noexcept override;
    [[nodiscard]] CyResult fortify(const CyLockstepStructureDesc& desc,
                                   u32& out_structure) noexcept override;
    [[nodiscard]] CyResult combatant(u32 unit, CyLockstepCombatant& out) const noexcept override;
    [[nodiscard]] CyResult structure(u32 index, CyLockstepStructure& out) const noexcept override;
    [[nodiscard]] CyResult events(u64 first, CyLockstepEvent* out, u32 capacity,
                                  u32& out_count) const noexcept override;
    [[nodiscard]] CyResult projectiles(CyLockstepProjectile* out, u32 capacity,
                                       u32& out_count) const noexcept override;

    /// One fixed step: the follower receives the issuer's commands for this tick, both advance,
    /// and their state hashes are compared.
    [[nodiscard]] Status tick(jobs::JobSystem* jobs = nullptr) noexcept;

    [[nodiscard]] u32 disagreements() const noexcept { return disagreements_; }

    /// Events the history holds, oldest first: the issuer's, numbered as it made them.
    [[nodiscard]] u32 history() const noexcept { return static_cast<u32>(history_.size()); }

private:
    [[nodiscard]] Status keep(Span<const LockstepEvent> made) noexcept;

    LockstepSession& issuer_;
    LockstepSession* follower_ = nullptr;
    /// A ring of `capacity_`: `oldest_` is the slot of the first event it holds.
    Array<LockstepEvent> history_;
    u32 capacity_ = kEventHistory;
    u32 oldest_ = 0;
    u32 follower_cursor_ = 0;
    u32 disagreements_ = 0;
};

/// Bind `adapter` as `host.game.lockstep`, or unbind with null.
void bind_lockstep(abi::Host& host, LockstepAdapter* adapter) noexcept;

}  // namespace cy::game_backend
