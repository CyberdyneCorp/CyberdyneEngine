// SPDX-License-Identifier: MIT
// `register_system`, ABI 1.5. `add-swift-m12-gaps`.
//
// Registration only: the thunk checks the engine, the phase (`[N]` — systems are declared while a
// module is brought up, never from inside a frame) and the descriptor's `struct_size`, and hands
// the whole descriptor to the host's registry. Scheduling is `cy::abi::ScriptSystems`', at the
// embedder's frame boundary, because a schedule is rebuilt between frames and never during one.

#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>

#include "thunks.h"

namespace cy::abi::game {

CyResult register_system(CyEngine engine, const CySystemDesc* desc) {
    if (engine == nullptr || desc == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "register_system needs an engine and a desc");
    }
    if (const CyResult phase = require_phase(engine->game, kPhaseNone, "register_system");
        phase != CY_RESULT_OK) {
        return phase;
    }
    CySystemDesc whole{};
    if (!read_sized(*desc, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "the system desc has a malformed struct_size");
    }
    if (const Status registered = engine->register_system(whole); !registered) {
        return report(registered.error());
    }
    clear_last_error();
    return CY_RESULT_OK;
}

}  // namespace cy::abi::game
