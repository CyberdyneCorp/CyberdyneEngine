// The diagnostic log and its compiler-output parser. Task 3.7.
//
// `shader-system`: "when a shader fails to compile, the error SHALL carry the Slang source file,
// line, and column". A `cy::Error` cannot; this can, and the parser is what turns a wall of text
// into something an editor can put a squiggle on.

#include <cy/backends/shader/diagnostics.h>
#include <cy/core/memory/scope.h>
#include <cy/test/test.h>

#include <string_view>

using cy::usize;
using namespace cy::shader;

CY_TEST_CASE("a diagnostic keeps its own text") {
    DiagnosticLog log(cy::current_allocator());
    CY_REQUIRE(log.add(Severity::Error, SourceLocation{"cy/brdf.slang", 41, 9},
                       "undefined identifier 'roughnes'", "30015")
                   .has_value());

    CY_REQUIRE_EQ(log.size(), usize{1});
    const Diagnostic entry = log.at(0);
    CY_CHECK(entry.severity == Severity::Error);
    CY_CHECK(std::string_view(entry.location.file) == "cy/brdf.slang");
    CY_CHECK_EQ(entry.location.line, 41U);
    CY_CHECK_EQ(entry.location.column, 9U);
    CY_CHECK(std::string_view(entry.message) == "undefined identifier 'roughnes'");
    CY_CHECK(std::string_view(entry.code) == "30015");
    CY_CHECK(log.has_errors());
    CY_CHECK_EQ(log.error_count(), 1U);
}

CY_TEST_CASE("Slang's own diagnostic format parses into a file, a line and a column") {
    DiagnosticLog log(cy::current_allocator());
    CY_REQUIRE(log.parse_compiler_output(
                      "src/shaders/lit.slang(41): error 30015: undefined identifier 'x'\n"
                      "src/shaders/lit.slang(12,5): warning 39999: unused parameter\n")
                   .has_value());

    CY_REQUIRE_EQ(log.size(), usize{2});
    const Diagnostic first = log.at(0);
    CY_CHECK(std::string_view(first.location.file) == "src/shaders/lit.slang");
    CY_CHECK_EQ(first.location.line, 41U);
    CY_CHECK_EQ(first.location.column, 0U);
    CY_CHECK(first.severity == Severity::Error);
    CY_CHECK(std::string_view(first.code) == "30015");
    CY_CHECK(std::string_view(first.message) == "undefined identifier 'x'");

    const Diagnostic second = log.at(1);
    CY_CHECK_EQ(second.location.line, 12U);
    CY_CHECK_EQ(second.location.column, 5U);
    CY_CHECK(second.severity == Severity::Warning);

    CY_CHECK_EQ(log.error_count(), 1U);
    CY_CHECK_EQ(log.warning_count(), 1U);
}

CY_TEST_CASE("the clang and gcc format parses too, and a path with a colon survives it") {
    DiagnosticLog log(cy::current_allocator());
    CY_REQUIRE(
        log.parse_compiler_output("shaders/lit.slang:41:9: error: expected ';'\n").has_value());
    CY_REQUIRE_EQ(log.size(), usize{1});
    const Diagnostic entry = log.at(0);
    CY_CHECK(std::string_view(entry.location.file) == "shaders/lit.slang");
    CY_CHECK_EQ(entry.location.line, 41U);
    CY_CHECK_EQ(entry.location.column, 9U);
    CY_CHECK(std::string_view(entry.message) == "expected ';'");
}

CY_TEST_CASE("a Rust-style continuation line becomes the previous diagnostic's location") {
    // Slang's current format puts the message on one line and the position on the next. Attaching
    // it is what keeps "the error carries the file, line and column" true against the toolchain as
    // it is rather than as an older version of it was.
    DiagnosticLog log(cy::current_allocator());
    CY_REQUIRE(log.parse_compiler_output("error 15300: cannot open file 'cy/helper.slang'\n"
                                         "  --> material.kernel:1:8\n")
                   .has_value());
    CY_REQUIRE_EQ(log.size(), usize{1});
    const Diagnostic entry = log.at(0);
    CY_CHECK(entry.severity == Severity::Error);
    CY_CHECK(std::string_view(entry.location.file) == "material.kernel");
    CY_CHECK_EQ(entry.location.line, 1U);
    CY_CHECK_EQ(entry.location.column, 8U);
    CY_CHECK(std::string_view(entry.message) == "cannot open file 'cy/helper.slang'");
}

CY_TEST_CASE("an unrecognised line is kept rather than dropped") {
    DiagnosticLog log(cy::current_allocator());
    // The second half of a two-part diagnostic carries what the first half is missing, and a parser
    // that dropped it would lose exactly the useful part.
    CY_REQUIRE(log.parse_compiler_output("lit.slang(41): error 30015: no such member\n"
                                         "        surface.roughnes = 1.0;\n"
                                         "                ^~~~~~~~\n")
                   .has_value());
    CY_CHECK_EQ(log.size(), usize{3});
    CY_CHECK_EQ(log.error_count(), 1U);
    CY_CHECK(log.at(1).severity == Severity::Note);
    CY_CHECK_EQ(log.at(1).location.line, 0U);
}

CY_TEST_CASE("a file whose name contains a severity word is not read as one") {
    DiagnosticLog log(cy::current_allocator());
    CY_REQUIRE(log.parse_compiler_output("shaders/errors.slang(7): error 1: bad\n").has_value());
    CY_REQUIRE_EQ(log.size(), usize{1});
    CY_CHECK(std::string_view(log.at(0).location.file) == "shaders/errors.slang");
    CY_CHECK_EQ(log.at(0).location.line, 7U);
}

CY_TEST_CASE("logs fold together, and clearing resets the counts") {
    DiagnosticLog first(cy::current_allocator());
    DiagnosticLog second(cy::current_allocator());
    CY_REQUIRE(first.add(Severity::Warning, "one").has_value());
    CY_REQUIRE(second.add(Severity::Error, "two").has_value());

    CY_REQUIRE(first.append(second).has_value());
    CY_CHECK_EQ(first.size(), usize{2});
    CY_CHECK_EQ(first.error_count(), 1U);
    CY_CHECK_EQ(first.warning_count(), 1U);

    first.clear();
    CY_CHECK(first.empty());
    CY_CHECK_FALSE(first.has_errors());
    CY_CHECK_EQ(first.warning_count(), 0U);
}
