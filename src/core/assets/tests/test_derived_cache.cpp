// The one derived-data cache, over its tiers. M5 task 5.1.
//
// The cases here are `asset-import-pipeline`'s own scenarios, in its words where they fit: a shared
// cache hit, deleting the cache losing nothing, and a changed dependency invalidating what it fed.
// The one that is not from the specification is the one that would otherwise be discovered in
// production — a record whose own key disagrees with the name it is filed under.
//
// Integration rather than unit: every case writes real files. The keys these artefacts are filed
// under are exercised in test_derivation.cpp, which touches nothing.

#include "temp_dir.h"

#include <cy/core/assets/derivation.h>
#include <cy/core/assets/derived_cache.h>
#include <cy/test/test.h>

#include <cstring>
#include <string>
#include <string_view>

using namespace cy::assets;
using cy::u32;
using cy::u8;

namespace {

DerivationKey key_of(const char* source_text, u32 version, const char* option) {
    DerivationKeyBuilder builder;
    builder.producer(DerivedKind::Import, "texture", version)
        .source("source", content_hash(source_text, std::strlen(source_text)))
        .text("option", option)
        .text("platform", "desktop")
        .text("profile", "client");
    auto key = builder.finish();
    CY_REQUIRE(key.has_value());
    return key.value();
}

cy::Array<u8> payload_of(std::string_view text) {
    cy::Array<u8> bytes;
    CY_REQUIRE(
        bytes.append(cy::Span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()))
            .has_value());
    return bytes;
}

/// A resolver over a fixed table, so a test can change one dependency and leave the rest alone.
struct Dependencies {
    struct Row {
        std::string name;
        std::string content;
        bool present = true;
    };
    cy::Array<Row> rows;

    static ContentHash digest(void* user, std::string_view name, bool* found) noexcept {
        auto* self = static_cast<Dependencies*>(user);
        for (const Row& row : self->rows) {
            if (row.name == name) {
                *found = row.present;
                return content_hash(row.content.data(), row.content.size());
            }
        }
        *found = false;
        return {};
    }
};

}  // namespace

CY_TEST_CASE("DerivedCache: a stored artefact comes back byte-identical") {
    test::TempDir directory("cache_roundtrip");
    const std::string local = directory.file("local");

    DerivedCache cache;
    DerivedCacheTiers tiers;
    tiers.local = local.c_str();
    CY_REQUIRE(cache.configure(tiers).has_value());

    const DerivationKey key = key_of("pixels", 1, "bc7");
    const cy::Array<u8> payload = payload_of("cooked bytes");

    DerivedArtefact artefact;
    artefact.kind = DerivedKind::Import;
    artefact.producer = "texture";
    artefact.payload = {payload.data(), payload.size()};
    CY_REQUIRE(cache.store(key, artefact).has_value());

    CacheResult result = cache.lookup(key, nullptr, nullptr);
    CY_REQUIRE(result.is_hit());
    CY_CHECK(result.tier == CacheTier::Local);
    CY_CHECK(result.entry.kind() == DerivedKind::Import);
    CY_CHECK(result.entry.producer() == "texture");
    CY_REQUIRE(result.entry.payload().size() == payload.size());
    CY_CHECK(std::memcmp(result.entry.payload().data(), payload.data(), payload.size()) == 0);
    CY_CHECK(cache.statistics().hits_local == 1);
}

CY_TEST_CASE("DerivedCache: an unknown key is a miss and not an error") {
    test::TempDir directory("cache_miss");
    const std::string local = directory.file("local");

    DerivedCache cache;
    DerivedCacheTiers tiers;
    tiers.local = local.c_str();
    CY_REQUIRE(cache.configure(tiers).has_value());

    CacheResult result = cache.lookup(key_of("nothing", 1, "bc7"), nullptr, nullptr);
    CY_CHECK(result.outcome == CacheOutcome::Miss);
    CY_CHECK(cache.statistics().misses == 1);
    CY_CHECK(std::string_view(result.reason).find("no tier") != std::string_view::npos);
}

