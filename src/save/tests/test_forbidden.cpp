// SPDX-License-Identifier: MIT
// The runtime half of the ten forbidden save patterns. `close-save-system-gaps`.
//
// `save-and-persistence` — "Forbidden save patterns": "The following SHALL NOT appear, and each
// SHALL be checkable". Where a pattern is a shape of code, tools/save/check_forbidden.py reads for
// it. Where it is a property of what a save DOES — which bytes a commit writes, what a failed load
// leaves behind — only running the save path can see it, and that is this file.
//
// Every case is named `forbidden save pattern <id>: ...` with the id check_forbidden.py uses, and
// the checker runs this suite once per id and fails when the filter selects nothing. So a renamed
// case is a failing check rather than a pattern that quietly stopped being checked.

#include <cy/save/archive.h>
#include <cy/save/container.h>
#include <cy/save/inspect.h>
#include <cy/save/storage.h>
#include <cy/test/test.h>

#include "fixtures.h"

#include <cstdio>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <utility>

using namespace cy;
using namespace cy::save;
using namespace cy::save::test;

namespace {

constexpr RegionKey kVillage{0x0B00'0000'0000'0001ULL};
constexpr RegionKey kQuarry{0x0B00'0000'0000'0002ULL};

SaveIdentity identity() noexcept {
    SaveIdentity id;
    id.build_id = "11.5.0+forbidden";
    id.project = AssetId(0xF0, 1);
    id.save = AssetId(0xF0, 2);
    id.campaign = AssetId(0xF0, 3);
    return id;
}

/// A Health whose every field holds a value no other field or header could hold by accident.
Health distinctive_health() noexcept {
    Health health;
    health.maximum = 1234.5F;
    health.current = 77.25F;
    health.revives = 0x5AFE'0001U;
    health.fraction = 0.123F;
    health.debug_counter = 0xDEAD'BEEF'CAFE'F00DULL;
    return health;
}

bool contains_bytes(Span<const u8> haystack, const void* needle, usize size) noexcept {
    if (size == 0 || haystack.size() < size) {
        return false;
    }
    for (usize at = 0; at + size <= haystack.size(); ++at) {
        if (std::memcmp(haystack.data() + at, needle, size) == 0) {
            return true;
        }
    }
    return false;
}

void populate(Overlay& overlay) {
    const Health health = distinctive_health();
    CY_REQUIRE(
        overlay.record_component(kVillage, entity(1), health_type(), &health, 1).has_value());
    Structure wall;
    wall.material = 3;
    wall.integrity = 90;
    CY_REQUIRE(
        overlay.record_component(kQuarry, entity(2), structure_type(), &wall, 1).has_value());
    CY_REQUIRE(overlay.destroy_entity(kQuarry, entity(3)).has_value());
    overlay.set_simulation_point(500);
}

/// A backend that records whether each write replaced an object, and what that object held.
class RecordingBackend final : public SaveBackend {
public:
    explicit RecordingBackend(Allocator& allocator) noexcept : inner_(allocator) {}

    [[nodiscard]] const char* name() const noexcept override { return "recording"; }
    [[nodiscard]] Status write(std::string_view key, Span<const u8> bytes) noexcept override {
        ++writes;
        if (inner_.exists(key)) {
            Array<u8> previous(test_allocator());
            const bool same =
                inner_.read(key, previous).has_value() && previous.size() == bytes.size() &&
                (bytes.empty() || std::memcmp(previous.data(), bytes.data(), bytes.size()) == 0);
            if (key != "current" && !same) {
                ++destructive_rewrites;
            }
        }
        return inner_.write(key, bytes);
    }
    [[nodiscard]] Status read(std::string_view key, Array<u8>& out) const noexcept override {
        return inner_.read(key, out);
    }
    [[nodiscard]] bool exists(std::string_view key) const noexcept override {
        return inner_.exists(key);
    }
    [[nodiscard]] Status remove(std::string_view key) noexcept override {
        return inner_.remove(key);
    }
    [[nodiscard]] Status list(std::string_view prefix, KeyVisitor visitor,
                              void* user) const noexcept override {
        return inner_.list(prefix, visitor, user);
    }

    u32 writes = 0;
    /// Writes that replaced an object with different bytes — anything but the one pointer whose
    /// atomic replacement IS the commit.
    u32 destructive_rewrites = 0;

private:
    MemoryBackend inner_;
};

void chunk_key(const assets::ContentHash& hash, char (&key)[128]) noexcept {
    char name[assets::ContentHash::kTextLength + 1] = {};
    hash.format(name);
    (void)std::snprintf(key, sizeof(key), "chunks/%s.cychunk", name);
}

}  // namespace

CY_TEST_CASE(
    "forbidden save pattern raw-memory: a component reaches a save as fields, never bytes") {
    Overlay overlay(test_allocator());
    populate(overlay);
    Array<u8> chunk(test_allocator());
    CY_REQUIRE(encode_region(overlay, kVillage, chunk).has_value());

    // A memory copy of the component would put its image — or any run of its native layout — into
    // the chunk. The persistent field is there as a value; the object is not.
    const Health health = distinctive_health();
    CY_CHECK_FALSE(contains_bytes(chunk.span(), &health, sizeof(health)));
    CY_CHECK_FALSE(contains_bytes(chunk.span(), &health.maximum, sizeof(health.maximum)));
    CY_CHECK_FALSE(contains_bytes(chunk.span(), &health.current, sizeof(health.current)));
    const ComponentDelta* delta = overlay.find_component(kVillage, entity(1), health_type().id);
    CY_REQUIRE(delta != nullptr);
    CY_CHECK_EQ(delta->record.size(), 1U);
    CY_CHECK(delta->record.contains(reflect::FieldId(kHealthRevives)));
}

CY_TEST_CASE(
    "forbidden save pattern derived-saved: derived and transient fields never reach a save") {
    MemoryBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(store).has_value());
    Overlay overlay(test_allocator());
    populate(overlay);
    CY_REQUIRE(archive.commit(overlay, identity()).has_value());

    Manifest manifest(test_allocator());
    LoadReport report;
    CY_REQUIRE(archive.read_manifest(1, manifest, report).has_value());
    const Health health = distinctive_health();
    for (const ChunkRef& chunk : manifest.chunks) {
        char key[128] = {};
        chunk_key(chunk.hash, key);
        Array<u8> bytes(test_allocator());
        CY_REQUIRE(store.read(key, bytes).has_value());
        CY_CHECK_FALSE(contains_bytes(bytes.span(), &health.fraction, sizeof(health.fraction)));
        CY_CHECK_FALSE(
            contains_bytes(bytes.span(), &health.debug_counter, sizeof(health.debug_counter)));
    }
    Overlay loaded(test_allocator());
    LoadReport loaded_report;
    CY_REQUIRE(archive.load(LoadPolicy(), loaded, loaded_report).has_value());
    const ComponentDelta* delta = loaded.find_component(kVillage, entity(1), health_type().id);
    CY_REQUIRE(delta != nullptr);
    CY_CHECK_FALSE(delta->record.contains(reflect::FieldId(kHealthFraction)));
    CY_CHECK_FALSE(delta->record.contains(reflect::FieldId(kHealthDebugCounter)));
}

CY_TEST_CASE("forbidden save pattern whole-world-resident: nothing resident, the same save") {
    MemoryBackend unloaded_store(test_allocator());
    MemoryBackend resident_store(test_allocator());
    SaveArchive unloaded_archive(test_allocator());
    SaveArchive resident_archive(test_allocator());
    CY_REQUIRE(unloaded_archive.open(unloaded_store).has_value());
    CY_REQUIRE(resident_archive.open(resident_store).has_value());

    Overlay unloaded(test_allocator());
    populate(unloaded);
    Overlay resident(test_allocator());
    populate(resident);
    CY_REQUIRE(resident.set_residency(kVillage, Residency::Resident).has_value());
    CY_REQUIRE(resident.set_residency(kQuarry, Residency::Resident).has_value());
    CY_REQUIRE_EQ(unloaded.resident_region_count(), 0U);

    CY_REQUIRE(unloaded_archive.commit(unloaded, identity()).has_value());
    CY_REQUIRE(resident_archive.commit(resident, identity()).has_value());
    Manifest a(test_allocator());
    Manifest b(test_allocator());
    LoadReport report;
    CY_REQUIRE(unloaded_archive.read_manifest(1, a, report).has_value());
    CY_REQUIRE(resident_archive.read_manifest(1, b, report).has_value());
    CY_REQUIRE_EQ(a.chunks.size(), b.chunks.size());
    for (usize index = 0; index < a.chunks.size(); ++index) {
        CY_CHECK(a.chunks[index].hash == b.chunks[index].hash);
    }
}

CY_TEST_CASE(
    "forbidden save pattern destructive-in-place: a commit replaces nothing but the pointer") {
    RecordingBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(store).has_value());
    Overlay overlay(test_allocator());
    populate(overlay);
    CY_REQUIRE(archive.commit(overlay, identity()).has_value());
    Structure rebuilt;
    rebuilt.material = 3;
    rebuilt.integrity = 20;
    CY_REQUIRE(
        overlay.record_component(kQuarry, entity(2), structure_type(), &rebuilt, 1).has_value());
    CY_REQUIRE(archive.append_journal(overlay).has_value());
    CY_REQUIRE(archive.compact(identity()).has_value());
    CY_REQUIRE(archive.commit(overlay, identity()).has_value());

