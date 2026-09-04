// Asynchronous loading over the job system. Task 3.3.4.
//
// Every case here runs against a real JobSystem and a real AsyncService, because the property the
// requirement is about — a worker never blocks — is not observable without them.

#include "temp_dir.h"

#include <cy/core/assets/asset_system.h>
#include <cy/core/assets/package.h>
#include <cy/core/memory/scope.h>
#include <cy/test/test.h>

#include <cstring>
#include <string>
#include <utility>

using namespace cy::assets;
using cy::u32;
using cy::u8;
using cy::usize;

namespace {

cy::Array<u8> corpus(usize size, u8 seed) {
    cy::Array<u8> bytes;
    CY_REQUIRE(bytes.resize(size).has_value());
    for (usize i = 0; i < size; ++i) {
        bytes[i] = static_cast<u8>(((i / 16) * 3) + (i % 7) + seed);
    }
    return bytes;
}

/// A job system, an async service, a virtual filesystem and an asset system, started and stopped
/// together. Every case needs all four, and the order they are started and shut down in is part of
/// what is being tested — the asset system must not outlive the threads its stages run on.
struct Harness {
    explicit Harness(const AssetSystemConfig& config = {}) {
        cy::jobs::JobSystemConfig job_config;
        job_config.worker_count = 3;
        CY_REQUIRE(jobs.start(job_config).has_value());
        CY_REQUIRE(async.start(jobs).has_value());
        CY_REQUIRE(assets.start(jobs, async, files, config).has_value());
        cy::jobs::reset_blocking_violations();
    }

