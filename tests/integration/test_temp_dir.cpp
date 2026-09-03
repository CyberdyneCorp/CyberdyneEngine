// The isolation fixture, against a real filesystem. Integration rather than unit: it does I/O, and
// the taxonomy places anything that does in this suite regardless of how quick it happens to be.

#include <cy/test/fixtures.h>
#include <cy/test/test.h>

#include <filesystem>
#include <string>

CY_TEST_CASE("temp dir: a test owns its directory and it disappears with the fixture") {
    std::string path;
    {
        cy::test::TempDir dir{"temp-dir-lifetime"};
        CY_REQUIRE(dir.valid());
        path = dir.path();
        CY_CHECK(std::filesystem::is_directory(path));
    }
    CY_CHECK_FALSE(std::filesystem::exists(path));
}

CY_TEST_CASE("temp dir: two fixtures never share a directory") {
    cy::test::TempDir first{"temp-dir-isolation"};
    cy::test::TempDir second{"temp-dir-isolation"};
    CY_REQUIRE(first.valid());
    CY_REQUIRE(second.valid());
    CY_CHECK_NE(first.path(), second.path());
}

CY_TEST_CASE("temp dir: a file written through the fixture reads back byte for byte") {
    cy::test::TempDir dir{"temp-dir-round-trip"};
    CY_REQUIRE(dir.valid());

    const std::string path = dir.file("trace.bin");
    const std::string contents{"cyberdyne\0engine\n", 17};
    CY_REQUIRE(cy::test::write_file(path, contents));

    std::string read_back;
    CY_REQUIRE(cy::test::read_file(path, read_back));
    CY_CHECK_EQ(read_back, contents);
}

CY_TEST_CASE("temp dir: a failure is reported, never thrown") {
    // The engine compiles with -fno-exceptions, so every fixture reports failure through a return
    // value. Reading a file that does not exist is the cheapest way to state that contract.
    cy::test::TempDir dir{"temp-dir-missing"};
    CY_REQUIRE(dir.valid());

    std::string read_back{"untouched"};
    CY_CHECK_FALSE(cy::test::read_file(dir.file("absent"), read_back));
}
