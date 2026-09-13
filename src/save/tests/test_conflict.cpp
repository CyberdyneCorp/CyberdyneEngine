// Deciding between a local and a remote copy of one save. M10 task 6.2.
//
// `save-and-persistence` — "Storage backends and the cloud boundary": "Conflict resolution between
// local and remote saves SHALL use logical metadata — generation, campaign identity, simulation
// point, progress markers, content version — and SHALL NOT be decided by file modification
// timestamps", with the scenario "a conflict is decided on meaning".
//
// The half of that claim a pure comparison can carry is here; the half that needs two real save
// directories, one of them written later than the other and behind it in every marker, is
// `test_archive.cpp`'s "a conflict is decided on meaning, not on which file was written last".

#include <cy/save/conflict.h>
#include <cy/save/container.h>
#include <cy/test/test.h>

#include "fixtures.h"

#include <string_view>

using namespace cy;
using namespace cy::save;
using namespace cy::save::test;

namespace {

/// One copy of one campaign, at the point the three ordered markers say.
SaveSummary copy_at(u32 generation, u64 simulation_point, u64 progress) noexcept {
    SaveSummary summary;
    summary.project = AssetId(1, 1);
    summary.save = AssetId(2, 2);
    summary.campaign = AssetId(3, 3);
    summary.generation = generation;
    summary.simulation_point = simulation_point;
    summary.progress = progress;
    return summary;
}

assets::ContentHash content(u8 fill) noexcept {
    assets::ContentHash hash;
    for (u8& byte : hash.bytes) {
        byte = fill;
    }
    return hash;
}

bool reason_is(const ConflictResolution& resolution, std::string_view expected) noexcept {
    return std::string_view(resolution.reason) == expected;
}

}  // namespace

CY_TEST_CASE("two copies of one save at the same point are not a conflict") {
    const ConflictResolution resolution =
        resolve_conflict(copy_at(4, 9000, 12), copy_at(4, 9000, 12));
    CY_CHECK_EQ(resolution.outcome, ConflictOutcome::Identical);
    CY_CHECK_FALSE(resolution.content_version_differs);
}

CY_TEST_CASE("the copy ahead on every marker is the one to keep") {
    // The ordinary cloud case: one device played on, the other has the save it left behind.
    const SaveSummary behind = copy_at(4, 9000, 12);
    const SaveSummary ahead = copy_at(6, 11000, 14);

    const ConflictResolution local_ahead = resolve_conflict(ahead, behind);
    CY_CHECK_EQ(local_ahead.outcome, ConflictOutcome::KeepLocal);
    CY_CHECK(reason_is(local_ahead, "further progress, and behind on nothing"));

    const ConflictResolution remote_ahead = resolve_conflict(behind, ahead);
    CY_CHECK_EQ(remote_ahead.outcome, ConflictOutcome::KeepRemote);
    CY_CHECK(reason_is(remote_ahead, "further progress, and behind on nothing"));
}

CY_TEST_CASE("a lead in generations alone is reported as the bookkeeping it is") {
    // Same play, saved more often on one device. It is still the copy to keep — it is behind on
    // nothing — but the reason must not read like progress, because a player prompt built on it
    // would then claim the other device lost ground it never had.
    const ConflictResolution resolution =
        resolve_conflict(copy_at(9, 9000, 12), copy_at(4, 9000, 12));
    CY_CHECK_EQ(resolution.outcome, ConflictOutcome::KeepLocal);
    CY_CHECK(reason_is(resolution, "more generations, and behind on nothing"));
}

CY_TEST_CASE("the simulation point decides when only the generation agrees") {
    const ConflictResolution resolution =
        resolve_conflict(copy_at(4, 9000, 12), copy_at(4, 11000, 12));
    CY_CHECK_EQ(resolution.outcome, ConflictOutcome::KeepRemote);
    CY_CHECK(reason_is(resolution, "a later simulation point, and behind on nothing"));
}