    CY_CHECK_GT(store.writes, 6U);
    CY_CHECK_EQ(store.destructive_rewrites, 0U);
}

CY_TEST_CASE(
    "forbidden save pattern monolithic-blob: profile, campaign and session are separate "
    "chunks") {
    MemoryBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(store).has_value());
    Overlay overlay(test_allocator());
    populate(overlay);
    Settings settings;
    settings.gamma = 2.2F;
    for (const Scope scope : {Scope::Profile, Scope::Campaign, Scope::Session}) {
        CY_REQUIRE(overlay.record_fragment(scope, settings_type(), &settings, 1).has_value());
    }
    CY_REQUIRE(archive.commit(overlay, identity()).has_value());

    Manifest manifest(test_allocator());
    LoadReport report;
    CY_REQUIRE(archive.read_manifest(1, manifest, report).has_value());
    for (const Scope scope : {Scope::Profile, Scope::Campaign, Scope::Session}) {
        const ChunkRef* chunk = manifest.find_chunk(scope, kGlobalRegion);
        CY_REQUIRE(chunk != nullptr);
        // Each scope's chunk decodes on its own into that scope's state and nothing else — which
        // is what lets a profile be read without a campaign, and a campaign be deleted without it.
        char key[128] = {};
        chunk_key(chunk->hash, key);
        Array<u8> bytes(test_allocator());
        CY_REQUIRE(store.read(key, bytes).has_value());
        Overlay alone(test_allocator());
        LoadReport decoded;
        CY_REQUIRE(decode_chunk(bytes.span(), LoadPolicy(), alone, decoded).has_value());
        CY_CHECK_EQ(alone.fragments().size(), 1U);
        CY_CHECK_EQ(alone.fragments()[0].scope, scope);
        CY_CHECK_EQ(alone.entry_count(), 0U);
    }
}

