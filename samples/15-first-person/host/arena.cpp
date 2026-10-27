// SPDX-License-Identifier: MIT
// arena.cpp — the level as data. See arena.h.

#include "arena.h"

namespace sample::fps {
namespace {

using cy::Vec3;

constexpr ArenaBox kBoxes[] = {
    // The floor, and the four walls round it.
    {Vec3{0.0F, -0.25F, 0.0F}, Vec3{14.0F, 0.25F, 14.0F}, Surface::Concrete},
    {Vec3{0.0F, 2.0F, -14.25F}, Vec3{14.5F, 2.0F, 0.25F}, Surface::PaintRed},
    {Vec3{0.0F, 2.0F, 14.25F}, Vec3{14.5F, 2.0F, 0.25F}, Surface::Plaster},
    {Vec3{-14.25F, 2.0F, 0.0F}, Vec3{0.25F, 2.0F, 14.0F}, Surface::PaintTeal},
    {Vec3{14.25F, 2.0F, 0.0F}, Vec3{0.25F, 2.0F, 14.0F}, Surface::Plaster},
    // The platform, 0.6 m high: above the character's 0.35 m step, so it takes a jump.
    {Vec3{-7.0F, 0.3F, 2.0F}, Vec3{2.0F, 0.3F, 2.0F}, Surface::Steel},
    // The beam across the passage at z = 4, its underside at 1.4 m, on two posts.
    {Vec3{5.0F, 1.65F, 4.0F}, Vec3{1.75F, 0.25F, 0.35F}, Surface::Steel},
    {Vec3{3.1F, 0.825F, 4.0F}, Vec3{0.15F, 0.825F, 0.35F}, Surface::Steel},
    {Vec3{6.9F, 0.825F, 4.0F}, Vec3{0.15F, 0.825F, 0.35F}, Surface::Steel},
    // Cover: a crate and a low wall.
    {Vec3{-3.0F, 0.6F, -3.0F}, Vec3{0.6F, 0.6F, 0.6F}, Surface::Wood},
    {Vec3{1.5F, 0.5F, -6.5F}, Vec3{1.4F, 0.5F, 0.3F}, Surface::Concrete},
    // Two pillars.
    {Vec3{-6.0F, 2.0F, -7.0F}, Vec3{0.5F, 2.0F, 0.5F}, Surface::Plaster},
    {Vec3{8.5F, 2.0F, -2.5F}, Vec3{0.5F, 2.0F, 0.5F}, Surface::Plaster},
    // The lamp strip on the north wall: it glows, and the GI carries its light.
    {Vec3{0.0F, 3.1F, -13.95F}, Vec3{4.0F, 0.12F, 0.06F}, Surface::Lamp},
};

constexpr PropBox kProps[] = {
    {"Crate0", Vec3{1.6F, 0.35F, -1.0F}, Vec3{0.35F, 0.35F, 0.35F}, 18.0F, Surface::Wood},
    {"Crate1", Vec3{2.4F, 0.35F, -1.2F}, Vec3{0.35F, 0.35F, 0.35F}, 18.0F, Surface::Wood},
    {"Crate2", Vec3{2.0F, 1.05F, -1.1F}, Vec3{0.35F, 0.35F, 0.35F}, 18.0F, Surface::Wood},
    {"Barrel", Vec3{-1.5F, 0.45F, -8.0F}, Vec3{0.3F, 0.45F, 0.3F}, 30.0F, Surface::Barrel},
};

constexpr TargetSpot kTargets[] = {
    {"T0", Vec3{-4.0F, 0.0F, -10.0F}, Vec3{-1.0F, 0.0F, -10.5F}},
    {"T1", Vec3{4.0F, 0.0F, -11.0F}, Vec3{7.5F, 0.0F, -11.0F}},
    {"T2", Vec3{-9.5F, 0.0F, -3.5F}, Vec3{-9.5F, 0.0F, -6.5F}},
    {"T3", Vec3{10.5F, 0.0F, -8.0F}, Vec3{10.5F, 0.0F, -11.0F}},
};

constexpr WeaponPart kWeapon[] = {
    {Vec3{0.0F, 0.0F, -0.12F}, Vec3{0.026F, 0.034F, 0.13F}, Surface::WeaponMetal},
    {Vec3{0.0F, 0.012F, -0.40F}, Vec3{0.011F, 0.011F, 0.15F}, Surface::WeaponMetal},
    {Vec3{0.0F, -0.07F, 0.0F}, Vec3{0.017F, 0.045F, 0.022F}, Surface::WeaponPolymer},
    {Vec3{0.0F, -0.08F, -0.15F}, Vec3{0.014F, 0.052F, 0.026F}, Surface::WeaponPolymer},
    {Vec3{0.035F, -0.13F, 0.08F}, Vec3{0.045F, 0.045F, 0.11F}, Surface::Sleeve},
};

struct SurfaceLook {
    Vec3 albedo;
    Vec3 emission;
    cy::f32 roughness = 0.5F;
    cy::f32 metallic = 0.0F;
};

/// Measured-ish linear albedos: weathered concrete about 40 %, painted walls, bare steel.
constexpr SurfaceLook kLooks[] = {
    {Vec3{0.40F, 0.39F, 0.37F}, Vec3{}, 0.85F, 0.0F},                             // Concrete
    {Vec3{0.62F, 0.20F, 0.12F}, Vec3{}, 0.70F, 0.0F},                             // PaintRed
    {Vec3{0.10F, 0.40F, 0.42F}, Vec3{}, 0.70F, 0.0F},                             // PaintTeal
    {Vec3{0.62F, 0.60F, 0.56F}, Vec3{}, 0.80F, 0.0F},                             // Plaster
    {Vec3{0.46F, 0.47F, 0.49F}, Vec3{}, 0.40F, 1.0F},                             // Steel
    {Vec3{0.90F, 0.88F, 0.80F}, Vec3{36000.0F, 26000.0F, 14000.0F}, 0.5F, 0.0F},  // Lamp
    {Vec3{0.48F, 0.33F, 0.19F}, Vec3{}, 0.75F, 0.0F},                             // Wood
    {Vec3{0.50F, 0.25F, 0.13F}, Vec3{}, 0.45F, 0.6F},  // Barrel: weathered copper
    {Vec3{0.80F, 0.42F, 0.08F}, Vec3{}, 0.60F, 0.0F},  // Target
    {Vec3{0.85F, 0.80F, 0.70F}, Vec3{}, 0.60F, 0.0F},  // TargetHead
    {Vec3{0.42F, 0.43F, 0.45F}, Vec3{}, 0.32F, 0.9F},  // WeaponMetal
    {Vec3{0.12F, 0.12F, 0.12F}, Vec3{}, 0.50F, 0.0F},  // WeaponPolymer
    {Vec3{0.20F, 0.26F, 0.14F}, Vec3{}, 0.85F, 0.0F},  // Sleeve
    // A clear sky's radiance, nits: a few thousand at the horizon, deeper blue overhead.
    {Vec3{}, Vec3{2600.0F, 3500.0F, 5000.0F}, 1.0F, 0.0F},  // Sky
    {Vec3{}, Vec3{2300.0F, 3200.0F, 4900.0F}, 1.0F, 0.0F},  // SkyZenith
};
static_assert(sizeof(kLooks) / sizeof(kLooks[0]) == static_cast<cy::usize>(Surface::Count));

[[nodiscard]] const SurfaceLook& look(Surface surface) noexcept {
    return kLooks[static_cast<cy::usize>(surface)];
}

constexpr ArenaBox kSky[] = {
    {Vec3{0.0F, 40.0F, -120.0F}, Vec3{130.0F, 46.0F, 1.0F}, Surface::Sky},
    {Vec3{0.0F, 40.0F, 120.0F}, Vec3{130.0F, 46.0F, 1.0F}, Surface::Sky},
    {Vec3{-120.0F, 40.0F, 0.0F}, Vec3{1.0F, 46.0F, 130.0F}, Surface::Sky},
    {Vec3{120.0F, 40.0F, 0.0F}, Vec3{1.0F, 46.0F, 130.0F}, Surface::Sky},
    {Vec3{0.0F, 86.0F, 0.0F}, Vec3{130.0F, 1.0F, 130.0F}, Surface::SkyZenith},
};

}  // namespace

cy::Span<const ArenaBox> sky_panels() noexcept {
    return {kSky, sizeof(kSky) / sizeof(kSky[0])};
}

cy::Span<const ArenaBox> arena_boxes() noexcept {
    return {kBoxes, sizeof(kBoxes) / sizeof(kBoxes[0])};
}

cy::Span<const PropBox> prop_boxes() noexcept {
    return {kProps, sizeof(kProps) / sizeof(kProps[0])};
}

cy::Span<const TargetSpot> target_spots() noexcept {
    return {kTargets, sizeof(kTargets) / sizeof(kTargets[0])};
}

cy::Span<const WeaponPart> weapon_parts() noexcept {
    return {kWeapon, sizeof(kWeapon) / sizeof(kWeapon[0])};
}

cy::Vec3 surface_albedo(Surface surface) noexcept {
    return look(surface).albedo;
}

cy::Vec3 surface_emission(Surface surface) noexcept {
    return look(surface).emission;
}

cy::f32 surface_roughness(Surface surface) noexcept {
    return look(surface).roughness;
}

cy::f32 surface_metallic(Surface surface) noexcept {
    return look(surface).metallic;
}

}  // namespace sample::fps
