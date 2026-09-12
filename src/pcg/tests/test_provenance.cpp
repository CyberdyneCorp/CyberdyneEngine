// Provenance: why is this here, and why is nothing here. M10 tasks.md 4.3.
//
// `procedural-content-generation` — "Provenance": "The editor SHALL answer WHY IS THIS HERE for any
// generated instance, naming the rule and the values that selected it. The editor SHALL ALSO ANSWER
// WHY IS NOTHING HERE for a location: which filter rejected it, with the value tested and the
// threshold applied."
//
// "Why is this here" can be reconstructed from the output — the instance exists, so its slot,
// region and node all follow from its identity. "Why is nothing here" cannot: a rejected candidate
// leaves no trace at all. Everything in this file is about the second one.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL: `why_nothing_here()`'s tie-break on `slot` was
// removed, leaving the nearest-wins comparison alone, and "two rejections at one distance give one
// answer" went red — the answer became whichever rejection happened to be recorded first, which
// would make the editor's explanation of an empty patch change after a regeneration that changed
// nothing. The tie-break was restored. Reported in this milestone's `verified_failing`.

#include <cy/test/test.h>

#include <cy/pcg/diagnostics.h>

#include "fixtures.h"

using cy::pcg::AttributeId;
using cy::pcg::GeneratedId;
using cy::pcg::GenerationProfile;
using cy::pcg::ProvenanceRecord;
using cy::pcg::RegionCoord;
using cy::pcg::RegionProvenance;
using cy::pcg::RejectionRecord;
using cy::pcg::StageProfile;
namespace test = cy::pcg::test;

CY_TEST_CASE("why is this here names the generator, the region, the seed and the deciding value") {
    RegionProvenance provenance(test::allocator());
    ProvenanceRecord record;
    record.identity = GeneratedId{0x1234};
    record.region = RegionCoord{2, 3, 0};
    record.seed = 99;
    record.generator_version = 4;
    record.stage = 7;
    record.stage_name = test::kScatterNode;
    record.generator_name = "forest";
    record.slot = 11;
    record.deciding_attribute = AttributeId{2};
    record.deciding_value = 0.42F;
    CY_REQUIRE(provenance.record_accepted(record));

    const ProvenanceRecord* found = provenance.why_here(GeneratedId{0x1234});
    CY_REQUIRE(found != nullptr);
    // Every field the requirement lists: "the generator, its version, the region, the seed, the
    // graph node that produced the output, and the attribute values that determined it".
    CY_CHECK(test::same_text(found->generator_name, "forest"));
    CY_CHECK_EQ(found->generator_version, 4u);
    CY_CHECK(found->region == RegionCoord{2, 3, 0});
    CY_CHECK_EQ(found->seed, 99u);
    CY_CHECK(test::same_text(found->stage_name, test::kScatterNode));
    CY_CHECK_EQ(found->slot, 11u);
    CY_CHECK_NEAR(found->deciding_value, 0.42F, 1e-6F);

    CY_CHECK(provenance.why_here(GeneratedId{0x9999}) == nullptr);
}

CY_TEST_CASE(
    "why is nothing here names the filter, the value tested and by how much it fell short") {
    RegionProvenance provenance(test::allocator());
    RejectionRecord rejected;
    rejected.region = RegionCoord{2, 3, 0};
    rejected.slot = 5;
    rejected.stage = 6;
    rejected.stage_name = test::kFilterNode;
    rejected.tested_attribute = AttributeId{2};
    rejected.tested_value = 0.04F;
    rejected.threshold = 0.08F;
    rejected.margin = 0.04F - 0.08F;
    rejected.position_x = 10.0F;
    rejected.position_z = 20.0F;
    CY_REQUIRE(provenance.record_rejected(rejected));

    const RejectionRecord* found = provenance.why_nothing_here(10.5F, 20.5F, 3.0F);
    CY_REQUIRE(found != nullptr);
    CY_CHECK(test::same_text(found->stage_name, test::kFilterNode));
    CY_CHECK_NEAR(found->tested_value, 0.04F, 1e-6F);
    CY_CHECK_NEAR(found->threshold, 0.08F, 1e-6F);
    // "AND BY HOW MUCH" — signed, so a reader can tell over from under.
    CY_CHECK_NEAR(found->margin, -0.04F, 1e-6F);

    // Nothing within the radius is a DIFFERENT answer from a rejection: it means no candidate was
    // ever generated there, which the editor should say rather than blaming a filter.
    CY_CHECK(provenance.why_nothing_here(400.0F, 400.0F, 3.0F) == nullptr);
}

