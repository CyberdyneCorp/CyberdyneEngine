// SPDX-License-Identifier: MIT
// The `time` thunk of ABI 1.3. `add-swift-game-api`.
//
// OWNER: implementer C (audio, spawning and time). See
// openspec/changes/add-swift-game-api/design.md.
//
// `time_get` has no backend: the clock lives on the host (`GameServices::clock`), written by the
// embedder and by `BehaviourRuntime` at phase boundaries. It is never refused by phase, because it
// is how a caller learns the phase.
//
// DETERMINISM. In a fixed step `frame_delta` and `interpolation` are written as zero, whatever the
// clock holds, so a fixed step cannot come to depend on the frame rate.

#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>

#include "thunks.h"

namespace cy::abi::game {

CyResult time_get(CyEngine engine, CyTime* out_time) {
    if (engine == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "the engine is null");
    }
    if (const CyResult allowed = require_phase(engine->game, kPhaseAny, "time_get");
        allowed != CY_RESULT_OK) {
        return allowed;
    }
    if (out_time == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "time_get needs an output");
    }

    const GameClock& clock = engine->game.clock;
    const bool fixed = clock.phase == CY_PHASE_FIXED_UPDATE;
    CyTime now{};
    now.struct_size = sizeof(CyTime);
    now.phase = static_cast<u32>(clock.phase);
    now.tick = clock.tick;
    now.fixed_delta = clock.fixed_delta;
    now.frame_delta = fixed ? 0.0 : clock.frame_delta;
    now.interpolation = fixed ? 0.0 : clock.interpolation;
    now.flags = clock.flags;
    if (!write_sized(*out_time, now)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "time_get: the CyTime struct_size is malformed");
    }
    clear_last_error();
    return CY_RESULT_OK;
}

}  // namespace cy::abi::game
