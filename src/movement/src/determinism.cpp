// SPDX-License-Identifier: MIT
// The mover's build configuration, read where the mover is compiled. See determinism.h.

#include <cy/movement/determinism.h>

namespace cy::movement {

determinism::BuildConfiguration movement_build() noexcept {
    return determinism::BuildConfiguration::from_build();
}

}  // namespace cy::movement
