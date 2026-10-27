// SPDX-License-Identifier: MIT
// ABI 1.15's decal thunks against a fake backend.
//
// What the thunks own: the phase rule — `decal_find_material` `[N F U]`, `decal_spawn` and
// `decal_status` `[N U]` — the argument and range checks, `struct_size` both ways, and UNAVAILABLE
// with no budget bound. The budget itself is integration.game_backend_decals's.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/decals.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstddef>
#include <cstring>
#include <limits>

namespace {

using cy::u32;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

constexpr CyDecalMaterial kScorch = 3;

class FakeDecals final : public cy::abi::game::DecalsBackend {
public:
    CyDecalDesc spawned{};
    u32 spawns = 0;

    CyResult find_material(const char* name, CyDecalMaterial& out) const noexcept override {
        if (std::strcmp(name, "impact.scorch") != 0) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no such decal material");
        }
        out = kScorch;
        return CY_RESULT_OK;
    }
    CyResult spawn(const CyDecalDesc& desc, CyDecal& out) noexcept override {
        if (desc.material != kScorch) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "a material the host did not hand out");
        }
        spawned = desc;
        ++spawns;
        out = 40U + spawns;
        return CY_RESULT_OK;
    }
    CyResult status(CyDecalStatus& out) const noexcept override {
        out.live = 12;
        out.capacity = 256;
        out.spawned = 300;
        out.evicted = 44;
        return CY_RESULT_OK;
    }
};

struct Bound {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    FakeDecals decals;
    Bound() { host.game.decals = &decals; }
    [[nodiscard]] CyEngine engine() noexcept { return &host; }
};

[[nodiscard]] CyDecalDesc mark() noexcept {
    CyDecalDesc made{};
    made.struct_size = sizeof(made);
    made.material = kScorch;
    made.pose.position[0] = 1.0F;
    made.pose.rotation[3] = 1.0F;
    made.half_extent[0] = 0.1F;
    made.half_extent[1] = 0.1F;
    made.half_extent[2] = 0.05F;
    made.importance = 0.5F;
    return made;
}

}  // namespace

CY_TEST_CASE("decals: without a bound budget every entry is UNAVAILABLE") {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    CyDecalMaterial material = CY_DECAL_MATERIAL_NULL;
    CY_CHECK_EQ(table().decal_find_material(&host, "impact.scorch", &material),
                CY_RESULT_UNAVAILABLE);
    const CyDecalDesc desc = mark();
    CyDecal decal = CY_DECAL_NULL;
    CY_CHECK_EQ(table().decal_spawn(&host, &desc, &decal), CY_RESULT_UNAVAILABLE);
    CyDecalStatus status{};
    status.struct_size = sizeof(status);
    CY_CHECK_EQ(table().decal_status(&host, &status), CY_RESULT_UNAVAILABLE);
}

CY_TEST_CASE("decals: a material resolves in every phase; a spawn only outside a fixed step") {
    Bound bound;
    CyDecalMaterial material = CY_DECAL_MATERIAL_NULL;
    CyDecalDesc desc = mark();
    CyDecal decal = CY_DECAL_NULL;
    {
        cy::abi::game::PhaseScope fixed(bound.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_REQUIRE_EQ(table().decal_find_material(bound.engine(), "impact.scorch", &material),
                      CY_RESULT_OK);
        CY_CHECK_EQ(table().decal_spawn(bound.engine(), &desc, &decal),
                    CY_RESULT_PERMISSION_DENIED);
        CyDecalStatus status{};
        status.struct_size = sizeof(status);
        CY_CHECK_EQ(table().decal_status(bound.engine(), &status), CY_RESULT_PERMISSION_DENIED);
    }
    CY_CHECK_EQ(material, kScorch);
    CY_CHECK_EQ(bound.decals.spawns, 0U);
    CyDecalMaterial missing = 9;
    CY_CHECK_EQ(table().decal_find_material(bound.engine(), "blood", &missing),
                CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(missing, 9U);

    cy::abi::game::PhaseScope frame(bound.host.game.clock, CY_PHASE_FRAME_UPDATE);
    CY_REQUIRE_EQ(table().decal_spawn(bound.engine(), &desc, &decal), CY_RESULT_OK);
    CY_CHECK_EQ(decal, 41U);
    CY_CHECK_EQ(bound.decals.spawned.half_extent[2], 0.05F);
    desc.material = 5;
    CY_CHECK_EQ(table().decal_spawn(bound.engine(), &desc, &decal), CY_RESULT_NOT_FOUND);
    // A status written into an older, shorter struct stops at its end.
    CyDecalStatus status{};
    status.struct_size = offsetof(CyDecalStatus, spawned);
    CY_REQUIRE_EQ(table().decal_status(bound.engine(), &status), CY_RESULT_OK);
    CY_CHECK_EQ(status.live, 12U);
    CY_CHECK_EQ(status.capacity, 256U);
    CY_CHECK_EQ(status.spawned, 0U);
    CY_CHECK_EQ(status.struct_size, static_cast<u32>(offsetof(CyDecalStatus, spawned)));
}

CY_TEST_CASE("decals: a description is whole and in range before the backend sees it") {
    Bound bound;
    CyDecal decal = CY_DECAL_NULL;
    // The fades' zeros are the defaults, so an end of zero past a start of zero is in range.
    CyDecalDesc defaults = mark();
    CY_REQUIRE_EQ(table().decal_spawn(bound.engine(), &defaults, &decal), CY_RESULT_OK);
    CY_CHECK_EQ(bound.decals.spawned.fade_end, 0.0F);

    const u32 before = bound.decals.spawns;
    const auto refused = [&](const CyDecalDesc& bad) {
        CyDecal none = CY_DECAL_NULL;
        CY_CHECK_EQ(table().decal_spawn(bound.engine(), &bad, &none), CY_RESULT_INVALID_ARGUMENT);
        CY_CHECK_EQ(none, CY_DECAL_NULL);
    };
    CyDecalDesc bad = mark();
    bad.half_extent[2] = 0.0F;
    refused(bad);
    bad = mark();
    bad.half_extent[0] = -0.1F;
    refused(bad);
    bad = mark();
    bad.importance = 1.5F;
    refused(bad);
    bad = mark();
    bad.fade_angle = 2.0F;
    refused(bad);
    bad = mark();
    bad.fade_start = 30.0F;
    bad.fade_end = 10.0F;
    refused(bad);
    bad = mark();
    bad.fade_start = 50.0F;  // past the default end
    refused(bad);
    bad = mark();
    bad.pose.rotation[0] = std::numeric_limits<float>::quiet_NaN();
    refused(bad);
    bad = mark();
    bad.reserved0 = 1;
    refused(bad);
    bad = mark();
    bad.reserved[1] = 1;
    refused(bad);
    bad = mark();
    bad.struct_size = 2;
    refused(bad);
    CY_CHECK_EQ(table().decal_spawn(bound.engine(), nullptr, &decal), CY_RESULT_INVALID_ARGUMENT);
    const CyDecalDesc good = mark();
    CY_CHECK_EQ(table().decal_spawn(bound.engine(), &good, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(bound.decals.spawns, before);
}
