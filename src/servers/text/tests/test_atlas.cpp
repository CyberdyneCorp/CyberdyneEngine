// The glyph atlas: packing, growth, eviction and the thrash counter. M5 task 5.3.

#include <cy/servers/text/atlas.h>
#include <cy/test/test.h>

#include <vector>

using namespace cy::text;
using cy::u32;
using cy::u8;
using cy::usize;

namespace {

GlyphKey key_of(u32 glyph) {
    GlyphKey key;
    key.face = FontHandle::from_slot(0, 1);
    key.glyph = glyph;
    return key;
}

GlyphMetrics metrics_of(u32 extent) {
    GlyphMetrics metrics;
    metrics.width = extent;
    metrics.height = extent;
    metrics.advance = static_cast<cy::f32>(extent);
    return metrics;
}

std::vector<u8> coverage_of(u32 extent, u8 value) {
    return std::vector<u8>(static_cast<usize>(extent) * extent, value);
}

}  // namespace

CY_TEST_CASE("atlas: a configuration that cannot work is refused at start") {
    GlyphAtlas atlas;
    GlyphAtlasConfig config;
    config.initial_extent = 100;  // not a power of two
    CY_CHECK(!atlas.start(config).has_value());

    config.initial_extent = 512;
    config.maximum_extent = 256;
    CY_CHECK(!atlas.start(config).has_value());
}

CY_TEST_CASE("atlas: a glyph goes in, comes back, and reports its rectangle") {
    GlyphAtlas atlas;
    GlyphAtlasConfig config;
    config.initial_extent = 64;
    config.maximum_extent = 64;
    CY_REQUIRE(atlas.start(config).has_value());

    const std::vector<u8> coverage = coverage_of(8, 200);
    auto slot = atlas.insert(key_of(1), metrics_of(8),
                             cy::Span<const u8>(coverage.data(), coverage.size()));
    CY_REQUIRE(slot.has_value());
    CY_CHECK(slot.value()->rect.size.x == 8);
    CY_CHECK(slot.value()->rect.size.y == 8);
    CY_CHECK(atlas.live_glyphs() == 1);

    const GlyphSlot* found = atlas.find(key_of(1));
    CY_REQUIRE(found != nullptr);
    CY_CHECK(found->rect == slot.value()->rect);
    CY_CHECK(atlas.find(key_of(2)) == nullptr);

    // The pixels really are in the texture where the slot says they are.
    const cy::Span<const u8> pixels = atlas.pixels();
    const usize offset = (static_cast<usize>(found->rect.position.y) * atlas.extent()) +
                         static_cast<usize>(found->rect.position.x);
    CY_CHECK(pixels[offset] == 200);
}

CY_TEST_CASE("atlas: a glyph that no atlas could hold is refused rather than grown for") {
    GlyphAtlas atlas;
    GlyphAtlasConfig config;
    config.initial_extent = 64;
    config.maximum_extent = 128;
    CY_REQUIRE(atlas.start(config).has_value());

    const std::vector<u8> coverage = coverage_of(200, 255);
    auto refused = atlas.insert(key_of(1), metrics_of(200),
                                cy::Span<const u8>(coverage.data(), coverage.size()));
    CY_REQUIRE(!refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::OutOfRange);
}

CY_TEST_CASE("atlas: it grows rather than failing, and everything survives the repack") {
    GlyphAtlas atlas;
    GlyphAtlasConfig config;
    config.initial_extent = 32;
    config.maximum_extent = 512;
    config.padding = 0;
    CY_REQUIRE(atlas.start(config).has_value());
    CY_CHECK(atlas.extent() == 32);

    // Sixteen sixteen-pixel glyphs need four times the initial atlas's area.
    for (u32 glyph = 1; glyph <= 16; ++glyph) {
        const std::vector<u8> coverage = coverage_of(16, static_cast<u8>(glyph * 10));
        CY_REQUIRE(atlas
                       .insert(key_of(glyph), metrics_of(16),
                               cy::Span<const u8>(coverage.data(), coverage.size()))
                       .has_value());
    }
    CY_CHECK(atlas.extent() > 32);
    CY_CHECK(atlas.diagnostics().atlas_growths >= 1);
    CY_CHECK(atlas.live_glyphs() == 16);

    // Every glyph is still findable and still carries ITS OWN pixels: a repack that mixed two
    // glyphs' coverage would pass a count check and render the wrong letters.
    for (u32 glyph = 1; glyph <= 16; ++glyph) {
        const GlyphSlot* found = atlas.find(key_of(glyph));
        CY_REQUIRE(found != nullptr);
        const cy::Span<const u8> pixels = atlas.pixels();
        const usize offset = (static_cast<usize>(found->rect.position.y) * atlas.extent()) +
                             static_cast<usize>(found->rect.position.x);
        CY_CHECK(pixels[offset] == static_cast<u8>(glyph * 10));
    }
}

