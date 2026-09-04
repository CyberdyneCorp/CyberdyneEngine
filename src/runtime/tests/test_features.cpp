// Build-time feature slicing, as data. Task 4.1.4.
//
// The test that matters here is the one that would be circular if it were written the obvious way:
// asserting `feature_enabled("CY_UI") == false` by writing `#if defined(CY_UI)` in the test tests
// the preprocessor against itself. So the assertions below are about the *table* — that it has an
// entry for every declared feature, that its entries agree with the macros in the one translation
// unit that is allowed to read them, and that an unknown name is distinguishable from a disabled
// one.

#include <cy/runtime/features.h>
#include <cy/test/test.h>

#include <cy_features.h>

#include <string_view>

namespace {

using namespace cy;
using namespace cy::runtime;

}  // namespace

CY_TEST_CASE("the feature table has an entry for every declared feature") {
    CY_REQUIRE_EQ(features().size(), usize{CY_FEATURE_COUNT});
    for (const FeatureState& feature : features()) {
        CY_REQUIRE(feature.name != nullptr);
        CY_CHECK(std::string_view(feature.name).substr(0, 3) == "CY_");
    }
}

CY_TEST_CASE("the table agrees with the build that produced it") {
    // This suite only exists when CY_BUILD_TESTS is on, so it is the one feature whose state the
    // test can assert without consulting the preprocessor: if the table said otherwise, the table
    // would be describing a different build from the one running.
    bool found = false;
    CY_CHECK(feature_enabled("CY_BUILD_TESTS", &found));
    CY_CHECK(found);

    // A feature no milestone has turned on yet: declared, and off. It was CY_RENDERER_VULKAN until
    // M3 made that one buildable — a Vulkan-enabled build then failed this case, which is the
    // table doing its job rather than the test finding a defect. CY_RENDERER_METAL is M7's, so it
    // stays off in every configuration this milestone can produce.
    found = false;
    CY_CHECK_FALSE(feature_enabled("CY_RENDERER_METAL", &found));
    CY_CHECK(found);
}

CY_TEST_CASE("an unknown feature is distinguishable from a disabled one") {
    // A caller asking about `CY_TYPO` should learn that it asked the wrong question, not that the
    // answer is no. Without `found` the two are the same `false`.
    bool found = true;
    CY_CHECK_FALSE(feature_enabled("CY_NOT_A_FEATURE", &found));
    CY_CHECK_FALSE(found);

    CY_CHECK_FALSE(feature_enabled(nullptr));
}

CY_TEST_CASE("the development-build report matches the profile this ran in") {
    // CY_DEVELOPMENT is defined in `debug` and `dev` and not in `profile` and `release`, so this
    // case asserts an equality rather than a value — the M0 lesson: a test written against
    // assertion behaviour without checking the profile passes in two configurations by testing
    // nothing.
#if defined(CY_DEVELOPMENT)
    CY_CHECK(development_build());
#else
    CY_CHECK_FALSE(development_build());
#endif

    // Nothing in the tree sets CY_DEDICATED_SERVER yet; the accessor reports the macro and is not
    // evidence that anything was excluded from the link. The check that gives it teeth is the
    // top-level cy_check_dedicated_server_link(), still an M0 stub until the renderer lands.
    CY_CHECK_FALSE(dedicated_server());
}
