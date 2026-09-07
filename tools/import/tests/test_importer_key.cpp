// `import_derivation_key` and the toolchain it was blind to. M7 task 1.1.
//
// THE BUG THESE CASES ARE THE REGRESSION FOR. Until M7, this function contributed the producer, the
// source hash, the variant, the profile and the options — and no compiler, no flags and no library
// versions. It is the function `cy_import_cli` uses. M6's spike built one importer at `-O2` and at
// `-O0`, ran both over the same glTF, and got the identical key `04a6fff1…`; M6's closing gate
// re-measured it on two importer binaries pointed at one cache and recorded **1 hit, 0 miss**.
// `asset-import-pipeline` requires "one cache covering all derived data", and one cache with two
// keys — one of which cannot see its own compiler — serves the wrong artefact and reports success.
//
// The remedy shipped at M6 but at layer 7 inside `tools/build/`, out of this module's reach. The
// assertions below are on the property that fixes it rather than on the mechanism: the key this
// function returns must be the key a builder produces when the toolchain is contributed, and must
// NOT be the key it produces when the toolchain is left out.
//
// Unit: nothing here opens a file.

#include <cy/core/assets/derivation.h>
#include <cy/core/assets/toolchain.h>
#include <cy/import/importer.h>
#include <cy/import/options.h>
#include <cy/test/test.h>

#include <string_view>

using namespace cy::import;
using cy::assets::DerivationKey;
using cy::assets::DerivationKeyBuilder;
using cy::assets::DerivedKind;

namespace {

constexpr std::string_view kExtensions[] = {".test"};
constexpr cy::assets::AssetKind kProduces[] = {cy::assets::AssetKind::Mesh};

ImporterInfo info() noexcept {
    ImporterInfo declared;
    declared.name = "test";
    declared.version = 3;
    declared.extensions = cy::Span<const std::string_view>(kExtensions);
    declared.produces = cy::Span<const cy::assets::AssetKind>(kProduces);
    declared.description = "A stand-in importer that exists to be keyed.";
    return declared;
}

cy::assets::ContentHash source() noexcept {
    return cy::assets::content_hash("source bytes", 12);
}

/// A request with no options and no resolver: `import_derivation_key` reads neither.
cy::assets::VariantKey variant() {
    auto key = cy::assets::VariantKey::parse("desktop-bc7");
    CY_REQUIRE(key.has_value());
    return key.value();
}

ImportRequest request(CookProfile profile = CookProfile::Client) {
    ImportRequest made;
    auto path = cy::assets::VirtualPath::normalise("assets/thing.test");
    CY_REQUIRE(path.has_value());
    made.source = *path;
    made.variant = variant();
    made.profile = profile;
    return made;
}

DerivationKey key_of(const ImportRequest& made) {
    auto key = import_derivation_key(info(), OptionsSchema{}, made, source());
    CY_REQUIRE(key.has_value());
    return key.value();
}

/// What the key would be if the toolchain were contributed — or omitted. The second is the M6
/// behaviour, reconstructed here so the difference can be asserted rather than described.
DerivationKey rebuilt(bool with_toolchain) {
    DerivationKeyBuilder builder;
    builder.producer(DerivedKind::Import, info().name, info().version);
    if (with_toolchain) {
        cy::assets::current_toolchain().contribute(builder);
    }
    builder.source("source", source())
        .text("variant", variant().view())
        .text("profile", cook_profile_name(CookProfile::Client));
    ImportOptions defaults;
    defaults.contribute_to(OptionsSchema{}, builder);
    auto key = builder.finish();
    CY_REQUIRE(key.has_value());
    return key.value();
}

}  // namespace

CY_TEST_CASE("import key: the same import produces the same key") {
    CY_CHECK(key_of(request()) == key_of(request()));
}

CY_TEST_CASE("import key: the toolchain is in it") {
    // The regression. If `toolchain.contribute(builder)` is removed from
    // `import_derivation_key`, this is the assertion that goes red — the key would collapse onto
    // the M6 spelling, which is the one that served an -O2 artefact to an -O0 binary.
    CY_CHECK(key_of(request()) == rebuilt(true));
    CY_CHECK(key_of(request()) != rebuilt(false));
}

CY_TEST_CASE("import key: the toolchain this binary reports is the one that reaches the key") {
    CY_CHECK(cy::assets::toolchain_is_complete(cy::assets::current_toolchain()));
}

CY_TEST_CASE("import key: the cook profile still separates two cooks of one source") {
    // The fields M6 got right are still there, in the same order — a merge that dropped one would
    // be a cache that overwrites a client cook with a dedicated-server cook.
    CY_CHECK(key_of(request(CookProfile::Client)) != key_of(request(CookProfile::DedicatedServer)));
    CY_CHECK(key_of(request(CookProfile::Client)) != key_of(request(CookProfile::Editor)));
}
