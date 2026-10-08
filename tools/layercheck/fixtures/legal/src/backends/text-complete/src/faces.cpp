// The control for the text stack: the backend module that owns FreeType, HarfBuzz, msdfgen and ICU
// may include all four.
#include <ft2build.h>
#include <hb.h>
#include <msdfgen.h>
#include <unicode/ubidi.h>

void open_face() {}
