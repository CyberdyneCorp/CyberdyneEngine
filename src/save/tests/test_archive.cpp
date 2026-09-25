// Generations, the journal, retention and the transactional guarantee. Tasks 6.2 and 6.4.

#include <cy/save/archive.h>
#include <cy/save/conflict.h>
#include <cy/save/inspect.h>
#include <cy/save/storage.h>
#include <cy/test/fixtures.h>
#include <cy/test/test.h>

#include "fixtures.h"
#include "inspect_fixture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>

using namespace cy;
using namespace cy::save;
using namespace cy::save::test;

namespace {

constexpr RegionKey kVillage{0x0A00'0000'0000'0001ULL};
constexpr RegionKey kQuarry{0x0A00'0000'0000'0002ULL};
constexpr RegionKey kSummit{0x0A00'0000'0000'0003ULL};

SaveIdentity identity() noexcept {
    SaveIdentity id;
    id.build_id = "6.0.0+test";
    id.project = AssetId(1, 1);
    id.save = AssetId(2, 2);
    id.campaign = AssetId(3, 3);
    id.session_seed = 0x5EED;
    return id;
}

void record_health(Overlay& overlay, RegionKey region, PersistentId id, u32 revives) {
    Health health;
    health.revives = revives;
    CY_REQUIRE(overlay.record_component(region, id, health_type(), &health, 1).has_value());
}

u32 revives_of(const Overlay& overlay, RegionKey region, PersistentId id) noexcept {
    const ComponentDelta* delta =
        overlay.find_component(region, id, reflect::TypeId(kHealthTypeId));
    if (delta == nullptr) {
        return 0xFFFF'FFFFU;
    }
    u32 value = 0;
    const Span<const u8> bytes = delta->record.bytes(reflect::FieldId(kHealthRevives));
    if (!serialize::decode_scalar(serialize::WireType::U32, bytes.data(),
                                  static_cast<u32>(bytes.size()), &value)) {
        return 0xFFFF'FFFEU;
    }
    return value;
}

/// A world of three regions, one of them destroyed content.
void populate(Overlay& overlay) {
    record_health(overlay, kVillage, entity(1), 3);
    record_health(overlay, kVillage, entity(2), 4);
    record_health(overlay, kQuarry, entity(10), 5);
    CY_REQUIRE(overlay.destroy_entity(kSummit, entity(20)).has_value());
    overlay.set_simulation_point(1000);
}

}  // namespace

CY_TEST_CASE("a committed generation loads back whole") {
    MemoryBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(store).has_value());

    Overlay written(test_allocator());
    populate(written);
    const Expected<u32, Error> generation = archive.commit(written, identity());
    CY_REQUIRE(generation.has_value());
    CY_CHECK_EQ(*generation, 1U);

    Overlay read(test_allocator());
    LoadPolicy policy;
    LoadReport report;
    CY_REQUIRE(archive.load(policy, read, report).has_value());
    CY_CHECK_FALSE(report.failed());
    CY_CHECK_EQ(report.generations_skipped, 0U);
    CY_CHECK_EQ(read.region_count(), 3U);
    CY_CHECK_EQ(revives_of(read, kVillage, entity(1)), 3U);
    CY_CHECK_EQ(revives_of(read, kQuarry, entity(10)), 5U);
    CY_CHECK_EQ(read.find_entry(kSummit, entity(20))->kind, EntryKind::Tombstone);
    CY_CHECK_EQ(read.simulation_point(), 1000U);
}

CY_TEST_CASE("an unloaded region round-trips without being loaded") {
    // M6's exit criterion, and `save-and-persistence`'s "A save with five per cent resident":
    // producing a save loads no region, and the state of one that was never resident survives.
    MemoryBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(store).has_value());

    Overlay written(test_allocator());
    // The village is resident and being played in; the quarry was visited long ago, its buildings
    // were destroyed, and it has since unloaded. The summit was never resident at all.
    CY_REQUIRE(written.set_residency(kVillage, Residency::Resident).has_value());
    record_health(written, kVillage, entity(1), 3);
    record_health(written, kQuarry, entity(10), 5);
    CY_REQUIRE(written.destroy_entity(kQuarry, entity(11)).has_value());
    CY_REQUIRE(written.set_residency(kQuarry, Residency::Unloaded).has_value());
    CY_REQUIRE(written.destroy_entity(kSummit, entity(20)).has_value());

    CY_REQUIRE_EQ(written.resident_region_count(), 1U);
    CY_REQUIRE(archive.commit(written, identity()).has_value());
    // Nothing was loaded to write the save: residency is unchanged by committing one.
    CY_CHECK_EQ(written.resident_region_count(), 1U);

    Overlay read(test_allocator());
    LoadPolicy policy;
    LoadReport report;
    CY_REQUIRE(archive.load(policy, read, report).has_value());
    CY_CHECK_EQ(revives_of(read, kQuarry, entity(10)), 5U);
    CY_CHECK_EQ(read.find_entry(kQuarry, entity(11))->kind, EntryKind::Tombstone);
    CY_CHECK_EQ(read.find_entry(kSummit, entity(20))->kind, EntryKind::Tombstone);
    // Loading a save does not make a region resident either. Streaming decides that, and it will
    // apply these deltas during activation.
    CY_CHECK_EQ(read.resident_region_count(), 0U);
}

CY_TEST_CASE("a region whose state did not change is not written again") {
    // Content-addressed chunks: "incremental writes touch only changed chunks", and generations
    // share storage rather than each holding a copy of the world.
    MemoryBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(store).has_value());

