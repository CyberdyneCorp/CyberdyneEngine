// SPDX-License-Identifier: MIT
// ABI 1.5's character thunks against a fake backend. `add-swift-m12-gaps`.
//
// What the thunks own: create and destroy in `[N F]`, move in `[F]` only and with the clock's
// fixed delta, state in every phase; the description's checks; and `struct_size` both ways. The
// controller itself — slopes, steps, pushes — is cy::physics' (integration.physics_behaviour), and
// the adapter over it is integration.game_backend_character.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/character.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstddef>
#include <limits>

namespace {

using cy::f32;
using cy::u32;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

class FakeCharacters final : public cy::abi::game::CharacterBackend {
public:
    u32 created = 0;
    u32 moves = 0;
    f32 last_delta = 0.0F;
    CyCharacterDesc last_desc{};
    CyCharacterInput last_input{};

    CyResult create(CyEntity, const CyCharacterDesc& desc) noexcept override {
        ++created;
        last_desc = desc;
        return CY_RESULT_OK;
    }
    CyResult destroy(CyEntity) noexcept override { return CY_RESULT_OK; }
    CyResult move(CyEntity, const CyCharacterInput& input, f32 delta) noexcept override {
        ++moves;
        last_input = input;
        last_delta = delta;
        return CY_RESULT_OK;
    }
    CyResult state(CyEntity, CyCharacterState& out) const noexcept override {
        out.ground = CY_GROUND_GROUNDED;
        out.flags = CY_CHARACTER_STEPPED_UP;
        out.ground_entity = 3;
        out.position[0] = 1.5F;
        out.platform_velocity[2] = 7.0F;
        return CY_RESULT_OK;
    }
};

struct Fixture {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    FakeCharacters characters;
    Fixture() noexcept {
        host.game.characters = &characters;
        host.game.clock.fixed_delta = 1.0 / 50.0;
    }
    [[nodiscard]] CyEngine engine() noexcept { return &host; }
};

}  // namespace

CY_TEST_CASE("a character moves only in a fixed step, by the clock's fixed delta") {
    Fixture fixture;
    const CyInterface& iface = table();
    CyCharacterDesc desc{};
    desc.struct_size = sizeof(desc);
    CY_CHECK_EQ(iface.character_create(fixture.engine(), 4, &desc), CY_RESULT_OK);

    CyCharacterInput input{};
    input.struct_size = sizeof(input);
    input.desired_velocity[0] = 2.0F;
    input.flags = CY_CHARACTER_JUMP;
    input.jump_speed = 5.0F;
    CY_CHECK_EQ(iface.character_move(fixture.engine(), 4, &input), CY_RESULT_PERMISSION_DENIED);
    {
        const cy::abi::game::PhaseScope frame(fixture.host.game.clock, CY_PHASE_FRAME_UPDATE);
        CY_CHECK_EQ(iface.character_move(fixture.engine(), 4, &input), CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(iface.character_create(fixture.engine(), 5, &desc),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(iface.character_destroy(fixture.engine(), 4), CY_RESULT_PERMISSION_DENIED);
    }
    {
        const cy::abi::game::PhaseScope fixed(fixture.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_CHECK_EQ(iface.character_move(fixture.engine(), 4, &input), CY_RESULT_OK);
    }
    CY_CHECK_EQ(fixture.characters.moves, 1U);
    CY_CHECK_EQ(fixture.characters.last_delta, 1.0F / 50.0F);
    CY_CHECK_EQ(fixture.characters.last_input.desired_velocity[0], 2.0F);
    CY_CHECK_EQ(fixture.characters.last_input.flags, CY_CHARACTER_JUMP);
}

CY_TEST_CASE("a character description is checked, and a short one is read as zeros") {
    Fixture fixture;
    const CyInterface& iface = table();
    CyCharacterDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.radius = -1.0F;
    CY_CHECK_EQ(iface.character_create(fixture.engine(), 4, &desc), CY_RESULT_INVALID_ARGUMENT);
    desc.radius = std::numeric_limits<float>::quiet_NaN();
    CY_CHECK_EQ(iface.character_create(fixture.engine(), 4, &desc), CY_RESULT_INVALID_ARGUMENT);
    desc.radius = 0.4F;
    desc.layer = 32;
    CY_CHECK_EQ(iface.character_create(fixture.engine(), 4, &desc), CY_RESULT_INVALID_ARGUMENT);
    desc.layer = 0;
    desc.start.position[0] = std::numeric_limits<float>::infinity();
    CY_CHECK_EQ(iface.character_create(fixture.engine(), 4, &desc), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.character_create(fixture.engine(), CY_ENTITY_NULL, &desc),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(fixture.characters.created, 0U);

    // A caller that knows only the first four fields: the rest reach the backend as zero, which
    // the adapter reads as "the default".
    CyCharacterDesc short_desc{};
    short_desc.radius = 0.4F;
    short_desc.mass = 99.0F;  // beyond the declared size, so it must not be read
    short_desc.struct_size = static_cast<uint32_t>(offsetof(CyCharacterDesc, max_slope_radians));
    CY_CHECK_EQ(iface.character_create(fixture.engine(), 4, &short_desc), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.characters.last_desc.radius, 0.4F);
    CY_CHECK_EQ(fixture.characters.last_desc.mass, 0.0F);

    fixture.host.game.characters = nullptr;
    CY_CHECK_EQ(iface.character_create(fixture.engine(), 4, &desc), CY_RESULT_UNAVAILABLE);
}

CY_TEST_CASE("a character's state answers in every phase and writes only the caller's prefix") {
    Fixture fixture;
    const CyInterface& iface = table();
    for (const CyPhase phase : {CY_PHASE_NONE, CY_PHASE_FIXED_UPDATE, CY_PHASE_FRAME_UPDATE}) {
        const cy::abi::game::PhaseScope scope(fixture.host.game.clock, phase);
        CyCharacterState state{};
        state.struct_size = sizeof(state);
        CY_CHECK_EQ(iface.character_state(fixture.engine(), 4, &state), CY_RESULT_OK);
        CY_CHECK_EQ(state.ground, CY_GROUND_GROUNDED);
        CY_CHECK_EQ(state.ground_entity, 3U);
        CY_CHECK_EQ(state.position[0], 1.5F);
        CY_CHECK_EQ(state.platform_velocity[2], 7.0F);
    }
    CyCharacterState short_state{};
    short_state.platform_velocity[2] = -1.0F;
    short_state.struct_size = static_cast<uint32_t>(offsetof(CyCharacterState, ground_entity));
    CY_CHECK_EQ(iface.character_state(fixture.engine(), 4, &short_state), CY_RESULT_OK);
    CY_CHECK_EQ(short_state.flags, CY_CHARACTER_STEPPED_UP);
    CY_CHECK_EQ(short_state.platform_velocity[2], -1.0F);
    CY_CHECK_EQ(short_state.struct_size,
                static_cast<uint32_t>(offsetof(CyCharacterState, ground_entity)));
    CyCharacterState malformed{};
    malformed.struct_size = 2U;
    CY_CHECK_EQ(iface.character_state(fixture.engine(), 4, &malformed), CY_RESULT_INVALID_ARGUMENT);
}
