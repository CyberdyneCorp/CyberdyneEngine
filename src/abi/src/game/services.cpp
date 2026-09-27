// SPDX-License-Identifier: MIT
// cy/abi/game/services.h's one out-of-line function, and the stubs' shared answer.
// `add-swift-game-api`.
//
// The messages are concatenated by hand rather than with `snprintf`, as the rest of src/abi/ does:
// three fixed strings need no format engine on a path every refused call takes.

#include <cy/abi/game/services.h>

#include <cy/abi/errors.h>

#include "thunks.h"

namespace cy::abi::game {
namespace {

const char* phase_name(CyPhase phase) noexcept {
    switch (phase) {
        case CY_PHASE_NONE:
            return "none";
        case CY_PHASE_FIXED_UPDATE:
            return "fixed update";
        case CY_PHASE_FRAME_UPDATE:
            return "frame update";
    }
    return "an unknown phase";
}

/// A message assembled from up to three parts, truncated rather than overrun.
struct Message {
    char text[kLastErrorCapacity] = {};
    usize length = 0;

    Message& operator<<(const char* part) noexcept {
        for (const char* cursor = part; cursor != nullptr && *cursor != '\0'; ++cursor) {
            if (length + 1 >= kLastErrorCapacity) {
                break;
            }
            text[length++] = *cursor;
        }
        text[length] = '\0';
        return *this;
    }
};

}  // namespace

CyResult require_phase(const GameServices& services, u32 allowed, const char* entry) noexcept {
    if ((phase_bit(services.clock.phase) & allowed) != 0U) {
        return CY_RESULT_OK;
    }
    Message message;
    message << (entry != nullptr ? entry : "this entry") << " may not be called during "
            << phase_name(services.clock.phase);
    return report(CY_RESULT_PERMISSION_DENIED, message.text);
}

}  // namespace cy::abi::game
