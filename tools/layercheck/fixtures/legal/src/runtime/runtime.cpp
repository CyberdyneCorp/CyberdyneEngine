// Layer 5 including layer 0. Downward, so legal.
#include "core/allocator.h"

namespace fx {
int tick() { return allocate(); }
}
