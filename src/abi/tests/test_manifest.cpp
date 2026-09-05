// module.toml, the declaration that turns a shared library into a module. Task 2.4.
//
// `native-abi` fixes what a manifest declares; these cases fix that it is *read*, and that a
// declaration nobody understood is an error rather than a silence. The parser modifies its input in
// place, so every case copies its text into a buffer of its own — which is also how a caller must
// use it, and is therefore worth demonstrating rather than hiding behind a helper.

#include <cy/abi/module.h>
#include <cy/test/test.h>

#include <cstring>

namespace {

// A mutable copy of a literal, because the parser NUL-terminates keys and values where they sit and
// hands back pointers into the text. A `const char*` would compile and then write to a literal.
struct Text {
    char buffer[512] = {};

    explicit Text(const char* source) noexcept {
        CY_REQUIRE(std::strlen(source) < sizeof(buffer));
        std::strncpy(buffer, source, sizeof(buffer) - 1);
    }
};

}  // namespace

CY_TEST_CASE("a manifest declares everything native-abi says it declares") {
    Text text(
        "# a game module\n"
        "name = \"character\"\n"
        "entry_symbol = \"cy_module_entry\"   # the default, stated anyway\n"
        "min_abi_major = 1\n"
        "min_abi_minor = 0\n"
        "hot_reload = true\n"
        "\n"
        "[platform.linux]\n"
        "library = \"libcharacter.so\"\n"
        "\n"
        "[platform.windows]\n"
        "library = \"character.dll\"\n");

    cy::Expected<cy::abi::ModuleManifest, cy::Error> parsed =
        cy::abi::parse_module_manifest(text.buffer);
    CY_REQUIRE(parsed.has_value());
    const cy::abi::ModuleManifest& manifest = parsed.value();

    CY_CHECK(std::strcmp(manifest.name, "character") == 0);
    CY_CHECK(std::strcmp(manifest.entry_symbol, "cy_module_entry") == 0);
    CY_CHECK_EQ(manifest.min_abi_major, 1U);
    CY_CHECK_EQ(manifest.min_abi_minor, 0U);
    CY_CHECK(manifest.hot_reload);
    CY_CHECK(std::strcmp(manifest.library_for(cy::abi::ModulePlatform::Linux), "libcharacter.so") ==
             0);
    CY_CHECK(std::strcmp(manifest.library_for(cy::abi::ModulePlatform::Windows), "character.dll") ==
             0);
    // A platform the module does not ship for is null rather than empty, so a loader that forgot to
    // check gets a null path instead of trying to open "".
    CY_CHECK(manifest.library_for(cy::abi::ModulePlatform::MacOS) == nullptr);
}

CY_TEST_CASE("the defaults are the ones a module can rely on") {
    Text text("name = \"minimal\"\n");
    cy::Expected<cy::abi::ModuleManifest, cy::Error> parsed =
        cy::abi::parse_module_manifest(text.buffer);
    CY_REQUIRE(parsed.has_value());
    CY_CHECK(std::strcmp(parsed.value().entry_symbol, "cy_module_entry") == 0);
    // Hot reload is opt-in. A module that did not say it can be reloaded must not be reloaded:
    // reloading one that holds unserializable state is exactly the silent corruption the spike
    // measured.
    CY_CHECK_FALSE(parsed.value().hot_reload);
}

CY_TEST_CASE("an unknown key is an error naming it, not a silence") {
    // The rule cmake/modules.cmake applies to the build-time manifests, for the same reason: a typo
    // that is skipped is a setting that silently did not apply, and the symptom arrives elsewhere.
    Text text("name = \"typo\"\nhot_relaod = true\n");
    cy::Expected<cy::abi::ModuleManifest, cy::Error> parsed =
        cy::abi::parse_module_manifest(text.buffer);
    CY_REQUIRE_FALSE(parsed.has_value());
    CY_CHECK_EQ(parsed.error().code, cy::ErrorCode::InvalidArgument);
}

CY_TEST_CASE("a malformed manifest is refused rather than half-applied") {
    {
        Text text("entry_symbol = \"cy_module_entry\"\n");
        CY_CHECK_FALSE(cy::abi::parse_module_manifest(text.buffer).has_value());  // no name
    }
    {
        Text text("name = character\n");
        CY_CHECK_FALSE(cy::abi::parse_module_manifest(text.buffer).has_value());  // unquoted
    }
    {
        Text text("name = \"x\"\nmin_abi_major = one\n");
        CY_CHECK_FALSE(cy::abi::parse_module_manifest(text.buffer).has_value());
    }
    {
        Text text("name = \"x\"\nhot_reload = yes\n");
        CY_CHECK_FALSE(cy::abi::parse_module_manifest(text.buffer).has_value());
    }
    {
        Text text("name = \"x\"\n[platform.plan9]\nlibrary = \"x.so\"\n");
        CY_CHECK_FALSE(cy::abi::parse_module_manifest(text.buffer).has_value());
    }
    {
        Text text("name = \"x\"\n[tools]\n");
        CY_CHECK_FALSE(cy::abi::parse_module_manifest(text.buffer).has_value());
    }
    {
        Text text("name = \"x\"\nno equals sign here\n");
        CY_CHECK_FALSE(cy::abi::parse_module_manifest(text.buffer).has_value());
    }
}

CY_TEST_CASE("the platform names are the build's platform names") {
    // cmake/modules.cmake's CY_MODULE_PLATFORMS, spelled identically. A manifest is read by the
    // build and by the loader, and two spellings of "macos" would be one bug with two homes.
    CY_CHECK(std::strcmp(cy::abi::module_platform_name(cy::abi::ModulePlatform::Linux), "linux") ==
             0);
    CY_CHECK(std::strcmp(cy::abi::module_platform_name(cy::abi::ModulePlatform::VisionOs),
                         "visionos") == 0);
    CY_CHECK(
        std::strcmp(cy::abi::module_platform_name(cy::abi::ModulePlatform::Count), "unknown") == 0);
    // The host resolves to something this build could actually load a library for.
    CY_CHECK(cy::abi::host_module_platform() != cy::abi::ModulePlatform::Count);
}
