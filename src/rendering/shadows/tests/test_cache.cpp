// The page cache, precise invalidation, and the fallback chain. Tasks 8.1 and 8.2.
//
// Three of the specification's scenarios are the first three cases here — static content is
// rendered once, an expensive page outlives a cheap one, and one object moving dirties a handful of
// pages rather than a light. The fourth group is the chain, which must always answer something:
// "the frame SHALL never wait for shadow page production".

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/shadows/bias.h>
#include <cy/rendering/shadows/fallback.h>
#include <cy/rendering/shadows/invalidation.h>

namespace {

using cy::rendering::CasterMotion;
using cy::rendering::InvalidationScratch;
using cy::rendering::InvalidationSource;
using cy::rendering::ShadowAddressSpace;
using cy::rendering::ShadowCacheConfig;
using cy::rendering::ShadowDeformationMode;
using cy::rendering::ShadowPageCache;
using cy::rendering::ShadowPageGeometry;
using cy::rendering::ShadowProjection;
using cy::rendering::ShadowSubstitution;
using cy::rendering::UpdateClass;
using cy::rendering::VirtualPage;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

VirtualPage page_at(cy::u32 light, cy::u8 level, cy::u16 x, cy::u16 y) noexcept {
    VirtualPage page;
    page.light_slot = light;
    page.level = level;
    page.x = x;
    page.y = y;
    return page;
}

ShadowAddressSpace spot_space() noexcept {
    ShadowAddressSpace space;
    space.projection = ShadowProjection::Spot;
    space.light_slot = 1;
    space.level = 0;
    space.basis = cy::rendering::shadow_basis(cy::Vec3{0.0F, 0.0F, -1.0F});
    space.position = cy::Vec3{0.0F, 0.0F, 0.0F};
    space.half_angle = 0.7F;
    space.extent = 100.0F;
    space.geometry = ShadowPageGeometry{128, 4096};
    return space;
}

}  // namespace

CY_TEST_CASE("a static scene renders its shadow pages once and then reports nothing") {
    ShadowPageCache cache(allocator());
    CY_REQUIRE(cache.initialize(ShadowCacheConfig{}).has_value());

    cache.begin_frame(1);
    const VirtualPage page = page_at(0, 0, 4, 7);
    cy::rendering::PageLookup lookup = cache.request(page, UpdateClass::Normal);
    CY_REQUIRE(lookup.needs_render);
    CY_CHECK_FALSE(lookup.starved);
    cache.record_render(page, 0.08F);
    CY_CHECK_EQ(cache.statistics().renders, 1U);

    for (cy::u64 frame = 2; frame <= 20; ++frame) {
        cache.begin_frame(frame);
        lookup = cache.request(page, UpdateClass::Normal);
        CY_CHECK_FALSE(lookup.needs_render);
        CY_CHECK_EQ(cache.statistics().renders, 0U);
        CY_CHECK_EQ(cache.statistics().hits, 1U);
    }

    // Re-rendering a page nobody invalidated is a defect, and it is counted rather than tolerated.
    cache.record_render(page, 0.08F);
    CY_CHECK_EQ(cache.statistics().redundant_renders, 1U);
}

CY_TEST_CASE("between two pages of equal recency, the cheap one is evicted") {
    ShadowPageCache cache(allocator());
    ShadowCacheConfig config;
    config.slots = 2;
    config.min_residency_frames = 0;
    CY_REQUIRE(cache.initialize(config).has_value());

    const VirtualPage cheap = page_at(0, 0, 1, 1);
    const VirtualPage expensive = page_at(0, 0, 2, 2);

    cache.begin_frame(1);
    CY_REQUIRE(cache.request(cheap, UpdateClass::Normal).needs_render);
    cache.record_render(cheap, 0.02F);
    CY_REQUIRE(cache.request(expensive, UpdateClass::Normal).needs_render);
    cache.record_render(expensive, 2.00F);

    // A third page on a later frame: both candidates are equally stale, so the weighting by
    // re-render cost is the only thing that decides.
    cache.begin_frame(5);
    const VirtualPage arrival = page_at(0, 0, 3, 3);
    CY_REQUIRE(cache.request(arrival, UpdateClass::Normal).needs_render);
    CY_CHECK_EQ(cache.statistics().evictions, 1U);
    CY_CHECK(cache.inspect(cheap) == nullptr);
    CY_REQUIRE(cache.inspect(expensive) != nullptr);
}

