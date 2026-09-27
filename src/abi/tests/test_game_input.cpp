// SPDX-License-Identifier: MIT
// ABI 1.3's `input_*` entries, against a fake `InputBackend`. `add-swift-game-api`.
//
// OWNER: implementer A. What is proven here is the boundary — the order of the checks, the phase
// each entry refuses, what reaches the backend, and what the caller's out-parameter looks like
// afterwards. What the answers MEAN is the adapter's, and is proven against a real input server in
// src/game_backend/tests/test_input_backend.cpp.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/input.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstddef>
#include <cstring>

namespace {

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

/// Answers from fields a case sets, and records what it was asked.
class FakeInput final : public cy::abi::game::InputBackend {
public:
    CyResult answer = CY_RESULT_OK;
    int calls = 0;
    const char* last_name = nullptr;
    cy::u32 last_user = 0;
    CyInputAction last_action = CY_INPUT_ACTION_INVALID;
    CyInputContext last_context = CY_INPUT_CONTEXT_NULL;
    cy::i32 last_priority = 0;
    cy::u32 seen_state_size = 0;
    cy::u32 seen_pointer_size = 0;

    CyResult find_action(const char* name, CyInputAction& out_action) noexcept override {
        ++calls;
        last_name = name;
        if (answer != CY_RESULT_OK) {
            return cy::abi::report(answer, "fake: no such action");
        }
        out_action = 7;
        return CY_RESULT_OK;
    }
    CyResult action_state(cy::u32 user, CyInputAction action,
                          CyInputActionState& out_state) noexcept override {
        ++calls;
        last_user = user;
        last_action = action;
        seen_state_size = out_state.struct_size;
        if (answer != CY_RESULT_OK) {
            return cy::abi::report(answer, "fake: refused");
        }
        out_state.flags = CY_INPUT_ACTION_PRESSED | CY_INPUT_ACTION_JUST_PRESSED;
        out_state.value[0] = 1.0F;
        out_state.press_count = 1;
        out_state.release_count = 1;
        out_state.tick = 99;
        return CY_RESULT_OK;
    }
    CyResult pointer(cy::u32 user, CyInputPointer& out_pointer) noexcept override {
        ++calls;
        last_user = user;
        seen_pointer_size = out_pointer.struct_size;
        out_pointer.flags = CY_INPUT_POINTER_PRESENT;
        out_pointer.buttons = CY_INPUT_BUTTON_LEFT;
        out_pointer.position[0] = 640.0F;
        out_pointer.position[1] = 360.0F;
        out_pointer.wheel[1] = 2.0F;
        return answer == CY_RESULT_OK ? CY_RESULT_OK : cy::abi::report(answer, "fake: refused");
    }
    CyResult modifiers(cy::u32 user, cy::u32& out_modifiers) noexcept override {
        ++calls;
        last_user = user;
        out_modifiers = CY_INPUT_MOD_SHIFT | CY_INPUT_MOD_CTRL;
        return answer == CY_RESULT_OK ? CY_RESULT_OK : cy::abi::report(answer, "fake: refused");
    }
    CyResult find_context(const char* name, CyInputContext& out_context) noexcept override {
        ++calls;
        last_name = name;
        if (answer != CY_RESULT_OK) {
            return cy::abi::report(answer, "fake: no such context");
        }
        out_context = 0x100000002ULL;
        return CY_RESULT_OK;
    }
    CyResult push_context(cy::u32 user, CyInputContext context,
                          cy::i32 priority) noexcept override {
        ++calls;
        last_user = user;
        last_context = context;
        last_priority = priority;
        return answer == CY_RESULT_OK ? CY_RESULT_OK : cy::abi::report(answer, "fake: refused");
    }
    CyResult pop_context(cy::u32 user, CyInputContext context) noexcept override {
        ++calls;
        last_user = user;
        last_context = context;
        return answer == CY_RESULT_OK ? CY_RESULT_OK : cy::abi::report(answer, "fake: refused");
    }
};

/// A host with the fake bound, in a chosen phase.
struct Bench {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    FakeInput input;

