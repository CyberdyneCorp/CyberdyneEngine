// The binary and text serialization forms over reflected data. Task 3.3.5.
//
// The reflected corpus is cy::demo::Health, which is the type set src/core/reflect/ owns at M1: it
// exercises every field attribute the specification's table names, including a Transient field that
// must appear in neither form.

#include <cy/core/assets/serialization.h>
#include <cy/core/reflect/demo/types.h>
#include <cy/core/reflect/demo/types.reflect.h>
#include <cy/core/reflect/reflect.h>
#include <cy/core/reflect/registry.h>
#include <cy/test/test.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace cy::assets;
using cy::u8;
using cy::usize;

namespace {

/// The registry, registered once for the whole binary. Registration is an explicit call rather than
/// a static initialiser — that is src/core/reflect/'s decision, and this is what honouring it looks
/// like from a consumer.
const cy::reflect::TypeInfo& health_type() {
    static const bool registered = [] {
        return cy::reflect::register_generated_types().has_value();
    }();
    CY_REQUIRE(registered);
    return cy::reflect::type_of<cy::demo::Health>();
}

std::string text_of(const cy::reflect::TypeInfo& type, const void* object) {
    cy::Array<char> out;
    CY_REQUIRE(write_text(type, object, out).has_value());
    return {out.data(), out.size()};
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    usize cursor = 0;
    while (cursor < text.size()) {
        const usize newline = text.find('\n', cursor);
        const usize end = newline == std::string::npos ? text.size() : newline;
        lines.push_back(text.substr(cursor, end - cursor));
        if (newline == std::string::npos) {
            break;
        }
        cursor = newline + 1;
    }
    return lines;
}

}  // namespace

CY_TEST_CASE("The binary form round-trips a reflected object") {
    const cy::reflect::TypeInfo& type = health_type();

    cy::demo::Health written;
    written.maximum = 250.0F;
    written.current = 87.5F;
    written.displayed = 42.0F;  // Transient: must not survive
    written.last_damage = 2;
    written.icon = 0x1234'5678'9ABC'DEF0ull;

    cy::reflect::ByteBuffer buffer;
    CY_REQUIRE(write_binary(type, &written, buffer).has_value());
    CY_CHECK(buffer.size() > kBinaryEnvelopeBytes);

    cy::demo::Health read;
    CY_REQUIRE(read_binary(type, buffer.data(), buffer.size(), &read).has_value());
    CY_CHECK_EQ(read.maximum, written.maximum);
    CY_CHECK_EQ(read.current, written.current);
    CY_CHECK_EQ(read.last_damage, written.last_damage);
    CY_CHECK_EQ(read.icon, written.icon);
    // Transient: excluded from serialization, so the reader left the default alone.
    CY_CHECK_EQ(read.displayed, 100.0F);
}

CY_TEST_CASE("The binary envelope names the type without reading the payload") {
    const cy::reflect::TypeInfo& type = health_type();
    cy::demo::Health value;
    cy::reflect::ByteBuffer buffer;
    CY_REQUIRE(write_binary(type, &value, buffer).has_value());

    const auto peeked = peek_binary(buffer.data(), buffer.size());
    CY_REQUIRE(peeked.has_value());
    CY_CHECK(peeked.value() == type.id);
}

CY_TEST_CASE("Scenario: Version mismatch") {
    // WHEN a cooked asset's format version is newer than the runtime supports
    // THEN loading SHALL fail with a clear diagnostic rather than misparsing.
    const cy::reflect::TypeInfo& type = health_type();
    cy::demo::Health value;
    cy::reflect::ByteBuffer buffer;
    CY_REQUIRE(write_binary(type, &value, buffer).has_value());

    cy::Array<u8> bytes;
    CY_REQUIRE(bytes.resize(buffer.size()).has_value());
    std::memcpy(bytes.data(), buffer.data(), buffer.size());

    // Bump the envelope's version to one from the future.
    bytes[8] = static_cast<u8>(kBinaryFormatVersion + 1);
    cy::demo::Health read;
    const auto refused = read_binary(type, bytes.data(), bytes.size(), &read);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::Unsupported);

    // A document with the wrong magic is not a document of this kind at all.
    bytes[0] = 'X';
    const auto not_ours = read_binary(type, bytes.data(), bytes.size(), &read);
    CY_REQUIRE_FALSE(not_ours.has_value());
    CY_CHECK(not_ours.error().code == cy::ErrorCode::InvalidArgument);
}

CY_TEST_CASE("The text form round-trips a reflected object") {
    const cy::reflect::TypeInfo& type = health_type();

    cy::demo::Health written;
    written.maximum = 250.0F;
    written.current = 87.5F;
    written.displayed = 42.0F;
    written.last_damage = 3;
    written.icon = 7;

    const std::string text = text_of(type, &written);
    CY_CHECK(text.find("type \"cy::demo::Health\"") == 0);
    CY_CHECK(text.find("maximum = 250") != std::string::npos);
    CY_CHECK(text.find("current = 87.5") != std::string::npos);
    // Transient, in neither form.
    CY_CHECK(text.find("displayed") == std::string::npos);

    cy::demo::Health read;
    CY_REQUIRE(read_text(type, text, &read).has_value());
    CY_CHECK_EQ(read.maximum, written.maximum);
    CY_CHECK_EQ(read.current, written.current);
    CY_CHECK_EQ(read.last_damage, written.last_damage);
    CY_CHECK_EQ(read.icon, written.icon);
    CY_CHECK_EQ(read.displayed, 100.0F);
}