CY_TEST_CASE("a page that has only just arrived is not evicted underneath its own light") {
    ShadowPageCache cache(allocator());
    ShadowCacheConfig config;
    config.slots = 1;
    config.min_residency_frames = 4;
    CY_REQUIRE(cache.initialize(config).has_value());

    cache.begin_frame(1);
    const VirtualPage held = page_at(0, 0, 1, 1);
    CY_REQUIRE(cache.request(held, UpdateClass::Normal).needs_render);
    cache.record_render(held, 0.05F);

    cache.begin_frame(2);
    const cy::rendering::PageLookup denied =
        cache.request(page_at(0, 0, 9, 9), UpdateClass::Normal);
    CY_CHECK(denied.starved);
    CY_CHECK_EQ(cache.statistics().starved, 1U);
    CY_CHECK_EQ(cache.statistics().evictions, 0U);
    // The frame is not stalled by the refusal; the fallback chain answers instead.
    CY_REQUIRE(cache.inspect(held) != nullptr);
}

CY_TEST_CASE(
    "a caster that moves dirties the pages it left and the pages it entered, and no more") {
    ShadowPageCache cache(allocator());
    CY_REQUIRE(cache.initialize(ShadowCacheConfig{}).has_value());
    const ShadowAddressSpace space = spot_space();

    // Bring a strip of the light's space into the cache and make it clean.
    cache.begin_frame(1);
    const cy::u32 side = space.geometry.pages_per_side();
    for (cy::u32 x = 0; x < side; ++x) {
        const VirtualPage page =
            page_at(1, 0, static_cast<cy::u16>(x), static_cast<cy::u16>(side / 2U));
        CY_REQUIRE(cache.request(page, UpdateClass::Normal).needs_render);
        cache.record_render(page, 0.03F);
    }
    const cy::u32 resident = cache.statistics().renders;
    CY_REQUIRE_EQ(resident, side);

    cache.begin_frame(2);
    VirtualPage scratch[128];
    CasterMotion motion;
    motion.instance_id = 4242;
    motion.previous =
        cy::Aabb::from_center_extents(cy::Vec3{-1.0F, 0.0F, -20.0F}, cy::Vec3{0.4F, 0.4F, 0.4F});
    motion.current =
        cy::Aabb::from_center_extents(cy::Vec3{1.0F, 0.0F, -20.0F}, cy::Vec3{0.4F, 0.4F, 0.4F});
    motion.mode = ShadowDeformationMode::Static;
    const cy::rendering::InvalidationReport report =
        invalidate_caster_motion(cache, space, motion, InvalidationScratch{scratch, 128});

    CY_CHECK_GT(report.pages_dirtied, 0U);
    CY_CHECK_LT(report.pages_dirtied, side);
    CY_CHECK_EQ(report.overflow, 0U);
    CY_CHECK_EQ(
        cache.statistics().invalidated_by[static_cast<cy::usize>(InvalidationSource::Instance)],
        report.pages_dirtied);

    // The diagnostic names what did it — the requirement is "the diagnostics SHALL name what
    // invalidated it", and a bare dirty flag cannot.
    cy::u32 dirty = 0;
    for (const cy::rendering::PageEntry& entry : cache.entries()) {
        if (entry.dirty) {
            ++dirty;
            CY_CHECK_EQ(entry.dirty_reason, InvalidationSource::Instance);
            CY_CHECK_EQ(entry.dirty_source_id, 4242U);
        }
    }
    CY_CHECK_GT(dirty, 0U);
    CY_CHECK_LT(dirty, side);
}

