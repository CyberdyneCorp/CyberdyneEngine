// SPDX-License-Identifier: MIT
// cy/abi/game/services.h — what is behind ABI 1.3's game-service entries. `add-swift-game-api`.
//
// The C header declares the entries; this is where each one finds the subsystem that answers it.
// The shape is the one `EditorServiceBackend` already set at 1.2: the ABI layer owns the THUNKS
// (argument checks, phase checks, `struct_size` handling) and knows nothing about any server; the
// subsystem is reached through an abstract BACKEND the embedder binds on `CyEngine_T::game`. So
// `cy_abi` gains no dependency on input, physics, the camera, navigation, audio or the scene, and a
// test binds a fake backend and exercises the boundary with no server at all.
//
//   entry (cy_abi.h)  ->  thunk (src/abi/src/game/<group>_thunks.cpp)  ->  backend
//   (cy/abi/game/<service>.h)
//                                                                  ->  adapter (src/game_backend/)
//
// --- WHAT A THUNK DOES BEFORE A BACKEND SEES ANYTHING ------------------------------------------
//
//   1. a null engine is INVALID_ARGUMENT;
//   2. `require_phase` refuses a phase the entry's `[N F U]` list does not name —
//   PERMISSION_DENIED;
//   3. an unbound backend is UNAVAILABLE;
//   4. required pointers are checked, and every sized struct is normalised to this build's size
//   with
//      `read_sized` / `write_sized`, so a backend always sees a whole struct;
//   5. while CY_TIME_RESIMULATING is set, presentation writes (audio, camera) succeed and do
//   nothing;
//   6. a successful structural call (spawn, destroy, first agent configure) bumps the bound world's
//      epoch, so every `CyBorrow` taken before it reads as stale.
//
// Backends therefore validate DOMAIN facts only — an unknown name, a stale handle, an entity that
// is not an agent — and report them with `cy::abi::report`, returning the code.
//
// --- THREADING -------------------------------------------------------------------------------
//
// `GameClock` is written by the embedder on the game thread, at stage boundaries, before any job
// for that stage is dispatched; the dispatch is the happens-before edge a worker's read needs.
// Nothing here is written by a worker.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>

#include <cstring>

namespace cy::abi::game {

class InputBackend;
class CameraBackend;
class PhysicsQueryBackend;
class NavigationBackend;
class AudioBackend;
class SpawnBackend;
class SceneBackend;
class PhysicsBodyBackend;
class CharacterBackend;

// --- Phases --------------------------------------------------------------------------------------

/// One bit per `CyPhase`, so an entry's `[N F U]` list is one constant.
[[nodiscard]] constexpr u32 phase_bit(CyPhase phase) noexcept {
    return 1U << static_cast<u32>(phase);
}

/// `N` in an entry's `[N F U]` list: module initialisation, a frame boundary, a tool.
inline constexpr u32 kPhaseNone = phase_bit(CY_PHASE_NONE);
/// `F`: a fixed simulation step, where only deterministic answers are allowed.
inline constexpr u32 kPhaseFixed = phase_bit(CY_PHASE_FIXED_UPDATE);
/// `U`: the variable-rate frame, where device and presentation state may be read.
inline constexpr u32 kPhaseFrame = phase_bit(CY_PHASE_FRAME_UPDATE);
/// `[N F U]`: every phase, for the entries no phase refuses.
inline constexpr u32 kPhaseAny = kPhaseNone | kPhaseFixed | kPhaseFrame;

/// The clock `time_get` reports and the phase every thunk checks. Written by the embedder — the
/// behaviour runtime and the frame loop — and only read by the thunks.
struct GameClock {
    CyPhase phase = CY_PHASE_NONE;
    u64 tick = 0;
    f64 fixed_delta = 1.0 / 60.0;
    f64 frame_delta = 0.0;
    f64 interpolation = 0.0;
    u32 flags = 0;  ///< CY_TIME_*

