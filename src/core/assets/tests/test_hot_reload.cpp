// The watcher and the reload entry point. Task 1.4.
//
// `core-assets-and-io`'s hot-reload requirement had nothing behind it at the end of M2 — no
// watcher, no `reload`. These cases are the two scenarios it names, run against a real filesystem
// and a real asset system, because both are about what happens when a file on disk changes
// underneath something that has already loaded it.
//
// Integration rather than unit: every case here writes files and one of them starts a job system.

#include "temp_dir.h"

#include <cy/core/assets/asset_system.h>
#include <cy/core/assets/hot_reload.h>
#include <cy/core/assets/package.h>
#include <cy/core/memory/scope.h>
#include <cy/test/test.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

using namespace cy::assets;
using cy::u32;
using cy::u8;
using cy::usize;

namespace {

/// Write a file whose contents are `text`. Used rather than `write_atomic` where the point is to
/// simulate a tool writing in place.
void write_text(const std::string& path, const char* text) {
    CY_REQUIRE(fs::write_atomic(path.c_str(), text, std::strlen(text)).has_value());
}

/// Poll until the watcher reports something or `attempts` polls have gone by.
///
/// A change is reported only once two polls agree about it — the settling rule that keeps a
/// half-written file out of a loader — so a test that polls once and asserts nothing happened would
/// be asserting the rule works, and a test that polls once and asserts something did would be
/// asserting it does not. Both matter, so the cases below do each explicitly and use this only
/// where "eventually" is the property.
u32 poll_until_reported(FileWatcher& watcher, cy::Array<FileEvent>& events, u32 attempts = 8) {
    for (u32 attempt = 0; attempt < attempts; ++attempt) {
        const auto reported = watcher.poll(events);
        CY_REQUIRE(reported.has_value());
        if (*reported != 0) {
            return *reported;
        }
    }
    return 0;
}

}  // namespace

CY_TEST_CASE("a file is reported only once its size and time have settled") {
    const test::TempDir directory("watch_settle");
    const std::string path = directory.file("shader.slang");
    FileWatcher watcher(cy::current_allocator());
    CY_REQUIRE(watcher.watch_file(path.c_str()).has_value());
    CY_CHECK_EQ(watcher.stats().watched, 1u);

    cy::Array<FileEvent> events;

    // Nothing there yet: a watched path that does not exist is legitimate and silent.
    CY_REQUIRE(watcher.poll(events).has_value());
    CY_CHECK_EQ(events.size(), 0u);

    write_text(path, "float4 main() { return 1; }");

    // The first poll that sees it does NOT report it. This is the scenario the settling rule is
    // for: an art tool part-way through writing a file has exactly this shape, and reporting it
    // here is how a loader gets handed half a texture.
    const auto first = watcher.poll(events);
    CY_REQUIRE(first.has_value());
    CY_CHECK_EQ(*first, 0u);

    // The second poll agrees with the first, so the file has settled and is reported.
    const auto second = watcher.poll(events);
    CY_REQUIRE(second.has_value());
    CY_REQUIRE_EQ(*second, 1u);
    CY_CHECK(events[0].change == FileChange::Created);
    CY_CHECK_EQ(std::strcmp(events[0].path, path.c_str()), 0);
    CY_CHECK_GT(events[0].size, 0u);

    // And it is reported once: a file that has not changed since is not news.
    CY_REQUIRE(watcher.poll(events).has_value());
    CY_CHECK_EQ(events.size(), 0u);
}

CY_TEST_CASE("an edit is reported, and a deletion is too") {
    const test::TempDir directory("watch_edit");
    const std::string path = directory.file("material.json");
    write_text(path, "{}");

    FileWatcher watcher(cy::current_allocator());
    CY_REQUIRE(watcher.watch_file(path.c_str()).has_value());

    cy::Array<FileEvent> events;
    CY_REQUIRE_EQ(poll_until_reported(watcher, events), 1u);
    CY_CHECK(events[0].change == FileChange::Created);

    // A different size, so the change is visible whatever the filesystem's timestamp resolution is
    // — one second on some of them, which is longer than this test takes.
    write_text(path, "{\"albedo\":[1,0,0]}");
    CY_REQUIRE_EQ(poll_until_reported(watcher, events), 1u);
    CY_CHECK(events[0].change == FileChange::Modified);
    CY_CHECK_EQ(events[0].size, 18u);

    CY_REQUIRE(fs::remove_file(path.c_str()).has_value());
    CY_REQUIRE_EQ(poll_until_reported(watcher, events), 1u);
    CY_CHECK(events[0].change == FileChange::Removed);

    // An explicitly watched path stays watched after its file disappears: an editor that saves by
    // writing a temporary and renaming over the target produces exactly a removal followed by a
    // creation, and a watcher that gave up on the removal would miss every save after the first.
    CY_CHECK_EQ(watcher.stats().watched, 1u);
    write_text(path, "{}");
    CY_REQUIRE_EQ(poll_until_reported(watcher, events), 1u);
    CY_CHECK(events[0].change == FileChange::Created);
}

