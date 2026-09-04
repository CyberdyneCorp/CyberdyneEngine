// The cases whose numbers are the claim, and which therefore do not fit a millisecond.
//
// `testing-and-quality`'s taxonomy places a test by what it costs, not by what it is about: a unit
// test has a millisecond, does no I/O and starts no thread. The case below builds four thousand
// rows of packed component data, and measured 1.5-1.8 ms in the Debug configuration. Shrinking the
// row count to fit would be testing a different claim — the claim *is* that the per-row overhead of
// the cooked form is zero at a size where a tagged form's would be tens of kilobytes — so it moved
// here instead.

#include <cy/core/memory/array.h>
#include <cy/core/serialize/cooked.h>
#include <cy/test/test.h>

#include "fixtures.h"

using namespace cy;
using namespace cy::serialize;
using namespace cy::serialize::test;

namespace {

struct Transform {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 z = 0.0F;
};

}  // namespace

CY_TEST_CASE("a cooked block carries no per-field tag: its size is exactly its columns") {
    // The specification's prohibition, measured rather than asserted in prose. Four thousand rows
    // of a twelve-byte transform is 48 000 bytes of payload; the whole stream is that plus two
    // fixed headers, so the per-row overhead is zero.
    constexpr u32 kRows = 4000;
    Array<u8> payload(test_allocator());
    CY_REQUIRE(payload.resize(kRows * sizeof(Transform)).has_value());

    CookedBlock block(test_allocator());
    CY_REQUIRE(block.add_column(reflect::TypeId(9201), sizeof(Transform)).has_value());
    block.set_row_count(kRows);
    block.set_payload(payload.span());

    Array<u8> stream(test_allocator());
    CookedWriter writer(stream);
    CY_REQUIRE(writer.begin_stream(1).has_value());
    CY_REQUIRE(writer.write_block(block).has_value());
    CY_REQUIRE(writer.end_stream().has_value());

    const usize overhead = stream.size() - payload.size();
    CY_CHECK_LT(overhead, 64U);
}
