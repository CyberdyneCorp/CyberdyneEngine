// SPDX-License-Identifier: MIT
// ABI 1.5's rigid-body thunks against a fake backend. `add-swift-m12-gaps`.
//
// What the thunks own: the phase lists (writes `[N F]`, the read `[N F U]`), UNAVAILABLE with no
// backend, the null and finiteness checks, and the optional halves of `set_velocity`,
// `apply_impulse` and `get_velocity`. The server's side — motion types, sleeping, the step guard —
// is integration.game_backend_bodies over the reference server.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/physics.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstring>
#include <limits>

namespace {

using cy::f32;
using cy::u32;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

class FakeBodies final : public cy::abi::game::PhysicsBodyBackend {
public:
    u32 calls = 0;
    CyEntity entity = CY_ENTITY_NULL;
    f32 vector[3] = {};
    bool had_point = false;
    bool had_linear = false;
    bool had_angular = false;

    CyResult apply_force(CyEntity target, const f32* force) noexcept override {
        return take(target, force);
    }
    CyResult apply_impulse(CyEntity target, const f32* impulse,
                           const f32* point) noexcept override {
        had_point = point != nullptr;
        return take(target, impulse);
    }
    CyResult apply_torque(CyEntity target, const f32* torque) noexcept override {
        return take(target, torque);
    }
    CyResult set_velocity(CyEntity target, const f32* linear,
                          const f32* angular) noexcept override {
        had_linear = linear != nullptr;
        had_angular = angular != nullptr;
        return take(target, linear != nullptr ? linear : angular);
    }
    CyResult velocity(CyEntity target, f32* out_linear, f32* out_angular) const noexcept override {
        if (target != 9U) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "the entity owns no physics body");
        }
        out_linear[0] = 1.0F;
        out_linear[1] = 2.0F;
        out_linear[2] = 3.0F;
        out_angular[0] = 4.0F;
        out_angular[1] = 5.0F;
        out_angular[2] = 6.0F;
        return CY_RESULT_OK;
    }

private:
    CyResult take(CyEntity target, const f32* xyz) noexcept {
        ++calls;
        entity = target;
        // The entry refuses a call with no vector before it reaches a backend; a fake that is
        // reached with none anyway records zeros rather than reading through a null pointer.
        if (xyz == nullptr) {
            std::memset(vector, 0, sizeof(vector));
        } else {
            std::memcpy(vector, xyz, sizeof(vector));
        }
        return CY_RESULT_OK;
    }
};

struct Fixture {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    FakeBodies bodies;
    Fixture() noexcept { host.game.bodies = &bodies; }
    [[nodiscard]] CyEngine engine() noexcept { return &host; }
};

}  // namespace

CY_TEST_CASE("the body writes are simulation: allowed in N and F, refused in a frame update") {
    Fixture fixture;
    const CyInterface& iface = table();
    const float push[3] = {0.0F, 10.0F, 0.0F};
    for (const CyPhase phase : {CY_PHASE_NONE, CY_PHASE_FIXED_UPDATE}) {
        const cy::abi::game::PhaseScope scope(fixture.host.game.clock, phase);
        CY_CHECK_EQ(iface.physics_apply_force(fixture.engine(), 9, push), CY_RESULT_OK);
        CY_CHECK_EQ(iface.physics_apply_impulse(fixture.engine(), 9, push, nullptr), CY_RESULT_OK);
        CY_CHECK_EQ(iface.physics_apply_torque(fixture.engine(), 9, push), CY_RESULT_OK);
        CY_CHECK_EQ(iface.physics_set_velocity(fixture.engine(), 9, push, nullptr), CY_RESULT_OK);
    }
    CY_CHECK_EQ(fixture.bodies.calls, 8U);
    CY_CHECK_EQ(fixture.bodies.entity, 9U);
    CY_CHECK_EQ(fixture.bodies.vector[1], 10.0F);

    const cy::abi::game::PhaseScope frame(fixture.host.game.clock, CY_PHASE_FRAME_UPDATE);
    CY_CHECK_EQ(iface.physics_apply_force(fixture.engine(), 9, push), CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(iface.physics_apply_impulse(fixture.engine(), 9, push, nullptr),
                CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(iface.physics_apply_torque(fixture.engine(), 9, push), CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(iface.physics_set_velocity(fixture.engine(), 9, push, push),
                CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(fixture.bodies.calls, 8U);
    // The read answers in a frame too.
    float linear[3] = {};
    CY_CHECK_EQ(iface.physics_get_velocity(fixture.engine(), 9, linear, nullptr), CY_RESULT_OK);
    CY_CHECK_EQ(linear[2], 3.0F);
}

CY_TEST_CASE("the body thunks refuse a malformed call before the backend sees it") {
    Fixture fixture;
    const CyInterface& iface = table();
    const float good[3] = {1.0F, 0.0F, 0.0F};
    const float nan[3] = {std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F};
    const float inf[3] = {0.0F, std::numeric_limits<float>::infinity(), 0.0F};
    CY_CHECK_EQ(iface.physics_apply_force(fixture.engine(), 9, nan), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.physics_apply_force(fixture.engine(), 9, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.physics_apply_force(fixture.engine(), CY_ENTITY_NULL, good),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.physics_apply_torque(fixture.engine(), 9, inf), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.physics_apply_impulse(fixture.engine(), 9, good, nan),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.physics_set_velocity(fixture.engine(), 9, nullptr, inf),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.physics_get_velocity(fixture.engine(), CY_ENTITY_NULL, nullptr, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.physics_apply_force(nullptr, 9, good), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(fixture.bodies.calls, 0U);

    fixture.host.game.bodies = nullptr;
    CY_CHECK_EQ(iface.physics_apply_force(fixture.engine(), 9, good), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(iface.physics_get_velocity(fixture.engine(), 9, nullptr, nullptr),
                CY_RESULT_UNAVAILABLE);
}

CY_TEST_CASE("the optional halves reach the backend as null, and two nulls change nothing") {
    Fixture fixture;
    const CyInterface& iface = table();
    const float spin[3] = {0.0F, 3.0F, 0.0F};
    const float point[3] = {1.0F, 1.0F, 1.0F};
    CY_CHECK_EQ(iface.physics_set_velocity(fixture.engine(), 9, nullptr, spin), CY_RESULT_OK);
    CY_CHECK_FALSE(fixture.bodies.had_linear);
    CY_CHECK(fixture.bodies.had_angular);
    CY_CHECK_EQ(iface.physics_set_velocity(fixture.engine(), 9, nullptr, nullptr), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.bodies.calls, 1U);
    CY_CHECK_EQ(iface.physics_apply_impulse(fixture.engine(), 9, spin, point), CY_RESULT_OK);
    CY_CHECK(fixture.bodies.had_point);

    // `get_velocity` writes whichever outputs it was given and answers the backend's NOT_FOUND.
    float angular[3] = {};
    CY_CHECK_EQ(iface.physics_get_velocity(fixture.engine(), 9, nullptr, angular), CY_RESULT_OK);
    CY_CHECK_EQ(angular[0], 4.0F);
    CY_CHECK_EQ(iface.physics_get_velocity(fixture.engine(), 8, nullptr, angular),
                CY_RESULT_NOT_FOUND);
}