CY_TEST_CASE("one campaign played twice is a divergence, not a decision") {
    // This device saved more often; that device played further. Neither is the answer, and the
    // engine "SHALL NEVER invent authoritative state" — so it reports and the game asks.
    const ConflictResolution resolution =
        resolve_conflict(copy_at(9, 9000, 12), copy_at(4, 11000, 15));
    CY_CHECK_EQ(resolution.outcome, ConflictOutcome::Divergent);
    CY_CHECK_EQ(std::string_view(conflict_outcome_name(resolution.outcome)), "divergent");
}

CY_TEST_CASE("a different campaign is not a conflict at all") {
    SaveSummary other = copy_at(1, 10, 0);
    other.campaign = AssetId(99, 99);
    const ConflictResolution resolution = resolve_conflict(copy_at(9, 9000, 12), other);
    CY_CHECK_EQ(resolution.outcome, ConflictOutcome::Unrelated);
    CY_CHECK(reason_is(resolution, "a different project, save slot or campaign"));
}

CY_TEST_CASE("a differing content version is reported and decides nothing") {
    // A save whose cooked content is not installed is refused by the LOAD, with
    // LoadFailure::MissingContent. Preferring the other copy here would be that policy written a
    // second time, in a place that cannot see what is installed.
    SaveSummary local = copy_at(6, 11000, 14);
    local.content_version = content(0xAB);
    SaveSummary remote = copy_at(4, 9000, 12);
    remote.content_version = content(0xCD);

    const ConflictResolution resolution = resolve_conflict(local, remote);
    CY_CHECK_EQ(resolution.outcome, ConflictOutcome::KeepLocal);
    CY_CHECK(resolution.content_version_differs);

    // And the same content version on two copies is not a difference.
    remote.content_version = content(0xAB);
    CY_CHECK_FALSE(resolve_conflict(local, remote).content_version_differs);
}

CY_TEST_CASE("a save's summary is the manifest's own metadata and nothing else") {
    Manifest manifest(test_allocator());
    manifest.project = AssetId(1, 1);
    manifest.save = AssetId(2, 2);
    manifest.campaign = AssetId(3, 3);
    manifest.generation = 7;
    manifest.simulation_point = 12345;
    manifest.progress = 42;
    manifest.content_version = content(0x11);

    const SaveSummary summary = summarise(manifest);
    CY_CHECK_EQ(summary.generation, 7U);
    CY_CHECK_EQ(summary.simulation_point, 12345ULL);
    CY_CHECK_EQ(summary.progress, 42ULL);
    CY_CHECK(summary.content_version == manifest.content_version);
    CY_CHECK(summary.campaign == manifest.campaign);
}

CY_TEST_CASE("the progress marker travels in the manifest, and costs nothing when there is none") {
    Manifest written(test_allocator());
    written.project = AssetId(1, 1);
    written.save = AssetId(2, 2);
    written.campaign = AssetId(3, 3);
    written.generation = 3;
    written.simulation_point = 500;
    written.progress = 77;

    Array<u8> with_progress(test_allocator());
    CY_REQUIRE(encode_manifest(written, with_progress).has_value());

    Manifest read(test_allocator());
    LoadReport report;
    CY_REQUIRE(
        decode_manifest(Span<const u8>(with_progress.data(), with_progress.size()), read, report)
            .has_value());
    CY_CHECK_EQ(read.progress, 77ULL);

    // A project that declares no progress writes no field: the bytes are the bytes a build from
    // before M10 wrote, and such a save reads back as zero rather than as a missing required field.
    written.progress = 0;
    Array<u8> without_progress(test_allocator());
    CY_REQUIRE(encode_manifest(written, without_progress).has_value());
    CY_CHECK_LT(without_progress.size(), with_progress.size());

    Manifest older(test_allocator());
    CY_REQUIRE(decode_manifest(Span<const u8>(without_progress.data(), without_progress.size()),
                               older, report)
                   .has_value());
    CY_CHECK_EQ(older.progress, 0ULL);
    CY_CHECK_EQ(older.simulation_point, 500ULL);
}
