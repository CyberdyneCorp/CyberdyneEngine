// SPDX-License-Identifier: MIT
// `node_find`, ABI 1.5. `add-swift-m12-gaps`.
//
// `[N F U]`: the tree's shape is simulation state, and names are unique among siblings, so a path
// names one node or none on every run. The backend is `cy::game_backend::ScriptSceneBridge`.

#include <cy/abi/errors.h>
#include <cy/abi/game/scene.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>

#include "thunks.h"

namespace cy::abi::game {

CyResult node_find(CyEngine engine, CyEntity from, const char* path, CyEntity* out_entity) {
    if (engine == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "the engine is null");
    }
    if (const CyResult phase = require_phase(engine->game, kPhaseAny, "node_find");
        phase != CY_RESULT_OK) {
        return phase;
    }
    if (engine->game.scene == nullptr) {
        return report(CY_RESULT_UNAVAILABLE, "no scene backend is bound to this engine");
    }
    if (path == nullptr || path[0] == '\0' || out_entity == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "node_find needs a path and an output");
    }
    CyEntity found = CY_ENTITY_NULL;
    if (const CyResult result = engine->game.scene->find(from, path, found);
        result != CY_RESULT_OK) {
        return result;
    }
    *out_entity = found;
    clear_last_error();
    return CY_RESULT_OK;
}

}  // namespace cy::abi::game
