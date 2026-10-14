// SPDX-License-Identifier: MIT
// cy/abi/game/illumination.h — the backend behind ABI 1.9's global illumination entries.
//
// `cy_abi` cannot name CyberGI: `cy::rendering-gi` is a rendering module and the table is in every
// build. So the illumination system is reached through this backend, which
// `cy::game_backend::IlluminationAdapter` implements over `cy::rendering::gi::IlluminationSystem`.
// A host that runs no GI binds none, and every 1.9 entry answers UNAVAILABLE.
//
// The thunks (src/abi/src/game/illumination_thunks.cpp) check the engine, the phase, the binding
// and the pointers, normalise every sized struct and refuse an enumerator out of range; a backend
// validates DOMAIN facts — an entity it does not know, a stale volume, a box with no volume — and
// reports them with `cy::abi::report`, returning the code.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>

namespace cy::abi::game {

/// The GI, as the 1.9 entries see it. Every method is called on the game thread.
class IlluminationBackend {
public:
    virtual ~IlluminationBackend() = default;

    [[nodiscard]] virtual CyResult settings(CyGiSettings& out) const noexcept = 0;
    /// `settings` is whole, its enumerators in range.
    [[nodiscard]] virtual CyResult set_settings(const CyGiSettings& settings) noexcept = 0;
    /// `desc` is whole, its enumerators in range and `sets` within the known bits.
    [[nodiscard]] virtual CyResult create_volume(const CyGiVolumeDesc& desc,
                                                 CyGiVolume& out) noexcept = 0;
    [[nodiscard]] virtual CyResult update_volume(CyGiVolume volume,
                                                 const CyGiVolumeDesc& desc) noexcept = 0;
    [[nodiscard]] virtual CyResult destroy_volume(CyGiVolume volume) noexcept = 0;
    /// `klass` is a `CyGiClass`, AUTOMATIC included.
    [[nodiscard]] virtual CyResult set_object_class(CyEntity entity, u32 klass) noexcept = 0;
    [[nodiscard]] virtual CyResult object_class(CyEntity entity, u32& out) const noexcept = 0;
    /// `mode` is a `CyGiEmission`.
    [[nodiscard]] virtual CyResult set_emission(CyEntity entity, u64 material,
                                                u32 mode) noexcept = 0;
    /// `normal` is null for the six-direction mean; both are finite.
    [[nodiscard]] virtual CyResult sample_irradiance(const f32 (&position)[3], const f32* normal,
                                                     CyGiIrradiance& out) const noexcept = 0;
    [[nodiscard]] virtual CyResult status(CyGiStatus& out) const noexcept = 0;
};

}  // namespace cy::abi::game
