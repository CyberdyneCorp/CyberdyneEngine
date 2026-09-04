// The filesystem watcher, and the reload it feeds. Task 1.4, carried forward from M2.
//
// `core-assets-and-io`'s hot-reload requirement has two halves and M2 had neither: "watch source
// files and cooked outputs" and "reload changed assets in place, preserving existing `Ref`s". Both
// are exercised here over a memory mount, which is what makes the settle period testable without a
// sleep: the test writes the file, steps its own clock, and polls.

#include <cy/core/assets/asset_system.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/assets/watch.h>
#include <cy/core/jobs/async.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/memory/scope.h>
#include <cy/test/test.h>

#include <string>
#include <utility>
#include <vector>

using namespace cy::assets;
using cy::i64;
using cy::u32;
using cy::u8;
using cy::usize;

namespace {

VirtualPath path_of(const char* raw) {
    auto path = VirtualPath::normalise(raw);
    CY_REQUIRE(path.has_value());
    return path.value();
}

/// What one poll reported, in the order it reported it.
struct Recorder {
    std::vector<std::string> paths;
    std::vector<FileChange> changes;

    static void observe(void* user, const WatchEvent& event) noexcept {
        auto* self = static_cast<Recorder*>(user);
        self->paths.emplace_back(event.path->view());
        self->changes.push_back(event.change);
    }

    void clear() {
        paths.clear();
        changes.clear();
    }
};

/// A memory mount is the whole fixture: it is writable, it is a `Mount` like any other, and it
/// costs no filesystem. The watcher cannot tell it from a directory mount, which is the point of
/// watching the namespace rather than a directory.
struct Fixture {
    VirtualFileSystem files;
    MemoryMount* memory = nullptr;

    Fixture() {
        auto mount = cy::make_unique<MemoryMount>(cy::current_allocator(), "memory");
        CY_REQUIRE(mount.has_value());
        memory = mount.value().get();
        CY_REQUIRE(files.mount_owned(std::move(mount.value()), 0).has_value());
    }

    void write(const char* raw, const char* text) const {
        CY_REQUIRE(
            memory->add(path_of(raw), text, std::char_traits<char>::length(text)).has_value());
    }
    void remove(const char* raw) const {
        CY_REQUIRE(memory->mark_deleted(path_of(raw)).has_value());
    }
};

constexpr i64 kSettleNs = 100'000'000;  // the default, spelled so the clock steps read plainly

}  // namespace

CY_TEST_CASE("the first poll is a baseline and reports nothing") {
    Fixture fixture;
    fixture.write("shaders/lit.slang", "float4 shade() { return 1; }");

    FileWatcher watcher(cy::current_allocator());
    CY_REQUIRE(watcher.start(fixture.files, FileWatcherConfig{}).has_value());
    CY_REQUIRE(watcher.watch(path_of("shaders")).has_value());

    Recorder recorder;
    auto reported = watcher.poll(0, &Recorder::observe, &recorder);
    CY_REQUIRE(reported.has_value());
    // A watch added over an existing tree opens with silence, not with one event per file that was
    // already there.
    CY_CHECK_EQ(*reported, 0U);
    CY_CHECK_EQ(watcher.tracked_count(), usize{1});
}

CY_TEST_CASE("an edited file is reported once it has settled, and not before") {
    Fixture fixture;
    fixture.write("shaders/lit.slang", "float4 shade() { return 1; }");

    FileWatcher watcher(cy::current_allocator());
    CY_REQUIRE(watcher.start(fixture.files, FileWatcherConfig{}).has_value());
    CY_REQUIRE(watcher.watch(path_of("shaders")).has_value());
    CY_REQUIRE(watcher.prime(0).has_value());

    fixture.write("shaders/lit.slang", "float4 shade() { return 2; }");

    Recorder recorder;
    // The poll that finds the change starts the settle period; it does not report.
    auto first = watcher.poll(kSettleNs, &Recorder::observe, &recorder);
    CY_REQUIRE(first.has_value());
    CY_CHECK_EQ(*first, 0U);

    // A poll a settle period later, with the content unchanged since, reports it.
    auto second = watcher.poll(2 * kSettleNs, &Recorder::observe, &recorder);
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(*second, 1U);
    CY_REQUIRE_EQ(recorder.paths.size(), usize{1});
    CY_CHECK(recorder.paths[0] == "shaders/lit.slang");
    CY_CHECK(recorder.changes[0] == FileChange::Modified);

    // And nothing more: a change is reported once, not on every poll that follows it.
    recorder.clear();
    auto third = watcher.poll(3 * kSettleNs, &Recorder::observe, &recorder);
    CY_REQUIRE(third.has_value());
    CY_CHECK_EQ(*third, 0U);
}