CY_TEST_CASE("a watched directory picks up files that appear in it") {
    const test::TempDir directory("watch_directory");
    write_text(directory.file("a.slang"), "a");

    FileWatcher watcher(cy::current_allocator());
    CY_REQUIRE(watcher.watch_directory(directory.c_str(), false).has_value());
    CY_CHECK_EQ(watcher.stats().roots, 1u);
    CY_CHECK_EQ(watcher.stats().watched, 1u);

    cy::Array<FileEvent> events;
    CY_REQUIRE_EQ(poll_until_reported(watcher, events), 1u);
    CY_CHECK(events[0].change == FileChange::Created);

    // A file nobody named is found by the walk the poll does.
    write_text(directory.file("b.slang"), "bb");
    CY_REQUIRE_EQ(poll_until_reported(watcher, events), 1u);
    CY_CHECK(events[0].change == FileChange::Created);
    CY_CHECK_EQ(events[0].size, 2u);
    CY_CHECK_EQ(watcher.stats().watched, 2u);

    // A file that came from the walk is dropped once it is gone and its removal has been reported;
    // a caller that never named it is not owed a Created if it comes back under another name.
    CY_REQUIRE(fs::remove_file(directory.file("b.slang").c_str()).has_value());
    CY_REQUIRE_EQ(poll_until_reported(watcher, events), 1u);
    CY_CHECK(events[0].change == FileChange::Removed);
    CY_CHECK_EQ(watcher.stats().watched, 1u);

    // Unwatching the root stops the walk, so nothing new is found.
    CY_REQUIRE(watcher.unwatch(directory.c_str()).has_value());
    write_text(directory.file("c.slang"), "ccc");
    CY_REQUIRE(watcher.poll(events).has_value());
    CY_CHECK_EQ(events.size(), 0u);
    CY_CHECK_EQ(watcher.stats().roots, 0u);
}

namespace {

/// The state a reload listener records, so a case can assert it was told.
struct Heard {
    u32 calls = 0;
    usize bytes = 0;
    usize previous_bytes = 0;
};

void on_reload(void* user, const ReloadEvent& event) noexcept {
    auto& heard = *static_cast<Heard*>(user);
    ++heard.calls;
    heard.bytes = event.bytes;
    heard.previous_bytes = event.previous_bytes;
}

/// A job system, an async service, a virtual filesystem and an asset system over a directory of
/// loose files — which is what a project mount is, and the case hot reload exists for.
struct LooseHarness {
    explicit LooseHarness(const std::string& root) {
        cy::jobs::JobSystemConfig job_config;
        job_config.worker_count = 2;
        CY_REQUIRE(jobs.start(job_config).has_value());
        CY_REQUIRE(async.start(jobs).has_value());
        auto mount = DirectoryMount::create(root.c_str(), MountKind::Project, true);
        CY_REQUIRE(mount.has_value());
        CY_REQUIRE(
            files.mount_owned(std::move(mount.value()), mount_priority::kProject).has_value());
        CY_REQUIRE(assets.start(jobs, async, files, AssetSystemConfig{}).has_value());
    }

    ~LooseHarness() {
        assets.shutdown();
        async.stop();
        jobs.shutdown();
    }

    LooseHarness(const LooseHarness&) = delete;
    LooseHarness& operator=(const LooseHarness&) = delete;

    cy::jobs::JobSystem jobs;
    cy::jobs::AsyncService async;
    VirtualFileSystem files;
    AssetSystem assets;
};

/// The native path a loose asset is served from: the mount root, plus the virtual path the id maps
/// to. Spelled out here because a test that hard-codes the spelling would go stale the day
/// `package_entry_path` changes, and this is the one place a test needs the answer.
std::string host_path_of(const std::string& root, cy::AssetId id) {
    const auto path = package_entry_path(id, VariantKey::any());
    CY_REQUIRE(path.has_value());
    return root + "/" + path->c_str();
}

}  // namespace

