// Pipeline state keys, the manifest, and the central state cache. Task 3.7.
//
// The behaviour under test is the one `shader-system` states as a prohibition: "Blocking the frame
// to compile a pipeline state SHALL NOT occur in shipping builds." So a miss must hand back the
// fallback and queue the work — and the test that matters is that `request()` never calls the
// builder.

#include <cy/backends/shader/pipeline.h>
#include <cy/core/memory/scope.h>
#include <cy/test/test.h>

using cy::u32;
using cy::u8;
using cy::usize;
using namespace cy::shader;

namespace {

cy::assets::ContentHash program_hash(const char* text) {
    return cy::assets::content_hash(text, std::char_traits<char>::length(text));
}

PipelineStateInputs base_inputs(const cy::assets::ContentHash* programs, usize count) {
    PipelineStateInputs inputs;
    inputs.programs = {programs, count};
    inputs.depth_stencil.format = cy::rhi::Format::D32Sfloat;
    inputs.depth_stencil.depth_test_enable = true;
    inputs.depth_stencil.depth_write_enable = true;
    inputs.sample_count = 1;
    return inputs;
}

/// A builder that counts and can be told to fail. The renderer's real one calls the device; this
/// one is why none of these cases needs a GPU.
struct Builder {
    u32 calls = 0;
    bool fail = false;
    PipelineObject next;

    static cy::Expected<PipelineObject, cy::Error> build(
        void* user, const PipelineStateKey&, cy::Span<const cy::assets::ContentHash>) noexcept {
        auto* self = static_cast<Builder*>(user);
        ++self->calls;
        if (self->fail) {
            return cy::fail(cy::ErrorCode::Internal, "the driver refused the pipeline");
        }
        return self->next;
    }
};

PipelineObject some_pipeline(u32 slot) {
    PipelineObject object;
    object.kind = PipelineKind::Graphics;
    object.graphics = cy::rhi::GraphicsPipelineHandle::from_slot(slot, 1);
    return object;
}

}  // namespace

CY_TEST_CASE("the state key covers every field the specification names") {
    const cy::assets::ContentHash programs[] = {program_hash("vertex"), program_hash("fragment")};
    const PipelineStateKey reference = derive_pipeline_state_key(base_inputs(programs, 2));
    CY_CHECK(derive_pipeline_state_key(base_inputs(programs, 2)) == reference);

    auto changed = base_inputs(programs, 2);
    changed.sample_count = 4;
    CY_CHECK(derive_pipeline_state_key(changed) != reference);

    changed = base_inputs(programs, 2);
    changed.depth_stencil.depth_compare = cy::rhi::CompareOp::Less;
    CY_CHECK(derive_pipeline_state_key(changed) != reference);

    changed = base_inputs(programs, 2);
    changed.rasterisation.cull_mode = cy::rhi::CullMode::None;
    CY_CHECK(derive_pipeline_state_key(changed) != reference);

    changed = base_inputs(programs, 2);
    changed.permutation = PermutationKey{3};
    CY_CHECK(derive_pipeline_state_key(changed) != reference);

    const cy::rhi::ColorAttachmentState attachment{.format = cy::rhi::Format::Rgba16Sfloat};
    changed = base_inputs(programs, 2);
    changed.color_attachments = {&attachment, 1};
    CY_CHECK(derive_pipeline_state_key(changed) != reference);

    const cy::assets::ContentHash other[] = {program_hash("vertex"), program_hash("other")};
    CY_CHECK(derive_pipeline_state_key(base_inputs(other, 2)) != reference);

    changed = base_inputs(programs, 2);
    changed.kind = PipelineKind::Compute;
    CY_CHECK(derive_pipeline_state_key(changed) != reference);
}