    Overlay overlay(test_allocator());
    populate(overlay);
    CY_REQUIRE(archive.commit(overlay, identity()).has_value());
    const usize after_first = store.object_count();
    const u32 writes_after_first = store.writes();

    // One region changes; the other two do not.
    record_health(overlay, kVillage, entity(1), 99);
    CY_REQUIRE(archive.commit(overlay, identity()).has_value());

    // One new chunk, one new manifest, one rewritten pointer. Not three chunks.
    CY_CHECK_EQ(store.writes() - writes_after_first, 3U);
    CY_CHECK_EQ(store.object_count(), after_first + 2U);
}

CY_TEST_CASE("a corrupted newest generation falls back to an earlier one and says so") {
    // "WHEN the newest generation fails verification THEN an earlier generation SHALL be loadable
    // and the failure reported."
    MemoryBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(store).has_value());

    Overlay overlay(test_allocator());
    populate(overlay);
    CY_REQUIRE(archive.commit(overlay, identity()).has_value());
    Manifest first(test_allocator());
    LoadReport reading;
    CY_REQUIRE(archive.read_manifest(1, first, reading).has_value());

    record_health(overlay, kVillage, entity(1), 99);
    CY_REQUIRE(archive.commit(overlay, identity()).has_value());
    Manifest second(test_allocator());
    CY_REQUIRE(archive.read_manifest(2, second, reading).has_value());

    // Damage the one chunk generation 2 does not share with generation 1.
    usize damaged = 0;
    for (const ChunkRef& chunk : second.chunks) {
        const ChunkRef* previous = first.find_chunk(chunk.scope, chunk.region);
        if (previous != nullptr && previous->hash == chunk.hash) {
            continue;
        }
        char text[assets::ContentHash::kTextLength + 1] = {};
        chunk.hash.format(text);
        char key[128] = {};
        (void)std::snprintf(key, sizeof(key), "chunks/%s.cychunk", text);
        CY_REQUIRE(store.corrupt(key).has_value());
        ++damaged;
    }
    CY_REQUIRE_EQ(damaged, 1U);

    Overlay read(test_allocator());
    LoadPolicy policy;
    LoadReport report;
    CY_REQUIRE(archive.load(policy, read, report).has_value());
    CY_CHECK_EQ(report.generations_skipped, 1U);
    // The older generation's value, not the newer one's, and not a mixture of the two.
    CY_CHECK_EQ(revives_of(read, kVillage, entity(1)), 3U);
}