    [[nodiscard]] bool resimulating() const noexcept {
        return (flags & CY_TIME_RESIMULATING) != 0U;
    }
};

/// Everything the 1.3 entries reach. Each pointer is borrowed: the embedder owns the backend and
/// keeps it alive while it is bound. A null backend makes its entries answer UNAVAILABLE, which is
/// what a dedicated server with no audio device, or a tool with no camera, looks like.
struct GameServices {
    InputBackend* input = nullptr;
    CameraBackend* camera = nullptr;
    PhysicsQueryBackend* physics = nullptr;
    NavigationBackend* navigation = nullptr;
    AudioBackend* audio = nullptr;
    SpawnBackend* spawn = nullptr;
    /// ABI 1.5: `node_find`, `physics_apply_*` / `physics_*_velocity` and `character_*`.
    SceneBackend* scene = nullptr;
    PhysicsBodyBackend* bodies = nullptr;
    CharacterBackend* characters = nullptr;
    GameClock clock;
};

/// Refuse a call made in a phase `allowed` does not contain. Reports PERMISSION_DENIED naming
/// `entry` and the current phase, and returns it; returns CY_RESULT_OK otherwise, touching no
/// last-error state. Holds in every build configuration.
[[nodiscard]] CyResult require_phase(const GameServices& services, u32 allowed,
                                     const char* entry) noexcept;

/// The phase for the duration of a scope, restored afterwards. What the behaviour runtime wraps
/// `fixed_update` and `frame_update` dispatch in, so a callback can never leave the phase set.
/// The phase a scheduler stage runs in: CY_STAGE_PRE_SIMULATION to CY_STAGE_POST_SIMULATION are a
/// fixed step (F), CY_STAGE_FRAME to CY_STAGE_UI the frame (U), and CY_STAGE_RENDER is neither (N).
/// cy_abi.h states the mapping above `CyPhase`; this is the one place that computes it.
[[nodiscard]] constexpr CyPhase phase_of_stage(CyStage stage) noexcept {
    if (stage <= CY_STAGE_POST_SIMULATION) {
        return CY_PHASE_FIXED_UPDATE;
    }
    return stage <= CY_STAGE_UI ? CY_PHASE_FRAME_UPDATE : CY_PHASE_NONE;
}

/// Sets the clock's phase for a scope and restores the previous one on exit, so an entry called
/// from inside a scheduled system sees the phase of the stage it runs in.
class PhaseScope {
public:
    PhaseScope(GameClock& clock, CyPhase phase) noexcept : clock_(clock), previous_(clock.phase) {
        clock_.phase = phase;
    }
    ~PhaseScope() { clock_.phase = previous_; }

    PhaseScope(const PhaseScope&) = delete;
    PhaseScope& operator=(const PhaseScope&) = delete;
    PhaseScope(PhaseScope&&) = delete;
    PhaseScope& operator=(PhaseScope&&) = delete;

private:
    GameClock& clock_;
    CyPhase previous_;
};

// --- struct_size ---------------------------------------------------------------------------------
//
// Every sized 1.3 struct begins with `uint32_t struct_size`. A caller compiled against an older,
// shorter struct sets a smaller size; zero means "the size this header declares" (`cy_abi.h`).

template <class T>
/// The number of bytes both sides know for `T`, or zero when `struct_size` is non-zero
/// but too small to hold even itself — which is a malformed struct, not an old one.
[[nodiscard]] u32 agreed_size(u32 struct_size) noexcept {
    if (struct_size == 0U) {
        return static_cast<u32>(sizeof(T));
    }
    if (struct_size < sizeof(u32)) {
        return 0U;
    }
    return struct_size < sizeof(T) ? struct_size : static_cast<u32>(sizeof(T));
}

template <class T>
/// Copy a caller's struct into a whole, zero-filled one of this build's size. False for a malformed
/// `struct_size`; `out` is then zero-filled. `out.struct_size` is always `sizeof(T)` afterwards, so
/// a backend never sees a short struct.
[[nodiscard]] bool read_sized(const T& in, T& out) noexcept {
    std::memset(&out, 0, sizeof(T));
    const u32 size = agreed_size<T>(in.struct_size);
    if (size == 0U) {
        return false;
    }
    std::memcpy(&out, &in, size);
    out.struct_size = static_cast<u32>(sizeof(T));
    return true;
}

template <class T>
/// Write `value` into the caller's struct, only the prefix the caller declared, and set the
/// caller's `struct_size` to the bytes written. False for a malformed `struct_size`, having
/// written nothing.
[[nodiscard]] bool write_sized(T& out, const T& value) noexcept {
    const u32 size = agreed_size<T>(out.struct_size);
    if (size == 0U) {
        return false;
    }
    std::memcpy(&out, &value, size);
    out.struct_size = size;
    return true;
}

}  // namespace cy::abi::game