CY_TEST_CASE("the disk cache is named by device, driver and engine version") {
    auto root = cy::assets::VirtualPath::normalise("cache/pipelines");
    CY_REQUIRE(root.has_value());

    PipelineDiskCacheKey key;
    key.device_name = "NVIDIA GeForce RTX 5060";
    key.vendor_id = 0x10DE;
    key.device_id = 0x2C05;
    key.driver_version = 580;
    key.engine_version = "0.3.0";

    auto path = pipeline_cache_path(*root, key);
    CY_REQUIRE(path.has_value());
    CY_CHECK(path->view().ends_with(".pcache"));

    // A driver upgrade is a different file, not a validity check inside the old one — a driver's
    // own blob format is opaque and asking it whether it is stale is what it cannot answer.
    PipelineDiskCacheKey upgraded = key;
    upgraded.driver_version = 590;
    auto after = pipeline_cache_path(*root, upgraded);
    CY_REQUIRE(after.has_value());
    CY_CHECK(*path != *after);

    // And the same machine finds yesterday's file.
    auto again = pipeline_cache_path(*root, key);
    CY_REQUIRE(again.has_value());
    CY_CHECK(*path == *again);
}

CY_TEST_CASE("a missing state hands back the fallback and does not build inline") {
    const cy::assets::ContentHash programs[] = {program_hash("vertex"), program_hash("fragment")};
    const PipelineStateKey key = derive_pipeline_state_key(base_inputs(programs, 2));

    Builder builder;
    builder.next = some_pipeline(1);
    PipelineStateCache cache(cy::current_allocator());
    cache.set_builder(&Builder::build, &builder);
    cache.set_fallback(some_pipeline(99));

    const PipelineStateCache::Request first = cache.request(key, {programs, 2});
    CY_CHECK(first.is_fallback);
    CY_CHECK(first.status == PipelineStatus::Compiling);
    CY_CHECK(first.pipeline.graphics == cache.fallback().graphics);
    // The whole requirement, in one assertion: the frame did not compile anything.
    CY_CHECK_EQ(builder.calls, 0U);
    CY_CHECK_EQ(cache.pending_count(), usize{1});

    auto built = cache.build_pending();
    CY_REQUIRE(built.has_value());
    CY_CHECK(*built);
    CY_CHECK_EQ(builder.calls, 1U);

    const PipelineStateCache::Request second = cache.request(key, {programs, 2});
    CY_CHECK_FALSE(second.is_fallback);
    CY_CHECK(second.status == PipelineStatus::Ready);
    CY_CHECK(second.pipeline.graphics == builder.next.graphics);
    CY_CHECK_EQ(cache.stats().hits, cy::u64{1});
    CY_CHECK_EQ(cache.stats().fallbacks, cy::u64{1});

    auto nothing_left = cache.build_pending();
    CY_REQUIRE(nothing_left.has_value());
    CY_CHECK_FALSE(*nothing_left);
}

CY_TEST_CASE("a build that fails is recorded once, not retried every frame") {
    const cy::assets::ContentHash programs[] = {program_hash("vertex")};
    const PipelineStateKey key = derive_pipeline_state_key(base_inputs(programs, 1));

    Builder builder;
    builder.fail = true;
    PipelineStateCache cache(cy::current_allocator());
    cache.set_builder(&Builder::build, &builder);
    cache.set_fallback(some_pipeline(99));

    (void)cache.request(key, {programs, 1});
    CY_REQUIRE(cache.build_pending().has_value());
    CY_CHECK(cache.status_of(key) == PipelineStatus::Failed);
    CY_CHECK_EQ(cache.stats().build_failures, cy::u64{1});

    (void)cache.request(key, {programs, 1});
    auto again = cache.build_pending();
    CY_REQUIRE(again.has_value());
    // Still failed, and the builder was not called a second time by the request.
    CY_CHECK_FALSE(*again);
    CY_CHECK_EQ(builder.calls, 1U);
}

