// The one place an SDL header may appear.
#include <SDL3/SDL.h>

#include "core/allocator.h"

namespace fx {
int open_window() { return allocate(); }
}
