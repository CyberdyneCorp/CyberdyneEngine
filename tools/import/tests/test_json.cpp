// The JSON reader glTF is parsed with. M5 task 5.1.
//
// Half of these cases are refusals, and that is the point: every one of them is something a lenient
// parser accepts and a strict one rejects, which is how two tools come to disagree about what a
// file means.

#include <cy/import/json.h>
#include <cy/test/test.h>

#include <string>
#include <string_view>

using namespace cy::import;
using cy::usize;

namespace {

JsonDocument parse(std::string_view text) {
    auto document = JsonDocument::parse(text);
    CY_REQUIRE(document.has_value());
    return std::move(document.value());
}

bool refuses(std::string_view text) {
    return !JsonDocument::parse(text).has_value();
}

}  // namespace

CY_TEST_CASE("json: an object's members are found by name and missing ones read as absent") {
    const JsonDocument document = parse(R"({"a": 1, "b": "two", "c": true, "d": null})");
    const JsonRef root = document.root();
    CY_CHECK(document.kind(root) == JsonKind::Object);
    CY_CHECK(document.size(root) == 4);
    CY_CHECK(document.number_or(document.member(root, "a"), -1.0) == 1.0);
    CY_CHECK(document.string_or(document.member(root, "b"), "") == "two");
    CY_CHECK(document.bool_or(document.member(root, "c"), false));
    CY_CHECK(document.kind(document.member(root, "d")) == JsonKind::Null);
    // The shape every optional glTF field reads as: absent, with the caller's default.
    CY_CHECK(document.member(root, "e") == JsonDocument::kNone);
    CY_CHECK(document.number_or(document.member(root, "e"), 7.0) == 7.0);
}

CY_TEST_CASE("json: arrays index, nest and know their length") {
    const JsonDocument document = parse(R"({"v": [1, [2, 3], {"x": 4}]})");
    const JsonRef v = document.member(document.root(), "v");
    CY_REQUIRE(document.size(v) == 3);
    CY_CHECK(document.number_or(document.at(v, 0), 0.0) == 1.0);
    CY_CHECK(document.number_or(document.at(document.at(v, 1), 1), 0.0) == 3.0);
    CY_CHECK(document.number_or(document.member(document.at(v, 2), "x"), 0.0) == 4.0);
    CY_CHECK(document.at(v, 3) == JsonDocument::kNone);
}

CY_TEST_CASE("json: numbers cover the forms glTF writes") {
    const JsonDocument document = parse(R"({"a": -0.5, "b": 1e3, "c": 2.5E-2, "d": 0, "e": -17})");
    const JsonRef root = document.root();
    CY_CHECK(document.number_or(document.member(root, "a"), 0.0) == -0.5);
    CY_CHECK(document.number_or(document.member(root, "b"), 0.0) == 1000.0);
    CY_CHECK(document.number_or(document.member(root, "c"), 0.0) == 0.025);
    CY_CHECK(document.integer_or(document.member(root, "d"), -1) == 0);
    CY_CHECK(document.integer_or(document.member(root, "e"), 0) == -17);
}

CY_TEST_CASE("json: escapes are decoded, including a surrogate pair") {
    const JsonDocument document = parse(R"({"s": "a\"b\\c\ndé😀"})");
    const std::string_view text = document.string_or(document.member(document.root(), "s"), "");
    CY_CHECK(text == "a\"b\\c\nd\xc3\xa9\xf0\x9f\x98\x80");
}

CY_TEST_CASE("json: the refusals that keep two parsers from disagreeing") {
    CY_CHECK(refuses("{\"a\": 1,}"));            // a trailing comma
    CY_CHECK(refuses("[1, 2,]"));                // and in an array
    CY_CHECK(refuses("{a: 1}"));                 // an unquoted key
    CY_CHECK(refuses("{\"a\": 01}"));            // a leading zero
    CY_CHECK(refuses("{\"a\": .5}"));            // no integer part
    CY_CHECK(refuses("{\"a\": 5.}"));            // no fractional digits
    CY_CHECK(refuses("{\"a\": NaN}"));           // not a JSON literal
    CY_CHECK(refuses("{\"a\": 1} {\"b\": 2}"));  // two documents in one file
    CY_CHECK(refuses("{\"a\": 1, \"a\": 2}"));   // a duplicate key: two parsers pick differently
    CY_CHECK(refuses("// a comment\n{}"));       // JSON has no comments
    CY_CHECK(refuses("{\"a\": \"unterminated"));
}

CY_TEST_CASE("json: nesting is bounded rather than allowed to overflow a stack") {
    std::string deep;
    for (usize index = 0; index < JsonDocument::kMaxDepth + 8; ++index) {
        deep += '[';
    }
    deep += '1';
    for (usize index = 0; index < JsonDocument::kMaxDepth + 8; ++index) {
        deep += ']';
    }
    const auto refused = JsonDocument::parse(deep);
    CY_REQUIRE(!refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::OutOfRange);
}

CY_TEST_CASE("json: a document outlives the text it was parsed from") {
    // Strings are copied into the document's own storage, so an importer may free the file's bytes
    // and go on reading names out of it — which is exactly what the glTF importer does with a GLB's
    // JSON chunk.
    JsonDocument document;
    {
        std::string text = R"({"name": "Chair"})";
        document = parse(text);
        text.assign(text.size(), 'x');
    }
    CY_CHECK(document.string_or(document.member(document.root(), "name"), "") == "Chair");
}
