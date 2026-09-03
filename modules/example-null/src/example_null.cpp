#include <cy/modules/example_null.h>

#include <cy_features.h>
#include <cy_modules.h>

// This translation unit is compiled only when the module is enabled, so the generated header must
// agree that it is. If it does not, the option and the header have come apart and every later
// #ifdef in the tree is unreliable.
#if !defined(CY_MODULE_EXAMPLE_NULL)
#    error \
        "example-null is being compiled, but CY_MODULE_EXAMPLE_NULL is not defined in cy_modules.h"
#endif

namespace cy::modules {
namespace {

// Expand the generated table once, so that a malformed one is a compile error in an ordinary engine
// translation unit rather than a surprise in whichever file first uses it.
// A row of an X-macro accumulator: the expansion is `0 +1 +1 ...`, so parenthesising the
// replacement list would produce `0 (+1)(+1)`, which is a call rather than a sum.
// NOLINTNEXTLINE(bugprone-macro-parentheses)
#define CY_EXAMPLE_NULL_COUNT_ROW(name, ident, level, level_index, layer, layer_index, hot) +1
constexpr int kTableRows = 0 CY_MODULE_TABLE(CY_EXAMPLE_NULL_COUNT_ROW);
#undef CY_EXAMPLE_NULL_COUNT_ROW

static_assert(kTableRows == CY_MODULE_COUNT,
              "CY_MODULE_TABLE and CY_MODULE_COUNT disagree: the generator is at fault");
static_assert(CY_FEATURE_COUNT > 0, "cy_features.h declares no features");

int g_registration_depth = 0;

}  // namespace

ModuleDescription example_null_description() {
    return ModuleDescription{
        /*name=*/"example-null",
        /*registration_level=*/"Core",
        /*registration_level_index=*/0,
        /*layer=*/"core",
        /*layer_index=*/0,
        /*hot_reload=*/false,
    };
}

void example_null_register() {
    ++g_registration_depth;
}

void example_null_unregister() {
    --g_registration_depth;
}

int example_null_registration_depth() {
    return g_registration_depth;
}

}  // namespace cy::modules