CY_TEST_CASE("two rejections at one distance give one answer") {
    // A "why is nothing here" whose answer depended on recording order would change after a
    // regeneration that changed nothing, and a designer would read that as the rules having moved.
    RegionProvenance provenance(test::allocator());
    RejectionRecord left;
    left.slot = 9;
    left.stage_name = "forest.filter-late";
    left.position_x = 4.0F;
    left.position_z = 0.0F;
    RejectionRecord right;
    right.slot = 2;
    right.stage_name = "forest.filter-early";
    right.position_x = -4.0F;
    right.position_z = 0.0F;
    CY_REQUIRE(provenance.record_rejected(left));
    CY_REQUIRE(provenance.record_rejected(right));

    const RejectionRecord* found = provenance.why_nothing_here(0.0F, 0.0F, 10.0F);
    CY_REQUIRE(found != nullptr);
    // The lower SLOT wins the tie, whichever was recorded first.
    CY_CHECK_EQ(found->slot, 2u);

    RegionProvenance reversed(test::allocator());
    CY_REQUIRE(reversed.record_rejected(right));
    CY_REQUIRE(reversed.record_rejected(left));
    const RejectionRecord* again = reversed.why_nothing_here(0.0F, 0.0F, 10.0F);
    CY_REQUIRE(again != nullptr);
    CY_CHECK_EQ(again->slot, 2u);
}

CY_TEST_CASE("the profiler attributes a run to a node rather than to the graph") {
    // "The profiler SHALL report per node: processor and GPU time, candidates in and out, memory,
    // and cache hit rate — so that a slow generator is attributable to a NODE rather than to the
    // graph."
    GenerationProfile profile(test::allocator());
    StageProfile fast;
    fast.name = "forest.noise";
    fast.stage = 0;
    fast.cpu_micros = 10;
    StageProfile slow;
    slow.name = test::kFlowNode;
    slow.stage = 1;
    slow.cpu_micros = 900;
    CY_REQUIRE(profile.stages.push_back(fast));
    CY_REQUIRE(profile.stages.push_back(slow));

    const StageProfile* dominant = profile.dominant_stage();
    CY_REQUIRE(dominant != nullptr);
    CY_CHECK(test::same_text(dominant->name, test::kFlowNode));

    // A zero hit rate and "nothing was looked up" read identically in a chart and mean opposite
    // things, which is why the misses are reported beside the rate.
    CY_CHECK_NEAR(profile.cache_hit_rate(), 0.0F, 1e-6F);
    profile.cache_hits = 3;
    profile.cache_misses = 1;
    CY_CHECK_NEAR(profile.cache_hit_rate(), 0.75F, 1e-6F);

    // A reset keeps the stage names — a profile that forgot which node was which would be a profile
    // that cannot attribute anything.
    profile.reset();
    CY_CHECK_EQ(profile.cache_hits, 0u);
    CY_REQUIRE_EQ(profile.stages.size(), 2u);
    CY_CHECK(test::same_text(profile.stages[1].name, test::kFlowNode));
    CY_CHECK_EQ(profile.stages[1].cpu_micros, 0u);

    // GPU time is zero and says so: nothing in this module dispatches to a device, and a number
    // invented for a path that does not exist is the false green this milestone's brief forbids.
    CY_CHECK_EQ(profile.stages[1].gpu_micros, 0u);
}
