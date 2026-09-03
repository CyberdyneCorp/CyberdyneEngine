// Fixture (b): a source file under src/core/ (layer 0) includes a header from src/scene/ (layer 4).
// No CMake target declares this link, which is exactly why the source-level check exists.
#include "core/allocator.h"
#include "scene/node.h"

int cy_fixture_allocate() { return 0; }
