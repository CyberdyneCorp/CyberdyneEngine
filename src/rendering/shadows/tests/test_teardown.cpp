// Teardown while full. Tasks 8.1 and 8.2.
//
// M5.5's gate found a Jolt job bridge destroying its free list underneath a worker, one run in
// forty, and every milestone since has been asked to test teardown under load rather than teardown
// of an empty object. Nothing in this module is threaded — `ShadowPageCache` and `PageRequestSet`
// are stepped once a frame on the frame thread and say so in their headers — so the load that can
// go wrong here is not concurrency but OCCUPANCY: a cache destroyed with every slot live, an index
// holding an entry for each of them, and a request set destroyed after its capacity was exceeded
// and its overflow path taken.
//
// The case is therefore built to be destroyed in the worst state it can reach, and it is meaningful
// because it is run under AddressSanitizer with leak detection on, and under ThreadSanitizer, in
// the sweep this suite belongs to. Under an ordinary build it still exercises the paths; under a
// sanitized one it is the assertion.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/shadows/cache.h>
#include <cy/rendering/shadows/invalidation.h>
#include <cy/rendering/shadows/pages.h>

namespace {

using cy::rendering::PageRequestSet;
using cy::rendering::ShadowCacheConfig;
using cy::rendering::ShadowPageCache;
using cy::rendering::UpdateClass;
using cy::rendering::VirtualPage;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

VirtualPage page_number(cy::u32 index) noexcept {
    VirtualPage page;
    page.light_slot = index % 7U;
    page.level = static_cast<cy::u8>((index / 7U) % 4U);
    page.x = static_cast<cy::u16>(index % 128U);
    page.y = static_cast<cy::u16>(index / 128U);
    return page;
}

}  // namespace

CY_TEST_CASE("a page cache destroyed with every slot live, pinned and dirty, frees everything") {
    for (cy::u32 round = 0; round < 8; ++round) {
        ShadowPageCache cache(allocator());
        ShadowCacheConfig config;
        config.slots = 512;
        config.min_residency_frames = 0;
        CY_REQUIRE(cache.initialize(config).has_value());

        // Fill it past capacity so eviction runs, the index is rehashed, and entries are recycled.
        for (cy::u64 frame = 1; frame <= 6; ++frame) {
            cache.begin_frame(frame);
            for (cy::u32 index = 0; index < 900; ++index) {
                const VirtualPage page = page_number(index + (round * 13U));
                const cy::rendering::PageLookup lookup = cache.request(page, UpdateClass::Normal);
                if (lookup.needs_render && !lookup.starved) {
                    cache.record_render(page, 0.01F);
                }
                if (index % 5U == 0U) {
                    (void)cache.invalidate(page, cy::rendering::InvalidationSource::Instance,
                                           index);
                }
                if (index % 97U == 0U) {
                    // Pinned entries can never be chosen as victims, so this leaves the cache with
                    // a set of slots it cannot recycle — the state a teardown is least likely to
                    // have been written for.
                    cache.set_pinned(page, true);
                }
            }
        }
        CY_CHECK_GT(cache.statistics().evictions + cache.statistics().starved, 0U);
        // Destroyed here, full, with the index populated and pinned entries live.
    }
}

CY_TEST_CASE("a request set destroyed after overflowing frees everything it did keep") {
    for (cy::u32 round = 0; round < 8; ++round) {
        PageRequestSet set(allocator());
        CY_REQUIRE(set.initialize(256).has_value());

        for (cy::u32 index = 0; index < 4000; ++index) {
            set.mark_page(page_number(index), 0.5F);
        }
        CY_CHECK_GT(set.overflow(), 0U);
        CY_CHECK_EQ(set.marks(), 4000U);
        CY_REQUIRE(set.compact().has_value());
        CY_CHECK_LE(set.pages().size(), 256U);
        // Destroyed here, having taken the overflow path and then compacted into a second array.
    }
}

CY_TEST_CASE("invalidation over a full cache with a scratch too small reports rather than writes") {
    ShadowPageCache cache(allocator());
    ShadowCacheConfig config;
    config.slots = 64;
    CY_REQUIRE(cache.initialize(config).has_value());
    cache.begin_frame(1);
    for (cy::u32 index = 0; index < 64; ++index) {
        const VirtualPage page = page_number(index);
        if (cache.request(page, UpdateClass::Normal).needs_render) {
            cache.record_render(page, 0.02F);
        }
    }

    cy::rendering::ShadowAddressSpace space;
    space.projection = cy::rendering::ShadowProjection::Spot;
    space.light_slot = 0;
    space.basis = cy::rendering::shadow_basis(cy::Vec3{0.0F, 0.0F, -1.0F});
    space.half_angle = 0.9F;
    space.extent = 200.0F;
    space.geometry = cy::rendering::ShadowPageGeometry{128, 4096};

    // A caster covering most of the light against a four-page scratch: the overflow is counted and
    // nothing is written past the array, which is the assertion AddressSanitizer makes for us.
    VirtualPage scratch[4];
    cy::rendering::CasterMotion motion;
    motion.instance_id = 1;
    motion.previous =
        cy::Aabb::from_center_extents(cy::Vec3{0.0F, 0.0F, -30.0F}, cy::Vec3{20.0F, 20.0F, 1.0F});
    motion.current =
        cy::Aabb::from_center_extents(cy::Vec3{2.0F, 0.0F, -30.0F}, cy::Vec3{20.0F, 20.0F, 1.0F});
    motion.mode = cy::rendering::ShadowDeformationMode::Dynamic;
    const cy::rendering::InvalidationReport report = invalidate_caster_motion(
        cache, space, motion, cy::rendering::InvalidationScratch{scratch, 4});
    CY_CHECK_GT(report.overflow, 0U);

    // A null scratch is refused rather than dereferenced.
    const cy::rendering::InvalidationReport refused = invalidate_caster_motion(
        cache, space, motion, cy::rendering::InvalidationScratch{nullptr, 0});
    CY_CHECK_EQ(refused.pages_dirtied, 0U);
}
