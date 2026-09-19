// The pipeline cache's path contract — Metal gap 6, and the finding beside it.
//
// AN INTEGRATION SUITE AND NOT A UNIT ONE, because it blocks on a filesystem: it writes a file,
// reads it back, and asks whether one exists. `testing-and-quality` puts anything that waits above
// the unit tier, and the harness enforces it — this case was written in `unit.rhi` first and the
// budget check refused it by name, which is the taxonomy doing its job rather than an
// inconvenience.
//
// WHAT THIS DOES NOT SHOW, and it is the finding that outweighs the signature change: NOTHING IN
// THE ENGINE CALLS EITHER OF THESE. `rhi-and-render-graph` requires the cache to be "persisted
// across runs, so a warm start compiles nothing"; M11.d's spike measured zero callers of
// `save_pipeline_cache` and `load_pipeline_cache` in `src/` and in `tests/`, so that requirement is
// unimplemented ABOVE the RHI. Taking a path rather than a blob is what makes a Metal backend able
// to implement it at all — `MTLBinaryArchive` serialises to a URL and has no byte form — and it
// does not implement the requirement. This suite checks the contract; it claims nothing more.

#include <cy/test/test.h>

#include <cy/backends/rhi/null/null_device.h>
#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>

namespace {

using cy::rhi::Device;

cy::rhi::DeviceDescription description() noexcept {
    cy::rhi::DeviceDescription desc;
    desc.application_name = "pipeline cache";
    return desc;
}

}  // namespace

CY_TEST_CASE("the pipeline cache round-trips through a path, and an absent one is a cold start") {
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Gpu);
    cy::Expected<Device*, cy::Error> device =
        cy::rhi::null::create_null_device(allocator, description());
    CY_REQUIRE(device.has_value());

    const char* path = "cy-pipeline-cache-round-trip.bin";
    (void)cy::assets::fs::remove_file(path);

    // A COLD START IS NOT AN ERROR. A first run has no cache; reporting that as a failure would
    // make every first run log one, and a log line nobody can act on is a log line everybody learns
    // to ignore. This is the one contract decision a path forces that a blob did not.
    CY_CHECK_FALSE(cy::assets::fs::exists(path));
    CY_CHECK((*device)->load_pipeline_cache(path).has_value());

    // The file is written even when the backend has nothing to persist, so that a host writes the
    // same "save the cache on shutdown" code on every backend and "did the save happen" stays an
    // answerable question on the one backend every test run has.
    CY_CHECK((*device)->save_pipeline_cache(path).has_value());
    CY_CHECK(cy::assets::fs::exists(path));
    CY_CHECK((*device)->load_pipeline_cache(path).has_value());

    (void)cy::assets::fs::remove_file(path);
    cy::rhi::null::destroy_null_device(allocator, *device);
}
