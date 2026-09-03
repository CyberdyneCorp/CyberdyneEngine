// Fixture (c): a source file outside platform/ includes an SDL header. SDL sits beneath the
// engine-owned Platform and DisplayServer interfaces and never appears above platform/ — design.md
// §4. This one is a layer 5 file, so the layer rule alone would not catch it: platform/ is layer 3,
// and 3 is below 5.
#include <SDL3/SDL.h>

#include "core/expected.h"

int cy_fixture_tick() { return 0; }
