// The text stack's half of the same violation: the layer-2 text server reaching for HarfBuzz and
// ICU instead of going through `cy::text::TextBackend`.
//
// `text-and-fonts`: "no HarfBuzz, ICU, or FreeType type SHALL appear outside the backend".
//
// tools/layercheck/layercheck.py --check thirdparty must reject both includes.

#include <hb.h>
#include <unicode/ubidi.h>

void shape_here() {}