CY_TEST_CASE("DerivedCache: a developer pulls a branch CI already cooked") {
    // `asset-import-pipeline`, "Shared cache hit": "WHEN a developer pulls a branch whose assets CI
    // already cooked THEN the cooked artefacts SHALL be fetched from the shared cache rather than
    // re-imported locally."
    test::TempDir directory("cache_shared");
    const std::string ci_local = directory.file("ci");
    const std::string developer_local = directory.file("developer");
    const std::string team = directory.file("shared");

    const DerivationKey key = key_of("pixels", 1, "bc7");
    const cy::Array<u8> payload = payload_of("what CI cooked");

    // Continuous integration cooks, and writes the tier it is allowed to write.
    {
        DerivedCache ci;
        DerivedCacheTiers tiers;
        tiers.local = ci_local.c_str();
        tiers.remote = team.c_str();
        tiers.write_remote = true;
        CY_REQUIRE(ci.configure(tiers).has_value());

        DerivedArtefact artefact;
        artefact.kind = DerivedKind::Import;
        artefact.producer = "texture";
        artefact.payload = {payload.data(), payload.size()};
        CY_REQUIRE(ci.store(key, artefact).has_value());
    }

    // The developer reads it, and may not write it.
    DerivedCache developer;
    DerivedCacheTiers tiers;
    tiers.local = developer_local.c_str();
    tiers.shared = team.c_str();
    CY_REQUIRE(developer.configure(tiers).has_value());

    CacheResult result = developer.lookup(key, nullptr, nullptr);
    CY_REQUIRE(result.is_hit());
    CY_CHECK(result.tier == CacheTier::Shared);
    CY_CHECK(std::memcmp(result.entry.payload().data(), payload.data(), payload.size()) == 0);
    CY_CHECK(developer.statistics().hits_shared == 1);
    CY_CHECK(developer.statistics().promotions == 1);

    // Promotion means the second read is local, which is what makes a cold machine warm itself.
    CacheResult again = developer.lookup(key, nullptr, nullptr);
    CY_REQUIRE(again.is_hit());
    CY_CHECK(again.tier == CacheTier::Local);
}

CY_TEST_CASE("DerivedCache: a changed dependency invalidates and names itself") {
    // "WHEN a shared shader include is edited THEN every shader that includes it SHALL be
    // re-cooked." The include is not in the key — nothing knew about it until the compile read it —
    // so this is the half the recorded dependency list answers.
    test::TempDir directory("cache_dependency");
    const std::string local = directory.file("local");

    DerivedCache cache;
    DerivedCacheTiers tiers;
    tiers.local = local.c_str();
    CY_REQUIRE(cache.configure(tiers).has_value());

    Dependencies table;
    CY_REQUIRE(
        table.rows.push_back({"shaders/common.slang", "float pi = 3.14;", true}).has_value());

    const DerivationKey key = key_of("shader source", 1, "spirv");
    const cy::Array<u8> payload = payload_of("compiled");

    DerivedDependency recorded;
    recorded.name = "shaders/common.slang";
    recorded.hash = content_hash(table.rows[0].content.data(), table.rows[0].content.size());

    DerivedArtefact artefact;
    artefact.kind = DerivedKind::Shader;
    artefact.producer = "slang";
    artefact.payload = {payload.data(), payload.size()};
    artefact.dependencies = {&recorded, 1};
    CY_REQUIRE(cache.store(key, artefact).has_value());

    CY_CHECK(cache.lookup(key, &Dependencies::digest, &table).is_hit());

    table.rows[0].content = "float pi = 3.14159;";
    CacheResult stale = cache.lookup(key, &Dependencies::digest, &table);
    CY_CHECK(stale.outcome == CacheOutcome::Invalidated);
    CY_CHECK(stale.stale == "shaders/common.slang");
    CY_CHECK(cache.statistics().invalidated == 1);
}

CY_TEST_CASE(
    "DerivedCache: a dependency that no longer exists invalidates for a different reason") {
    test::TempDir directory("cache_dependency_gone");
    const std::string local = directory.file("local");

    DerivedCache cache;
    DerivedCacheTiers tiers;
    tiers.local = local.c_str();
    CY_REQUIRE(cache.configure(tiers).has_value());

    Dependencies table;
    CY_REQUIRE(table.rows.push_back({"textures/stone.png", "pixels", true}).has_value());

    const DerivationKey key = key_of("material", 1, "std");
    const cy::Array<u8> payload = payload_of("cooked material");
    DerivedDependency recorded;
    recorded.name = "textures/stone.png";
    recorded.hash = content_hash(table.rows[0].content.data(), table.rows[0].content.size());

    DerivedArtefact artefact;
    artefact.kind = DerivedKind::Import;
    artefact.producer = "gltf";
    artefact.payload = {payload.data(), payload.size()};
    artefact.dependencies = {&recorded, 1};
    CY_REQUIRE(cache.store(key, artefact).has_value());

    table.rows[0].present = false;
    CacheResult gone = cache.lookup(key, &Dependencies::digest, &table);
    CY_CHECK(gone.outcome == CacheOutcome::Invalidated);
    CY_CHECK(std::string_view(gone.reason).find("no longer exists") != std::string_view::npos);
}