CY_TEST_CASE(
    "a tree swaying inside its declared envelope dirties nothing; a moving light dirties itself") {
    ShadowPageCache cache(allocator());
    CY_REQUIRE(cache.initialize(ShadowCacheConfig{}).has_value());
    const ShadowAddressSpace space = spot_space();

    VirtualPage scratch[64];
    CasterMotion tree;
    tree.instance_id = 7;
    tree.mode = ShadowDeformationMode::Bounded;
    tree.envelope = 1.5F;
    tree.previous =
        cy::Aabb::from_center_extents(cy::Vec3{0.0F, 0.0F, -20.0F}, cy::Vec3{1.0F, 3.0F, 1.0F});
    tree.current = tree.previous;

    // Bring exactly the pages the tree casts into into the cache, and make them clean. A page the
    // cache does not hold has no contents to invalidate, which is why the report counts tracked
    // pages and not projected ones.
    cache.begin_frame(1);
    const cy::u32 covered = pages_covering(space,
                                           cy::Aabb{tree.previous.min - cy::Vec3{1.5F, 1.5F, 1.5F},
                                                    tree.previous.max + cy::Vec3{1.5F, 1.5F, 1.5F}},
                                           scratch, 64);
    CY_REQUIRE(covered > 0U);
    CY_REQUIRE(covered <= 64U);
    for (cy::u32 index = 0; index < covered; ++index) {
        CY_REQUIRE(cache.request(scratch[index], UpdateClass::Normal).needs_render);
        cache.record_render(scratch[index], 0.01F);
    }

    cache.begin_frame(2);
    const cy::rendering::InvalidationReport still =
        invalidate_caster_motion(cache, space, tree, InvalidationScratch{scratch, 64});
    CY_CHECK_EQ(still.pages_dirtied, 0U);
    CY_CHECK_EQ(still.casters_unchanged, 1U);

    // `AlwaysDirty` is reported per caster, because it is the mode that silently removes the
    // benefit of caching.
    CasterMotion flag = tree;
    flag.mode = ShadowDeformationMode::AlwaysDirty;
    const cy::rendering::InvalidationReport always =
        invalidate_caster_motion(cache, space, flag, InvalidationScratch{scratch, 64});
    CY_CHECK_EQ(always.always_dirty_casters, 1U);
    CY_CHECK_GT(always.pages_dirtied, 0U);

    // A light that moves dirties its own space, and the cost is attributed to that light rather
    // than appearing as an unexplained rise in renders.
    cache.begin_frame(3);
    const cy::rendering::InvalidationReport moved = invalidate_light(cache, 1, 99);
    CY_CHECK_GT(moved.pages_dirtied, 0U);
    CY_CHECK_EQ(
        cache.statistics().invalidated_by[static_cast<cy::usize>(InvalidationSource::LightMoved)],
        moved.pages_dirtied);
    CY_CHECK_EQ(invalidate_light(cache, 55, 99).pages_dirtied, 0U);
}

