// SPDX-License-Identifier: MIT
#include "../wind_field_preview.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::sample::editor_window;

CY_TEST_CASE("editor weather publishes the wind image sampled by a scene material") {
    auto& allocator = system_allocator(MemoryDomain::World);
    for (const f64 origin : {0.0, 1'000'000.0}) {
        WindFieldPreview preview(allocator);
        const world::WorldVec3d centre{origin, 0.0, origin};
        CY_REQUIRE(preview.initialize(centre).has_value());
        CY_CHECK(preview.covers(centre));
        CY_CHECK_FALSE(preview.covers(world::WorldVec3d{origin + 512.0, 0.0, origin}));
        CY_CHECK_EQ(preview.image().field, preview.field());
        CY_CHECK_GT(preview.image().tiles, 0U);
        CY_CHECK(environment::is_field_image(preview.image().words.span()));

        for (const f64 height : {0.0, 24.0, 72.0}) {
            for (const f64 offset : {-96.0, 0.0, 96.0}) {
                const world::WorldVec3d position{origin + offset, height, origin + 64.0};
                const auto local = environment::image_local(preview.image(), position);
                const auto image = environment::sample_field_image(preview.image().words.span(),
                                                                   local.x, local.y, local.z);
                const auto store = preview.store().sample_deterministic(preview.field(), position);
                CY_REQUIRE(store.resolved);
                CY_CHECK_NEAR(image.x(), store.value.x(), 0.0001F);
                CY_CHECK_NEAR(image.y(), store.value.y(), 0.0001F);
                CY_CHECK_NEAR(image.z(), store.value.z(), 0.0001F);
            }
        }
    }
}