CY_TEST_CASE("a failure at any write step leaves the previous save loadable") {
    // "Transactional tests simulating failure after every write phase and verifying the previous
    // save remains valid." The write budget fails the store's Nth write, so N walks every step of
    // the sequence — chunks, manifest, pointer — including the one that switches the save.
    for (u32 budget = 0; budget < 8; ++budget) {
        MemoryBackend store(test_allocator());
        SaveArchive archive(test_allocator());
        CY_REQUIRE(archive.open(store).has_value());

        Overlay overlay(test_allocator());
        populate(overlay);
        CY_REQUIRE(archive.commit(overlay, identity()).has_value());

        record_health(overlay, kVillage, entity(1), 99);
        store.set_write_budget(budget);
        const Expected<u32, Error> second = archive.commit(overlay, identity());
        store.set_write_budget(MemoryBackend::kNoLimit);

        Overlay read(test_allocator());
        LoadPolicy policy;
        LoadReport report;
        CY_REQUIRE(archive.load(policy, read, report).has_value());
        CY_CHECK_FALSE(report.failed());
        // Either the second commit completed — and the new value is there — or it did not, and the
        // first generation is intact. Never a partial save, and never an unloadable one.
        const u32 expected = second.has_value() ? 99U : 3U;
        CY_CHECK_EQ(revives_of(read, kVillage, entity(1)), expected);
        CY_CHECK_EQ(read.region_count(), 3U);
    }
}

CY_TEST_CASE("the journal appends what changed and replays in order") {
    MemoryBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(store).has_value());

    Overlay overlay(test_allocator());
    populate(overlay);
    CY_REQUIRE(archive.commit(overlay, identity()).has_value());
    overlay.clear_dirty();

    record_health(overlay, kVillage, entity(1), 50);
    CY_REQUIRE(archive.append_journal(overlay).has_value());
    overlay.clear_dirty();
    record_health(overlay, kVillage, entity(1), 60);
    CY_REQUIRE(overlay.destroy_entity(kQuarry, entity(10)).has_value());
    CY_REQUIRE(archive.append_journal(overlay).has_value());

    Overlay read(test_allocator());
    LoadPolicy policy;
    LoadReport report;
    CY_REQUIRE(archive.load(policy, read, report).has_value());
    // The last append wins, which is what replaying in order means.
    CY_CHECK_EQ(revives_of(read, kVillage, entity(1)), 60U);
    CY_CHECK_EQ(read.find_entry(kQuarry, entity(10))->kind, EntryKind::Tombstone);
}

CY_TEST_CASE("an append with nothing dirty writes nothing") {
    MemoryBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(store).has_value());
    Overlay overlay(test_allocator());
    populate(overlay);
    CY_REQUIRE(archive.commit(overlay, identity()).has_value());
    overlay.clear_dirty();

    const u32 before = store.writes();
    CY_REQUIRE(archive.append_journal(overlay).has_value());
    CY_CHECK_EQ(store.writes(), before);
}

CY_TEST_CASE("compaction folds the journal into a new base and drops it") {
    MemoryBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(store).has_value());

    Overlay overlay(test_allocator());
    populate(overlay);
    CY_REQUIRE(archive.commit(overlay, identity()).has_value());
    overlay.clear_dirty();
    record_health(overlay, kVillage, entity(1), 77);
    CY_REQUIRE(archive.append_journal(overlay).has_value());

    const Expected<u32, Error> compacted = archive.compact(identity());
    CY_REQUIRE(compacted.has_value());
    CY_CHECK_EQ(*compacted, 2U);

    // The journal of the previous base is gone, and the value it carried is in the new base.
    Overlay read(test_allocator());
    LoadPolicy policy;
    LoadReport report;
    CY_REQUIRE(archive.load(policy, read, report).has_value());
    CY_CHECK_EQ(revives_of(read, kVillage, entity(1)), 77U);

    Manifest base(test_allocator());
    LoadReport reading;
    CY_REQUIRE(archive.read_manifest(2, base, reading).has_value());
    CY_CHECK_EQ(base.generation, 2U);
}

