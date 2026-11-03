// SPDX-License-Identifier: MIT
// Issue #87: a file outside src/backends/audio-miniaudio/ that includes stb_vorbis. The library is
// included as `stb_vorbis.c` with STB_VORBIS_HEADER_ONLY, its own convention, so that is the name
// the rule matches.
//
// tools/layercheck/layercheck.py --check thirdparty must reject this file.

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

void touch_vorbis() {}
