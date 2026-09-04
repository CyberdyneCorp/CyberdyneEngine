// The canonical text form: determinism, exact floats, and a one-line diff for a one-value change.

#include <cy/core/memory/array.h>
#include <cy/core/serialize/tagged.h>
#include <cy/core/serialize/text.h>
#include <cy/test/test.h>

#include <cstdlib>
#include <string_view>

#include "fixtures.h"

using namespace cy;
using namespace cy::serialize;
using namespace cy::serialize::test;

namespace {

[[nodiscard]] std::string_view view_of(const Array<char>& text) noexcept {
    return {text.data(), text.size()};
}

/// How many lines of `left` and `right` differ, comparing position by position. What a textual diff
/// of two files of equal shape would report.
[[nodiscard]] usize differing_lines(std::string_view left, std::string_view right) noexcept {
    usize differences = 0;
    usize left_offset = 0;
    usize right_offset = 0;
    while (left_offset < left.size() || right_offset < right.size()) {
        const usize left_end = left.find('\n', left_offset);
        const usize right_end = right.find('\n', right_offset);
        const std::string_view left_line =
            left.substr(left_offset, (left_end == std::string_view::npos ? left.size() : left_end) -
                                         left_offset);
        const std::string_view right_line = right.substr(
            right_offset,
            (right_end == std::string_view::npos ? right.size() : right_end) - right_offset);
        if (left_line != right_line) {
            ++differences;
        }
        left_offset = (left_end == std::string_view::npos) ? left.size() : left_end + 1;
        right_offset = (right_end == std::string_view::npos) ? right.size() : right_end + 1;
    }
    return differences;
}

[[nodiscard]] Status write_record_text(Array<char>& out, const ValueRecord& record,
                                       TextOptions options = {}) noexcept {
    TextWriter writer(out, options);
    return writer.write_record(0, record);
}

}  // namespace

CY_TEST_CASE("a record round-trips through the text form") {
    Spread source;
    source.flag = true;
    source.small_signed = -128;
    source.medium_signed = 32767;
    source.large_signed = -2147483647 - 1;
    source.huge_signed = -9007199254740993LL;
    source.small = 255;
    source.medium = 65535;
    source.large = 4294967295U;
    source.huge = 18446744073709551615ULL;
    source.single = 0.1F;
    source.doubled = 0.1;

    ValueRecord record(test_allocator());
    CY_REQUIRE(record_from_object(spread_type(), &source, Purpose::Asset, record).has_value());
    record.set_schema_version(2);

    Array<char> text(test_allocator());
    CY_REQUIRE(write_record_text(text, record).has_value());

    TextScanner scanner(view_of(text));
    const Expected<TextLine, Error> line = scanner.next();
    CY_REQUIRE(line.has_value());

    ValueRecord parsed(test_allocator());
    CY_REQUIRE(read_record_text(scanner, line.value(), parsed).has_value());

    CY_CHECK_EQ(parsed.type(), record.type());
    CY_CHECK_EQ(parsed.schema_version(), 2U);
    CY_REQUIRE_EQ(parsed.size(), record.size());
    for (usize index = 0; index < record.size(); ++index) {
        const FieldValue& expected = record.fields()[index];
        const FieldValue& actual = parsed.fields()[index];
        CY_CHECK_EQ(actual.id, expected.id);
        CY_CHECK_EQ(actual.wire, expected.wire);
        const Span<const u8> expected_bytes = record.bytes(expected);
        const Span<const u8> actual_bytes = parsed.bytes(actual);
        CY_REQUIRE_EQ(actual_bytes.size(), expected_bytes.size());
        for (usize byte = 0; byte < expected_bytes.size(); ++byte) {
            CY_REQUIRE_EQ(actual_bytes[byte], expected_bytes[byte]);
        }
    }
}

CY_TEST_CASE("the shortest float text that round-trips is the one written") {
    char text[kFloatTextCapacity] = {};
    CY_REQUIRE(format_f32(0.5F, text, sizeof(text)).has_value());
    CY_CHECK_EQ(std::string_view(text), std::string_view("0.5"));

    CY_REQUIRE(format_f32(0.1F, text, sizeof(text)).has_value());
    // Not "0.100000001": the shortest decimal that parses back to the same bits.
    CY_CHECK_EQ(std::string_view(text), std::string_view("0.1"));

    CY_REQUIRE(format_f64(0.1, text, sizeof(text)).has_value());
    CY_CHECK_EQ(std::string_view(text), std::string_view("0.1"));
}