CY_TEST_CASE("retention keeps the policy's generations and collects the rest") {
    MemoryBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    ArchiveConfig config;
    config.retained_generations = 2;
    CY_REQUIRE(archive.open(store, config).has_value());

    Overlay overlay(test_allocator());
    populate(overlay);
    for (u32 pass = 0; pass < 5; ++pass) {
        record_health(overlay, kVillage, entity(1), pass);
        CY_REQUIRE(archive.commit(overlay, identity()).has_value());
    }

    Array<u32> generations(test_allocator());
    CY_REQUIRE(archive.generations(generations).has_value());
    CY_CHECK_EQ(generations.size(), 2U);
    CY_CHECK_EQ(generations[0], 4U);
    CY_CHECK_EQ(generations[1], 5U);

    // Both retained generations still load, which is the point of retaining them.
    for (const u32 generation : generations) {
        Manifest manifest(test_allocator());
        LoadReport report;
        CY_REQUIRE(archive.read_manifest(generation, manifest, report).has_value());
        for (const ChunkRef& chunk : manifest.chunks) {
            char text[assets::ContentHash::kTextLength + 1] = {};
            chunk.hash.format(text);
            char key[128] = {};
            (void)std::snprintf(key, sizeof(key), "chunks/%s.cychunk", text);
            CY_CHECK(store.exists(key));
        }
    }
}

CY_TEST_CASE("a store with nothing committed reports that, rather than failing obscurely") {
    MemoryBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(store).has_value());
    Overlay read(test_allocator());
    LoadPolicy policy;
    LoadReport report;
    const Status loaded = archive.load(policy, read, report);
    CY_REQUIRE_FALSE(loaded.has_value());
    CY_CHECK_EQ(loaded.error().code, ErrorCode::NotFound);
}

CY_TEST_CASE("an archive that retains no generation is refused at open") {
    MemoryBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    ArchiveConfig config;
    config.retained_generations = 0;
    CY_CHECK_FALSE(archive.open(store, config).has_value());
}

