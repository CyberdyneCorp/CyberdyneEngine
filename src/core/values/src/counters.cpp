// See counters.h.

#include "counters.h"

namespace cy::values::detail {

Counters& counters() noexcept {
    static Counters instance;
    return instance;
}

}  // namespace cy::values::detail