CY_TEST_CASE("every float bit pattern the writer produces parses back to itself") {
    // A spread of awkward values rather than a sweep: denormals, the extremes, and values whose
    // shortest form is not their %g default.
    const f32 values[] = {0.0F,           -0.0F,          1.0F,        -1.0F,
                          0.1F,           1.0F / 3.0F,    1e-38F,      3.4028235e38F,
                          -3.4028235e38F, 1.1754944e-38F, 16777217.0F, 1e-45F};
    for (const f32 value : values) {
        char text[kFloatTextCapacity] = {};
        const Expected<usize, Error> written = format_f32(value, text, sizeof(text));
        CY_REQUIRE(written.has_value());
        CY_REQUIRE_EQ(strtof(text, nullptr), value);
    }
}

CY_TEST_CASE("infinities and not-a-number have one spelling each and round-trip") {
    char text[kFloatTextCapacity] = {};
    const f32 infinity = 1.0F / 0.0F;
    CY_REQUIRE(format_f32(infinity, text, sizeof(text)).has_value());
    CY_CHECK_EQ(std::string_view(text), std::string_view("inf"));
    CY_REQUIRE(format_f32(-infinity, text, sizeof(text)).has_value());
    CY_CHECK_EQ(std::string_view(text), std::string_view("-inf"));
    CY_REQUIRE(format_f32(infinity - infinity, text, sizeof(text)).has_value());
    CY_CHECK_EQ(std::string_view(text), std::string_view("nan"));
}

CY_TEST_CASE("writing the same record twice produces the same characters") {
    Health health;
    health.maximum = 1.0F / 3.0F;

    ValueRecord record(test_allocator());
    CY_REQUIRE(record_from_object(health_type(), &health, Purpose::Snapshot, record).has_value());

    Array<char> first(test_allocator());
    Array<char> second(test_allocator());
    CY_REQUIRE(write_record_text(first, record).has_value());
    CY_REQUIRE(write_record_text(second, record).has_value());
    CY_CHECK_EQ(view_of(first), view_of(second));
}

CY_TEST_CASE("one changed property is one changed line") {
    Health before;
    before.maximum = 100.0F;
    before.current = 50.0F;
    before.revives = 1;

    Health after = before;
    after.current = 51.0F;

    ValueRecord before_record(test_allocator());
    ValueRecord after_record(test_allocator());
    CY_REQUIRE(
        record_from_object(health_type(), &before, Purpose::Snapshot, before_record).has_value());
    CY_REQUIRE(
        record_from_object(health_type(), &after, Purpose::Snapshot, after_record).has_value());

    Array<char> before_text(test_allocator());
    Array<char> after_text(test_allocator());
    CY_REQUIRE(write_record_text(before_text, before_record).has_value());
    CY_REQUIRE(write_record_text(after_text, after_record).has_value());

    CY_CHECK_EQ(differing_lines(view_of(before_text), view_of(after_text)), 1U);
}

CY_TEST_CASE("the canonical form carries no name, so a rename churns no file") {
    Health health;
    ValueRecord record(test_allocator());
    CY_REQUIRE(record_from_object(health_type(), &health, Purpose::Asset, record).has_value());

    Array<char> canonical(test_allocator());
    TextWriter plain(canonical);
    CY_REQUIRE(plain.write_record(0, record, &health_type()).has_value());
    CY_CHECK_EQ(view_of(canonical).find("maximum"), std::string_view::npos);
    CY_CHECK_EQ(view_of(canonical).find("Health"), std::string_view::npos);

    Array<char> annotated(test_allocator());
    TextWriter commented(annotated, TextOptions{/*annotate=*/true});
    CY_REQUIRE(commented.write_record(0, record, &health_type()).has_value());
    CY_CHECK_NE(view_of(annotated).find("maximum"), std::string_view::npos);
}

CY_TEST_CASE("an annotated file reads back to the same record as the canonical one") {
    Health health;
    health.maximum = 42.5F;
    ValueRecord record(test_allocator());
    CY_REQUIRE(record_from_object(health_type(), &health, Purpose::Asset, record).has_value());

    Array<char> annotated(test_allocator());
    TextWriter writer(annotated, TextOptions{/*annotate=*/true});
    CY_REQUIRE(writer.write_record(0, record, &health_type()).has_value());

    TextScanner scanner(view_of(annotated));
    const Expected<TextLine, Error> line = scanner.next();
    CY_REQUIRE(line.has_value());
    ValueRecord parsed(test_allocator());
    CY_REQUIRE(read_record_text(scanner, line.value(), parsed).has_value());
    CY_CHECK_EQ(parsed.size(), record.size());
    CY_CHECK(parsed.contains(reflect::FieldId(kHealthMaximum)));
}