CY_TEST_CASE(
    "forbidden save pattern cooked-migration: other cooked content is refused, not "
    "migrated") {
    MemoryBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(store).has_value());
    Overlay overlay(test_allocator());
    populate(overlay);
    SaveIdentity id = identity();
    id.content_version = assets::content_hash("cooked-a", 8);
    CY_REQUIRE(archive.commit(overlay, id).has_value());
    const u64 bytes_before = store.total_bytes();
    const usize objects_before = store.object_count();

    LoadPolicy policy;
    policy.content_version = assets::content_hash("cooked-b", 8);
    Overlay loaded(test_allocator());
    LoadReport report;
    CY_CHECK_FALSE(archive.load(policy, loaded, report).has_value());
    CY_CHECK_EQ(report.failure, LoadFailure::MissingContent);
    CY_CHECK_EQ(loaded.entry_count(), 0U);
    // Nothing was rewritten to make the save fit: the store is exactly as the commit left it.
    CY_CHECK_EQ(store.total_bytes(), bytes_before);
    CY_CHECK_EQ(store.object_count(), objects_before);
}

CY_TEST_CASE(
    "forbidden save pattern bespoke-crypto: integrity is the content hash, and it names the "
    "chunk") {
    MemoryBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(store).has_value());
    Overlay overlay(test_allocator());
    populate(overlay);
    CY_REQUIRE(archive.commit(overlay, identity()).has_value());
    Manifest manifest(test_allocator());
    LoadReport ignored;
    CY_REQUIRE(archive.read_manifest(1, manifest, ignored).has_value());
    const ChunkRef* quarry = manifest.find_chunk(Scope::World, kQuarry);
    CY_REQUIRE(quarry != nullptr);
    char key[128] = {};
    chunk_key(quarry->hash, key);
    CY_REQUIRE(store.corrupt(key).has_value());

    Overlay loaded(test_allocator());
    LoadReport report;
    CY_CHECK_FALSE(archive.load(LoadPolicy(), loaded, report).has_value());
    CY_CHECK_EQ(report.failure, LoadFailure::CorruptChunk);
    CY_CHECK(report.chunk == quarry->hash);
}