CY_TEST_CASE("a file still being written is not reported until it stops changing") {
    // `core-assets-and-io`'s "malformed file mid-write" scenario, from the watcher's side: an
    // editor writing in two steps produces two different contents, and a watcher that reported the
    // first would hand the reload path a truncated file.
    Fixture fixture;
    fixture.write("shaders/lit.slang", "original");

    FileWatcher watcher(cy::current_allocator());
    CY_REQUIRE(watcher.start(fixture.files, FileWatcherConfig{}).has_value());
    CY_REQUIRE(watcher.watch(path_of("shaders")).has_value());
    CY_REQUIRE(watcher.prime(0).has_value());

    Recorder recorder;
    fixture.write("shaders/lit.slang", "half-w");
    CY_REQUIRE(watcher.poll(kSettleNs, &Recorder::observe, &recorder).has_value());
    fixture.write("shaders/lit.slang", "half-written-then-finished");
    // A settle period after the FIRST sighting, but the content changed in between, so the clock
    // restarted and nothing is reported.
    auto during = watcher.poll(2 * kSettleNs, &Recorder::observe, &recorder);
    CY_REQUIRE(during.has_value());
    CY_CHECK_EQ(*during, 0U);
    CY_CHECK(recorder.paths.empty());

    auto after = watcher.poll(4 * kSettleNs, &Recorder::observe, &recorder);
    CY_REQUIRE(after.has_value());
    CY_CHECK_EQ(*after, 1U);
    CY_CHECK(recorder.changes[0] == FileChange::Modified);
    // Two settle periods were started for one reported change, which is what the counter is for.
    CY_CHECK_EQ(watcher.stats().settling, 2U);
}

CY_TEST_CASE("a same-size edit is a change, which is why the fingerprint is content") {
    // The edit a size-and-timestamp watcher misses, and the one shader iteration makes constantly:
    // a number changed, the file the same length.
    Fixture fixture;
    fixture.write("shaders/lit.slang", "return 1;");

    FileWatcher watcher(cy::current_allocator());
    CY_REQUIRE(watcher.start(fixture.files, FileWatcherConfig{}).has_value());
    CY_REQUIRE(watcher.watch(path_of("shaders/lit.slang")).has_value());
    CY_REQUIRE(watcher.prime(0).has_value());

    fixture.write("shaders/lit.slang", "return 2;");

    Recorder recorder;
    CY_REQUIRE(watcher.poll(kSettleNs, &Recorder::observe, &recorder).has_value());
    auto reported = watcher.poll(2 * kSettleNs, &Recorder::observe, &recorder);
    CY_REQUIRE(reported.has_value());
    CY_CHECK_EQ(*reported, 1U);
    CY_CHECK(recorder.changes[0] == FileChange::Modified);
}

