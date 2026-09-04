// File and directory access, and the atomic write. Task 3.3.5.

#include "temp_dir.h"

#include <cy/core/assets/file.h>
#include <cy/test/test.h>

#include <cstring>
#include <string>

using namespace cy::assets;
using cy::u8;
using cy::usize;

namespace {

cy::Status write_file(const std::string& path, const char* text) {
    return fs::write_atomic(path.c_str(), text, std::strlen(text));
}

/// The bytes as text. `reinterpret_cast` rather than a cast through `void*`: -Wcast-align has
/// nothing to complain about here — `char` has the weakest alignment there is — and the two-step
/// cast is what clang-tidy flags.
std::string as_text(const cy::Array<u8>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string read_file(const std::string& path) {
    cy::Array<u8> bytes;
    CY_REQUIRE(fs::read_whole(path.c_str(), bytes).has_value());
    return as_text(bytes);
}

}  // namespace

CY_TEST_CASE("A file reads, writes, seeks and reports its size") {
    const test::TempDir directory("file_basics");
    const std::string path = directory.file("payload.bin");

    {
        auto file = File::open(path.c_str(), FileMode::Write);
        CY_REQUIRE(file.has_value());
        CY_REQUIRE(file.value().write("0123456789", 10).has_value());
        CY_REQUIRE(file.value().flush().has_value());
    }

    auto file = File::open(path.c_str(), FileMode::Read);
    CY_REQUIRE(file.has_value());
    CY_CHECK_EQ(file.value().size().value(), 10u);

    char buffer[4] = {};
    CY_REQUIRE(file.value().seek(3, SeekOrigin::Begin).has_value());
    CY_CHECK_EQ(file.value().read(buffer, 4).value(), 4u);
    CY_CHECK(std::memcmp(buffer, "3456", 4) == 0);
    CY_CHECK_EQ(file.value().tell().value(), 7u);

    // read_at does not move the cursor, which is what lets one package file serve concurrent reads.
    char at[2] = {};
    CY_CHECK_EQ(file.value().read_at(0, at, 2).value(), 2u);
    CY_CHECK(std::memcmp(at, "01", 2) == 0);
    CY_CHECK_EQ(file.value().tell().value(), 7u);

    // A read past the end is a short read, not a failure.
    char tail[16] = {};
    CY_CHECK_EQ(file.value().read_at(8, tail, 16).value(), 2u);
}

CY_TEST_CASE("Opening a file that is not there reports NotFound") {
    const test::TempDir directory("file_missing");
    const auto missing = File::open(directory.file("absent.bin").c_str(), FileMode::Read);
    CY_REQUIRE_FALSE(missing.has_value());
    CY_CHECK(missing.error().code == cy::ErrorCode::NotFound);
}

CY_TEST_CASE("Scenario: Interrupted save") {
    // WHEN the process is killed during a save
    // THEN the previous file SHALL remain intact and the temporary file SHALL be discarded on
    //      next start.
    const test::TempDir directory("interrupted_save");
    const std::string save = directory.file("profile.sav");
    CY_REQUIRE(write_file(save, "the previous save").has_value());

    // A save killed after the temporary was written and before the rename leaves exactly this on
    // disk: the old file, and a temporary beside it. Reproduced rather than described, because the
    // requirement is about the state a crash leaves and not about the code path that leaves it.
    const std::string temporary = directory.file("profile.sav.9999.0.cytmp");
    {
        auto file = File::open(temporary.c_str(), FileMode::Write);
        CY_REQUIRE(file.has_value());
        CY_REQUIRE(file.value().write("half a new save", 15).has_value());
    }

    // The previous file is intact.
    CY_CHECK(read_file(save) == "the previous save");

    // Next start: the temporary is discarded.
    const auto discarded = fs::discard_temporaries(directory.c_str());
    CY_REQUIRE(discarded.has_value());
    CY_CHECK_EQ(discarded.value(), 1u);
    CY_CHECK_FALSE(fs::exists(temporary.c_str()));
    CY_CHECK(read_file(save) == "the previous save");
}

CY_TEST_CASE("An atomic write replaces the file whole and leaves no temporary") {
    const test::TempDir directory("atomic_write");
    const std::string save = directory.file("profile.sav");
    CY_REQUIRE(write_file(save, "first").has_value());
    CY_REQUIRE(write_file(save, "second, and longer").has_value());
    CY_CHECK(read_file(save) == "second, and longer");

    const auto leftovers = fs::discard_temporaries(directory.c_str());
    CY_REQUIRE(leftovers.has_value());
    CY_CHECK_EQ(leftovers.value(), 0u);
}

CY_TEST_CASE("Directories are created, enumerated in order, moved, copied and removed") {
    const test::TempDir directory("directories");
    CY_REQUIRE(fs::create_directories(directory.file("a/b").c_str()).has_value());
    CY_REQUIRE(write_file(directory.file("a/b/two.txt"), "2").has_value());
    CY_REQUIRE(write_file(directory.file("a/one.txt"), "1").has_value());
    CY_REQUIRE(write_file(directory.file("zzz.txt"), "z").has_value());

    struct Sink {
        std::string names;
    } sink;
    CY_REQUIRE(fs::enumerate(
                   directory.c_str(), true,
                   [](void* user, const DirectoryEntry& entry) noexcept {
                       static_cast<Sink*>(user)->names += entry.name;
                       static_cast<Sink*>(user)->names += ";";
                       return true;
                   },
                   &sink)
                   .has_value());
    // Sorted, so a listing is the same on every platform and in every filesystem.
    CY_CHECK(sink.names == "a;a/b;a/b/two.txt;a/one.txt;zzz.txt;");

    CY_CHECK(fs::is_directory(directory.file("a").c_str()));
    CY_CHECK_EQ(fs::file_size(directory.file("zzz.txt").c_str()).value(), 1u);

    CY_REQUIRE(fs::copy_file(directory.file("zzz.txt").c_str(), directory.file("copy.txt").c_str())
                   .has_value());
    CY_CHECK(read_file(directory.file("copy.txt")) == "z");
    CY_REQUIRE(
        fs::move_file(directory.file("copy.txt").c_str(), directory.file("moved.txt").c_str())
            .has_value());
    CY_CHECK_FALSE(fs::exists(directory.file("copy.txt").c_str()));
    CY_CHECK(read_file(directory.file("moved.txt")) == "z");
    CY_REQUIRE(fs::remove_file(directory.file("moved.txt").c_str()).has_value());
    CY_CHECK_FALSE(fs::exists(directory.file("moved.txt").c_str()));
}

CY_TEST_CASE("A walk stops when the visitor says so") {
    const test::TempDir directory("walk_stop");
    CY_REQUIRE(write_file(directory.file("a.txt"), "a").has_value());
    CY_REQUIRE(write_file(directory.file("b.txt"), "b").has_value());
    CY_REQUIRE(write_file(directory.file("c.txt"), "c").has_value());

    unsigned seen = 0;
    CY_REQUIRE(fs::enumerate(
                   directory.c_str(), false,
                   [](void* user, const DirectoryEntry&) noexcept {
                       ++*static_cast<unsigned*>(user);
                       return false;
                   },
                   &seen)
                   .has_value());
    CY_CHECK_EQ(seen, 1u);
}

CY_TEST_CASE("A file is memory-mapped where the platform allows") {
    const test::TempDir directory("mapping");
    const std::string path = directory.file("mapped.bin");
    CY_REQUIRE(write_file(path, "mapped contents").has_value());

    if (!memory_mapping_available()) {
        // Reported rather than skipped: a build with no mapping must still say so out loud.
        CY_TEST_MESSAGE("memory mapping is not available on this platform");
        CY_CHECK_FALSE(MappedFile::map(path.c_str()).has_value());
        return;
    }

    CY_CHECK(memory_mapping_granularity() >= 4096u);
    auto mapping = MappedFile::map(path.c_str());
    CY_REQUIRE(mapping.has_value());
    CY_CHECK_EQ(mapping.value().size(), 15u);
    CY_CHECK(std::memcmp(mapping.value().data(), "mapped contents", 15) == 0);

    // An unaligned offset is handled by the mapping rather than by the caller.
    auto tail = MappedFile::map(path.c_str(), 7, 8);
    CY_REQUIRE(tail.has_value());
    CY_CHECK(std::memcmp(tail.value().data(), "contents", 8) == 0);

    CY_CHECK_FALSE(MappedFile::map(path.c_str(), 100).has_value());
}