CY_TEST_CASE(
    "forbidden save pattern boolean-load: every failed load is a Status and a named reason") {
    static_assert(std::is_same_v<decltype(std::declval<SaveArchive&>().load(
                                     std::declval<const LoadPolicy&>(), std::declval<Overlay&>(),
                                     std::declval<LoadReport&>())),
                                 Status>);
    static_assert(
        std::is_same_v<decltype(decode_chunk(
                           std::declval<Span<const u8>>(), std::declval<const LoadPolicy&>(),
                           std::declval<Overlay&>(), std::declval<LoadReport&>())),
                       Status>);

    // Four different failures, four different named reasons.
    struct Case {
        LoadPolicy policy;
        bool corrupt = false;
        LoadFailure expected = LoadFailure::None;
    };
    Case cases[3];
    cases[0].policy.compatibility = Compatibility::ExactBuild;
    cases[0].policy.build_id = "12.0.0";
    cases[0].expected = LoadFailure::IncompatibleBuild;
    cases[1].policy.content_version = assets::content_hash("other", 5);
    cases[1].expected = LoadFailure::MissingContent;
    cases[2].corrupt = true;
    cases[2].expected = LoadFailure::CorruptChunk;
    for (Case& each : cases) {
        MemoryBackend store(test_allocator());
        SaveArchive archive(test_allocator());
        CY_REQUIRE(archive.open(store).has_value());
        Overlay overlay(test_allocator());
        populate(overlay);
        SaveIdentity id = identity();
        id.content_version = assets::content_hash("mine", 4);
        CY_REQUIRE(archive.commit(overlay, id).has_value());
        if (each.corrupt) {
            Manifest manifest(test_allocator());
            LoadReport ignored;
            CY_REQUIRE(archive.read_manifest(1, manifest, ignored).has_value());
            char key[128] = {};
            chunk_key(manifest.chunks[0].hash, key);
            CY_REQUIRE(store.corrupt(key).has_value());
        }
        Overlay loaded(test_allocator());
        LoadReport report;
        const Status status = archive.load(each.policy, loaded, report);
        CY_CHECK_FALSE(status.has_value());
        CY_CHECK_EQ(report.failure, each.expected);
        CY_CHECK(report.detail != nullptr);
        CY_CHECK_GT(std::strlen(report.detail), 0U);
    }
    MemoryBackend empty(test_allocator());
    SaveArchive nothing(test_allocator());
    CY_REQUIRE(nothing.open(empty).has_value());
    Overlay loaded(test_allocator());
    LoadReport report;
    const Status status = nothing.load(LoadPolicy(), loaded, report);
    CY_CHECK_FALSE(status.has_value());
    CY_CHECK_EQ(status.error().code, ErrorCode::NotFound);
}

CY_TEST_CASE("forbidden save pattern invented-state: an unrecoverable load leaves nothing behind") {
    MemoryBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(store).has_value());
    Overlay overlay(test_allocator());
    populate(overlay);
    CY_REQUIRE(archive.commit(overlay, identity()).has_value());

    // The LAST chunk of the only generation is the damaged one, so a loader that applied chunks as
    // it went has already applied the others when it meets the damage.
    Manifest manifest(test_allocator());
    LoadReport ignored;
    CY_REQUIRE(archive.read_manifest(1, manifest, ignored).has_value());
    char key[128] = {};
    chunk_key(manifest.chunks.back().hash, key);
    CY_REQUIRE(store.corrupt(key).has_value());

    Overlay loaded(test_allocator());
    LoadReport report;
    CY_CHECK_FALSE(archive.load(LoadPolicy(), loaded, report).has_value());
    CY_CHECK_EQ(report.failure, LoadFailure::CorruptChunk);
    CY_CHECK_EQ(loaded.region_count(), 0U);
    CY_CHECK_EQ(loaded.entry_count(), 0U);
    CY_CHECK_EQ(loaded.fragments().size(), 0U);
    CY_CHECK_EQ(loaded.simulation_point(), 0U);
}

CY_TEST_CASE("forbidden save pattern invented-state: a fallback restores the older save exactly") {
    MemoryBackend store(test_allocator());
    SaveArchive archive(test_allocator());
    CY_REQUIRE(archive.open(store).has_value());
    Overlay first(test_allocator());
    populate(first);
    CY_REQUIRE(archive.commit(first, identity()).has_value());
    Overlay second(test_allocator());
    populate(second);
    Structure rebuilt;
    rebuilt.material = 9;
    rebuilt.integrity = 9;
    CY_REQUIRE(
        second.record_component(kQuarry, entity(2), structure_type(), &rebuilt, 1).has_value());
    second.set_simulation_point(600);
    CY_REQUIRE(archive.commit(second, identity()).has_value());

    Manifest newest(test_allocator());
    LoadReport ignored;
    CY_REQUIRE(archive.read_manifest(2, newest, ignored).has_value());
    const ChunkRef* quarry = newest.find_chunk(Scope::World, kQuarry);
    CY_REQUIRE(quarry != nullptr);
    char key[128] = {};
    chunk_key(quarry->hash, key);
    CY_REQUIRE(store.corrupt(key).has_value());

    Overlay loaded(test_allocator());
    LoadReport report;
    CY_REQUIRE(archive.load(LoadPolicy(), loaded, report).has_value());
    CY_CHECK_EQ(report.generations_skipped, 1U);
    // Exactly generation 1: no field of generation 2 survived and none was filled in.
    SaveDiff diff(test_allocator());
    CY_REQUIRE(diff_overlays(first, loaded, diff).has_value());
    CY_CHECK(diff.empty());
    CY_CHECK_EQ(loaded.simulation_point(), 500U);
}