CY_TEST_CASE("DerivedCache: a record with dependencies and no resolver is not silently trusted") {
    test::TempDir directory("cache_no_resolver");
    const std::string local = directory.file("local");

    DerivedCache cache;
    DerivedCacheTiers tiers;
    tiers.local = local.c_str();
    CY_REQUIRE(cache.configure(tiers).has_value());

    const DerivationKey key = key_of("source", 1, "opt");
    const cy::Array<u8> payload = payload_of("cooked");
    DerivedDependency recorded;
    recorded.name = "an/include";
    recorded.hash = content_hash("contents", 8);

    DerivedArtefact artefact;
    artefact.kind = DerivedKind::Shader;
    artefact.producer = "slang";
    artefact.payload = {payload.data(), payload.size()};
    artefact.dependencies = {&recorded, 1};
    CY_REQUIRE(cache.store(key, artefact).has_value());

    CacheResult result = cache.lookup(key, nullptr, nullptr);
    CY_CHECK(result.outcome == CacheOutcome::Invalidated);
    CY_CHECK(std::string_view(result.reason).find("no resolver") != std::string_view::npos);
}

CY_TEST_CASE("DerivedCache: a corrupted payload is deleted rather than served") {
    test::TempDir directory("cache_corrupt");
    const std::string local = directory.file("local");

    DerivedCache cache;
    DerivedCacheTiers tiers;
    tiers.local = local.c_str();
    CY_REQUIRE(cache.configure(tiers).has_value());

    const DerivationKey key = key_of("source", 1, "opt");
    const cy::Array<u8> payload = payload_of("cooked bytes that will be damaged");
    DerivedArtefact artefact;
    artefact.kind = DerivedKind::Import;
    artefact.producer = "texture";
    artefact.payload = {payload.data(), payload.size()};
    CY_REQUIRE(cache.store(key, artefact).has_value());
    CY_REQUIRE(cache.contains(key));

    // Damage the last byte of the record, which is inside the payload.
    char text[DerivationKey::kTextLength + 1] = {};
    key.format(text);
    const std::string record_path = local + "/" + std::string(text, 2) + "/" + text + ".cyderive";
    cy::Array<u8> bytes;
    CY_REQUIRE(fs::read_whole(record_path.c_str(), bytes).has_value());
    bytes[bytes.size() - 1] = static_cast<u8>(bytes[bytes.size() - 1] ^ 0xFFU);
    CY_REQUIRE(fs::write_atomic(record_path.c_str(), bytes.data(), bytes.size()).has_value());

    CacheResult result = cache.lookup(key, nullptr, nullptr);
    CY_CHECK(result.outcome == CacheOutcome::Corrupt);
    CY_CHECK(cache.statistics().corrupt == 1);
    // Deleted, not reported as a build failure: the cache is disposable, so the honest response to
    // a record it cannot trust is to forget it and cook again.
    CY_CHECK(!cache.contains(key));
}

CY_TEST_CASE("DerivedCache: deleting the cache loses nothing") {
    // "WHEN the cache is deleted THEN the project SHALL remain complete and the next build SHALL
    // regenerate the derived data." Nothing here is a project file, so the assertion a test can
    // make is the other half: after `clear_local`, the same store reproduces the same entry.
    test::TempDir directory("cache_disposable");
    const std::string local = directory.file("local");

    DerivedCache cache;
    DerivedCacheTiers tiers;
    tiers.local = local.c_str();
    CY_REQUIRE(cache.configure(tiers).has_value());

    const DerivationKey key = key_of("source", 1, "opt");
    const cy::Array<u8> payload = payload_of("derived");
    DerivedArtefact artefact;
    artefact.kind = DerivedKind::Import;
    artefact.producer = "texture";
    artefact.payload = {payload.data(), payload.size()};

    CY_REQUIRE(cache.store(key, artefact).has_value());
    CY_REQUIRE(cache.contains(key));

    CY_REQUIRE(cache.clear_local().has_value());
    CY_CHECK(!cache.contains(key));
    CY_CHECK(cache.lookup(key, nullptr, nullptr).outcome == CacheOutcome::Miss);

    CY_REQUIRE(cache.store(key, artefact).has_value());
    CacheResult again = cache.lookup(key, nullptr, nullptr);
    CY_REQUIRE(again.is_hit());
    CY_CHECK(std::memcmp(again.entry.payload().data(), payload.data(), payload.size()) == 0);
}