    ~Harness() {
        assets.shutdown();
        async.stop();
        jobs.shutdown();
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    void mount(const std::string& package_path, cy::i32 priority) {
        PackageOpenOptions options;
        auto reader = PackageReader::open(package_path.c_str(), options);
        CY_REQUIRE(reader.has_value());
        auto package =
            cy::make_unique<PackageMount>(cy::current_allocator(), std::move(reader.value()));
        CY_REQUIRE(package.has_value());
        CY_REQUIRE(files.mount_owned(std::move(package.value()), priority).has_value());
    }

    cy::jobs::JobSystem jobs;
    cy::jobs::AsyncService async;
    VirtualFileSystem files;
    AssetSystem assets;
};

/// Write a package holding one entry per id, with the given payloads.
void write_package(const std::string& path, cy::Span<const cy::AssetId> ids,
                   cy::Span<const cy::Array<u8>*> payloads) {
    PackageWriter writer;
    PackageManifest manifest;
    CY_REQUIRE(manifest.set_build_id("test").has_value());
    CY_REQUIRE(writer.set_manifest(manifest).has_value());
    for (usize i = 0; i < ids.size(); ++i) {
        PackageWriter::EntryOptions options;
        options.kind = AssetKind::Binary;
        CY_REQUIRE(writer.add(ids[i], VariantKey::any(), payloads[i]->span(), options).has_value());
    }
    CY_REQUIRE(writer.write(path.c_str()).has_value());
}

}  // namespace

CY_TEST_CASE("An asset loads asynchronously and the payload matches") {
    const test::TempDir directory("assets_load");
    const std::string path = directory.file("content.cypak");
    const cy::AssetId id = mint_asset_id();
    const cy::Array<u8> payload = corpus(40000, 1);
    const cy::Array<u8>* payloads[] = {&payload};
    write_package(path, cy::Span<const cy::AssetId>(&id, 1),
                  cy::Span<const cy::Array<u8>*>(payloads, 1));

    Harness harness;
    harness.mount(path, mount_priority::kBasePackage);

    const auto request = harness.assets.load_async(id);
    CY_REQUIRE(request.has_value());
    CY_REQUIRE(harness.assets.wait(request.value()).has_value());
    CY_CHECK(harness.assets.is_complete(request.value()));

    const auto asset = harness.assets.result(request.value());
    CY_REQUIRE(asset.has_value());
    CY_CHECK(asset.value()->id() == id);
    CY_CHECK_EQ(asset.value()->size(), payload.size());
    CY_CHECK(std::memcmp(asset.value()->bytes().data(), payload.data(), payload.size()) == 0);
    CY_CHECK_FALSE(asset.value()->is_placeholder());

    const LoadProgress progress = harness.assets.progress(request.value());
    CY_CHECK(progress.done);
    CY_CHECK(progress.stage == LoadStage::Complete);
    CY_CHECK_EQ(progress.completed, 1u);

    CY_REQUIRE(harness.assets.forget(request.value()).has_value());
}

CY_TEST_CASE("No worker ever blocks during a load") {
    // `core-jobs-and-concurrency` refuses a blocking region on a thread executing a job and COUNTS
    // the refusal, in every configuration — so this assertion is not silently vacuous in Profile
    // and Shipping the way an assertion-based one would be.
    const test::TempDir directory("assets_no_blocking");
    const std::string path = directory.file("content.cypak");
    cy::AssetId ids[8];
    cy::Array<u8> payloads[8];
    const cy::Array<u8>* pointers[8];
    for (usize i = 0; i < 8; ++i) {
        ids[i] = mint_asset_id();
        payloads[i] = corpus(20000 + (i * 1000), static_cast<u8>(i));
        pointers[i] = &payloads[i];
    }
    write_package(path, cy::Span<const cy::AssetId>(ids, 8),
                  cy::Span<const cy::Array<u8>*>(pointers, 8));

    Harness harness;
    harness.mount(path, mount_priority::kBasePackage);
    const cy::u64 before = cy::jobs::blocking_violations();

    const auto request = harness.assets.load_batch(cy::Span<const cy::AssetId>(ids, 8));
    CY_REQUIRE(request.has_value());
    CY_REQUIRE(harness.assets.wait(request.value()).has_value());

    for (usize i = 0; i < 8; ++i) {
        const auto asset = harness.assets.result_at(request.value(), i);
        CY_REQUIRE(asset.has_value());
        CY_CHECK_EQ(asset.value()->size(), payloads[i].size());
    }
    CY_CHECK_EQ(cy::jobs::blocking_violations(), before);
    CY_REQUIRE(harness.assets.forget(request.value()).has_value());
}

CY_TEST_CASE("Scenario: Duplicate request is coalesced") {
    // WHEN two systems request the same asset concurrently
    // THEN one load SHALL be performed and both requests SHALL receive the same Ref.
    const test::TempDir directory("assets_coalesced");
    const std::string path = directory.file("content.cypak");
    const cy::AssetId id = mint_asset_id();
    const cy::Array<u8> payload = corpus(30000, 2);
    const cy::Array<u8>* payloads[] = {&payload};
    write_package(path, cy::Span<const cy::AssetId>(&id, 1),
                  cy::Span<const cy::Array<u8>*>(payloads, 1));

    Harness harness;
    harness.mount(path, mount_priority::kBasePackage);

    const auto first = harness.assets.load_async(id);
    const auto second = harness.assets.load_async(id);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_CHECK(first.value() != second.value());  // two requests

    CY_REQUIRE(harness.assets.wait(first.value()).has_value());
    CY_REQUIRE(harness.assets.wait(second.value()).has_value());

    const auto a = harness.assets.result(first.value());
    const auto b = harness.assets.result(second.value());
    CY_REQUIRE(a.has_value());
    CY_REQUIRE(b.has_value());
    // The same Ref: one object, not two copies of one file's bytes.
    CY_CHECK(a.value().get() == b.value().get());

    const AssetSystemStats stats = harness.assets.stats();
    CY_CHECK_EQ(stats.loads_started, 1u);  // ONE load
    CY_CHECK_EQ(stats.loads_coalesced, 1u);
    CY_CHECK_EQ(stats.resident_assets, 1u);

    CY_REQUIRE(harness.assets.forget(first.value()).has_value());
    CY_REQUIRE(harness.assets.forget(second.value()).has_value());
}

CY_TEST_CASE("Scenario: Dependencies load with the parent") {
    // WHEN a material referencing three textures is loaded
    // THEN the dependency graph SHALL be resolved and all four assets loaded before the request
    //      reports completion.
    const test::TempDir directory("assets_dependencies");
    const std::string path = directory.file("material.cypak");
    const cy::AssetId material = mint_asset_id();
    const cy::AssetId textures[3] = {mint_asset_id(), mint_asset_id(), mint_asset_id()};

    {
        PackageWriter writer;
        PackageManifest manifest;
        CY_REQUIRE(manifest.set_build_id("test").has_value());
        CY_REQUIRE(writer.set_manifest(manifest).has_value());

        PackageWriter::EntryOptions material_options;
        material_options.kind = AssetKind::Material;
        const cy::Array<u8> material_bytes = corpus(512, 3);
        CY_REQUIRE(writer
                       .add(material, VariantKey::any(), material_bytes.span(), material_options,
                            cy::Span<const cy::AssetId>(textures, 3))
                       .has_value());
        for (usize i = 0; i < 3; ++i) {
            PackageWriter::EntryOptions texture_options;
            texture_options.kind = AssetKind::Texture;
            const cy::Array<u8> texture_bytes = corpus(4096, static_cast<u8>(10 + i));
            CY_REQUIRE(
                writer.add(textures[i], VariantKey::any(), texture_bytes.span(), texture_options)
                    .has_value());
        }
        CY_REQUIRE(writer.write(path.c_str()).has_value());
    }

    Harness harness;
    harness.mount(path, mount_priority::kBasePackage);

    const auto request = harness.assets.load_async(material);
    CY_REQUIRE(request.has_value());
    CY_REQUIRE(harness.assets.wait(request.value()).has_value());

    // All four resident by the time the request completed — the textures were never asked for by
    // name, and they are there.
    for (const cy::AssetId texture : textures) {
        const auto resident = harness.assets.find_resident(texture);
        CY_CHECK(resident.has_value());
    }
    const AssetSystemStats stats = harness.assets.stats();
    CY_CHECK_EQ(stats.dependency_loads, 3u);
    CY_CHECK_EQ(stats.resident_assets, 4u);

    CY_REQUIRE(harness.assets.forget(request.value()).has_value());
}

CY_TEST_CASE("Scenario: Cancellation") {
    // WHEN a pending load is cancelled (level unloaded before it completed)
    // THEN the request SHALL stop at the next stage boundary and release any partial results.
    const test::TempDir directory("assets_cancel");
    const std::string path = directory.file("content.cypak");
    constexpr usize kCount = 24;
    cy::AssetId ids[kCount];
    cy::Array<u8> payloads[kCount];
    const cy::Array<u8>* pointers[kCount];
    for (usize i = 0; i < kCount; ++i) {
        ids[i] = mint_asset_id();
        payloads[i] = corpus(200000, static_cast<u8>(i));
        pointers[i] = &payloads[i];
    }
    write_package(path, cy::Span<const cy::AssetId>(ids, kCount),
                  cy::Span<const cy::Array<u8>*>(pointers, kCount));

    Harness harness;
    harness.mount(path, mount_priority::kBasePackage);

    const auto request = harness.assets.load_batch(cy::Span<const cy::AssetId>(ids, kCount));
    CY_REQUIRE(request.has_value());
    CY_REQUIRE(harness.assets.cancel(request.value()).has_value());

    // Waiting still terminates: a cancelled stage signals its completion rather than abandoning it,
    // which is what stops a cancelled request from hanging its waiter forever.
    CY_REQUIRE(harness.assets.wait(request.value()).has_value());

    const AssetSystemStats stats = harness.assets.stats();
    CY_CHECK_EQ(stats.loads_started, kCount);
    // Every load either completed before the cancellation reached it or stopped at a stage
    // boundary. Both are correct; what must not happen is a load that is neither.
    CY_CHECK_EQ(stats.loads_completed + stats.loads_cancelled + stats.loads_failed, kCount);
    // A cancelled slot holds nothing: partial results were released rather than published.
    CY_CHECK(stats.resident_assets == stats.loads_completed);

    CY_REQUIRE(harness.assets.forget(request.value()).has_value());
}

CY_TEST_CASE("Cancelling one request does not cancel another that shares the load") {
    const test::TempDir directory("assets_cancel_shared");
    const std::string path = directory.file("content.cypak");
    const cy::AssetId id = mint_asset_id();
    const cy::Array<u8> payload = corpus(50000, 4);
    const cy::Array<u8>* payloads[] = {&payload};
    write_package(path, cy::Span<const cy::AssetId>(&id, 1),
                  cy::Span<const cy::Array<u8>*>(payloads, 1));

    Harness harness;
    harness.mount(path, mount_priority::kBasePackage);

    const auto mine = harness.assets.load_async(id);
    const auto theirs = harness.assets.load_async(id);
    CY_REQUIRE(mine.has_value());
    CY_REQUIRE(theirs.has_value());

    CY_REQUIRE(harness.assets.cancel(mine.value()).has_value());
    CY_REQUIRE(harness.assets.wait(theirs.value()).has_value());

    const auto asset = harness.assets.result(theirs.value());
    CY_REQUIRE(asset.has_value());
    CY_CHECK_EQ(asset.value()->size(), payload.size());

    CY_REQUIRE(harness.assets.forget(mine.value()).has_value());
    CY_REQUIRE(harness.assets.forget(theirs.value()).has_value());
}

CY_TEST_CASE("Scenario: Source deleted") {
    // WHEN a referenced asset no longer exists
    // THEN loading SHALL yield a typed placeholder and a diagnostic naming the referrer, rather
    //      than failing the whole load.
    const test::TempDir directory("assets_missing");
    const std::string path = directory.file("content.cypak");
    const cy::AssetId present = mint_asset_id();
    const cy::Array<u8> payload = corpus(1024, 5);
    const cy::Array<u8>* payloads[] = {&payload};
    write_package(path, cy::Span<const cy::AssetId>(&present, 1),
                  cy::Span<const cy::Array<u8>*>(payloads, 1));

    Harness harness;
    harness.mount(path, mount_priority::kBasePackage);

    const cy::AssetId missing = mint_asset_id();
    LoadOptions options;
    options.expected_kind = AssetKind::Texture;
    options.referrer = present;

    const cy::AssetId batch[] = {present, missing};
    const auto request = harness.assets.load_batch(cy::Span<const cy::AssetId>(batch, 2), options);
    CY_REQUIRE(request.has_value());
    CY_REQUIRE(harness.assets.wait(request.value()).has_value());

    // The whole load did not fail: the present asset is there...
    const auto first = harness.assets.result_at(request.value(), 0);
    CY_REQUIRE(first.has_value());
    CY_CHECK_FALSE(first.value()->is_placeholder());

    // ...and the missing one resolved to a TYPED placeholder rather than to an error.
    const auto second = harness.assets.result_at(request.value(), 1);
    CY_REQUIRE(second.has_value());
    CY_CHECK(second.value()->is_placeholder());
    CY_CHECK(second.value()->kind() == AssetKind::Texture);
    CY_CHECK_EQ(harness.assets.stats().placeholders_served, 1u);

    CY_REQUIRE(harness.assets.forget(request.value()).has_value());
}

CY_TEST_CASE("A blocking load returns the asset and never blocks a worker") {
    const test::TempDir directory("assets_blocking_load");
    const std::string path = directory.file("content.cypak");
    const cy::AssetId id = mint_asset_id();
    const cy::Array<u8> payload = corpus(8192, 6);
    const cy::Array<u8>* payloads[] = {&payload};
    write_package(path, cy::Span<const cy::AssetId>(&id, 1),
                  cy::Span<const cy::Array<u8>*>(payloads, 1));

    Harness harness;
    harness.mount(path, mount_priority::kBasePackage);
    const cy::u64 before = cy::jobs::blocking_violations();

    const auto asset = harness.assets.load(id);
    CY_REQUIRE(asset.has_value());
    CY_CHECK_EQ(asset.value()->size(), payload.size());
    CY_CHECK_EQ(cy::jobs::blocking_violations(), before);
}

CY_TEST_CASE("Immediate retention drops an asset as soon as nothing holds it") {
    const test::TempDir directory("assets_retention_immediate");
    const std::string path = directory.file("content.cypak");
    const cy::AssetId id = mint_asset_id();
    const cy::Array<u8> payload = corpus(4096, 7);
    const cy::Array<u8>* payloads[] = {&payload};
    write_package(path, cy::Span<const cy::AssetId>(&id, 1),
                  cy::Span<const cy::Array<u8>*>(payloads, 1));

    AssetSystemConfig config;
    config.retention = RetentionPolicy::Immediate;
    Harness harness(config);
    harness.mount(path, mount_priority::kBasePackage);

    {
        const auto asset = harness.assets.load(id);
        CY_REQUIRE(asset.has_value());
        CY_CHECK_EQ(harness.assets.stats().resident_assets, 1u);
    }
    // The last Ref went out of scope, and with it the memory.
    CY_CHECK_EQ(harness.assets.stats().resident_assets, 0u);
    CY_CHECK_EQ(harness.assets.stats().resident_bytes, 0u);
    CY_CHECK_EQ(harness.assets.stats().evictions, 1u);
}

CY_TEST_CASE("Time-delayed retention keeps an asset until update() collects it") {
    const test::TempDir directory("assets_retention_delayed");
    const std::string path = directory.file("content.cypak");
    const cy::AssetId id = mint_asset_id();
    const cy::Array<u8> payload = corpus(4096, 8);
    const cy::Array<u8>* payloads[] = {&payload};
    write_package(path, cy::Span<const cy::AssetId>(&id, 1),
                  cy::Span<const cy::Array<u8>*>(payloads, 1));

    AssetSystemConfig config;
    config.retention = RetentionPolicy::TimeDelayed;
    config.retention_delay_ns = 60'000'000'000;  // a minute: far longer than this test
    Harness harness(config);
    harness.mount(path, mount_priority::kBasePackage);

    {
        const auto asset = harness.assets.load(id);
        CY_REQUIRE(asset.has_value());
    }
    // Nothing holds it, and it is still resident — which is the point of the policy.
    CY_CHECK_EQ(harness.assets.stats().resident_assets, 1u);
    harness.assets.update();
    CY_CHECK_EQ(harness.assets.stats().resident_assets, 1u);

    // Re-requesting it is free: the load is not performed again.
    const cy::u64 started = harness.assets.stats().loads_started;
    const auto again = harness.assets.load(id);
    CY_REQUIRE(again.has_value());
    CY_CHECK_EQ(harness.assets.stats().loads_started, started);
}

CY_TEST_CASE("A retired asset that is revived and released is collected again") {
    // REGRESSION. A retired asset's one reference is held by the retention policy rather than by
    // any `Ref` — `on_last_reference` resurrects the count from zero to one so the policy can keep
    // it. Reviving the slot used to clear the retired flag WITHOUT taking that reference over, so
    // the asset was thereafter owned by nothing: dropping the reviving `Ref` never brought the
    // count to zero, `update()` never saw it as retired again, and it lived until the process
    // exited. The leak was invisible except under LeakSanitizer; this asserts the observable half
    // of it.
    const test::TempDir directory("assets_retention_revive");
    const std::string path = directory.file("content.cypak");
    const cy::AssetId id = mint_asset_id();
    const cy::Array<u8> payload = corpus(4096, 11);
    const cy::Array<u8>* payloads[] = {&payload};
    write_package(path, cy::Span<const cy::AssetId>(&id, 1),
                  cy::Span<const cy::Array<u8>*>(payloads, 1));

    AssetSystemConfig config;
    config.retention = RetentionPolicy::TimeDelayed;
    config.retention_delay_ns = 0;  // collected by the very next update()
    Harness harness(config);
    harness.mount(path, mount_priority::kBasePackage);

    {
        const auto first = harness.assets.load(id);
        CY_REQUIRE(first.has_value());
    }
    CY_CHECK_EQ(harness.assets.stats().resident_assets, 1u);  // retired, still resident

    {
        // The revival: a second request rides the retired slot rather than reloading it.
        const auto again = harness.assets.load(id);
        CY_REQUIRE(again.has_value());
        CY_CHECK_EQ(harness.assets.stats().loads_coalesced, 1u);
    }

    // Nothing holds it again, so it is retired again — and the delay is zero, so update() takes it.
    harness.assets.update();
    CY_CHECK_EQ(harness.assets.stats().resident_assets, 0u);
    CY_CHECK_EQ(harness.assets.stats().resident_bytes, 0u);
}

CY_TEST_CASE("Budget-based retention evicts the least recently used first") {
    const test::TempDir directory("assets_retention_budget");
    const std::string path = directory.file("content.cypak");
    constexpr usize kCount = 4;
    cy::AssetId ids[kCount];
    cy::Array<u8> payloads[kCount];
    const cy::Array<u8>* pointers[kCount];
    for (usize i = 0; i < kCount; ++i) {
        ids[i] = mint_asset_id();
        payloads[i] = corpus(usize{16} * 1024, static_cast<u8>(i));
        pointers[i] = &payloads[i];
    }
    write_package(path, cy::Span<const cy::AssetId>(ids, kCount),
                  cy::Span<const cy::Array<u8>*>(pointers, kCount));

    AssetSystemConfig config;
    config.retention = RetentionPolicy::BudgetBased;
    config.residency_budget_bytes = usize{40} * 1024;  // room for two of the four
    Harness harness(config);
    harness.mount(path, mount_priority::kBasePackage);

    for (const cy::AssetId id : ids) {
        const auto asset = harness.assets.load(id);
        CY_REQUIRE(asset.has_value());
    }
    CY_CHECK_EQ(harness.assets.stats().resident_assets, kCount);

    harness.assets.update();
    CY_CHECK(harness.assets.stats().resident_bytes <= config.residency_budget_bytes);

    // The two most recently used survive; the two oldest went.
    CY_CHECK_FALSE(harness.assets.find_resident(ids[0]).has_value());
    CY_CHECK(harness.assets.find_resident(ids[kCount - 1]).has_value());
    CY_CHECK_EQ(harness.assets.stats().evictions, 2u);
}

CY_TEST_CASE("preload holds an asset and release lets it go") {
    const test::TempDir directory("assets_preload");
    const std::string path = directory.file("content.cypak");
    const cy::AssetId id = mint_asset_id();
    const cy::Array<u8> payload = corpus(4096, 9);
    const cy::Array<u8>* payloads[] = {&payload};
    write_package(path, cy::Span<const cy::AssetId>(&id, 1),
                  cy::Span<const cy::Array<u8>*>(payloads, 1));

    AssetSystemConfig config;
    config.retention = RetentionPolicy::Immediate;
    Harness harness(config);
    harness.mount(path, mount_priority::kBasePackage);

    CY_REQUIRE(harness.assets.preload(cy::Span<const cy::AssetId>(&id, 1)).has_value());
    CY_CHECK_EQ(harness.assets.stats().resident_assets, 1u);

    // Resident without anybody holding a Ref, which is what "explicit residency control" means.
    const auto found = harness.assets.find_resident(id);
    CY_CHECK(found.has_value());

    CY_REQUIRE(harness.assets.release(cy::Span<const cy::AssetId>(&id, 1)).has_value());
    CY_CHECK_EQ(harness.assets.stats().resident_assets, 1u);  // the Ref above still holds it
}

CY_TEST_CASE("An asset system that is not running refuses work rather than crashing") {
    AssetSystem assets;
    CY_CHECK_FALSE(assets.is_running());
    CY_CHECK_FALSE(assets.load(mint_asset_id()).has_value());
    CY_CHECK_FALSE(assets.load_async(mint_asset_id()).has_value());
    CY_CHECK_FALSE(assets.find_resident(mint_asset_id()).has_value());
    assets.update();  // a no-op rather than a fault
    assets.shutdown();
}

CY_TEST_CASE("A memory-mapped entry is served without a copy") {
    const test::TempDir directory("assets_mapped");
    const std::string path = directory.file("mapped.cypak");
    const cy::AssetId id = mint_asset_id();
    const cy::Array<u8> payload = corpus(20000, 10);
    {
        PackageWriter writer;
        PackageManifest manifest;
        CY_REQUIRE(manifest.set_build_id("test").has_value());
        CY_REQUIRE(writer.set_manifest(manifest).has_value());
        PackageWriter::EntryOptions options;
        options.method = CompressionMethod::None;
        options.method_is_explicit = true;
        options.align_for_mapping = true;
        CY_REQUIRE(writer.add(id, VariantKey::any(), payload.span(), options).has_value());
        CY_REQUIRE(writer.write(path.c_str()).has_value());
    }

    Harness harness;
    harness.mount(path, mount_priority::kBasePackage);

    const auto asset = harness.assets.load(id);
    CY_REQUIRE(asset.has_value());
    CY_CHECK_EQ(asset.value()->size(), payload.size());
    CY_CHECK(std::memcmp(asset.value()->bytes().data(), payload.data(), payload.size()) == 0);
    if (memory_mapping_available()) {
        CY_CHECK(asset.value()->is_mapped());
        CY_CHECK_EQ(harness.assets.stats().entries_mapped, 1u);
    }
}