CY_TEST_CASE("a manifest records uses, sorts stably and round-trips") {
    const cy::assets::ContentHash common[] = {program_hash("vertex"), program_hash("fragment")};
    const cy::assets::ContentHash rare[] = {program_hash("rare")};

    PipelineManifest manifest(cy::current_allocator());
    const PipelineStateKey hot = derive_pipeline_state_key(base_inputs(common, 2));
    const PipelineStateKey cold = derive_pipeline_state_key(base_inputs(rare, 1));
    CY_REQUIRE(manifest.record(cold, {rare, 1}).has_value());
    for (u32 index = 0; index < 5; ++index) {
        CY_REQUIRE(manifest.record(hot, {common, 2}).has_value());
    }
    CY_CHECK_EQ(manifest.size(), usize{2});
    CY_CHECK(manifest.contains(hot));

    manifest.sort();
    // Descending use count: warming does the states a level leans on first, which is what makes an
    // interrupted warm-up still useful.
    CY_REQUIRE(manifest.entry_at(0) != nullptr);
    CY_CHECK(manifest.entry_at(0)->key == hot);
    CY_CHECK_EQ(manifest.entry_at(0)->uses, 5U);

    cy::Array<u8> bytes(cy::current_allocator());
    CY_REQUIRE(manifest.serialize(bytes).has_value());
    auto parsed = PipelineManifest::parse(cy::current_allocator(), {bytes.data(), bytes.size()});
    CY_REQUIRE(parsed.has_value());
    CY_CHECK_EQ(parsed->size(), usize{2});
    CY_CHECK(parsed->contains(hot));
    CY_CHECK_EQ(parsed->entry_at(0)->uses, 5U);
    CY_CHECK_EQ(parsed->entry_at(0)->program_count, 2U);

    CY_CHECK_FALSE(manifest.record(hot, {}).has_value());
}

CY_TEST_CASE("warming builds the manifest's states and reports progress") {
    const cy::assets::ContentHash first[] = {program_hash("a")};
    const cy::assets::ContentHash second[] = {program_hash("b")};

    PipelineManifest manifest(cy::current_allocator());
    auto a = base_inputs(first, 1);
    auto b = base_inputs(second, 1);
    CY_REQUIRE(manifest.record(derive_pipeline_state_key(a), {first, 1}).has_value());
    CY_REQUIRE(manifest.record(derive_pipeline_state_key(b), {second, 1}).has_value());

    Builder builder;
    builder.next = some_pipeline(1);
    PipelineStateCache cache(cy::current_allocator());
    cache.set_builder(&Builder::build, &builder);

    struct Progress {
        usize last_completed = 0;
        usize total = 0;
        u32 calls = 0;

        static void observe(void* user, usize completed, usize total, const PipelineStateKey&,
                            bool) noexcept {
            auto* self = static_cast<Progress*>(user);
            self->last_completed = completed;
            self->total = total;
            ++self->calls;
        }
    } progress;

    CY_REQUIRE(cache.warm(manifest, &Progress::observe, &progress).has_value());
    CY_CHECK_EQ(builder.calls, 2U);
    CY_CHECK_EQ(progress.calls, 2U);
    CY_CHECK_EQ(progress.last_completed, usize{2});
    CY_CHECK_EQ(progress.total, usize{2});
    CY_CHECK_EQ(cache.stats().warmed, cy::u64{2});
    CY_CHECK(cache.status_of(derive_pipeline_state_key(a)) == PipelineStatus::Ready);
    // Nothing is pending afterwards: a warmed state is not a queued one.
    CY_CHECK_EQ(cache.pending_count(), usize{0});
}

CY_TEST_CASE("a rebuilt shader invalidates the pipelines that named it and nothing else") {
    const cy::assets::ContentHash reloaded = program_hash("vertex");
    const cy::assets::ContentHash untouched = program_hash("other");
    const cy::assets::ContentHash uses_it[] = {reloaded};
    const cy::assets::ContentHash does_not[] = {untouched};

    Builder builder;
    builder.next = some_pipeline(1);
    PipelineStateCache cache(cy::current_allocator());
    cache.set_builder(&Builder::build, &builder);

    const PipelineStateKey affected = derive_pipeline_state_key(base_inputs(uses_it, 1));
    const PipelineStateKey unaffected = derive_pipeline_state_key(base_inputs(does_not, 1));
    (void)cache.request(affected, {uses_it, 1});
    (void)cache.request(unaffected, {does_not, 1});
    CY_REQUIRE(cache.build_pending().has_value());
    CY_REQUIRE(cache.build_pending().has_value());
    CY_CHECK(cache.status_of(affected) == PipelineStatus::Ready);
    CY_CHECK(cache.status_of(unaffected) == PipelineStatus::Ready);

    CY_CHECK_EQ(cache.invalidate_by_program(reloaded), 1U);
    CY_CHECK(cache.status_of(affected) == PipelineStatus::Missing);
    CY_CHECK(cache.status_of(unaffected) == PipelineStatus::Ready);

    CY_CHECK_FALSE(
        cache.invalidate(derive_pipeline_state_key(base_inputs(nullptr, 0))).has_value());
}
