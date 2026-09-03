// A relative include of a sibling at the same layer, and a downward include spelled with the src/
// prefix. Both spellings resolve to the same layer.
#include "runtime.h"
#include "src/core/allocator.h"

namespace fx {
int main_loop() { return 0; }
}