CY_TEST_CASE("the size of the world does not set the cost of an autosave") {
    // "WHEN the large-world benchmark runs THEN save cost SHALL scale with changes rather than with
    // world size." A thousand regions, one of them changed: the autosave writes one object. The
    // assertion is on the WRITES rather than on a duration, because a wall-clock threshold in a
    // test is a flake and a write count is the thing the requirement is actually about.
    MemoryBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(store).has_value());

    Overlay overlay(test_allocator());
    constexpr u32 kRegions = 1000;
    for (u32 region = 0; region < kRegions; ++region) {
        record_health(overlay, RegionKey(0x0C00'0000'0000'0000ULL + region), entity(region), 1);
    }
    CY_REQUIRE(archive.commit(overlay, identity()).has_value());
    overlay.clear_dirty();

    const u32 baseline = store.writes();
    record_health(overlay, RegionKey(0x0C00'0000'0000'0007ULL), entity(7), 42);
    CY_CHECK_EQ(overlay.dirty_region_count(), 1U);
    CY_REQUIRE(archive.append_journal(overlay).has_value());
    CY_CHECK_EQ(store.writes() - baseline, 1U);

    // And it is the change that comes back, over a base of a thousand regions.
    Overlay read(test_allocator());
    LoadPolicy policy;
    LoadReport report;
    CY_REQUIRE(archive.load(policy, read, report).has_value());
    CY_CHECK_EQ(read.region_count(), kRegions);
    CY_CHECK_EQ(revives_of(read, RegionKey(0x0C00'0000'0000'0007ULL), entity(7)), 42U);
}

CY_TEST_CASE("a filesystem archive round-trips an unloaded region") {
    // The same exit criterion over real files, so the atomic-write path and the directory layout
    // are exercised rather than assumed.
    cy::test::TempDir directory("save_archive");
    CY_REQUIRE(directory.valid());

    FilesystemBackend store;
    CY_REQUIRE(store.open(directory.path().c_str()).has_value());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(store).has_value());

    Overlay written(test_allocator());
    populate(written);
    CY_REQUIRE(archive.commit(written, identity()).has_value());

    // A fresh archive over a fresh backend: nothing is carried in memory between the two.
    FilesystemBackend reopened;
    CY_REQUIRE(reopened.open(directory.path().c_str()).has_value());
    SaveArchive second(test_allocator());
    CY_REQUIRE(second.open(reopened).has_value());

    Overlay read(test_allocator());
    LoadPolicy policy;
    LoadReport report;
    CY_REQUIRE(second.load(policy, read, report).has_value());
    CY_CHECK_EQ(read.region_count(), 3U);
    CY_CHECK_EQ(revives_of(read, kQuarry, entity(10)), 5U);
    CY_CHECK_EQ(read.find_entry(kSummit, entity(20))->kind, EntryKind::Tombstone);
}

CY_TEST_CASE("a conflict is decided on meaning, not on which file was written last") {
    // `save-and-persistence` — "Storage backends and the cloud boundary": conflict resolution
    // "SHALL use logical metadata ... and SHALL NOT be decided by file modification timestamps".
    //
    // Two real save directories, written in the order that makes the WRONG answer the tempting one:
    // the device that played further is written FIRST, so its files are the older ones on disk, and
    // the device that is behind on every marker is written SECOND and is therefore the newest thing
    // in the filesystem. A resolver that reached for a modification time would answer "keep local"
    // here, and the metadata says the opposite.
    cy::test::TempDir remote_dir("save_conflict_remote");
    cy::test::TempDir local_dir("save_conflict_local");
    CY_REQUIRE(remote_dir.valid());
    CY_REQUIRE(local_dir.valid());

    SaveIdentity ahead = identity();
    ahead.progress = 40;
    SaveIdentity behind = identity();
    behind.progress = 12;

    // The remote copy: played further, saved twice, and written to disk first.
    FilesystemBackend remote_store;
    CY_REQUIRE(remote_store.open(remote_dir.path().c_str()).has_value());
    SaveArchive remote_archive(test_allocator());
    CY_REQUIRE(remote_archive.open(remote_store).has_value());
    Overlay remote_state(test_allocator());
    populate(remote_state);
    remote_state.set_simulation_point(11000);
    CY_REQUIRE(remote_archive.commit(remote_state, ahead).has_value());
    CY_REQUIRE(remote_archive.commit(remote_state, ahead).has_value());

    // The local copy: an older point in the same campaign, and the newest bytes on this machine.
    FilesystemBackend local_store;
    CY_REQUIRE(local_store.open(local_dir.path().c_str()).has_value());
    SaveArchive local_archive(test_allocator());
    CY_REQUIRE(local_archive.open(local_store).has_value());
    Overlay local_state(test_allocator());
    populate(local_state);
    local_state.set_simulation_point(9000);
    CY_REQUIRE(local_archive.commit(local_state, behind).has_value());

    // The inversion this case rests on, asserted rather than assumed: the copy that must LOSE is
    // the one the filesystem calls newest.
    const std::filesystem::path local_pointer = std::filesystem::path(local_dir.path()) / "current";
    const std::filesystem::path remote_pointer =
        std::filesystem::path(remote_dir.path()) / "current";
    CY_REQUIRE(std::filesystem::exists(local_pointer));
    CY_REQUIRE(std::filesystem::exists(remote_pointer));
    CY_CHECK(std::filesystem::last_write_time(local_pointer) >=
             std::filesystem::last_write_time(remote_pointer));

    // A conflict decision reads manifests, and a manifest names no chunk it has to read.
    Manifest local_manifest(test_allocator());
    Manifest remote_manifest(test_allocator());
    LoadReport report;
    const Expected<u32, Error> local_generation = local_archive.active_generation();
    const Expected<u32, Error> remote_generation = remote_archive.active_generation();
    CY_REQUIRE(local_generation.has_value());
    CY_REQUIRE(remote_generation.has_value());
    CY_REQUIRE(local_archive.read_manifest(*local_generation, local_manifest, report).has_value());
    CY_REQUIRE(
        remote_archive.read_manifest(*remote_generation, remote_manifest, report).has_value());

    const ConflictResolution resolution = resolve_conflict(local_manifest, remote_manifest);
    CY_CHECK_EQ(resolution.outcome, ConflictOutcome::KeepRemote);
    CY_CHECK_EQ(remote_manifest.progress, 40ULL);
    CY_CHECK_EQ(local_manifest.progress, 12ULL);
}

CY_TEST_CASE("a refused load still names the build that wrote the save after the load returns") {
    MemoryBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(store).has_value());
    Overlay written(test_allocator());
    populate(written);
    CY_REQUIRE(archive.commit(written, identity()).has_value());

    LoadPolicy policy;
    policy.compatibility = Compatibility::ExactBuild;
    policy.build_id = "6.0.1+test";
    Overlay read(test_allocator());
    LoadReport report;
    CY_CHECK_FALSE(archive.load(policy, read, report).has_value());
    CY_CHECK_EQ(report.failure, LoadFailure::IncompatibleBuild);

    // Reuse the stack the load ran on before the report is read, as any caller's next call does.
    // Before `subject`, `detail` pointed into the manifest `load_generation` had declared as a
    // local, and this read printed whatever the next call had left there.
    Manifest unrelated(test_allocator());
    LoadReport scratch;
    CY_REQUIRE(archive.read_manifest(1, unrelated, scratch).has_value());
    CY_CHECK_EQ(std::string_view(report.subject), std::string_view("6.0.0+test"));
    CY_CHECK_EQ(std::string_view(report.detail),
                std::string_view("the save was written by a build this policy does not accept"));
}

CY_TEST_CASE("the committed inspector fixture is what its generator writes, byte for byte") {
    // src/save/tests/data/inspect-campaign/ is inspect_fixture.h's campaign, committed. Every
    // object in it must be the generator's and the generator must write nothing that is not in it —
    // so the fixture cannot drift from the table that describes it, and a change to the container's
    // encoding fails here rather than leaving a fixture that quietly means something else.
    const char* regenerate = std::getenv("CY_SAVE_WRITE_FIXTURE");
    if (regenerate != nullptr && regenerate[0] != '\0') {
        FilesystemBackend target;
        CY_REQUIRE(target.open(regenerate).has_value());
        CY_REQUIRE(write_fixture_campaign(target, test_allocator()).has_value());
    }
    MemoryBackend generated(test_allocator());
    CY_REQUIRE(write_fixture_campaign(generated, test_allocator()).has_value());
    FilesystemBackend committed;
    CY_REQUIRE(committed.open(CY_SAVE_FIXTURE_DIR "/inspect-campaign").has_value());

    struct Comparison {
        const SaveBackend* generated = nullptr;
        const SaveBackend* committed = nullptr;
        u32 objects = 0;
        u32 differing = 0;
    };
    const auto compare = [](void* user, std::string_view key) noexcept {
        auto& state = *static_cast<Comparison*>(user);
        ++state.objects;
        Array<u8> mine(test_allocator());
        Array<u8> theirs(test_allocator());
        const bool read = state.generated->read(key, mine).has_value() &&
                          state.committed->read(key, theirs).has_value();
        const bool same =
            read && mine.size() == theirs.size() &&
            (mine.empty() || std::memcmp(mine.data(), theirs.data(), mine.size()) == 0);
        state.differing += same ? 0U : 1U;
        return true;
    };
    Comparison check{&generated, &committed};
    CY_REQUIRE(committed.list("", compare, &check).has_value());
    CY_CHECK_EQ(check.differing, 0U);
    CY_CHECK_EQ(static_cast<usize>(check.objects), generated.object_count());
    CY_CHECK_GT(check.objects, 3U);
}

CY_TEST_CASE("a save is inspected from the committed fixture, as inspect_fixture.h describes it") {
    // The fixture on disk, read through the filesystem backend — the same bytes `cy_save_inspect`
    // is pointed at by m11a:save-inspector. Here rather than in unit.save because it reads a
    // directory, which costs a unit case its whole budget.
    FilesystemBackend committed;
    CY_REQUIRE(committed.open(CY_SAVE_FIXTURE_DIR "/inspect-campaign").has_value());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(committed).has_value());
    reflect::TypeRegistry types;
    CY_REQUIRE(fixture_types(types).has_value());
    InspectOptions options;
    options.types = &types;
    SaveInspection inspection(test_allocator());
    CY_REQUIRE(inspect_save(archive, 0, options, inspection).has_value());
    CY_CHECK_FALSE(inspection.restore.failed());
    CY_CHECK_EQ(inspection.generation, 3U);
    CY_CHECK_EQ(inspection.generations.size(), 3U);
    CY_CHECK_EQ(inspection.manifest.simulation_point, kFixtureTicks[2]);
    CY_CHECK_EQ(inspection.modified, 2U);
    CY_CHECK_EQ(inspection.created, 0U);
    CY_CHECK_EQ(inspection.tombstoned, 1U);
    CY_CHECK_EQ(inspection.fragments, 1U);
}
