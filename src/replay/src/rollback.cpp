// The rollback loop. M9 task 3.2.

#include <cy/replay/rollback.h>

namespace cy::replay {

EffectVerdict RollbackEngine::offer(u64 kind, u64 instance, bool predicted) noexcept {
    // ================================================================================================
    // THE MUTATION THAT PROVES THE DUPLICATE-EFFECT TEST CAN FAIL IS THIS FUNCTION'S BODY.
    //
    // Replacing everything below with `return EffectVerdict::Realise;` leaves the whole rollback
    // machinery intact — the window, the restore, the re-simulation, the commands — and removes
    // only the ledger's decision. `tests/test_rollback.cpp`'s duplicate-effect case then goes red
    // naming the number of explosions, which is the demonstration `replay-and-rollback`'s M9 delta
    // asks for and src/replay/README.md pastes.
    // ================================================================================================
    ++offered_;
    if (resimulating_) {
        ++window_offered_;
    }
    const EffectVerdict verdict = ledger_->realise(kind, instance, at_, predicted);
    if (verdict == EffectVerdict::SuppressedAlreadyRealised) {
        ++suppressed_;
        if (resimulating_) {
            ++window_suppressed_;
        }
    } else if (verdict == EffectVerdict::DeferredUntilConfirmed) {
        ++deferred_;
    }
    return verdict;
}

Status RollbackEngine::roll_back(u64 to_tick, u64 through_tick, ecs::World& world,
                                 const determinism::StateProviderRegistry& registry,
                                 const RollbackHooks& hooks, RollbackReport& out) noexcept {
    out = RollbackReport{};
    out.through_tick = through_tick;

    if (through_tick < to_tick) {
        // Backward simulation is not attempted, here or anywhere: "Reverse playback SHALL seek an
        // earlier checkpoint and replay forward."
        return fail(ErrorCode::InvalidArgument, "replay: a rollback window ends before it begins");
    }
    if (!hooks.runnable()) {
        return fail(ErrorCode::InvalidArgument, "replay: a rollback needs a step hook");
    }

    const determinism::SimulationPoint request{epochs_->current(), to_tick};
    WindowRefusal refusal = WindowRefusal::None;
    const StateCapture* capture = ring_->find(request, refusal);
    if (capture == nullptr) {
        // A REPORT AND NOT AN ERROR. The session decides between resynchronising, disconnecting and
        // continuing; this module's job is to say which side of the window the request fell off.
        out.refusal = refusal == WindowRefusal::None ? WindowRefusal::Empty : refusal;
        ++refusals_;
        return ok();
    }

    if (Status restored = capture->restore(world, registry); !restored) {
        return restored;
    }

    // The timeline has been reset, so the epoch is left. `epoch.h` requires the reason, and a
    // diagnostic reading a stale cache's stamp wants to be told "a checkpoint was restored".
    out.epoch = epochs_->advance(determinism::EpochReason::CheckpointRestore);
    out.restored_tick = capture->point().tick;

    RollbackCursor cursor(*log_);
    if (Status opened = cursor.open(out.restored_tick, through_tick); !opened) {
        return opened;
    }

    window_offered_ = 0;
    window_suppressed_ = 0;
    resimulating_ = true;
    Status outcome = ok();
    for (u64 tick = out.restored_tick; tick <= through_tick; ++tick) {
        scratch_.clear();
        if (Status gathered = cursor.commands_for(tick, scratch_); !gathered) {
            outcome = gathered;
            break;
        }
        // The point is set BEFORE the feed and the step, so every effect the tick offers carries
        // the tick it was produced at rather than the tick the rollback was requested from.
        set_point(determinism::SimulationPoint{out.epoch, tick});
        if (hooks.feed != nullptr) {
            if (Status fed = hooks.feed(hooks.user, tick, scratch_.span()); !fed) {
                outcome = fed;
                break;
            }
        }
        if (Status stepped = hooks.step(hooks.user, tick); !stepped) {
            outcome = stepped;
            break;
        }
        out.commands_replayed += static_cast<u32>(scratch_.size());
        ++out.ticks_resimulated;
    }
    resimulating_ = false;

    out.effects_offered = window_offered_;
    out.effects_suppressed = window_suppressed_;
    if (!outcome) {
        return outcome;
    }
    out.performed = true;
    ++rollbacks_;
    return ok();
}

void RollbackEngine::advance_window(u64 oldest_tick) noexcept {
    ledger_->prune_before(oldest_tick);
}

}  // namespace cy::replay
