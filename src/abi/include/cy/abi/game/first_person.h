// SPDX-License-Identifier: MIT
// cy/abi/game/first_person.h — the backend behind ABI 1.13's first-person camera and weapon view.
//
// `cy_abi` cannot name `cy::servers-camera`'s first-person behaviours or the renderer's weapon
// view: the table is in every build and a dedicated server draws nothing. So they are reached
// through this backend, which `cy::game_backend::FirstPersonAdapter`
// (src/game_backend/first_person/) implements. A host that draws nothing binds none, and every 1.13
// entry answers UNAVAILABLE.
//
// The thunks (src/abi/src/game/first_person_thunks.cpp) check the engine, the phase, the binding,
// the pointers and the camera, normalise every sized struct and refuse a number out of its range, a
// flag they do not know or a nonzero reserved word; a backend validates DOMAIN facts — a camera
// with no behaviours, a full table — and reports them with `cy::abi::report`, returning the code.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>

namespace cy::abi::game {

/// The first-person cameras and their weapon views, as the 1.13 entries see them. Every method is
/// called on the game thread, in no phase or a frame update, with a non-null camera.
class FirstPersonBackend {
public:
    virtual ~FirstPersonBackend() = default;

    /// `desc` is whole and every number in its range.
    [[nodiscard]] virtual CyResult configure(CyCamera camera,
                                             const CyFirstPersonDesc& desc) noexcept = 0;
    /// `input` is whole, its flags known and its numbers finite.
    [[nodiscard]] virtual CyResult input(CyCamera camera,
                                         const CyFirstPersonInput& input) noexcept = 0;
    /// Both finite.
    [[nodiscard]] virtual CyResult kick(CyCamera camera, f32 pitch, f32 yaw) noexcept = 0;
    [[nodiscard]] virtual CyResult reset(CyCamera camera, f32 yaw, f32 pitch) noexcept = 0;
    [[nodiscard]] virtual CyResult state(CyCamera camera,
                                         CyFirstPersonState& out) const noexcept = 0;
    [[nodiscard]] virtual CyResult release(CyCamera camera) noexcept = 0;
    /// `desc` is whole; with `layers` set, its lens in range.
    [[nodiscard]] virtual CyResult configure_viewmodel(CyCamera camera,
                                                       const CyViewmodelDesc& desc) noexcept = 0;
    [[nodiscard]] virtual CyResult viewmodel(CyCamera camera,
                                             CyViewmodelDesc& out) const noexcept = 0;
};

}  // namespace cy::abi::game
