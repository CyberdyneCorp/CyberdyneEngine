// SPDX-License-Identifier: MIT
#pragma once
// The first-person adapter behind ABI 1.13's `first_person_*` and `viewmodel_*` entries:
// `cy::camera::FirstPersonCamera` for each camera a game gives behaviours to, and the weapon view
// drawn beside it.
//
// A game drives it through the table — `first_person_input` every frame update, a kick per shot —
// and the HOST runs it: once a frame it calls `advance(seconds)`, which applies each camera's input
// since the last frame, then reads `pose(camera)` to place the eye at the character's head and the
// weapon's instances in the eye's space (`camera::first_person_eye`, `first_person_weapon`), and
// `viewmodel(camera)` for the weapon view it hands the frame (`viewmodel::assembly_view`,
// `ViewmodelFrame`, `write_viewmodel`). Nothing here draws, and nothing feeds back into a fixed
// step.

#include <cy/abi/cy_abi.h>
#include <cy/abi/game/first_person.h>
#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/viewmodel/viewmodel.h>
#include <cy/servers/camera/first_person.h>

namespace cy::game_backend {

/// The engine's settings for an ABI description, field for field; `motion_reduction` is one minus
/// the engine's `motion_scale`.
[[nodiscard]] camera::FirstPersonSettings settings_of(const CyFirstPersonDesc& desc) noexcept;

/// The weapon view an ABI description describes, drawing with `fov_now` when the description
/// follows the first-person camera's lens (`fov` zero). Disabled when `layers` is zero.
[[nodiscard]] rendering::viewmodel::ViewmodelSettings viewmodel_of(const CyViewmodelDesc& desc,
                                                                   f32 fov_now) noexcept;

class FirstPersonAdapter final : public abi::game::FirstPersonBackend {
public:
    explicit FirstPersonAdapter(Allocator& allocator) noexcept;

    /// How many cameras may have first-person behaviours at once: one a local player, normally.
    [[nodiscard]] Status create(u32 capacity) noexcept;
    /// One frame for every camera: the input that arrived since the last frame, over `seconds`.
    void advance(f32 seconds) noexcept;
    /// The camera's last frame, or null for a camera with no behaviours.
    [[nodiscard]] const camera::FirstPersonPose* pose(CyCamera camera) const noexcept;
    /// The camera's weapon view as the frame draws it — its lens following the behaviours' when it
    /// was set to — or a disabled one for a camera without.
    [[nodiscard]] rendering::viewmodel::ViewmodelSettings viewmodel(CyCamera camera) const noexcept;
    [[nodiscard]] u32 cameras() const noexcept;

    // --- abi::game::FirstPersonBackend -----------------------------------------------------------
    [[nodiscard]] CyResult configure(CyCamera camera,
                                     const CyFirstPersonDesc& desc) noexcept override;
    [[nodiscard]] CyResult input(CyCamera camera,
                                 const CyFirstPersonInput& input) noexcept override;
    [[nodiscard]] CyResult kick(CyCamera camera, f32 pitch, f32 yaw) noexcept override;
    [[nodiscard]] CyResult reset(CyCamera camera, f32 yaw, f32 pitch) noexcept override;
    [[nodiscard]] CyResult state(CyCamera camera, CyFirstPersonState& out) const noexcept override;
    [[nodiscard]] CyResult release(CyCamera camera) noexcept override;
    [[nodiscard]] CyResult configure_viewmodel(CyCamera camera,
                                               const CyViewmodelDesc& desc) noexcept override;
    [[nodiscard]] CyResult viewmodel(CyCamera camera, CyViewmodelDesc& out) const noexcept override;

private:
    struct Slot {
        CyCamera camera = 0;
        camera::FirstPersonCamera behaviours;
        /// The input since the last frame: the look summed, the rest the latest.
        camera::FirstPersonInput pending;
        CyViewmodelDesc weapon{};
    };

    [[nodiscard]] Slot* find(CyCamera camera) noexcept;
    [[nodiscard]] const Slot* find(CyCamera camera) const noexcept;

    Array<Slot> slots_;
    u32 capacity_ = 0;
};

/// Bind `adapter` as `host.game.first_person`, or unbind with null.
void bind_first_person(cy::abi::Host& host, FirstPersonAdapter* adapter) noexcept;

}  // namespace cy::game_backend
