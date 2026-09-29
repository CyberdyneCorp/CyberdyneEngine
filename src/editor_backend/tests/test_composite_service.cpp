// SPDX-License-Identifier: MIT
// One editor-service binding over MaterialService and NavigationService. Issue #28, task 2.2.

#include <cy/abi/cy_abi.h>
#include <cy/abi/host.h>
#include <cy/editor/composite_service.h>
#include <cy/editor/material_service.h>
#include <cy/editor/navigation_service.h>
#include <cy/test/test.h>

#include <algorithm>
#include <string>
#include <vector>

#include "navigation_seam.h"

using namespace cy;
using namespace cy::editor;
using namespace cy::editor::testing;

namespace {

struct Services {
    FixtureSeam seam;
    MaterialService material{allocator()};
    NavigationService navigation{allocator(), &seam};
    CompositeEditorService composite{allocator()};
    CyServiceSession session = nullptr;

    Services() {
        CY_REQUIRE(composite.route("material.", material).has_value());
        CY_REQUIRE(composite.route("preview.", material).has_value());
        CY_REQUIRE(composite.route("vfx.", material).has_value());
        CY_REQUIRE(composite.route("navigation.", navigation).has_value());
        CY_REQUIRE_EQ(composite.open(&session), CY_RESULT_OK);
    }
    ~Services() { composite.close(session); }
    Services(const Services&) = delete;
    Services& operator=(const Services&) = delete;
    Services(Services&&) = delete;
    Services& operator=(Services&&) = delete;
};

[[nodiscard]] bool has(const std::vector<std::string>& names, const char* name) {
    return std::ranges::find(names, name) != names.end();
}

}  // namespace

CY_TEST_CASE("composite: routes material.* and navigation.* to the right child") {
    Services services;
    const Event catalogue = call(services.composite, services.session, 1, "material.catalogue.get");
    CY_CHECK(catalogue.is(CY_SERVICE_EVENT_COMPLETED));
    CY_REQUIRE(catalogue.payload.size() >= 4);
    Decoder decoder(catalogue.payload);
    CY_CHECK_EQ(decoder.u32_(), 3U);  // the material catalogue's schema

    Payload overlay;
    overlay.u32_(kWorld).u32_(1);
    const Event set =
        call(services.composite, services.session, 2, "navigation.overlay.set", overlay);
    CY_CHECK(set.is(CY_SERVICE_EVENT_COMPLETED));
    CyServiceSession_T* const child =
        services.composite.child_session(services.session, services.navigation);
    CY_REQUIRE(child != nullptr);
    CY_CHECK_EQ(NavigationService::overlay(child, kWorld), 1U);
    CY_CHECK(services.composite.child_session(services.session, services.material) != nullptr);

    const Event unrouted = call(services.composite, services.session, 3, "physics.step");
    CY_CHECK(unrouted.is(CY_SERVICE_EVENT_FAILED));
    CY_CHECK_EQ(unrouted.code(), "operation-unsupported");
}

CY_TEST_CASE("composite: capabilities.get merges every child's operations") {
    Services services;
    const Event merged = call(services.composite, services.session, 1, "capabilities.get");
    CY_REQUIRE(merged.is(CY_SERVICE_EVENT_COMPLETED));
    u64 features = 0;
    const std::vector<std::string> names = capability_names(merged, features);
    CY_CHECK(has(names, "material.compile"));
    CY_CHECK(has(names, "preview.reload"));
    CY_CHECK(has(names, "navigation.bake"));
    CY_CHECK(has(names, "navigation.point.pick"));
    CY_CHECK_EQ(std::ranges::count(names, std::string("capabilities.get")), 1);
    CY_CHECK_NE(features & 0x1U, 0U);
    CY_CHECK_NE(features & kNavigationFeature, 0U);
}

CY_TEST_CASE("composite: a material request succeeds while a navigation bake is pending") {
    Services services;
    square_world(services.seam);
    Payload bake = bake_request(bake_settings());
    CY_REQUIRE_EQ(submit(services.composite, services.session, 10, "navigation.bake", bake),
                  CY_RESULT_OK);
    bool present = false;
    const Event first = poll_once(services.composite, services.session, present);
    CY_REQUIRE(present);
    CY_CHECK(first.is(CY_SERVICE_EVENT_PROGRESS));

    const CyResult accepted =
        submit(services.composite, services.session, 11, "material.catalogue.get");
    CY_REQUIRE_EQ(accepted, CY_RESULT_OK);
    if (accepted != CY_RESULT_OK) {
        return;
    }
    bool material_done = false;
    bool bake_done_first = false;
    for (u32 attempt = 0; attempt < 64 && !material_done; ++attempt) {
        const Event event = poll_once(services.composite, services.session, present);
        if (!present) {
            continue;
        }
        if (event.request == 11) {
            CY_CHECK(event.is(CY_SERVICE_EVENT_COMPLETED));
            material_done = true;
        } else if (event.request == 10 && is_terminal(event)) {
            bake_done_first = true;
        }
    }
    CY_CHECK(material_done);
    CY_CHECK_FALSE(bake_done_first);
    const std::vector<Event> rest = drain(services.composite, services.session, 10);
    CY_REQUIRE_FALSE(rest.empty());
    CY_CHECK((!rest.empty() && rest.back().is(CY_SERVICE_EVENT_COMPLETED)));
    CyServiceSession_T* const child =
        services.composite.child_session(services.session, services.navigation);
    CY_CHECK(NavigationService::mesh(child, kWorld) != nullptr);
}

CY_TEST_CASE("composite: capabilities wait for a busy child and a route table refuses misuse") {
    Services services;
    square_world(services.seam);
    CY_REQUIRE_EQ(submit(services.composite, services.session, 30, "navigation.bake",
                         bake_request(bake_settings())),
                  CY_RESULT_OK);
    CY_REQUIRE_EQ(submit(services.composite, services.session, 31, "capabilities.get"),
                  CY_RESULT_OK);
    bool bake_ended = false;
    bool merged_after_bake = false;
    for (u32 attempt = 0; attempt < 64; ++attempt) {
        bool present = false;
        const Event event = poll_once(services.composite, services.session, present);
        if (present && event.request == 30 && is_terminal(event)) {
            bake_ended = true;
        }
        if (present && event.request == 31) {
            CY_CHECK(event.is(CY_SERVICE_EVENT_COMPLETED));
            u64 features = 0;
            CY_CHECK(has(capability_names(event, features), "navigation.bake"));
            merged_after_bake = bake_ended;
            break;
        }
    }
    CY_CHECK(merged_after_bake);

    CompositeEditorService table(allocator());
    CY_CHECK(table.route("material.", services.material).has_value());
    CY_CHECK_FALSE(table.route("material.", services.navigation).has_value());
    CY_CHECK_FALSE(table.route("", services.navigation).has_value());
    CyServiceSession open = nullptr;
    CY_REQUIRE_EQ(table.open(&open), CY_RESULT_OK);
    CY_CHECK_FALSE(table.route("navigation.", services.navigation).has_value());
    table.close(open);
}
