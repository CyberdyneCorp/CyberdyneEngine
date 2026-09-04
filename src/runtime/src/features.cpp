#include <cy/runtime/features.h>

#include <cy_features.h>

#include <cstring>

namespace cy::runtime {
namespace {

// The one place in the engine where the generated feature macros are read. Everything else asks
// `features()`. `CY_FEATURE_TABLE` is emitted by tools/gen/generate_headers.py precisely so that
// this expansion is possible without naming a single feature here — adding a feature to
// cmake/features.cmake therefore adds a row with no edit to this file.
#define CY_RUNTIME_FEATURE_ROW(name, enabled) FeatureState{name, (enabled) != 0},

constexpr FeatureState kFeatures[] = {CY_FEATURE_TABLE(CY_RUNTIME_FEATURE_ROW)};

#undef CY_RUNTIME_FEATURE_ROW

static_assert(sizeof(kFeatures) / sizeof(kFeatures[0]) == CY_FEATURE_COUNT,
              "the generated feature table and its count disagree, which means cy_features.h was "
              "written by a generator this file does not match");

}  // namespace

Span<const FeatureState> features() noexcept {
    return {kFeatures, CY_FEATURE_COUNT};
}

bool feature_enabled(const char* name, bool* found) noexcept {
    if (found != nullptr) {
        *found = false;
    }
    if (name == nullptr) {
        return false;
    }
    for (const FeatureState& feature : kFeatures) {
        if (std::strcmp(feature.name, name) == 0) {
            if (found != nullptr) {
                *found = true;
            }
            return feature.enabled;
        }
    }
    return false;
}

}  // namespace cy::runtime
