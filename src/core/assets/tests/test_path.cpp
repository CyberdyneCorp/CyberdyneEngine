// VirtualPath: normalisation, traversal, and the properties `core-assets-and-io` states of a path.
// Task 3.3.2.

#include <cy/core/assets/path.h>
#include <cy/test/test.h>

using cy::assets::VirtualPath;

namespace {

VirtualPath normalised(const char* raw) {
    auto path = VirtualPath::normalise(raw);
    CY_REQUIRE(path.has_value());
    return path.value();
}

}  // namespace

CY_TEST_CASE("Paths are normalised into one canonical form") {
    CY_CHECK(normalised("textures/stone.ktx2").view() == "textures/stone.ktx2");
    CY_CHECK(normalised("/textures/stone.ktx2").view() == "textures/stone.ktx2");
    CY_CHECK(normalised("textures//stone.ktx2").view() == "textures/stone.ktx2");
    CY_CHECK(normalised("./textures/./stone.ktx2").view() == "textures/stone.ktx2");
    CY_CHECK(normalised("textures/stone.ktx2/").view() == "textures/stone.ktx2");
    CY_CHECK(normalised("textures/old/../stone.ktx2").view() == "textures/stone.ktx2");
    CY_CHECK(normalised("").empty());
}

CY_TEST_CASE("Paths are case-sensitive") {
    // Two files differing only in case are two files on every platform the engine targets, and a
    // case-folding resolver is one that works locally and fails on the build server.
    CY_CHECK(normalised("Textures/Stone.ktx2") != normalised("textures/stone.ktx2"));
}

CY_TEST_CASE("Path traversal outside a mount is rejected") {
    CY_CHECK_FALSE(VirtualPath::normalise("../secrets").has_value());
    CY_CHECK_FALSE(VirtualPath::normalise("textures/../../secrets").has_value());
    CY_CHECK_FALSE(VirtualPath::normalise("a/b/../../../c").has_value());

    const auto escape = VirtualPath::normalise("../secrets");
    CY_REQUIRE_FALSE(escape.has_value());
    CY_CHECK(escape.error().code == cy::ErrorCode::PermissionDenied);

    // A `..` that stays inside is fine: the rule is about escaping, not about the spelling.
    CY_CHECK(normalised("a/b/../c").view() == "a/c");
}

CY_TEST_CASE("A backslash is not a separator") {
    // It is an ordinary filename character on the platforms the engine targets, so accepting it as
    // a separator would make one path mean two things.
    CY_CHECK_FALSE(VirtualPath::normalise("textures\\stone.ktx2").has_value());
}

CY_TEST_CASE("A control character in a path is rejected") {
    const char raw[] = {'a', '\x01', 'b', '\0'};
    CY_CHECK_FALSE(VirtualPath::normalise(raw).has_value());
}

CY_TEST_CASE("A path longer than the maximum is refused rather than truncated") {
    cy::usize length = cy::assets::kMaxPathLength + 1;
    std::string_view too_long(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        length > 256 ? 256 : length);
    CY_CHECK_FALSE(VirtualPath::normalise(too_long).has_value());
}

CY_TEST_CASE("A path reports its parts") {
    const VirtualPath path = normalised("textures/rock/stone.ktx2");
    CY_CHECK(path.file_name() == "stone.ktx2");
    CY_CHECK(path.parent() == "textures/rock");
    CY_CHECK(path.extension() == ".ktx2");
    CY_CHECK(normalised(".gitignore").extension().empty());  // a hidden file has no extension
    CY_CHECK(normalised("plain").parent().empty());
}

CY_TEST_CASE("Containment is segment-aware") {
    const VirtualPath file = normalised("textures/stone.ktx2");
    CY_CHECK(file.is_within(normalised("textures")));
    CY_CHECK(file.is_within(VirtualPath{}));  // the root contains everything
    CY_CHECK_FALSE(file.is_within(normalised("textures_old")));
}

CY_TEST_CASE("Joining normalises the result") {
    const VirtualPath base = normalised("textures");
    auto joined = base.join("rock/stone.ktx2");
    CY_REQUIRE(joined.has_value());
    CY_CHECK(joined.value().view() == "textures/rock/stone.ktx2");

    // A join may not escape past its own root either.
    CY_CHECK_FALSE(base.join("../../secrets").has_value());
}
