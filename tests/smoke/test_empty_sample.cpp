// The milestone gate, as a test. Task 4.1.4.
//
// It runs samples/00-empty headless with a frame limit and asserts what M0 closes on: the process
// exits cleanly, and it left behind a trace file with the shape `diagnostics-profiling-and-crash`
// requires — a magic, a version, the metadata chunk that resolves identifiers, at least one event
// chunk, and the index at the tail that makes a long capture partially loadable.
//
// The shape is checked against include/cy/core/diagnostics/format.h, which is the format's one
// declaration. That is on purpose: a change to the writer that this test does not see is a change
// that broke the artefact for tools/trace/trace_inspect.py too.

#include <cy/test/fixtures.h>
#include <cy/test/test.h>

#include <cy/core/diagnostics/format.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "process.h"

namespace {

namespace format = cy::diag::format;

std::vector<cy::u32> chunk_tags(const std::string& bytes) {
    std::vector<cy::u32> tags;
    cy::usize offset = sizeof(format::FileHeader);
    while (offset + sizeof(format::ChunkHeader) <= bytes.size()) {
        format::ChunkHeader header{};
        std::memcpy(&header, bytes.data() + offset, sizeof(header));
        tags.push_back(header.tag);
        offset += sizeof(header) + header.payload_bytes;
    }
    return tags;
}

bool contains(const std::vector<cy::u32>& tags, cy::u32 tag) {
    return std::ranges::find(tags, tag) != tags.end();
}

bool contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

CY_TEST_CASE("samples/00-empty runs headless, exits cleanly and writes a trace") {
    cy::test::TempDir directory{"smoke_empty_sample"};
    CY_REQUIRE(directory.valid());
    const std::string trace = directory.file("00-empty.cytrace");

    const cy::test::smoke::ProcessResult result =
        cy::test::smoke::run(cy::test::smoke::quoted(CY_SAMPLE_EMPTY) +
                             " --headless --frames 30 --trace " + cy::test::smoke::quoted(trace));

    CY_REQUIRE(result.ran);
    CY_CHECK_EQ(result.exit_code, 0);
    CY_CHECK(contains(result.output, "display=headless"));
    CY_CHECK(contains(result.output, "30 frames"));
    CY_CHECK(contains(result.output, "frame limit"));
    // Thirty frames paced at the simulation step must have run simulation steps: a zero here means
    // the loop free-spun and the fixed-step half of the frame was never exercised.
    CY_CHECK_FALSE(contains(result.output, " 0 simulation ticks"));

    std::string bytes;
    CY_REQUIRE(cy::test::read_file(trace, bytes));
    CY_REQUIRE(bytes.size() > sizeof(format::FileHeader) + sizeof(cy::u64));

    format::FileHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    CY_CHECK_EQ(std::memcmp(header.magic, format::kMagic, sizeof(format::kMagic)), 0);
    CY_CHECK_EQ(header.format_version, format::kFormatVersion);
    CY_CHECK_EQ(header.header_bytes, static_cast<cy::u32>(sizeof(format::FileHeader)));
    CY_CHECK_EQ(header.compression, static_cast<cy::u8>(format::Compression::None));
    CY_CHECK(header.trace_id != 0);

    const std::vector<cy::u32> tags = chunk_tags(bytes);
    CY_CHECK(contains(tags, format::kChunkMeta));
    CY_CHECK(contains(tags, format::kChunkEvents));
    CY_CHECK(contains(tags, format::kChunkIndex));

    // The last eight bytes are the index chunk's own offset, which is what lets a reader open a
    // long capture without reading it. Follow it and check it lands on the chunk it names.
    cy::u64 index_offset = 0;
    std::memcpy(&index_offset, bytes.data() + bytes.size() - sizeof(index_offset),
                sizeof(index_offset));
    CY_REQUIRE(index_offset + sizeof(format::ChunkHeader) <= bytes.size());
    format::ChunkHeader index{};
    std::memcpy(&index, bytes.data() + index_offset, sizeof(index));
    CY_CHECK_EQ(index.tag, format::kChunkIndex);
}

CY_TEST_CASE("samples/00-empty rejects a command line it does not understand") {
    const cy::test::smoke::ProcessResult result =
        cy::test::smoke::run(cy::test::smoke::quoted(CY_SAMPLE_EMPTY) + " --no-such-option");

    CY_REQUIRE(result.ran);
    CY_CHECK_EQ(result.exit_code, 2);
}