CY_TEST_CASE("a new file is added and a deleted one is removed") {
    Fixture fixture;
    fixture.write("shaders/lit.slang", "one");

    FileWatcher watcher(cy::current_allocator());
    CY_REQUIRE(watcher.start(fixture.files, FileWatcherConfig{}).has_value());
    CY_REQUIRE(watcher.watch(path_of("shaders")).has_value());
    CY_REQUIRE(watcher.prime(0).has_value());

    fixture.write("shaders/unlit.slang", "two");
    Recorder recorder;
    CY_REQUIRE(watcher.poll(kSettleNs, &Recorder::observe, &recorder).has_value());
    auto added = watcher.poll(2 * kSettleNs, &Recorder::observe, &recorder);
    CY_REQUIRE(added.has_value());
    CY_CHECK_EQ(*added, 1U);
    CY_REQUIRE_EQ(recorder.changes.size(), usize{1});
    CY_CHECK(recorder.changes[0] == FileChange::Added);
    CY_CHECK(recorder.paths[0] == "shaders/unlit.slang");

    recorder.clear();
    fixture.remove("shaders/unlit.slang");
    auto removed = watcher.poll(3 * kSettleNs, &Recorder::observe, &recorder);
    CY_REQUIRE(removed.has_value());
    CY_CHECK_EQ(*removed, 1U);
    CY_CHECK(recorder.changes[0] == FileChange::Removed);
    // A removal is not remembered: what is gone is not tracked, so re-creating it is an Add.
    CY_CHECK_EQ(watcher.tracked_count(), usize{1});
}

CY_TEST_CASE("unwatching forgets what that watch brought in") {
    Fixture fixture;
    fixture.write("shaders/lit.slang", "one");
    fixture.write("materials/stone.mat", "two");

    FileWatcher watcher(cy::current_allocator());
    CY_REQUIRE(watcher.start(fixture.files, FileWatcherConfig{}).has_value());
    CY_REQUIRE(watcher.watch(path_of("shaders")).has_value());
    CY_REQUIRE(watcher.watch(path_of("materials")).has_value());
    CY_REQUIRE(watcher.prime(0).has_value());
    CY_CHECK_EQ(watcher.tracked_count(), usize{2});

    CY_REQUIRE(watcher.unwatch(path_of("shaders")).has_value());
    CY_CHECK_EQ(watcher.watch_count(), usize{1});
    CY_CHECK_EQ(watcher.tracked_count(), usize{1});
    CY_CHECK_FALSE(watcher.unwatch(path_of("shaders")).has_value());

    // The surviving watch still works, and its root index survived the renumbering.
    fixture.write("materials/stone.mat", "changed");
    Recorder recorder;
    CY_REQUIRE(watcher.poll(kSettleNs, &Recorder::observe, &recorder).has_value());
    auto reported = watcher.poll(2 * kSettleNs, &Recorder::observe, &recorder);
    CY_REQUIRE(reported.has_value());
    CY_CHECK_EQ(*reported, 1U);
    CY_CHECK(recorder.paths[0] == "materials/stone.mat");
}

// --- The acting half: AssetSystem::reload -------------------------------------------------------
//
// `core-assets-and-io`: "reload changed assets in place, preserving existing `Ref`s", and "reload
// SHALL notify dependents". The property that makes it worth having is the first one: a material
// holding a texture must keep holding the same texture object, or every dependent has to be found
// and rewritten, which is the thing hot reload exists to avoid.

namespace {

/// A job system, an async service, a memory-mounted namespace and an asset system, started and
/// stopped in the order the asset system requires — it must not outlive the threads its stages run
/// on. The same shape as test_asset_system.cpp's harness, over a loose mount rather than a package.
struct LooseHarness {
    LooseHarness() {
        cy::jobs::JobSystemConfig job_config;
        job_config.worker_count = 2;
        CY_REQUIRE(jobs.start(job_config).has_value());
        CY_REQUIRE(async.start(jobs).has_value());

        auto mount = cy::make_unique<MemoryMount>(cy::current_allocator(), "memory");
        CY_REQUIRE(mount.has_value());
        memory = mount.value().get();
        CY_REQUIRE(files.mount_owned(std::move(mount.value()), 0).has_value());

        CY_REQUIRE(assets.start(jobs, async, files, AssetSystemConfig{}).has_value());
    }

    ~LooseHarness() {
        assets.shutdown();
        async.stop();
        jobs.shutdown();
    }

    LooseHarness(const LooseHarness&) = delete;
    LooseHarness& operator=(const LooseHarness&) = delete;

    /// Put `text` where the loader will look for `id`. The spelling of that path is
    /// `package_entry_path`'s, which is the one place it is decided.
    void publish(cy::AssetId id, const char* text) const {
        auto path = package_entry_path(id, VariantKey::any());
        CY_REQUIRE(path.has_value());
        CY_REQUIRE(
            memory->add(path.value(), text, std::char_traits<char>::length(text)).has_value());
    }