CY_TEST_CASE("atlas: under pressure it evicts the least recently used") {
    // `text-and-fonts`: "WHEN many fonts and sizes are used THEN least-recently-used glyphs SHALL
    // be evicted, and thrashing SHALL be reported as a diagnostic."
    GlyphAtlas atlas;
    GlyphAtlasConfig config;
    config.initial_extent = 32;
    config.maximum_extent = 32;
    config.padding = 0;
    CY_REQUIRE(atlas.start(config).has_value());

    // Four sixteen-pixel glyphs exactly fill a thirty-two pixel atlas.
    for (u32 glyph = 1; glyph <= 4; ++glyph) {
        const std::vector<u8> coverage = coverage_of(16, static_cast<u8>(glyph));
        CY_REQUIRE(atlas
                       .insert(key_of(glyph), metrics_of(16),
                               cy::Span<const u8>(coverage.data(), coverage.size()))
                       .has_value());
    }
    // Touch the first three, leaving the fourth the least recently used.
    CY_CHECK(atlas.find(key_of(1)) != nullptr);
    CY_CHECK(atlas.find(key_of(2)) != nullptr);
    CY_CHECK(atlas.find(key_of(3)) != nullptr);

    const std::vector<u8> coverage = coverage_of(16, 99);
    CY_REQUIRE(
        atlas
            .insert(key_of(5), metrics_of(16), cy::Span<const u8>(coverage.data(), coverage.size()))
            .has_value());
    CY_CHECK(atlas.diagnostics().glyphs_evicted >= 1);
    // The untouched one goes first. Eviction takes a batch rather than exactly one — repacking per
    // insertion at the ceiling would cost a copy of the whole atlas per glyph — so the oldest of
    // the touched ones may go with it, and the assertion is about the ORDER rather than the count.
    CY_CHECK(atlas.find(key_of(4)) == nullptr);
    CY_CHECK(atlas.find(key_of(3)) != nullptr);
    CY_CHECK(atlas.find(key_of(5)) != nullptr);
}

CY_TEST_CASE("atlas: re-inserting something it threw away is counted as thrashing") {
    // The number that says the atlas is too small. Occupancy does not: an atlas at 99% that never
    // thrashes is exactly the right size, and one at 60% that thrashes every frame is not.
    GlyphAtlas atlas;
    GlyphAtlasConfig config;
    config.initial_extent = 32;
    config.maximum_extent = 32;
    config.padding = 0;
    CY_REQUIRE(atlas.start(config).has_value());

    const std::vector<u8> coverage = coverage_of(16, 1);
    for (u32 glyph = 1; glyph <= 4; ++glyph) {
        CY_REQUIRE(atlas
                       .insert(key_of(glyph), metrics_of(16),
                               cy::Span<const u8>(coverage.data(), coverage.size()))
                       .has_value());
    }
    CY_CHECK(atlas.diagnostics().thrashes == 0);

    // Cycle through more glyphs than fit, then come back to the first — which by now has been
    // evicted, and re-inserting it is the definition of a thrash.
    for (u32 glyph = 5; glyph <= 12; ++glyph) {
        CY_REQUIRE(atlas
                       .insert(key_of(glyph), metrics_of(16),
                               cy::Span<const u8>(coverage.data(), coverage.size()))
                       .has_value());
    }
    CY_REQUIRE(atlas.find(key_of(1)) == nullptr);
    CY_REQUIRE(
        atlas
            .insert(key_of(1), metrics_of(16), cy::Span<const u8>(coverage.data(), coverage.size()))
            .has_value());
    CY_CHECK(atlas.diagnostics().thrashes >= 1);
}

CY_TEST_CASE("atlas: the dirty region covers what changed and no more") {
    GlyphAtlas atlas;
    GlyphAtlasConfig config;
    config.initial_extent = 64;
    config.maximum_extent = 64;
    CY_REQUIRE(atlas.start(config).has_value());
    atlas.clear_dirty();
    CY_CHECK(atlas.dirty_region().is_empty());

    const std::vector<u8> coverage = coverage_of(8, 128);
    auto slot = atlas.insert(key_of(1), metrics_of(8),
                             cy::Span<const u8>(coverage.data(), coverage.size()));
    CY_REQUIRE(slot.has_value());
    // An uploader re-uploads this rather than the whole texture, which is the difference between a
    // few hundred bytes per frame and sixteen kilobytes.
    CY_CHECK(atlas.dirty_region() == slot.value()->rect);
    atlas.clear_dirty();
    CY_CHECK(atlas.dirty_region().is_empty());
}

CY_TEST_CASE("atlas: occupancy is what was packed over what there is") {
    GlyphAtlas atlas;
    GlyphAtlasConfig config;
    config.initial_extent = 64;
    config.maximum_extent = 64;
    config.padding = 0;
    CY_REQUIRE(atlas.start(config).has_value());
    CY_CHECK(atlas.occupancy() == 0.0f);

    const std::vector<u8> coverage = coverage_of(32, 255);
    CY_REQUIRE(
        atlas
            .insert(key_of(1), metrics_of(32), cy::Span<const u8>(coverage.data(), coverage.size()))
            .has_value());
    // A 32-pixel glyph in a 64-pixel atlas is a quarter of it.
    CY_CHECK(atlas.occupancy() > 0.2f);
    CY_CHECK(atlas.occupancy() < 0.3f);
}