CY_TEST_CASE("text to binary to text reproduces the file") {
    Spread source;
    source.single = 1.0F / 7.0F;
    source.doubled = -1.0 / 9.0;
    source.huge = 12345678901234567890ULL;

    ValueRecord record(test_allocator());
    CY_REQUIRE(record_from_object(spread_type(), &source, Purpose::Asset, record).has_value());

    Array<char> first_text(test_allocator());
    CY_REQUIRE(write_record_text(first_text, record).has_value());

    // text -> record -> binary
    TextScanner scanner(view_of(first_text));
    const Expected<TextLine, Error> line = scanner.next();
    CY_REQUIRE(line.has_value());
    ValueRecord from_text(test_allocator());
    CY_REQUIRE(read_record_text(scanner, line.value(), from_text).has_value());

    Array<u8> binary(test_allocator());
    {
        TaggedWriter writer(binary);
        CY_REQUIRE(writer.begin_stream().has_value());
        CY_REQUIRE(writer.begin_chunk(chunk_tag('R', 'E', 'C', 'S')).has_value());
        CY_REQUIRE(writer.write_record(from_text).has_value());
        CY_REQUIRE(writer.end_chunk().has_value());
        CY_REQUIRE(writer.end_stream().has_value());
    }

    // binary -> record -> text
    TaggedReader reader(binary.data(), binary.size());
    CY_REQUIRE(reader.read_header().has_value());
    const Expected<TaggedChunk, Error> chunk = reader.next_chunk();
    CY_REQUIRE(chunk.has_value());
    Array<ValueRecord> records(test_allocator());
    CY_REQUIRE(read_records(chunk->payload, records).has_value());
    CY_REQUIRE_EQ(records.size(), 1U);

    Array<char> second_text(test_allocator());
    CY_REQUIRE(write_record_text(second_text, records[0]).has_value());
    CY_CHECK_EQ(view_of(first_text), view_of(second_text));
}

CY_TEST_CASE("a quoted name survives spaces, quotes and backslashes") {
    Array<char> text(test_allocator());
    TextWriter writer(text);
    CY_REQUIRE(writer.begin_line(1).has_value());
    CY_REQUIRE(writer.word("name").has_value());
    CY_REQUIRE(writer.word_quoted("Turret \"Heavy\" \\ Radar").has_value());
    CY_REQUIRE(writer.end_line().has_value());

    TextScanner scanner(view_of(text));
    const Expected<TextLine, Error> line = scanner.next();
    CY_REQUIRE(line.has_value());
    CY_CHECK_EQ(line->depth(), 1U);
    CY_REQUIRE_EQ(line->count(), 2U);

    Array<char> unescaped(test_allocator());
    const Expected<std::string_view, Error> name = line->word_unquoted(1, unescaped);
    CY_REQUIRE(name.has_value());
    CY_CHECK_EQ(name.value(), std::string_view("Turret \"Heavy\" \\ Radar"));
}

CY_TEST_CASE("blank lines and whole-line comments are not data") {
    const std::string_view source = "\n# a comment\n  entity 1\n\n    name \"x\"\n";
    TextScanner scanner(source);

    const Expected<TextLine, Error> first = scanner.next();
    CY_REQUIRE(first.has_value());
    CY_CHECK_EQ(first->word(0), std::string_view("entity"));
    CY_CHECK_EQ(first->depth(), 1U);
    CY_CHECK_EQ(first->number(), 3U);

    const Expected<TextLine, Error> second = scanner.next();
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(second->word(0), std::string_view("name"));
    CY_CHECK_EQ(second->depth(), 2U);

    CY_CHECK_FALSE(scanner.next().has_value());
}

CY_TEST_CASE("a pushed-back line is handed out again") {
    const std::string_view source = "a\nb\n";
    TextScanner scanner(source);
    CY_REQUIRE(scanner.next().has_value());
    scanner.push_back();
    const Expected<TextLine, Error> again = scanner.next();
    CY_REQUIRE(again.has_value());
    CY_CHECK_EQ(again->word(0), std::string_view("a"));
    const Expected<TextLine, Error> onward = scanner.next();
    CY_REQUIRE(onward.has_value());
    CY_CHECK_EQ(onward->word(0), std::string_view("b"));
}