    void unpublish(cy::AssetId id) const {
        auto path = package_entry_path(id, VariantKey::any());
        CY_REQUIRE(path.has_value());
        CY_REQUIRE(memory->mark_deleted(path.value()).has_value());
    }

    cy::jobs::JobSystem jobs;
    cy::jobs::AsyncService async;
    VirtualFileSystem files;
    MemoryMount* memory = nullptr;
    AssetSystem assets;
};

std::string text_of(const AssetData& data) {
    return {reinterpret_cast<const char*>(data.bytes().data()), data.size()};
}

struct ReloadRecorder {
    u32 calls = 0;
    usize bytes = 0;

    static void observe(void* user, const ReloadEvent& event) noexcept {
        auto* self = static_cast<ReloadRecorder*>(user);
        ++self->calls;
        self->bytes = event.bytes;
    }
};

}  // namespace

CY_TEST_CASE("a reload replaces the bytes inside the asset every Ref already holds") {
    LooseHarness harness;
    const cy::AssetId id = mint_asset_id();
    harness.publish(id, "first");

    const auto asset = harness.assets.load(id);
    CY_REQUIRE(asset.has_value());
    CY_CHECK(text_of(*asset.value()) == "first");

    ReloadRecorder recorder;
    CY_REQUIRE(harness.assets.add_reload_observer(&ReloadRecorder::observe, &recorder).has_value());

    harness.publish(id, "second, and longer");
    CY_REQUIRE(harness.assets.reload(id).has_value());

    // THE ASSERTION. The `Ref` taken before the reload — the one a material would be holding — now
    // reads the new content, without having been re-acquired and without the object having moved.
    CY_CHECK(text_of(*asset.value()) == "second, and longer");
    CY_CHECK_EQ(harness.assets.stats().reloads_completed, 1U);
    CY_CHECK_EQ(recorder.calls, 1U);
    CY_CHECK_EQ(recorder.bytes, usize{18});

    // A removed observer is not called again, and removing one that is not registered is not an
    // error worth reporting.
    harness.assets.remove_reload_observer(&ReloadRecorder::observe, &recorder);
    harness.publish(id, "third");
    CY_REQUIRE(harness.assets.reload(id).has_value());
    CY_CHECK_EQ(recorder.calls, 1U);
    CY_CHECK(text_of(*asset.value()) == "third");

    // The payloads the two reloads replaced are released here, not at the swap: a reader may still
    // have been holding a span into them.
    harness.assets.update();
    CY_CHECK(text_of(*asset.value()) == "third");
}

CY_TEST_CASE("a reload that cannot read leaves the old asset in use") {
    // `core-assets-and-io`'s "Reload failure keeps the old asset": the previously loaded asset
    // remains in use and the failure is reported rather than swallowed.
    LooseHarness harness;
    const cy::AssetId id = mint_asset_id();
    harness.publish(id, "the good bytes");

    const auto asset = harness.assets.load(id);
    CY_REQUIRE(asset.has_value());

    harness.unpublish(id);
    const cy::Status reloaded = harness.assets.reload(id);
    CY_CHECK_FALSE(reloaded.has_value());
    CY_CHECK(text_of(*asset.value()) == "the good bytes");
    CY_CHECK_EQ(harness.assets.stats().reloads_failed, 1U);
    CY_CHECK_EQ(harness.assets.stats().reloads_completed, 0U);
}

CY_TEST_CASE("reloading an asset that is not resident is refused rather than loading it") {
    LooseHarness harness;
    const cy::AssetId id = mint_asset_id();
    harness.publish(id, "never loaded");

    const cy::Status reloaded = harness.assets.reload(id);
    CY_CHECK_FALSE(reloaded.has_value());
    CY_CHECK(reloaded.error().code == cy::ErrorCode::NotFound);
    // And it did not start a load behind the caller's back.
    CY_CHECK_EQ(harness.assets.stats().loads_started, 0U);
}
