// The second half of the same violation: a file outside src/backends/audio-miniaudio/ that includes
// miniaudio.h.
//
// `audio`: "No backend type SHALL appear in any engine or game-facing header outside its module."
// Two libraries in one fixture because one rule with one example is a rule that has been checked for
// one library.
//
// tools/layercheck/layercheck.py --check thirdparty must reject this file too.

#include "miniaudio.h"

void touch_audio() {
    ma_device device;
    (void)device;
}
