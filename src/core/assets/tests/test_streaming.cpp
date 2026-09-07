// Partial residency: mip levels, mesh LODs and audio chunks, under a budget. M7 tasks 2.1 and 2.2.
//
// `core-assets-and-io` — "Streaming". The two scenarios the requirement names are here as cases,
// and so is the sentence between them that has no scenario and is the one a frame depends on:
// **"Streaming SHALL never block the frame: a not-yet-resident level SHALL fall back to the
// highest resident level."**
//
// Integration rather than unit: every case starts a job system and an async service, because the
// reads run on the one thread where blocking is legal and a fixture that read on the calling thread
// would be testing something else.

#include <cy/core/assets/streaming.h>
#include <cy/core/jobs/async.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/memory/scope.h>
#include <cy/test/test.h>

#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace cy::assets;
using cy::i64;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;

namespace {

constexpr u32 kLevelBytes = 1024;

VirtualPath path_of(const char* raw) {
    auto path = VirtualPath::normalise(raw);
    CY_REQUIRE(path.has_value());
    return path.value();
}

cy::AssetId id_of(cy::u64 low) {
    return cy::AssetId{0x1234'5678'9abc'def0ULL, low};
}

/// A job system, an async service, a memory-mounted namespace and a streaming system, started and
/// stopped in the order that makes the teardown meaningful.
struct Harness {
    cy::jobs::JobSystem jobs;
    cy::jobs::AsyncService async;
    VirtualFileSystem files;
    MemoryMount* memory = nullptr;
    StreamingSystem streaming;
    i64 frame_ns = 1;

    explicit Harness(u64 budget) {
        cy::jobs::JobSystemConfig job_config;
        job_config.worker_count = 3;
        CY_REQUIRE(jobs.start(job_config).has_value());
        CY_REQUIRE(async.start(jobs).has_value());

        auto mount = cy::make_unique<MemoryMount>(cy::current_allocator(), "memory");
        CY_REQUIRE(mount.has_value());
        memory = mount.value().get();
        CY_REQUIRE(files.mount_owned(std::move(mount.value()), 0).has_value());

        CY_REQUIRE(streaming.start(async, files, budget).has_value());
    }

    ~Harness() {
        streaming.shutdown();
        async.stop();
        jobs.shutdown();
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    /// A file of `levels` equal-sized levels, each filled with its own level index so a reader can
    /// tell which one it got.
    void write_asset(const char* name, u32 levels) const {
        std::vector<u8> bytes(static_cast<usize>(levels) * kLevelBytes);
        for (u32 level = 0; level < levels; ++level) {
            for (u32 byte = 0; byte < kLevelBytes; ++byte) {
                bytes[(static_cast<usize>(level) * kLevelBytes) + byte] = static_cast<u8>(level);
            }
        }
        CY_REQUIRE(memory->add(path_of(name), bytes.data(), bytes.size()).has_value());
    }

    [[nodiscard]] static std::vector<StreamLevel> ladder(u32 levels) {
        std::vector<StreamLevel> declared;
        declared.reserve(levels);
        for (u32 level = 0; level < levels; ++level) {
            declared.push_back(StreamLevel{static_cast<u64>(level) * kLevelBytes, kLevelBytes});
        }
        return declared;
    }

    void declare(cy::AssetId id, const char* name, u32 levels) {
        const std::vector<StreamLevel> rungs = ladder(levels);
        StreamDeclaration declaration;
        declaration.id = id;
        declaration.kind = StreamKind::TextureMip;
        declaration.source = path_of(name);
        declaration.levels = {rungs.data(), rungs.size()};
        CY_REQUIRE(streaming.declare(declaration).has_value());
    }

    /// Drive `update` until the asset's resident level reaches `level`, or give up. Returns what it
    /// reached, so a case can assert the fallback rather than hang on it.
    u32 pump_until(cy::AssetId id, u32 level, u32 attempts = 200) {
        for (u32 attempt = 0; attempt < attempts; ++attempt) {
            streaming.update(frame_ns++);
            if (streaming.resident_level(id) == level) {
                return level;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        streaming.update(frame_ns++);
        return streaming.resident_level(id);
    }

    void pump(u32 times = 8) {
        for (u32 index = 0; index < times; ++index) {
            streaming.update(frame_ns++);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

}  // namespace

CY_TEST_CASE("streaming: a declared ladder loads its base without anybody asking for a level") {
    Harness harness(u64{64} * 1024);
    harness.write_asset("textures/wall.cytex", 4);
    harness.declare(id_of(1), "textures/wall.cytex", 4);

    CY_CHECK(harness.streaming.resident_level(id_of(1)) == StreamingSystem::kInvalidLevel);
    CY_REQUIRE(harness.streaming.request(id_of(1), {}, 0, harness.frame_ns).has_value());
    CY_CHECK(harness.pump_until(id_of(1), 0) == 0);

    const cy::Span<const u8> bytes = harness.streaming.resident_bytes(id_of(1));
    CY_REQUIRE(bytes.size() == kLevelBytes);
    CY_CHECK(bytes[0] == 0);
}

CY_TEST_CASE("streaming: approaching a surface swaps a higher level in") {
    // The specification's first scenario. "WHEN the camera approaches a textured surface and
    // feedback requests a higher mip THEN the mip SHALL be scheduled for load and swapped in when
    // ready, with the lower mip used meanwhile."
    Harness harness(u64{64} * 1024);
    harness.write_asset("textures/wall.cytex", 4);
    harness.declare(id_of(1), "textures/wall.cytex", 4);

    CY_REQUIRE(harness.streaming.request(id_of(1), {}, 3, harness.frame_ns).has_value());

    // THE LEVEL IS NOT RESIDENT AT THE MOMENT IT IS ASKED FOR, and the call did not block waiting
    // for it. This is the half of the requirement that has no scenario.
    CY_CHECK(harness.streaming.resident_level(id_of(1)) == StreamingSystem::kInvalidLevel);

    CY_CHECK(harness.pump_until(id_of(1), 3) == 3);
    const cy::Span<const u8> bytes = harness.streaming.resident_bytes(id_of(1));
    CY_REQUIRE(bytes.size() == kLevelBytes);
    CY_CHECK(bytes[0] == 3);
    CY_CHECK(harness.streaming.level_bytes(id_of(1), {}, 0).size() == kLevelBytes);
}

CY_TEST_CASE("streaming: a not-yet-resident level falls back to the highest resident one") {
    // Task 2.2, and the sentence a frame depends on. The budget holds two levels; the asset wants
    // four. What `resident_bytes` returns is never empty once the base is in, and never a level
    // that was not read.
    Harness harness(u64{2} * kLevelBytes);
    harness.write_asset("textures/wall.cytex", 4);
    harness.declare(id_of(1), "textures/wall.cytex", 4);

    CY_REQUIRE(harness.streaming.request(id_of(1), {}, 3, harness.frame_ns).has_value());
    harness.pump(24);

    const u32 level = harness.streaming.resident_level(id_of(1));
    CY_REQUIRE(level != StreamingSystem::kInvalidLevel);
    // Two levels' worth of budget: the base and one more, and never the one that was asked for.
    CY_CHECK(level <= 1);
    const cy::Span<const u8> bytes = harness.streaming.resident_bytes(id_of(1));
    CY_REQUIRE(bytes.size() == kLevelBytes);
    CY_CHECK(bytes[0] == static_cast<u8>(level));
    CY_CHECK(harness.streaming.stats().resident_bytes <= harness.streaming.budget_bytes());
    // The shortfall is REPORTED rather than silently absorbed.
    CY_CHECK(harness.streaming.stats().requests_deferred > 0);
}

CY_TEST_CASE("streaming: the budget evicts the least recently requested level, never a base") {
    // The specification's second scenario, plus the exemption the fallback guarantee needs: "WHEN
    // the residency budget is exceeded THEN the least recently requested levels SHALL be evicted
    // first, and the eviction SHALL be recorded in streaming statistics."
    Harness harness(u64{4} * kLevelBytes);
    harness.write_asset("textures/a.cytex", 2);
    harness.write_asset("textures/b.cytex", 2);
    harness.declare(id_of(1), "textures/a.cytex", 2);
    harness.declare(id_of(2), "textures/b.cytex", 2);

    CY_REQUIRE(harness.streaming.request(id_of(1), {}, 1, harness.frame_ns).has_value());
    CY_REQUIRE(harness.streaming.request(id_of(2), {}, 1, harness.frame_ns).has_value());
    CY_CHECK(harness.pump_until(id_of(1), 1) == 1);
    CY_CHECK(harness.pump_until(id_of(2), 1) == 1);
    CY_CHECK(harness.streaming.stats().evictions == 0);

    // Asset 2 keeps being asked for; asset 1 is not. Then the budget drops to three levels.
    for (u32 frame = 0; frame < 4; ++frame) {
        CY_REQUIRE(harness.streaming.request(id_of(2), {}, 1, harness.frame_ns).has_value());
        harness.streaming.update(harness.frame_ns++);
    }
    harness.streaming.set_budget_bytes(u64{3} * kLevelBytes);
    harness.pump(4);

    const StreamingStats stats = harness.streaming.stats();
    CY_CHECK(stats.evictions >= 1);
    CY_CHECK(stats.bytes_evicted >= kLevelBytes);
    CY_CHECK(stats.resident_bytes <= harness.streaming.budget_bytes());
    // The least recently requested level went, and it was not anybody's base.
    CY_CHECK(harness.streaming.resident_level(id_of(1)) == 0);
    CY_CHECK(harness.streaming.resident_level(id_of(2)) == 1);
    CY_CHECK(harness.streaming.level_bytes(id_of(1), {}, 0).size() == kLevelBytes);
}

CY_TEST_CASE("streaming: a level whose read fails leaves the level below it in use") {
    Harness harness(u64{64} * 1024);
    // Only two levels of bytes on disk; the ladder declares four. Reading level 2 runs off the end.
    harness.write_asset("textures/short.cytex", 2);
    harness.declare(id_of(7), "textures/short.cytex", 4);

    CY_REQUIRE(harness.streaming.request(id_of(7), {}, 3, harness.frame_ns).has_value());
    harness.pump(24);

    CY_CHECK(harness.streaming.stats().load_failures > 0);
    // The asset is still usable, at the level that did read.
    CY_CHECK(harness.streaming.resident_level(id_of(7)) == 1);
    CY_CHECK(harness.streaming.resident_bytes(id_of(7)).size() == kLevelBytes);
}

CY_TEST_CASE("streaming: an asset whose base alone exceeds the budget is refused by name") {
    Harness harness(u64{kLevelBytes} / 2);
    harness.write_asset("textures/huge.cytex", 2);

    const std::vector<StreamLevel> rungs = Harness::ladder(2);
    StreamDeclaration declaration;
    declaration.id = id_of(9);
    declaration.source = path_of("textures/huge.cytex");
    declaration.levels = {rungs.data(), rungs.size()};
    auto refused = harness.streaming.declare(declaration);
    CY_REQUIRE(!refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::OutOfRange);
}

CY_TEST_CASE("streaming: shutting down under load waits for its own reads") {
    // Rule: teardown under load. The service thread writes into a `Read` cell that `shutdown`
    // frees, and a shutdown that did not wait would be a use-after-free that reproduces once in
    // forty runs — which is precisely the shape M5.5's gate found in the Jolt job bridge.
    //
    // Repeated, because a race that happens once in a while does not happen in one iteration.
    for (u32 iteration = 0; iteration < 40; ++iteration) {
        Harness harness(u64{1024} * 1024);
        for (u32 asset = 0; asset < 8; ++asset) {
            const std::string name = "textures/asset" + std::to_string(asset) + ".cytex";
            harness.write_asset(name.c_str(), 8);
            harness.declare(id_of(asset), name.c_str(), 8);
            CY_REQUIRE(
                harness.streaming.request(id_of(asset), {}, 7, harness.frame_ns).has_value());
        }
        // One update issues sixty-four reads and does NOT wait for them. The destructor runs while
        // they are in flight, which is the whole of the case.
        harness.streaming.update(harness.frame_ns++);
    }
    CY_CHECK(true);
}

CY_TEST_CASE("streaming: re-declaring an asset drops what was resident under the old offsets") {
    Harness harness(u64{64} * 1024);
    harness.write_asset("textures/wall.cytex", 4);
    harness.declare(id_of(1), "textures/wall.cytex", 4);
    CY_REQUIRE(harness.streaming.request(id_of(1), {}, 2, harness.frame_ns).has_value());
    CY_CHECK(harness.pump_until(id_of(1), 2) == 2);
    const u64 before = harness.streaming.stats().resident_bytes;
    CY_CHECK(before == 3 * kLevelBytes);

    harness.declare(id_of(1), "textures/wall.cytex", 4);
    CY_CHECK(harness.streaming.stats().resident_bytes == 0);
    CY_CHECK(harness.streaming.resident_level(id_of(1)) == StreamingSystem::kInvalidLevel);
}