    explicit Bench(CyPhase phase = CY_PHASE_NONE) noexcept {
        host.game.input = &input;
        host.game.clock.phase = phase;
        cy::abi::clear_last_error();
    }
    CyEngine engine() noexcept { return &host; }
};

CyInputActionState whole_state() noexcept {
    CyInputActionState state{};
    state.struct_size = sizeof(CyInputActionState);
    return state;
}

}  // namespace

CY_TEST_CASE("input: find_action resolves a name and leaves the output alone on failure") {
    Bench bench;
    CyInputAction action = CY_INPUT_ACTION_INVALID;
    CY_CHECK_EQ(table().input_find_action(bench.engine(), "select", &action), CY_RESULT_OK);
    CY_CHECK_EQ(action, 7U);
    CY_CHECK(std::strcmp(bench.input.last_name, "select") == 0);
    CY_CHECK_EQ(cy::abi::last_error_code(), CY_RESULT_OK);

    bench.input.answer = CY_RESULT_NOT_FOUND;
    action = 123;
    CY_CHECK_EQ(table().input_find_action(bench.engine(), "nope", &action), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(action, 123U);
    CY_CHECK_EQ(cy::abi::last_error_code(), CY_RESULT_NOT_FOUND);
}

CY_TEST_CASE("input: every entry refuses a null engine, null pointers and a missing backend") {
    const CyInterface& iface = table();
    CyInputAction action = 0;
    CyInputActionState state = whole_state();
    CyInputPointer pointer{};
    cy::u32 modifiers = 0;
    CyInputContext context = 0;

    CY_CHECK_EQ(iface.input_find_action(nullptr, "a", &action), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.input_action_state(nullptr, 0, 0, &state), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.input_pointer(nullptr, 0, &pointer), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.input_push_context(nullptr, 0, 1, 0), CY_RESULT_INVALID_ARGUMENT);

    Bench bench;
    CY_CHECK_EQ(iface.input_find_action(bench.engine(), nullptr, &action),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.input_find_action(bench.engine(), "a", nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.input_action_state(bench.engine(), 0, 0, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.input_action_state_by_name(bench.engine(), 0, nullptr, &state),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.input_pointer(bench.engine(), 0, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.input_modifiers(bench.engine(), 0, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.input_find_context(bench.engine(), "menu", nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(bench.input.calls, 0);

    bench.host.game.input = nullptr;
    CY_CHECK_EQ(iface.input_find_action(bench.engine(), "a", &action), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(iface.input_action_state(bench.engine(), 0, 0, &state), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(iface.input_action_state_by_name(bench.engine(), 0, "a", &state),
                CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(iface.input_pointer(bench.engine(), 0, &pointer), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(iface.input_modifiers(bench.engine(), 0, &modifiers), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(iface.input_find_context(bench.engine(), "m", &context), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(iface.input_push_context(bench.engine(), 0, 1, 0), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(iface.input_pop_context(bench.engine(), 0, 1), CY_RESULT_UNAVAILABLE);
}

CY_TEST_CASE("input: action state is callable in every phase and copied whole") {
    for (const CyPhase phase : {CY_PHASE_NONE, CY_PHASE_FIXED_UPDATE, CY_PHASE_FRAME_UPDATE}) {
        Bench bench(phase);
        CyInputActionState state = whole_state();
        CY_CHECK_EQ(table().input_action_state(bench.engine(), 2, 5, &state), CY_RESULT_OK);
        CY_CHECK_EQ(bench.input.last_user, 2U);
        CY_CHECK_EQ(bench.input.last_action, 5U);
        // The backend always sees a whole struct of this build's size.
        CY_CHECK_EQ(bench.input.seen_state_size, sizeof(CyInputActionState));
        CY_CHECK_EQ(state.struct_size, sizeof(CyInputActionState));
        CY_CHECK_EQ(state.flags, CY_INPUT_ACTION_PRESSED | CY_INPUT_ACTION_JUST_PRESSED);
        CY_CHECK_EQ(state.press_count, 1U);
        CY_CHECK_EQ(state.release_count, 1U);
        CY_CHECK_EQ(state.tick, 99U);
    }
}

CY_TEST_CASE("input: action state reports the backend's domain errors and keeps the output") {
    Bench bench;
    for (const CyResult code : {CY_RESULT_NOT_FOUND, CY_RESULT_OUT_OF_RANGE}) {
        bench.input.answer = code;
        CyInputActionState state = whole_state();
        state.tick = 5;
        CY_CHECK_EQ(table().input_action_state(bench.engine(), 9, 0, &state), code);
        CY_CHECK_EQ(state.tick, 5U);
        CY_CHECK_EQ(state.flags, 0U);
    }
}

CY_TEST_CASE("input: a caller compiled against a shorter action state gets only its prefix") {
    Bench bench;
    CyInputActionState state{};
    std::memset(&state, 0xCD, sizeof(state));
    const auto prefix = static_cast<cy::u32>(offsetof(CyInputActionState, press_count));
    state.struct_size = prefix;
    CY_CHECK_EQ(table().input_action_state(bench.engine(), 0, 0, &state), CY_RESULT_OK);
    CY_CHECK_EQ(state.struct_size, prefix);
    CY_CHECK_EQ(state.value[0], 1.0F);
    CY_CHECK_EQ(state.press_count, 0xCDCDU);  // past the caller's struct: untouched
    CY_CHECK_EQ(bench.input.seen_state_size, sizeof(CyInputActionState));

    // A size too small to hold itself is malformed; the backend is not asked.
    state.struct_size = 2;
    const int calls = bench.input.calls;
    CY_CHECK_EQ(table().input_action_state(bench.engine(), 0, 0, &state),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(bench.input.calls, calls);
}

CY_TEST_CASE("input: action state by name is a find and a read, stopping at the find") {
    Bench bench(CY_PHASE_FIXED_UPDATE);
    CyInputActionState state = whole_state();
    CY_CHECK_EQ(table().input_action_state_by_name(bench.engine(), 1, "select", &state),
                CY_RESULT_OK);
    CY_CHECK(std::strcmp(bench.input.last_name, "select") == 0);
    CY_CHECK_EQ(bench.input.last_action, 7U);  // what find_action answered
    CY_CHECK_EQ(bench.input.last_user, 1U);
    CY_CHECK_EQ(state.press_count, 1U);

    bench.input.answer = CY_RESULT_NOT_FOUND;
    bench.input.calls = 0;
    state = whole_state();
    CY_CHECK_EQ(table().input_action_state_by_name(bench.engine(), 1, "nope", &state),
                CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(bench.input.calls, 1);  // the find, and no read
    CY_CHECK_EQ(state.press_count, 0U);
}

CY_TEST_CASE("input: the pointer and the modifiers are device state, refused in a fixed step") {
    Bench fixed(CY_PHASE_FIXED_UPDATE);
    CyInputPointer pointer{};
    cy::u32 modifiers = 0xFFU;
    CY_CHECK_EQ(table().input_pointer(fixed.engine(), 0, &pointer), CY_RESULT_PERMISSION_DENIED);
    CY_CHECK(std::strstr(cy::abi::last_error_message(), "input_pointer") != nullptr);
    CY_CHECK_EQ(table().input_modifiers(fixed.engine(), 0, &modifiers),
                CY_RESULT_PERMISSION_DENIED);
    CY_CHECK(std::strstr(cy::abi::last_error_message(), "input_modifiers") != nullptr);
    CY_CHECK_EQ(fixed.input.calls, 0);
    CY_CHECK_EQ(modifiers, 0xFFU);

    for (const CyPhase phase : {CY_PHASE_NONE, CY_PHASE_FRAME_UPDATE}) {
        Bench bench(phase);
        pointer = CyInputPointer{};
        CY_CHECK_EQ(table().input_pointer(bench.engine(), 3, &pointer), CY_RESULT_OK);
        CY_CHECK_EQ(bench.input.last_user, 3U);
        CY_CHECK_EQ(bench.input.seen_pointer_size, sizeof(CyInputPointer));
        CY_CHECK_EQ(pointer.struct_size, sizeof(CyInputPointer));  // zero asked for this size
        CY_CHECK_EQ(pointer.buttons, CY_INPUT_BUTTON_LEFT);
        CY_CHECK_EQ(pointer.position[0], 640.0F);
        CY_CHECK_EQ(pointer.wheel[1], 2.0F);
        CY_CHECK_EQ(table().input_modifiers(bench.engine(), 0, &modifiers), CY_RESULT_OK);
        CY_CHECK_EQ(modifiers, CY_INPUT_MOD_SHIFT | CY_INPUT_MOD_CTRL);
    }
}

CY_TEST_CASE("input: a pointer caller compiled against a shorter struct gets only its prefix") {
    Bench bench(CY_PHASE_FRAME_UPDATE);
    CyInputPointer pointer{};
    std::memset(&pointer, 0xAB, sizeof(pointer));
    const auto prefix = static_cast<cy::u32>(offsetof(CyInputPointer, position));
    pointer.struct_size = prefix;
    CY_CHECK_EQ(table().input_pointer(bench.engine(), 0, &pointer), CY_RESULT_OK);
    CY_CHECK_EQ(pointer.struct_size, prefix);
    CY_CHECK_EQ(pointer.buttons, CY_INPUT_BUTTON_LEFT);
    cy::u32 untouched = 0;
    std::memcpy(&untouched, &pointer.position[0], sizeof(untouched));
    CY_CHECK_EQ(untouched, 0xABABABABU);
}

CY_TEST_CASE("input: contexts are found, pushed and popped in every phase") {
    for (const CyPhase phase : {CY_PHASE_NONE, CY_PHASE_FIXED_UPDATE, CY_PHASE_FRAME_UPDATE}) {
        Bench bench(phase);
        CyInputContext context = CY_INPUT_CONTEXT_NULL;
        CY_CHECK_EQ(table().input_find_context(bench.engine(), "menu", &context), CY_RESULT_OK);
        CY_CHECK_EQ(context, 0x100000002ULL);
        CY_CHECK_EQ(table().input_push_context(bench.engine(), 1, context, -4), CY_RESULT_OK);
        CY_CHECK_EQ(bench.input.last_user, 1U);
        CY_CHECK_EQ(bench.input.last_context, context);
        CY_CHECK_EQ(bench.input.last_priority, -4);
        CY_CHECK_EQ(table().input_pop_context(bench.engine(), 1, context), CY_RESULT_OK);
    }
}

CY_TEST_CASE("input: context errors come back as the backend reported them") {
    Bench bench;
    CyInputContext context = 55;
    bench.input.answer = CY_RESULT_NOT_FOUND;
    CY_CHECK_EQ(table().input_find_context(bench.engine(), "gone", &context), CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(context, 55U);
    bench.input.answer = CY_RESULT_ALREADY_EXISTS;
    CY_CHECK_EQ(table().input_push_context(bench.engine(), 0, 55, 0), CY_RESULT_ALREADY_EXISTS);
    bench.input.answer = CY_RESULT_OUT_OF_RANGE;
    CY_CHECK_EQ(table().input_pop_context(bench.engine(), 40, 55), CY_RESULT_OUT_OF_RANGE);
    CY_CHECK_EQ(cy::abi::last_error_code(), CY_RESULT_OUT_OF_RANGE);

    // A success after a failure clears the last error, so a stale message is never read as its.
    bench.input.answer = CY_RESULT_OK;
    CY_CHECK_EQ(table().input_pop_context(bench.engine(), 0, 55), CY_RESULT_OK);
    CY_CHECK_EQ(cy::abi::last_error_code(), CY_RESULT_OK);
}