CY_TEST_CASE("Text to binary to text preserves values and ordering") {
    // `core-assets-and-io`: "Both SHALL round-trip: text -> binary -> text SHALL preserve values
    // and ordering."
    const cy::reflect::TypeInfo& type = health_type();

    cy::demo::Health original;
    original.maximum = 1234.5F;
    original.current = 0.125F;
    original.last_damage = 1;
    original.icon = 0xFFFF'FFFF'FFFF'FFFFull;
    const std::string first = text_of(type, &original);

    cy::demo::Health from_text;
    CY_REQUIRE(read_text(type, first, &from_text).has_value());

    cy::reflect::ByteBuffer binary;
    CY_REQUIRE(write_binary(type, &from_text, binary).has_value());

    cy::demo::Health from_binary;
    CY_REQUIRE(read_binary(type, binary.data(), binary.size(), &from_binary).has_value());

    const std::string second = text_of(type, &from_binary);
    CY_CHECK(first == second);
}

CY_TEST_CASE("Scenario: Meaningful version control diffs") {
    // WHEN a designer moves one entity in a scene
    // THEN the text diff SHALL show only that entity's transform change, with stable ordering
    //      elsewhere.
    //
    // Stated at the level this milestone actually has — one reflected object rather than a scene of
    // them, because scenes are `serialization-and-prefabs` at a later milestone. The property is
    // the same and it is the one that makes a scene diff readable: change one value, change one
    // line.
    const cy::reflect::TypeInfo& type = health_type();

    cy::demo::Health before;
    before.maximum = 100.0F;
    before.current = 100.0F;
    before.last_damage = 0;
    before.icon = 11;

    cy::demo::Health after = before;
    after.current = 62.5F;

    const std::vector<std::string> first = lines_of(text_of(type, &before));
    const std::vector<std::string> second = lines_of(text_of(type, &after));

    CY_REQUIRE_EQ(first.size(), second.size());
    usize changed = 0;
    for (usize i = 0; i < first.size(); ++i) {
        if (first[i] != second[i]) {
            ++changed;
            CY_CHECK(second[i].find("current = 62.5") != std::string::npos);
        }
    }
    CY_CHECK_EQ(changed, 1u);
}

CY_TEST_CASE("Text field lines are ordered by identifier, not by declaration or name") {
    // The ordering is the whole diff-friendliness argument: an identifier is the only thing about a
    // field that does not move when it is renamed, reordered or re-typed.
    const cy::reflect::TypeInfo& type = health_type();
    cy::demo::Health value;
    const std::vector<std::string> lines = lines_of(text_of(type, &value));

    unsigned previous = 0;
    for (usize i = 1; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        const usize first_digit = line.find_first_not_of(' ');
        CY_REQUIRE(first_digit != std::string::npos);
        const auto id =
            static_cast<unsigned>(std::strtoul(line.c_str() + first_digit, nullptr, 10));
        CY_CHECK(id > previous);
        previous = id;
    }
}

CY_TEST_CASE("A text document for a different type is refused") {
    const cy::reflect::TypeInfo& type = health_type();
    cy::demo::Health value;
    const auto refused = read_text(type, "type \"cy::demo::Other\" 999\n", &value);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::InvalidArgument);

    CY_CHECK_FALSE(read_text(type, "not a document\n", &value).has_value());
}

CY_TEST_CASE("A field the type no longer has is skipped, not an error") {
    // That is a removed field, and the manifest's tombstone is what makes skipping it safe.
    const cy::reflect::TypeInfo& type = health_type();
    cy::demo::Health value;
    std::string text = text_of(type, &value);
    text += "  60000 a_field_removed_long_ago = 5\n";
    CY_CHECK(read_text(type, text, &value).has_value());
}

CY_TEST_CASE("A malformed field value is refused rather than reinterpreted") {
    const cy::reflect::TypeInfo& type = health_type();
    cy::demo::Health value;
    const std::string text = text_of(type, &value);
    const usize maximum_line = text.find("maximum = ");
    CY_REQUIRE(maximum_line != std::string::npos);
    std::string broken = text.substr(0, maximum_line + 10) + "not-a-number\n";
    CY_CHECK_FALSE(read_text(type, broken, &value).has_value());
}

CY_TEST_CASE("The text form names the type, so a reader can pick one") {
    const cy::reflect::TypeInfo& type = health_type();
    cy::demo::Health value;
    const auto peeked = peek_text(text_of(type, &value));
    CY_REQUIRE(peeked.has_value());
    CY_CHECK(peeked.value() == type.id);
}
