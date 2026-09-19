// Fixture (i): a source file outside platform/ includes an X11 header. M11.d widened the `sdl`
// check from "no SDL header" to "no window-system header": SDL was the only implementation of
// Platform and DisplayServer when the rule was written, and the rule is about the porting surface
// rather than about SDL. A layer 4 file, so the layer rule alone would not catch it — platform/ is
// layer 3, and 3 is below 4.
#include <X11/Xlib.h>

#include "core/expected.h"

int cy_fixture_viewport() { return 0; }