CY_TEST_CASE("the fallback chain always answers, and says which rung it took") {
    ShadowPageCache cache(allocator());
    CY_REQUIRE(cache.initialize(ShadowCacheConfig{}).has_value());

    const VirtualPage wanted = page_at(2, 0, 8, 8);
    cy::rendering::FallbackOptions options;
    options.coarser_levels = 3;
    options.tail_level = 4;

    // Nothing is resident: the chain reaches the end and says so rather than stalling.
    cy::rendering::FallbackResult result = resolve_shadow_lookup(cache, 1, wanted, options);
    CY_CHECK_EQ(result.substitution, ShadowSubstitution::Unshadowed);

    options.approximation_available = true;
    CY_CHECK_EQ(resolve_shadow_lookup(cache, 1, wanted, options).substitution,
                ShadowSubstitution::Approximation);

    // A coarser page of the same light, on the shared lattice.
    cache.begin_frame(1);
    const VirtualPage coarse = page_at(2, 1, 4, 4);
    CY_REQUIRE(cache.request(coarse, UpdateClass::Normal).needs_render);
    cache.record_render(coarse, 0.05F);
    result = resolve_shadow_lookup(cache, 1, wanted, options);
    CY_CHECK_EQ(result.substitution, ShadowSubstitution::CoarserPage);
    CY_CHECK(result.coarser);
    CY_CHECK(result.page == coarse);

    // The requested page, rendered, wins over the coarse one.
    CY_REQUIRE(cache.request(wanted, UpdateClass::Normal).needs_render);
    cache.record_render(wanted, 0.05F);
    result = resolve_shadow_lookup(cache, 1, wanted, options);
    CY_CHECK_EQ(result.substitution, ShadowSubstitution::Requested);
    CY_CHECK_FALSE(result.coarser);

    // Dirty it, and remove the coarse page's cleanliness too: what is left is the stale contents of
    // the page itself, which is a rung and not a black square.
    cache.invalidate(wanted, InvalidationSource::Instance, 5);
    cache.invalidate(coarse, InvalidationSource::Instance, 5);
    result = resolve_shadow_lookup(cache, 2, wanted, options);
    CY_CHECK_EQ(result.substitution, ShadowSubstitution::StalePage);
    CY_CHECK_EQ(result.physical_slot, cache.inspect(wanted)->physical_slot);

    // Too stale to be worth substituting: the chain moves on rather than showing last minute's
    // shadow.
    options.max_stale_frames = 2;
    CY_CHECK_EQ(resolve_shadow_lookup(cache, 40, wanted, options).substitution,
                ShadowSubstitution::Approximation);

    cy::rendering::SubstitutionLedger ledger;
    ledger.record(ShadowSubstitution::Requested);
    ledger.record(ShadowSubstitution::StalePage);
    ledger.record(ShadowSubstitution::Unshadowed);
    CY_CHECK_EQ(ledger.total(), 3U);
    CY_CHECK_EQ(ledger.substituted(), 2U);
    ledger.reset();
    CY_CHECK_EQ(ledger.total(), 0U);

    // The shadow-critical level is derived, so a project cannot pin something so fine that the
    // guarantee stops being one.
    CY_CHECK_EQ(
        static_cast<cy::u32>(cy::rendering::shadow_critical_level(ShadowPageGeometry{128, 4096})),
        5U);
}

CY_TEST_CASE("shadow bias is derived from three measurements, and overrides are counted") {
    cy::rendering::BiasInputs inputs;
    inputs.texel_world_size = 0.01F;
    inputs.n_dot_l = 1.0F;
    inputs.geometric_error = 0.0F;
    const cy::rendering::DerivedBias flat = derive_shadow_bias(inputs);

    // A coarser texel needs more bias. Nothing is authored per light: the number comes from the
    // footprint, which is what makes "no per-light tuning by default" true.
    inputs.texel_world_size = 0.08F;
    const cy::rendering::DerivedBias coarse = derive_shadow_bias(inputs);
    CY_CHECK_GT(coarse.constant, flat.constant);
    CY_CHECK_GT(coarse.normal_offset, flat.normal_offset);

    // A grazing surface needs a bigger slope term.
    inputs.texel_world_size = 0.01F;
    inputs.n_dot_l = 0.2F;
    CY_CHECK_GT(derive_shadow_bias(inputs).slope_scale, flat.slope_scale);

    // A coarser caster representation displaces the surface, so the normal offset carries it. This
    // is the term a conventional shadow map has no reason to have.
    inputs.n_dot_l = 1.0F;
    inputs.geometric_error = 0.5F;
    CY_CHECK_GT(derive_shadow_bias(inputs).normal_offset, flat.normal_offset + 0.4F);

    // Receiver-plane depth lets the constant shrink without detaching the contact shadow.
    inputs.geometric_error = 0.0F;
    inputs.receiver_plane_available = true;
    CY_CHECK_LT(derive_shadow_bias(inputs).constant, flat.constant);

    cy::rendering::BiasOverrideLedger ledger;
    const cy::rendering::DerivedBias override_value{flat.constant * 4.0F, flat.slope_scale, 0.0F};
    for (cy::u32 light = 0; light < 20; ++light) {
        ledger.record(flat, light < 2 ? &override_value : nullptr);
    }
    CY_CHECK_EQ(ledger.lights_total, 20U);
    CY_CHECK_EQ(ledger.lights_overridden, 2U);
    CY_CHECK_FALSE(ledger.derivation_suspect());
    for (cy::u32 light = 0; light < 6; ++light) {
        ledger.record(flat, &override_value);
    }
    CY_CHECK(ledger.derivation_suspect());
    CY_CHECK_NEAR(ledger.worst_ratio, 4.0F, 1e-3F);
}