CY_TEST_CASE("a reload replaces the payload under the references that hold it") {
    const test::TempDir directory("reload_in_place");
    const cy::AssetId id = cy::AssetId(0x5eed'1234ULL, 0x0000'0001ULL);
    const std::string file = host_path_of(directory.c_str(), id);
    CY_REQUIRE(
        fs::create_directories((std::string(directory.c_str()) + "/" + kPackageMountRoot).c_str())
            .has_value());
    write_text(file, "first");

    LooseHarness harness(directory.c_str());
    Heard heard;
    CY_REQUIRE(harness.assets.add_reload_listener(&on_reload, &heard).has_value());

    const auto loaded = harness.assets.load(id);
    CY_REQUIRE(loaded.has_value());
    // A reference of its own, copied rather than bound: an independent `Ref` outliving the load
    // request is the thing the requirement is about, and a const reference to the request's own
    // would not be one.
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    cy::Ref<AssetData> held = loaded.value();
    CY_REQUIRE(held);
    const AssetData* address_before = held.get();
    CY_CHECK_EQ(held->size(), 5u);

    // The edit, and the reload. THE `Ref` IS NOT REACQUIRED: this is the requirement — an existing
    // reference sees the new content, so a material holding a texture keeps working.
    write_text(file, "second-and-longer");
    CY_REQUIRE(harness.assets.reload(id).has_value());

    CY_CHECK_EQ(static_cast<const void*>(held.get()), static_cast<const void*>(address_before));
    CY_CHECK_EQ(held->size(), 17u);
    CY_CHECK_EQ(std::memcmp(held->bytes().data(), "second-and-longer", 17), 0);

    // And the dependents were told, with both sizes, so a listener rebuilding a GPU resource knows
    // whether its allocation still fits.
    CY_CHECK_EQ(heard.calls, 1u);
    CY_CHECK_EQ(heard.previous_bytes, 5u);
    CY_CHECK_EQ(heard.bytes, 17u);

    const AssetSystemStats stats = harness.assets.stats();
    CY_CHECK_EQ(stats.reloads, 1u);
    CY_CHECK_EQ(stats.reload_failures, 0u);

    // A listener that has been removed is not called again.
    harness.assets.remove_reload_listener(&on_reload, &heard);
    write_text(file, "third");
    CY_REQUIRE(harness.assets.reload(id).has_value());
    CY_CHECK_EQ(heard.calls, 1u);
    CY_CHECK_EQ(held->size(), 5u);
}

CY_TEST_CASE("a failed reload keeps the asset that works") {
    // `core-assets-and-io` — "Reload failure keeps the old asset": a re-import that meets a
    // malformed or missing file leaves the loaded asset in use and reports the failure.
    const test::TempDir directory("reload_failure");
    const cy::AssetId id = cy::AssetId(0x5eed'4321ULL, 0x0000'0002ULL);
    const std::string file = host_path_of(directory.c_str(), id);
    CY_REQUIRE(
        fs::create_directories((std::string(directory.c_str()) + "/" + kPackageMountRoot).c_str())
            .has_value());
    write_text(file, "the good payload");

    LooseHarness harness(directory.c_str());
    const auto loaded = harness.assets.load(id);
    CY_REQUIRE(loaded.has_value());
    // A reference of its own, copied rather than bound: an independent `Ref` outliving the load
    // request is the thing the requirement is about, and a const reference to the request's own
    // would not be one.
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    cy::Ref<AssetData> held = loaded.value();
    CY_REQUIRE(held);
    CY_CHECK_EQ(held->size(), 16u);

    CY_REQUIRE(fs::remove_file(file.c_str()).has_value());
    const cy::Status reloaded = harness.assets.reload(id);
    CY_CHECK_FALSE(reloaded.has_value());

    // Still the old bytes, still the same object, and the failure is counted rather than only
    // returned — an edit that never seems to take effect should be a number somebody can look at.
    CY_CHECK_EQ(held->size(), 16u);
    CY_CHECK_EQ(std::memcmp(held->bytes().data(), "the good payload", 16), 0);
    CY_CHECK_EQ(harness.assets.stats().reloads, 0u);
    CY_CHECK_EQ(harness.assets.stats().reload_failures, 1u);
}

CY_TEST_CASE("reloading an asset nobody loaded is refused rather than guessed at") {
    const test::TempDir directory("reload_absent");
    LooseHarness harness(directory.c_str());
    const cy::AssetId id = cy::AssetId(0xdead'beefULL, 0x0000'0003ULL);

    // There is nothing resident, so there is nothing to replace in place. Loading it is the
    // caller's answer, and saying so is better than quietly starting a load the caller did not ask
    // for and cannot wait on.
    const cy::Status refused = harness.assets.reload(id);
    CY_CHECK_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::NotFound);
}