CY_TEST_CASE("DerivedCache: clearing the local tier never touches the shared one") {
    test::TempDir directory("cache_clear_scope");
    const std::string local = directory.file("local");
    const std::string shared = directory.file("shared");

    const DerivationKey key = key_of("source", 1, "opt");
    const cy::Array<u8> payload = payload_of("what the team cooked");

    {
        DerivedCache writer;
        DerivedCacheTiers tiers;
        tiers.local = local.c_str();
        tiers.remote = shared.c_str();
        tiers.write_remote = true;
        CY_REQUIRE(writer.configure(tiers).has_value());
        DerivedArtefact artefact;
        artefact.kind = DerivedKind::Import;
        artefact.producer = "texture";
        artefact.payload = {payload.data(), payload.size()};
        CY_REQUIRE(writer.store(key, artefact).has_value());
        CY_REQUIRE(writer.clear_local().has_value());
    }

    const std::string second = directory.file("second");
    DerivedCache reader;
    DerivedCacheTiers tiers;
    tiers.local = second.c_str();
    tiers.shared = shared.c_str();
    CY_REQUIRE(reader.configure(tiers).has_value());
    CY_CHECK(reader.lookup(key, nullptr, nullptr).is_hit());
}

CY_TEST_CASE("DerivedCache: a disabled cache misses everything and stores nothing") {
    // The clean-build configuration. It is a supported one rather than an accident, so it is here
    // rather than left to be discovered by a gate that wanted to prove a cook works from cold.
    DerivedCache cache;
    DerivedCacheTiers tiers;
    CY_REQUIRE(cache.configure(tiers).has_value());
    CY_CHECK(!cache.is_enabled());

    const DerivationKey key = key_of("source", 1, "opt");
    const cy::Array<u8> payload = payload_of("derived");
    DerivedArtefact artefact;
    artefact.kind = DerivedKind::Import;
    artefact.producer = "texture";
    artefact.payload = {payload.data(), payload.size()};

    CY_CHECK(cache.store(key, artefact).has_value());
    CY_CHECK(cache.lookup(key, nullptr, nullptr).outcome == CacheOutcome::Miss);
    CY_CHECK(cache.statistics().stores == 0);
}

CY_TEST_CASE("DerivedCache: a record filed under the wrong key is refused") {
    // The one corruption content addressing cannot shrug off. A record whose own key disagrees with
    // its file name would serve one computation's artefact for another's, which is exactly what the
    // addressing exists to make impossible — so it is a refusal rather than a hit.
    test::TempDir directory("cache_wrong_key");
    const std::string local = directory.file("local");

    DerivedCache cache;
    DerivedCacheTiers tiers;
    tiers.local = local.c_str();
    CY_REQUIRE(cache.configure(tiers).has_value());

    const DerivationKey mine = key_of("source", 1, "opt");
    const DerivationKey other = key_of("source", 2, "opt");
    const cy::Array<u8> payload = payload_of("derived");
    DerivedArtefact artefact;
    artefact.kind = DerivedKind::Import;
    artefact.producer = "texture";
    artefact.payload = {payload.data(), payload.size()};
    CY_REQUIRE(cache.store(other, artefact).has_value());

    // Move the record to the other key's name, which is what a mis-shared cache directory would do.
    char mine_text[DerivationKey::kTextLength + 1] = {};
    char other_text[DerivationKey::kTextLength + 1] = {};
    mine.format(mine_text);
    other.format(other_text);
    const std::string from =
        local + "/" + std::string(other_text, 2) + "/" + other_text + ".cyderive";
    const std::string shard = local + "/" + std::string(mine_text, 2);
    CY_REQUIRE(fs::create_directories(shard.c_str()).has_value());
    const std::string to = shard + "/" + mine_text + ".cyderive";
    CY_REQUIRE(fs::move_file(from.c_str(), to.c_str()).has_value());

    CacheResult result = cache.lookup(mine, nullptr, nullptr);
    CY_CHECK(result.outcome == CacheOutcome::Corrupt);
}
